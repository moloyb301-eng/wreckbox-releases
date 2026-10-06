#include "player/milkdrop.h"

#include <GL/glew.h>
#include <GL/wglew.h>
#include <projectM-4/playlist.h>
#include <projectM-4/projectM.h>
#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <random>

namespace wb::player {
namespace {

std::string utf8(const std::filesystem::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

// One hidden window (+ a legacy context when asked), only to get wglGetProcAddress working for the ARB pbuffer / context
// functions and a DC to make pbuffers from. A window belongs to its thread, so it's created and dropped on the same one.
struct Dummy {
    HWND wnd = nullptr;
    HDC dc = nullptr;
    HGLRC rc = nullptr;
    ~Dummy() {
        wglMakeCurrent(nullptr, nullptr);
        if (rc) wglDeleteContext(rc);
        if (dc) ReleaseDC(wnd, dc);
        if (wnd) DestroyWindow(wnd);
    }
    bool create(bool with_context = true) {
        static const bool registered = [] {
            WNDCLASSW wc{};
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"wb_milkdrop_probe";
            return RegisterClassW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        }();
        if (!registered) return false;
        wnd = CreateWindowExW(0, L"wb_milkdrop_probe", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!wnd) return false;
        dc = GetDC(wnd);
        PIXELFORMATDESCRIPTOR pfd{};
        pfd.nSize = sizeof pfd;
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        int fmt = ChoosePixelFormat(dc, &pfd);
        if (!fmt || !SetPixelFormat(dc, fmt, &pfd)) return false;
        if (!with_context) return true;
        rc = wglCreateContext(dc);
        return rc && wglMakeCurrent(dc, rc);
    }
};

}  // namespace

struct MilkDrop::Impl {
    MilkDropConfig cfg;
    std::string error;
    bool failed = false;

    HPBUFFERARB pbuf = nullptr;
    HDC dc = nullptr;
    HGLRC rc = nullptr;
    int pixel_format = 0;
    int w = 0, h = 0;

    projectm_handle pm = nullptr;
    projectm_playlist_handle list = nullptr;
    bool user_lock = false;
    double auto_advance = 30;
    int failed_presets = 0;
    // projectM's shuffled "previous" picks another random preset, so we keep our own history of what was shown.
    std::vector<uint32_t> history;
    int64_t shown = -1;
    bool stepping_back = false;
    std::mt19937 rng{std::random_device{}()};

    GLuint pbo[2] = {0, 0};
    uint64_t frames = 0;
    std::vector<uint8_t> pixels;

    // Loading a preset compiles shaders, so anything that can switch presets needs the context on this thread.
    bool make_current() {
        if (failed) return false;
        if (wglGetCurrentContext() == rc) return true;
        return wglMakeCurrent(dc, rc) || fail("lost the OpenGL context");
    }
    bool fail(std::string why) {
        failed = true;
        if (error.empty()) error = std::move(why);
        return false;
    }

    bool make_pbuffer(HDC from) {
        const int attrs[] = {WGL_DRAW_TO_PBUFFER_ARB, 1, WGL_SUPPORT_OPENGL_ARB, 1, WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB,
                             WGL_COLOR_BITS_ARB, 32, WGL_ALPHA_BITS_ARB, 8, WGL_DEPTH_BITS_ARB, 24, WGL_STENCIL_BITS_ARB, 8, 0};
        if (!pixel_format) {
            UINT n = 0;
            if (!wglChoosePixelFormatARB || !wglChoosePixelFormatARB(from, attrs, nullptr, 1, &pixel_format, &n) || !n)
                return fail("no off-screen OpenGL pixel format");
        }
        pbuf = wglCreatePbufferARB(from, pixel_format, w, h, nullptr);
        if (!pbuf) return fail("can't create an OpenGL pbuffer");
        dc = wglGetPbufferDCARB(pbuf);
        return dc != nullptr;
    }
    void free_pbuffer() {
        if (pbuf) {
            if (dc) wglReleasePbufferDCARB(pbuf, dc);
            wglDestroyPbufferARB(pbuf);
        }
        pbuf = nullptr;
        dc = nullptr;
    }

