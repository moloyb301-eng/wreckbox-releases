#include "dasvis/spout.h"

#include <dxgi.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <set>
#include <string>

namespace wb::dasvis {

namespace {

constexpr size_t kNameLen = 256;     // each name in the list, as Spout keeps them
constexpr size_t kMaxSenders = 64;   // Spout 2.007's default list size

// What a receiver reads under the sender's name (Spout's SharedTextureInfo, 280 bytes).
struct SharedTextureInfo {
    uint32_t share_handle, width, height;
    DWORD format, usage;
    wchar_t description[128];
    uint32_t partner_id;
};
static_assert(sizeof(SharedTextureInfo) == 280);

// A named shared memory block: created at `size`, or the one already there (another Spout program's, maybe smaller or
// larger: then all of it is mapped). Returns the view and its usable size.
HANDLE open_map(const char* name, size_t size) {
    return CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, DWORD(size), name);
}

struct View {
    void* p = nullptr;
    size_t size = 0;
    explicit View(HANDLE map) {
        if (!map) return;
        p = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        MEMORY_BASIC_INFORMATION mi{};
        if (p && VirtualQuery(p, &mi, sizeof mi)) size = mi.RegionSize;
    }
    ~View() {
        if (p) UnmapViewOfFile(p);
    }
};

// The list of senders: names in 256-byte slots, the first empty slot ending it. Changed under "SpoutSenderNames_mutex".
std::set<std::string> read_names(const View& v) {
    std::set<std::string> out;
    const size_t slots = std::min(kMaxSenders, v.size / kNameLen);
    for (size_t i = 0; i < slots; ++i) {
        const char* s = static_cast<const char*>(v.p) + i * kNameLen;
        if (!*s) break;
        out.insert(std::string(s, strnlen(s, kNameLen - 1)));
    }
    return out;
}

void write_names(const View& v, const std::set<std::string>& names) {
    const size_t slots = std::min(kMaxSenders, v.size / kNameLen);
    char* p = static_cast<char*>(v.p);
    size_t i = 0;
    for (const auto& n : names) {
        if (i >= slots) break;
        std::memset(p + i * kNameLen, 0, kNameLen);
        std::memcpy(p + i * kNameLen, n.data(), std::min(n.size(), kNameLen - 1));
        ++i;
    }
    if (i < slots) p[i * kNameLen] = 0;
}

void change_names(HANDLE map, HANDLE mutex, const std::function<void(std::set<std::string>&)>& f) {
    if (!map) return;
    const bool locked = mutex && WaitForSingleObject(mutex, 100) != WAIT_TIMEOUT;
    {
        View v(map);
        if (v.p) {
            auto names = read_names(v);
            f(names);
            write_names(v, names);
        }
    }
    if (locked) ReleaseMutex(mutex);
}

}  // namespace

SpoutSender::SpoutSender(std::string name) : name_(std::move(name)) {}

SpoutSender::~SpoutSender() { stop(); }

bool SpoutSender::start(ID3D11Device* dev, UINT w, UINT h) {
    // The shared texture (the old way of sharing: a 32-bit handle any Direct3D 9–12 receiver can open).
    shared_.Reset();
    D3D11_TEXTURE2D_DESC d{w, h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0,
                           D3D11_RESOURCE_MISC_SHARED};
    Microsoft::WRL::ComPtr<IDXGIResource> res;
    HANDLE handle = nullptr;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &shared_)) || FAILED(shared_.As(&res)) || FAILED(res->GetSharedHandle(&handle))) {
        shared_.Reset();
        return false;
    }
    w_ = w, h_ = h;
    // Its details, under the sender's name.
    if (!info_) info_ = open_map(name_.c_str(), sizeof(SharedTextureInfo));
    View iv(info_);
    if (!iv.p) return false;
    SharedTextureInfo info{};
    info.share_handle = uint32_t(reinterpret_cast<uintptr_t>(handle)), info.width = w, info.height = h;
    info.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    std::memcpy(iv.p, &info, sizeof info);
    // In the list of senders, and the active one if there's none.
    if (!names_) {
        names_ = open_map("SpoutSenderNames", kMaxSenders * kNameLen);
        names_mutex_ = CreateMutexA(nullptr, FALSE, "SpoutSenderNames_mutex");
        change_names(names_, names_mutex_, [this](std::set<std::string>& n) { n.insert(name_); });
        active_ = open_map("ActiveSenderName", kNameLen);
        View av(active_);
        if (av.p) {
            char* a = static_cast<char*>(av.p);
            HANDLE there = *a ? OpenFileMappingA(FILE_MAP_READ, FALSE, a) : nullptr;  // is the active one still sending?
            if (!there) {
                std::memset(a, 0, kNameLen);
                std::memcpy(a, name_.data(), std::min(name_.size(), kNameLen - 1));
            } else {
                CloseHandle(there);
            }
        }
    }
    return true;
}

void SpoutSender::stop() {
    if (names_) {
        change_names(names_, names_mutex_, [this](std::set<std::string>& n) { n.erase(name_); });
        View av(active_);
        if (av.p && std::strncmp(static_cast<char*>(av.p), name_.c_str(), kNameLen) == 0) std::memset(av.p, 0, kNameLen);
    }
    for (HANDLE* h : {&info_, &names_, &active_, &names_mutex_, &access_})
        if (*h) CloseHandle(*h), *h = nullptr;
    shared_.Reset();
    w_ = h_ = 0;
}

bool SpoutSender::send(ID3D11Device* dev, ID3D11Texture2D* frame) {
    if (!dev || !frame) return false;
    D3D11_TEXTURE2D_DESC d;
    frame->GetDesc(&d);
    if (d.Format != DXGI_FORMAT_B8G8R8A8_UNORM) return false;
    if ((!shared_ || d.Width != w_ || d.Height != h_) && !start(dev, d.Width, d.Height)) return false;
    // Receivers read under "<name>_SpoutAccessMutex" when it's there: copy under it, so a frame is never read half done.
    if (!access_) access_ = CreateMutexA(nullptr, FALSE, (name_ + "_SpoutAccessMutex").c_str());
    const bool locked = access_ && WaitForSingleObject(access_, 4) != WAIT_TIMEOUT;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(&ctx);
    ctx->CopyResource(shared_.Get(), frame);
    ctx->Flush();  // the receiver's device sees it only once it's on the GPU
    if (locked) ReleaseMutex(access_);
    return true;
}

}  // namespace wb::dasvis
