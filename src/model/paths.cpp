#include "model/paths.h"

#include "model/model.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace wb::paths {
namespace {

fs::path g_root, g_settings;
std::optional<fs::path> g_downloads;
bool g_overridden = false;

fs::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

}  // namespace

void init(std::optional<fs::path> override_root) {
    g_overridden = override_root.has_value();
    if (override_root) {
        g_root = *override_root;
        g_settings = *override_root / L"_settings";
        g_downloads.reset();
    } else {
        // USERPROFILE\Music rather than FOLDERID_Music: the Flutter build uses the former, and the two differ when
        // Music is redirected (e.g. into OneDrive).
        const fs::path home = known_folder(FOLDERID_Profile);
        g_root = home / L"Music" / L"WreckBox";
        g_settings = known_folder(FOLDERID_RoamingAppData) / L"local.wreckbox" / L"wreckbox";
        g_downloads = home / L"Downloads";
    }
    std::error_code ec;
    for (const auto& d : {g_root, tracks(), inbox(), cache(), artwork(), g_settings}) fs::create_directories(d, ec);
}

const fs::path& root() { return g_root; }
const fs::path& settings_dir() { return g_settings; }
const std::optional<fs::path>& downloads() { return g_downloads; }
bool overridden() { return g_overridden; }

bool is_audio(const fs::path& p) {
    std::wstring ext = p.extension().wstring();
    if (ext.empty()) return false;
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    for (const wchar_t* a : {L".mp3", L".wav", L".aif", L".aiff", L".flac", L".m4a", L".alac", L".aac", L".ogg", L".opus"})
        if (ext == a) return true;
    return false;
}

void write_atomic(const fs::path& file, const std::string& contents) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    // Per-thread temp name: concurrent writers of the same file (e.g. two cover downloads) never share a temp file;
    // the last rename wins with identical content.
    fs::path tmp = file;
    tmp += L"." + std::to_wstring(GetCurrentThreadId()) + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(contents.data(), std::streamsize(contents.size()));
        out.close();
        if (!out) throw std::runtime_error("can't write " + narrow(tmp.wstring()));
    }
    if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("can't replace " + narrow(file.wstring()));
}

std::optional<std::string> read_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace wb::paths