    void make_pbos() {
        if (pbo[0]) glDeleteBuffers(2, pbo);
        glGenBuffers(2, pbo);
        for (GLuint b : pbo) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, b);
            glBufferData(GL_PIXEL_PACK_BUFFER, GLsizeiptr(w) * h * 4, nullptr, GL_STREAM_READ);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        frames = 0;
        pixels.assign(size_t(w) * h * 4, 0);
    }

    bool init() {
        const char* off = std::getenv("WRECKBOX_NO_MILKDROP");
        if (off && *off && *off != '0') return fail("disabled by WRECKBOX_NO_MILKDROP");
        w = cfg.width;
        h = cfg.height;
        Dummy probe;  // the legacy context is only for loading the ARB functions; dropped before we return
        if (!probe.create()) return fail("no OpenGL on this PC");
        glewExperimental = GL_TRUE;
        if (glewInit() != GLEW_OK || !WGLEW_ARB_pbuffer || !WGLEW_ARB_create_context || !WGLEW_ARB_pixel_format)
            return fail("OpenGL has no off-screen rendering");
        if (!make_pbuffer(probe.dc)) return false;
        const int ctx_attrs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3,
                                 WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
        rc = wglCreateContextAttribsARB(dc, nullptr, ctx_attrs);
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(probe.rc);
        probe.rc = nullptr;
        if (!rc) return fail("OpenGL 3.3 isn't available");
        if (!wglMakeCurrent(dc, rc)) return fail("can't use the OpenGL context");
        glewExperimental = GL_TRUE;
        if (glewInit() != GLEW_OK) return fail("OpenGL failed to start");
        if (!GLEW_VERSION_3_3) return fail("OpenGL 3.3 isn't available");
        make_pbos();

        pm = projectm_create();
        if (!pm) return fail("projectM failed to start");
        projectm_set_window_size(pm, size_t(w), size_t(h));
        projectm_set_soft_cut_duration(pm, 3);
        projectm_set_hard_cut_enabled(pm, false);  // beat-triggered hard cuts are jarring; presets change by the timer
        projectm_set_beat_sensitivity(pm, 1.0f);
        if (!cfg.texture_dir.empty()) {
            std::string t = utf8(cfg.texture_dir);
            const char* paths[] = {t.c_str()};
            projectm_set_texture_search_paths(pm, paths, 1);
        }
        list = projectm_playlist_create(pm);
        if (!list) return fail("projectM's playlist failed to start");
        projectm_playlist_set_shuffle(list, true);
        projectm_playlist_set_retry_count(list, 10);  // a preset that won't compile → try another, never a black screen
        projectm_playlist_set_preset_switch_failed_event_callback(
            list, [](const char*, const char*, void* self) { ++static_cast<Impl*>(self)->failed_presets; }, this);
        for (auto& d : cfg.preset_dirs) {
            std::error_code ec;
            if (std::filesystem::is_directory(d, ec)) projectm_playlist_add_path(list, utf8(d).c_str(), true, false);
        }
        projectm_playlist_set_preset_switched_event_callback(
            list,
            [](bool, unsigned index, void* p) {
                auto& s = *static_cast<Impl*>(p);
                if (!s.stepping_back && s.shown >= 0 && s.shown != index) {
                    s.history.push_back(uint32_t(s.shown));
                    if (s.history.size() > 200) s.history.erase(s.history.begin());
                }
                s.shown = index;
            },
            this);
        apply_timing();
        if (projectm_playlist_size(list)) projectm_playlist_play_next(list, true);
        wglMakeCurrent(nullptr, nullptr);  // so the thread that draws can take the context (render() does)
        return true;
    }

    // 0 = never by itself: a long duration plus the lock (manual next / previous still load).
    void apply_timing() {
        projectm_set_preset_duration(pm, auto_advance > 0 ? auto_advance : 1e6);
        projectm_set_preset_locked(pm, user_lock || auto_advance <= 0);
    }

    void release() {
        if (rc && dc && wglMakeCurrent(dc, rc)) {  // projectM frees its GL objects on destroy
            if (list) projectm_playlist_destroy(list);
            if (pm) projectm_destroy(pm);
            if (pbo[0]) glDeleteBuffers(2, pbo);
        }
        list = nullptr;
        pm = nullptr;
        pbo[0] = pbo[1] = 0;
        wglMakeCurrent(nullptr, nullptr);
        if (rc) wglDeleteContext(rc);
        rc = nullptr;
        free_pbuffer();
    }
    ~Impl() { release(); }
};

MilkDrop::MilkDrop(const MilkDropConfig& cfg) : p_(std::make_unique<Impl>()) {
    p_->cfg = cfg;
    try {
        p_->init();
    } catch (const std::exception& e) {
        p_->fail(e.what());
    }
    if (p_->failed) p_->release();  // leave nothing half-built (or current) behind
}
MilkDrop::~MilkDrop() = default;

bool MilkDrop::ok() const { return !p_->failed; }
const std::string& MilkDrop::error() const { return p_->error; }

MilkDrop::Frame MilkDrop::render(const float* mono, size_t n) {
    auto& s = *p_;
    if (s.failed) return {};
    if (!s.make_current()) return {};
    if (n) projectm_pcm_add_float(s.pm, mono, unsigned(n), PROJECTM_MONO);
    projectm_opengl_render_frame(s.pm);

    // Frame i is read into pbo[i % 2] now; the other buffer holds frame i-1, which the GPU finished long ago.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, s.pbo[s.frames % 2]);
    glReadPixels(0, 0, s.w, s.h, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
    Frame out;
    if (s.frames > 0) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, s.pbo[(s.frames + 1) % 2]);
        if (void* src = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY)) {
            std::memcpy(s.pixels.data(), src, s.pixels.size());
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            out = {s.pixels.data(), s.w, s.h};
        }
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    ++s.frames;
    return out;
}

bool MilkDrop::resize(int width, int height) {
    auto& s = *p_;
    if (s.failed) return false;
    if (width == s.w && height == s.h) return true;
    // Same context, new pbuffer (same pixel format): projectM keeps its state and textures.
    HDC old_dc = s.dc;
    HPBUFFERARB old = s.pbuf;
    s.w = width;
    s.h = height;
    Dummy window;  // a DC to make the new pbuffer from (the one init used lived on another thread)
    if (!window.create(false) || !s.rc || !wglMakeCurrent(old_dc, s.rc)) return s.fail("lost the OpenGL context");
    s.pbuf = nullptr;
    s.dc = nullptr;
    if (!s.make_pbuffer(window.dc)) {
        s.pbuf = old;  // so ~MilkDrop still frees it
        s.dc = old_dc;
        return false;
    }
    wglMakeCurrent(nullptr, nullptr);
    wglReleasePbufferDCARB(old, old_dc);
    wglDestroyPbufferARB(old);
    if (!wglMakeCurrent(s.dc, s.rc)) return s.fail("lost the OpenGL context");
    s.make_pbos();
    projectm_set_window_size(s.pm, size_t(width), size_t(height));
    return true;
}

void MilkDrop::next() {
    if (p_->make_current() && p_->list) projectm_playlist_play_next(p_->list, true);
}
void MilkDrop::previous() {
    auto& s = *p_;
    if (!s.make_current() || !s.list || s.history.empty()) return;
    const uint32_t back = s.history.back();
    s.history.pop_back();
    s.stepping_back = true;
    projectm_playlist_set_position(s.list, back, true);
    s.stepping_back = false;
}
void MilkDrop::random() {
    auto& s = *p_;
    uint32_t n = s.make_current() ? projectm_playlist_size(s.list) : 0;
    if (n) projectm_playlist_set_position(s.list, std::uniform_int_distribution<uint32_t>(0, n - 1)(s.rng), true);
}
void MilkDrop::lock(bool on) {
    p_->user_lock = on;
    if (!p_->failed) p_->apply_timing();
}
bool MilkDrop::locked() const { return p_->user_lock; }
void MilkDrop::set_auto_advance(double seconds) {
    p_->auto_advance = seconds;
    if (!p_->failed) p_->apply_timing();
}

std::string MilkDrop::preset_name() const {
    auto& s = *p_;
    if (s.failed || !s.list || !projectm_playlist_size(s.list)) return {};
    char* item = projectm_playlist_item(s.list, projectm_playlist_get_position(s.list));
    if (!item) return {};
    auto stem = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(item))).stem().u8string();
    projectm_playlist_free_string(item);
    return std::string(stem.begin(), stem.end());
}
size_t MilkDrop::preset_count() const { return p_->failed || !p_->list ? 0 : projectm_playlist_size(p_->list); }
int MilkDrop::failed_presets() const { return p_->failed_presets; }

}  // namespace wb::player
