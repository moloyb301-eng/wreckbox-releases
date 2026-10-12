// The full-screen visualizer: MilkDrop presets (player::MilkDrop, drawn from its pixels) by default, DasVis (WreckBox's
// own, dasvis::DasVis, drawing straight into the window's buffer on the same GPU device), or Winamp 2 style —
// spectrum bars (normal / fire / line) with falling peak caps, the oscilloscope (dots / lines / solid), both, or off, in
// Classic Winamp colours or WreckBox pastel. Without OpenGL 3.3 it shows the Winamp bars. Both draw the audio being heard
// as each frame appears (AudioOutput::heard_index), plus WreckBox's beat pulse over MilkDrop. Click: next preset
// (Winamp modes: step through them), right-click has every option, N / P / R / L / M / V and [ / ] (sync) keys; Esc,
// F11 or a double-click leave.
// The bar maths is player::Visualizer (unit-tested); this file draws it and runs the screen.
#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>

#include "model/paths.h"
#include "model/settings.h"
#include "ui/artwork.h"
#include "ui/view.h"

#include <qrcodegen.hpp>

#include "dasvis/video.h"
#include "net/http_client.h"
#include "net/sync_server.h"

#include <shlobj.h>
#include <shobjidl.h>

#include <psapi.h>

namespace wb::ui {

using namespace theme;
using player::VisOptions;
using clock_t_ = std::chrono::steady_clock;

namespace {

D2D1_COLOR_F mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    t = std::clamp(t, 0.f, 1.f);
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}
D2D1_COLOR_F ramp(D2D1_COLOR_F lo, D2D1_COLOR_F mid, D2D1_COLOR_F hi, float t) { return t < 0.5f ? mix(lo, mid, t * 2) : mix(mid, hi, t * 2 - 1); }
D2D1_COLOR_F faded(D2D1_COLOR_F c, float a) { return with_alpha(c, c.a * a); }

// Winamp 2's default colours: spectrum green → yellow → red from the bottom, grey peak caps, grey-white scope.
constexpr auto kWinampLow = argb(0xFF18C018), kWinampMid = argb(0xFFE8D818), kWinampHigh = argb(0xFFF03018);
constexpr auto kWinampPeak = argb(0xFFC8C8C8), kWinampScopeLow = argb(0xFF7C8690);

// WRECKBOX_PERF=1 also logs what MilkDrop does (quality drops) to wreckbox.log.
void perf_log(const std::string& line) {
    wchar_t v[4]{};
    if (!GetEnvironmentVariableW(L"WRECKBOX_PERF", v, 4) || v[0] != L'1') return;
    std::ofstream(paths::app_log(), std::ios::app) << iso_seconds_now() << " " << line << "\n";
}

// WRECKBOX_PERF=1: every 2 s, where the memory is — private bytes, and the C++ heaps' committed vs in-use bytes (the gap is
// freed memory the heap keeps), so a report from a weak PC says what grew.
void perf_memory() {
    static ULONGLONG last = 0;
    wchar_t v[4]{};
    if (GetTickCount64() - last < 2000 || !GetEnvironmentVariableW(L"WRECKBOX_PERF", v, 4) || v[0] != L'1') return;
    last = GetTickCount64();
    PROCESS_MEMORY_COUNTERS_EX pm{};
    pm.cb = sizeof pm;
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof pm);
    HANDLE heaps[64];
    const DWORD n = std::min<DWORD>(GetProcessHeaps(64, heaps), 64);
    double committed = 0, allocated = 0;
    std::string each;
    for (DWORD i = 0; i < n; ++i) {
        HEAP_SUMMARY s{};
        s.cb = sizeof s;
        if (!HeapSummary(heaps[i], 0, &s)) continue;
        committed += s.cbCommitted, allocated += s.cbAllocated;
        if (s.cbCommitted > 1 << 20) each += std::format(" [{:.1f}/{:.1f}]", s.cbAllocated / 1048576.0, s.cbCommitted / 1048576.0);
    }
    perf_log(std::format("mem: private {:.1f} MB, working set {:.1f} MB; {} heaps: {:.1f} MB in use of {:.1f} MB committed{}", pm.PrivateUsage / 1048576.0,
                         pm.WorkingSetSize / 1048576.0, n, allocated / 1048576.0, committed / 1048576.0, each));
}

constexpr int kQualities[] = {540, 720, 1080};
constexpr int kAdvances[] = {0, 15, 30, 60};

std::wstring clock_text(int64_t ms) {
    const int64_t s = std::max<int64_t>(0, ms) / 1000;
    return s >= 3600 ? std::format(L"{}:{:02}:{:02}", s / 3600, s / 60 % 60, s % 60) : std::format(L"{}:{:02}", s / 60, s % 60);
}

}  // namespace

// MARK: Window

void View::log_memory() { perf_memory(); }

void View::set_fullscreen(bool on) {
    if (on == fullscreen_) return;
    fullscreen_ = on;
    eq_open_ = url_open_ = false;
    if (bug_open_) close_bug_report();
    // Borderless over the whole monitor, then back exactly where it was (Raymond Chen's recipe).
    if (on) {
        vis_.opt = VisOptions::from_json(Settings::current().extra.value("visualizer", json::object()));
        wall_.reset();  // the device is made again for full screen: the wallpaper starts again after
        saved_style_ = GetWindowLongW(hwnd_, GWL_STYLE);
        GetWindowPlacement(hwnd_, &saved_place_);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(hwnd_, GWL_STYLE, saved_style_ & ~WS_OVERLAPPEDWINDOW);
        if (vis_.opt.das_screens == 1 && GetSystemMetrics(SM_CMONITORS) > 1)  // all the screens, as one picture
            mi.rcMonitor = {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                            GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                            GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
        const RECT& m = mi.rcMonitor;
        // No z-order change: it's the active window when asked (and automated checks stay behind other windows).
        SetWindowPos(hwnd_, nullptr, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        moved_at_ = audio_at_ = clock_t_::now();
        vis_at_ = {};
        milk_tried_ = false;
        milk_quality_ = vis_.opt.quality;
        slow_frames_ = 0;
        milk_heard_ = -1;
        pulse_ = {};
        frame_at_ = {};
        bt_output_ = player::default_output_is_bluetooth();  // looked at each visit: the default speaker may have changed
        if (bt_output_) perf_log("visualizer: Bluetooth speaker, pictures drawn 180 ms later");
        MONITORINFOEXW mx{};
        mx.cbSize = sizeof mx;
        DEVMODEW dm{};
        dm.dmSize = sizeof dm;
        das_hz_ = GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mx) && EnumDisplaySettingsW(mx.szDevice, ENUM_CURRENT_SETTINGS, &dm) &&
                          dm.dmDisplayFrequency > 1
                      ? float(dm.dmDisplayFrequency)
                      : 60.f;
    } else {
        if (milk_pending_.valid()) milk_pending_.wait();  // left before it finished starting
        milk_pending_ = {};
        milk_.reset();  // free the GPU and its memory
        das_free();
        g_.clear_stream();
        SetTimer(hwnd_, kTrimTimer, 8000, nullptr);  // once the OpenGL driver has let go too
        milk_heard_ = -1;
        SetWindowLongW(hwnd_, GWL_STYLE, saved_style_);
        SetWindowPlacement(hwnd_, &saved_place_);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        wall_restart();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void View::mouse_moved() {
    if (!fullscreen_) return;
    const bool was_hidden = overlay_alpha() < 1;
    moved_at_ = clock_t_::now();
    if (was_hidden) InvalidateRect(hwnd_, nullptr, FALSE);
}

// The overlay (panel, hint, exit button) shows for 4 s after the mouse moves, then fades; always while paused or while
// the equalizer is open.
float View::overlay_alpha() const {
    if (!player_.playing() || eq_open_) return 1;
    const float since = std::chrono::duration<float>(clock_t_::now() - moved_at_).count();
    return std::clamp(1 - (since - 4) / 0.6f, 0.f, 1.f);
}

bool View::hide_cursor() const { return fullscreen_ && overlay_alpha() == 0; }

bool View::double_click() {
    if (!fullscreen_) return false;
    vis_.opt.mode = mode_before_click_;  // the double-click's first click changed the mode: undo that
    save_vis();
    set_fullscreen(false);
    return true;
}

void View::set_vis_panel(bool on) {
    vis_.opt.panel = on;
    if (!on) eq_open_ = false;  // it hangs from the bar
    save_vis();
    mouse_moved();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void View::save_vis() {
    auto& s = Settings::current();
    s.extra["visualizer"] = vis_.opt.to_json();
    try {
        s.save();
    } catch (const std::exception&) {
        // Only the visualizer's options; the next change tries again.
    }
}

bool View::vis_key(WPARAM vk) {
    switch (vk) {
        case VK_ESCAPE:
        case VK_F11: set_fullscreen(false); return true;
        case VK_SPACE: player_.toggle(); return true;
        case VK_LEFT:
        case VK_RIGHT: player_.seek(player_.time_ms() + (vk == VK_LEFT ? -5000 : 5000)); return true;
        case 'V':
            vis_.opt.classic = !vis_.opt.classic;
            save_vis();
            return true;
        case 'N':
        case 'P':
        case 'R':
        case 'L':
            mouse_moved();
            if (das_ && das_->ok() && vis_.opt.mode == VisOptions::Mode::dasvis) {
                if (vk == 'N' || vk == 'R') das_->next();
                else if (vk == 'P') das_->previous();
                else das_->lock(!das_->locked());
                audio_at_ = clock_t_::now();
            } else if (milk_ && milk_->ok()) {
                if (vk == 'N') milk_->next();
                else if (vk == 'P') milk_->previous();
                else if (vk == 'R') milk_->random();
                else milk_->lock(!milk_->locked());
                audio_at_ = clock_t_::now();  // show the change even while paused
            }
            return true;
        case 'F':
        case 'X':
            mouse_moved();
            if (das_ && das_->ok() && vis_.opt.mode == VisOptions::Mode::dasvis) das_rate(vk == 'F');
            return true;
        case 'M': vis_use(VisOptions::Mode::milkdrop); return true;
        case 'D':
            if (GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_SHIFT) < 0) return das_dev_ = !das_dev_, true;  // the dev view
            vis_use(VisOptions::Mode::dasvis);
            return true;
        case VK_F5: das_reload(); return true;
        case 'H': set_vis_panel(!vis_.opt.panel); return true;
        case VK_OEM_4:  // [  pictures earlier
        case VK_OEM_6:  // ]  pictures later
            vis_.opt.sync_ms = std::clamp(vis_.opt.sync_ms + (vk == VK_OEM_6 ? 25 : -25), -300, 300);
            milk_heard_ = -1;
            save_vis();
            show_toast(std::format(L"Sync {:+} ms{}", vis_.opt.sync_ms, bt_output_ ? L" (+180 ms Bluetooth)" : L""));
            return true;
        default: mouse_moved(); return false;
    }
}

// MARK: Screen

std::filesystem::path View::user_presets_dir() const { return paths::settings_dir() / L"milkdrop" / L"presets"; }

void View::milk_start() {
    milk_tried_ = true;
    std::error_code ec;
    std::filesystem::create_directories(user_presets_dir(), ec);  // lazily: where the user drops their own .milk files
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto base = std::filesystem::path(exe).parent_path() / L"milkdrop";
    player::MilkDropConfig cfg;
    cfg.height = milk_quality_;
    cfg.width = int(std::lround(milk_quality_ * g_.width() / std::max(1.f, g_.height()))) & ~1;
    // The pack's categories, minus "! Transition" (made for blending, not watching) and, unless "All presets", the slow
    // Fractal and Hypnotic ones that barely move with the beat.
    std::error_code dir_ec;
    for (const auto& d : std::filesystem::directory_iterator(base / L"presets", dir_ec)) {
        const auto name = d.path().filename().wstring();
        if (!d.is_directory() || name.starts_with(L"!")) continue;
        if (!vis_.opt.all_presets && (name == L"Fractal" || name == L"Hypnotic")) continue;
        cfg.preset_dirs.push_back(d.path());
    }
    cfg.preset_dirs.push_back(user_presets_dir());
    cfg.hard_cuts = vis_.opt.hard_cuts;
    cfg.beat_sensitivity = float(vis_.opt.beat_sensitivity);
    cfg.texture_dir = base / L"textures";
    milk_w_ = cfg.width, milk_h_ = cfg.height;
    milk_asked_ = clock_t_::now();
    // OpenGL setup and reading ~10,000 preset files take most of a second: do it off the UI thread (the context is
    // released at the end, and render() takes it over here).
    milk_pending_ = std::async(std::launch::async, [cfg] { return std::make_unique<player::MilkDrop>(cfg); });
}

// True once the engine is up (or has failed, and the Winamp bars take over).
void View::milk_poll() {
    if (!milk_pending_.valid() || milk_pending_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    auto m = milk_pending_.get();
    if (!m->ok()) {
        milk_why_ = m->error();
        perf_log("MilkDrop unavailable: " + milk_why_);
        return;
    }
    perf_log(std::format("MilkDrop started in {:.0f} ms: {} presets at {}x{}",
                         std::chrono::duration<double, std::milli>(clock_t_::now() - milk_asked_).count(), m->preset_count(), milk_w_, milk_h_));
    m->set_auto_advance(vis_.opt.auto_advance);
    milk_ = std::move(m);
    audio_at_ = clock_t_::now();
}

int64_t View::heard_end(const player::AudioOutput& out, float frames_ahead) const {
    // The frame drawn now is on screen `frames_ahead` frames from now; Bluetooth and your calibration (+ = later) make
    // the sound that is heard then older.
    const float ms = frames_ahead * frame_dt_ * 1000 - float(vis_.opt.sync_ms + (bt_output_ ? 180 : 0));
    return out.heard_index(clock_t_::now() + std::chrono::duration_cast<clock_t_::duration>(std::chrono::duration<float, std::milli>(ms)));
}

// One MilkDrop frame, W × H in shape and `height` pixels tall, of what's heard as it appears (silence while paused). Also
// moves the beat pulse.
player::MilkDrop::Frame View::milk_frame(float W, float H, bool playing, int height) {
    const auto now = clock_t_::now();
    {
        size_t n = 0;
        player::AudioOutput* out = player_.output();
        if (playing && out) {
            // What's heard while this frame is on screen: it's read back one frame later, then presented — about two
            // frames from now. projectM analyses the newest 12 ms it's given, so it must get exactly this, every frame.
            const int64_t end = heard_end(*out, 2);
            if (milk_heard_ < 0 || end - milk_heard_ > int64_t(pcm_.size())) milk_heard_ = end - 800;
            n = size_t(std::max<int64_t>(0, end - milk_heard_));
            out->tap_until(end, pcm_.data(), n);
            milk_heard_ = std::max(milk_heard_, end);
            feed_samples_ += int64_t(n), feed_empty_ += n == 0;
            if (feed_since_ == clock_t_::time_point{}) feed_since_ = now;
            if (++feed_frames_ == 300) {
                const double s = std::chrono::duration<double>(now - feed_since_).count();
                perf_log(std::format("MilkDrop feed: {} frames in {:.2f} s ({:.1f} fps), {:.0f} samples a frame, {} frames without new "
                                     "audio; device latency {} ms, Bluetooth {}, sync {:+} ms",
                                     feed_frames_, s, feed_frames_ / s, double(feed_samples_) / feed_frames_, feed_empty_,
                                     out->latency_frames() * 1000 / player::kRate, bt_output_ ? "yes" : "no", vis_.opt.sync_ms));
                feed_frames_ = feed_empty_ = 0, feed_samples_ = 0, feed_since_ = now;
            }
        } else if (!playing) {
            std::fill(pcm_.begin(), pcm_.begin() + 512, 0.f);
            n = 512;
            milk_heard_ = -1;
        }
        pulse_.update(pcm_.data(), playing ? n : 0, frame_dt_);
        const int h = height, w = int(std::lround(h * W / std::max(1.f, H))) & ~1;
        if ((w != milk_w_ || h != milk_h_) && milk_->resize(w, h)) milk_w_ = w, milk_h_ = h;
        return milk_->ok() ? milk_->render(pcm_.data(), n) : player::MilkDrop::Frame{};
    }
}

void View::milk_screen(float W, float H) {
    const auto now = clock_t_::now();
    const bool playing = player_.playing();
    if (playing) audio_at_ = now;
    // Paused: keep rendering silence for 4 s so the picture calms down, then hold the last frame (no timer, 0% CPU).
    const bool live = playing || now - audio_at_ < std::chrono::seconds(4);
    vis_settling_ = !playing && live;
    if (live)
        if (const auto f = milk_frame(W, H, playing, milk_quality_)) g_.upload_stream(f.bgra, f.width, f.height);
    // The beat pulse: each kick zooms the picture in and flashes it, then it eases back over ~200 ms.
    const float e = vis_.opt.pulse ? pulse_.envelope() : 0.f;
    const bool strong = vis_.opt.pulse == 2;
    if (!g_.draw_stream(Rect{0, 0, W, H}, true, 1 + (strong ? 0.05f : 0.02f) * e)) g_.fill(Rect{0, 0, W, H}, black);
    else if (e > 0.02f) g_.fill(Rect{0, 0, W, H}, with_alpha(white, (strong ? 0.18f : 0.08f) * e));
}

void View::show_toast(std::wstring note) {
    toast_ = std::move(note);
    toast_until_ = clock_t_::now() + std::chrono::milliseconds(1500);
    SetTimer(hwnd_, kTooltipTimer, 1550, nullptr);  // clears it even when nothing else repaints (paused)
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void View::draw_toast() {
    if (toast_.empty() || clock_t_::now() >= toast_until_) return;
    const TextStyle st{Font::ui, 14, 600, text};
    const float w = g_.measure(toast_, st) + 36, x = (g_.width() - w) / 2;
    const Rect r = Rect::xywh(x, 64, w, 38);
    g_.fill_round(r, 19, argb(0xE01C1C21));
    g_.stroke_round(r, 19, glass_border);
    g_.text(toast_, Rect{r.l + 18, r.t, r.r, r.b}, st);
}

void View::milk_restart() {
    if (milk_pending_.valid()) milk_pending_.wait();
    milk_pending_ = {};
    milk_.reset();
    g_.clear_stream();
    milk_w_ = milk_h_ = 0;
    milk_tried_ = false;
    milk_heard_ = -1;
}

// MARK: DasVis

bool View::on_battery() const {
    SYSTEM_POWER_STATUS s{};
    return GetSystemPowerStatus(&s) && s.ACLineStatus == 0;
}

bool View::fast_gpu() const { return fullscreen_ && vis_.opt.mode == VisOptions::Mode::dasvis && vis_.opt.das_fast_gpu; }

UINT View::frame_interval() const {
    if (!fullscreen_ || vis_.opt.mode != VisOptions::Mode::dasvis) return 1;
    // A 120 Hz screen still gets 60; on battery, or with a MilkDrop scene too slow for 60, 30.
    const float fps = (vis_.opt.das_battery && on_battery()) || das_milk_slow_ ? 30.f : 60.f;
    return UINT(std::max(1.f, std::round(das_hz_ / fps)));
}

std::filesystem::path View::das_scenes_dir() const { return paths::settings_dir() / L"dasvis" / L"scenes"; }

void View::das_free() {
    if (!das_) return;
    if (das_->ok() && !das_gpu_.empty()) {  // what each scene cost here, so the director knows next time
        json costs = json::object();
        for (const auto& [k, ms] : das_->costs()) costs[k] = std::round(double(ms) * 100) / 100;
        if (vis_.opt.das_costs.value(das_gpu_, json::object()) != costs) vis_.opt.das_costs[das_gpu_] = costs, save_vis();
    }
    das_.reset();
    {
        std::lock_guard lock(vj_->m);
        vj_->state = R"({"open":false})", vj_->commands.clear();
    }
    vj_card_ = false;
    spout_.reset();
    screens_.clear();
    das_shown_ = {};
    das_cover_for_.clear();
    das_cover_.reset();
    das_map_.reset();  // in memory only; it's in the cache for next time
    das_map_for_.clear();
}

namespace {

std::string song_key(const player::Item& it) { return it.track_id ? *it.track_id : it.location; }

// Loads the map from the cache, or analyses the song and caches it. For a worker thread, at background priority, so
// playback and the window never feel it.
std::shared_ptr<const dasvis::SongMap> map_for(const std::string& key, const std::filesystem::path& file, double bpm) {
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    const auto t0 = clock_t_::now();
    const auto cache = dasvis::map_cache_file(paths::cache() / L"dasvis" / L"maps", key, file);
    auto m = dasvis::SongMap::load(cache);
    std::string why;
    if (!m) {
        m = dasvis::analyse_file(file, bpm, &why);
        if (m) m->save(cache);
        perf_log(m ? std::format("DasVis song map in {:.1f} s: {:.1f} BPM, {} parts, {} drops", std::chrono::duration<double>(clock_t_::now() - t0).count(),
                                 m->bpm, m->parts.size(), m->drops.size())
                   : "DasVis song map: " + why);
    }
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    return m ? std::make_shared<const dasvis::SongMap>(std::move(*m)) : nullptr;
}

}  // namespace

void View::das_request_map(const player::Item& item, double bpm) {
    das_map_for_ = song_key(item);
    das_map_.reset();
    das_map_busy_ = false;
    if (player::is_url(item.location) || item.location.empty()) return;  // radio: live only
    das_map_busy_ = true;
    auto result = std::make_shared<std::shared_ptr<const dasvis::SongMap>>();
    jobs_.run([key = das_map_for_, file = std::filesystem::path(widen(item.location)), bpm, result] { *result = map_for(key, file, bpm); },
              [this, key = das_map_for_, result] {
                  if (das_map_for_ != key) return;  // another song by now
                  das_map_ = *result;
                  das_map_busy_ = false;
                  das_prepare_next();
              });
}

void View::das_prepare_next() {
    const auto& items = player_.queue().items();
    const size_t next = size_t(player_.queue().index() + 1);
    if ((!das_ && !wall_) || das_next_busy_ || next >= items.size() || player::is_url(items[next].location)) return;
    const player::Item it = items[next];
    const auto row = it.track_id ? store_.row(*it.track_id) : std::nullopt;
    const double bpm = row && row->bpm() ? *row->bpm() : 0;
    das_next_busy_ = true;
    jobs_.run([key = song_key(it), file = std::filesystem::path(widen(it.location)), bpm] { map_for(key, file, bpm); },
              [this] { das_next_busy_ = false; });
}

void View::das_reload() {
    if (!das_ || !das_->ok()) return;
    const std::string errors = das_->reload_user_scenes();
    show_toast(errors.empty() ? std::format(L"{} scenes", das_->scene_count()) : L"Your scenes have errors (shown at the top)");
}

// The frame's inputs, the same for every DasVis drawing it (full screen, the other screens, the wallpaper): the audio
// heard as it appears, the song, its map, its look. Also asks for what the song needs: its map, cover and lyrics.
dasvis::Inputs View::das_inputs(float budget_ms) {
    auto& o = vis_.opt;
    player::AudioOutput* out = player_.output();
    static_assert(player::Visualizer::kFft == dasvis::Features::kFft);
    if (player_.playing() && out) out->tap_until(heard_end(*out, 1), tap_.data(), tap_.size());  // on screen at the next refresh
    else std::fill(tap_.begin(), tap_.end(), 0.f);
    const player::Item* cur = player_.current();
    const auto row = cur && cur->track_id ? store_.row(*cur->track_id) : std::nullopt;
    const double bpm = row && row->bpm() ? *row->bpm() : 0;
    das_feat_.update(tap_.data(), frame_dt_, bpm);
    if (cur && song_key(*cur) != das_map_for_) das_request_map(*cur, bpm);
    // Where the song is when this frame appears (a refresh from now), less the calibration (+ = pictures later).
    das_song_t_ = double(player_.time_ms()) / 1000 + frame_dt_ - (o.sync_ms + (bt_output_ ? 180 : 0)) / 1000.0;
    dasvis::Inputs in;
    in.f = &das_feat_;
    in.dt = frame_dt_;
    in.song = cur ? song_key(*cur) : std::string();
    if (row && !row->camelot().empty()) in.camelot = row->camelot();
    in.intensity = o.das_intensity;
    in.budget_ms = budget_ms;
    in.map = das_map_.get();
    in.song_time = das_song_t_;
    // Your likes and skips for this genre; glitch transitions for the genres that suit them.
    std::string genre = row ? row->genre : std::string();
    std::transform(genre.begin(), genre.end(), genre.begin(), [](char c) { return char(std::tolower(static_cast<unsigned char>(c))); });
    if (genre != das_genre_) {
        das_genre_ = genre;
        das_taste_.clear();
        if (const auto g = o.das_taste.find(genre.empty() ? "any" : genre); g != o.das_taste.end() && g->is_object())
            for (const auto& [scene, w] : g->items())
                if (w.is_number()) das_taste_[scene] = w.get<float>();
    }
    in.taste = &das_taste_;
    in.glitchy = genre.find("dubstep") != std::string::npos || genre.find("drum") != std::string::npos || genre.find("dnb") != std::string::npos;
    // The cover: decoded off the UI thread at 512 px, once for every DasVis, kept while the song plays.
    if (in.song != das_cover_for_) {
        das_cover_for_ = in.song;
        das_cover_.reset();
        if (row) {
            auto cover = std::make_shared<DasCover>();
            jobs_.run(
                [this, t = row->track, cover] {
                    const auto file = store_.ensure_artwork(t);
                    const auto bmp = file ? decode_image(*file, 512) : nullptr;
                    UINT w = 0, h = 0;
                    if (!bmp || FAILED(bmp->GetSize(&w, &h))) return;
                    cover->px.resize(size_t(w) * h * 4);
                    bmp->CopyPixels(nullptr, w * 4, UINT(cover->px.size()), cover->px.data());
                    cover->w = int(w), cover->h = int(h);
                },
                [this, song = in.song, cover] {
                    if (das_cover_for_ == song && !cover->px.empty()) das_cover_ = cover;
                });
        }
    }
    if (o.das_lyrics && cur && in.song != das_lyrics_for_) das_request_lyrics(*cur, row);
    return in;
}

// What one DasVis is given as the song goes: its cover once decoded, and (words) the title card as the song starts and
// the lyric line being sung. `shown` remembers what it has.
void View::das_dress(dasvis::DasVis& d, DasShown& shown, const dasvis::Inputs& in, bool words) {
    const player::Item* cur = player_.current();
    if (in.song != shown.song) {
        shown = {in.song};
        d.set_cover(nullptr, 0, 0);
        int letters = 0;  // the title card: drawn once by Direct2D into DasVis's texture, on the GPU
        const bool card = words && cur && !cur->title.empty() && g_.draw_title_card(d.title_target(768, 192), wide(cur->title), wide(cur->artist), &letters);
        d.show_title(card ? letters : 0, Gfx::kTitleShare);
    }
    if (!shown.cover && das_cover_ && !das_cover_for_.empty() && in.song.starts_with(das_cover_for_))  // the other screens' songs end "#2"…
        d.set_cover(das_cover_->px.data(), das_cover_->w, das_cover_->h), shown.cover = true;
    if (!words || !vis_.opt.das_lyrics || !das_lyrics_ || das_lyrics_for_ != in.song) return;
    // A new line: drawn the same way, in place of the card. Between lines the last one dissolves by itself.
    const int line = das_lyrics_->at(in.song_time);
    if (line < 0 || line == shown.lyric) return;
    shown.lyric = line;
    const auto& l = das_lyrics_->lines[size_t(line)];
    int letters = 0;
    if (g_.draw_title_card(d.title_target(1536, 192), wide(l.text), L"", &letters)) d.show_lyric(letters, l.start, l.end);
}

namespace {

std::string file_text(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return f ? std::string(std::istreambuf_iterator<char>(f), {}) : std::string();
}

}  // namespace

// The song's lyrics, off the UI thread: a .lrc next to it, else the cache, else LRCLIB (only with lyrics turned on:
// the title, artist, album and length are sent, nothing else). "None" is cached too, so a song is asked about once.
void View::das_request_lyrics(const player::Item& item, const std::optional<TrackRow>& row) {
    das_lyrics_for_ = song_key(item);
    das_lyrics_.reset();
    if (player::is_url(item.location) || item.location.empty()) return;  // radio: none
    std::string album;
    double secs = 0;
    if (row) {
        album = row->track.album.value_or("");
        secs = double(row->track.duration_ms.value_or(0)) / 1000;
    }
    const auto cache = paths::cache() / L"dasvis" / L"lyrics" / widen(std::format("{:016x}.lrc", std::hash<std::string>{}(das_lyrics_for_)));
    auto result = std::make_shared<dasvis::Lyrics>();
    jobs_.run(
        [file = std::filesystem::path(widen(item.location)), title = item.title, artist = item.artist, album, secs, cache, result] {
            std::error_code ec;
            std::string lrc = file_text(std::filesystem::path(file).replace_extension(L".lrc"));
            if (lrc.empty() && std::filesystem::exists(cache, ec)) lrc = file_text(cache);
            else if (lrc.empty() && !title.empty() && !artist.empty()) {
                std::string q = "track_name=" + http::url_encode(title) + "&artist_name=" + http::url_encode(artist);
                const bool exact = !album.empty() && secs > 0;  // /api/get matches the length too; /api/search otherwise
                if (exact) q += "&album_name=" + http::url_encode(album) + std::format("&duration={:.0f}", secs);
                const auto r = http::get(std::string("https://lrclib.net/api/") + (exact ? "get?" : "search?") + q,
                                         {{"User-Agent", "WreckBox " WB_VERSION " (DasVis lyrics)"}}, std::chrono::seconds(10));
                if (r.status == 200 || r.status == 404) {  // an answer (not a network failure): kept, even if it's "none"
                    lrc = r.status == 200 ? dasvis::lrclib_synced(r.body) : std::string();
                    std::filesystem::create_directories(cache.parent_path(), ec);
                    std::ofstream(cache, std::ios::binary) << lrc;
                }
            }
            *result = dasvis::parse_lrc(lrc);
        },
        [this, key = das_lyrics_for_, result] {
            if (das_lyrics_for_ == key && !result->lines.empty()) das_lyrics_ = result;
        });
}

bool View::das_screen() {
    const auto now = clock_t_::now();
    const bool playing = player_.playing();
    if (playing) audio_at_ = now;
    // Paused: the picture keeps moving for 4 s so it calms down, then holds (no frames, 0 % CPU), as MilkDrop does.
    const bool live = playing || now - audio_at_ < std::chrono::seconds(4);
    vis_settling_ = !playing && live;
    if (das_ && das_gen_ != g_.generation()) das_free();  // the device was rebuilt: its textures went with it
    auto& o = vis_.opt;
    const std::string gpu = g_.device_name();
    if (!das_) {
        das_gpu_ = gpu;
        das_ = std::make_unique<dasvis::DasVis>(g_.d3d(), das_config(false));
        das_gen_ = g_.generation();
        das_feat_ = {};
        das_tier_saved_ = false;
        das_frames_ = 0;
        wchar_t v[4]{};  // WRECKBOX_DASVIS_DEV=1 opens with the song map drawn over the picture (Ctrl+Shift+D)
        das_dev_ = das_dev_ || (GetEnvironmentVariableW(L"WRECKBOX_DASVIS_DEV", v, 4) && v[0] == L'1');
        perf_log(das_->ok() ? std::format("DasVis started on {}: {} scenes, {} tier{}", gpu, das_->scene_count(), dasvis::tier_name(das_->tier()),
                                          das_->user_errors().empty() ? "" : "; your scenes have errors")
                            : "DasVis unavailable: " + das_->error());
        if (das_->ok()) das_open_screens();
    }
    if (!das_->ok()) return false;

    const auto back = g_.back_buffer();
    if (!back) return false;
    g_.flush();  // anything Direct2D queued goes under the picture, not over it
    if (!live) {  // paused and settled: the same pictures, nothing moves on
        das_->redraw(back.Get());
        for (auto& s : screens_)
            if (s->ok()) s->vis().redraw(s->back().Get()), s->present();
        return true;
    }
    // The other screens share the frame's time (each its own scene).
    const dasvis::Inputs in = das_inputs(0.75f * 1000.f * float(frame_interval()) / das_hz_ / float(1 + screens_.size()));
    // The panel's line (only worked out while the panel or the dev view shows it).
    if (overlay_alpha() <= 0 && !das_dev_) {
    } else if (das_map_) {
        const auto st = das_map_->at(das_song_t_ + das_->map_offset());
        if (st.to_drop < 16 && (st.section == dasvis::Section::build || st.gap))
            das_status_ = st.to_drop < 1 ? L"Build-up · the drop is next" : std::format(L"Build-up · drop in {} bars", int(std::ceil(st.to_drop)));
        else das_status_ = wide(dasvis::section_name(st.section));
    } else das_status_ = das_map_busy_ ? L"Analysing the song…" : L"Live";
    das_dress(*das_, das_shown_, in, true);
    vj_apply();
    // The MilkDrop scene: projectM starts when the scene comes on (about a second, off the UI thread; the last frame
    // holds meanwhile), renders a frame for each DasVis frame while it's on screen, and is let go 20 s after it leaves
    // (~60 MB: only while it's wanted).
    if (o.das_milkdrop) {
        const bool want = das_->wants_milkdrop();
        if (want) das_milk_at_ = clock_t_::now();
        if (want && !milk_ && !milk_pending_.valid() && !milk_tried_) milk_start();  // once a visit if it fails
        milk_poll();
        if (want && milk_ && milk_->ok()) {
            if (!das_milk_shown_) milk_->random();  // a different preset each time it comes on
            // The two together too slow for 60 fps (frames 25 % late for ~1.5 s, so one preset's shader compile doesn't
            // count; MilkDrop alone nearly fills an Intel UHD's frame): a steady 30 for the rest of this showing (every
            // other frame of MilkDrop judders more than 30 does).
            das_milk_late_ = frame_dt_ > 1.25f * float(frame_interval()) / das_hz_ ? das_milk_late_ + 1 : 0;
            if (!das_milk_slow_ && das_milk_late_ > 90) das_milk_slow_ = true, perf_log("DasVis: MilkDrop scene at 30 fps (too slow together for 60)");
            // 540p: DasVis draws its scenes at about that size or less anyway.
            if (const auto f = milk_frame(g_.width(), g_.height(), player_.playing(), 540)) das_->set_milkdrop(f.bgra, f.width, f.height);
        }
        das_milk_shown_ = want && milk_;
        das_milk_slow_ = das_milk_slow_ && das_milk_shown_;
        if (!want && milk_ && clock_t_::now() - das_milk_at_ > std::chrono::seconds(20)) milk_restart(), perf_log("MilkDrop let go");
    }
    das_->render(in, back.Get());
    if (spout_) spout_->send(g_.d3d(), back.Get());  // before the player panel is drawn over it
    das_lights();
    for (size_t i = 0; i < screens_.size(); ++i) {
        DasWindow& s = *screens_[i];
        if (!s.ok()) continue;
        dasvis::Inputs other = in;
        other.song += std::format("#{}", i + 2);  // its own look and scenes for the same song (the same cover, the same drops)
        das_dress(s.vis(), s.shown, other, false);
        s.vis().render(other, s.back().Get());
        s.present();
    }
    // Remember the measured tier for this graphics card, so the next visit starts there.
    if (!das_tier_saved_ && o.das_quality == 0 && das_->tier_settled()) {
        das_tier_saved_ = true;
        std::string t = dasvis::tier_name(das_->tier());
        std::transform(t.begin(), t.end(), t.begin(), [](char c) { return char(std::tolower(c)); });
        if (o.das_tiers.value(gpu, std::string()) != t) o.das_tiers[gpu] = t, save_vis();
    }
    if (++das_frames_ % 300 == 0)
        perf_log(std::format("DasVis: {}, {} tier at {:.0f} %, GPU {:.2f} ms a frame (budget {:.1f}), every {} refresh(es) of {:.0f} Hz ({:.0f} fps), flash budget {:.2f}{}",
                             das_->scene_name(), dasvis::tier_name(das_->tier()), das_->scale() * 100, das_->gpu_ms(), in.budget_ms,
                             frame_interval(), das_hz_, 1 / frame_dt_, das_->flash_budget(), screens_.empty() ? "" : std::format(", {} other screen(s)", screens_.size())));
    return true;
}

// The DasVis settings for this graphics card: the measured (or chosen) tier, what each scene cost here before. The
// wallpaper's: Low, always (it runs while you work).
dasvis::Config View::das_config(bool wallpaper) const {
    const auto& o = vis_.opt;
    const std::string gpu = g_.device_name();
    dasvis::Config cfg;
    const std::string known = o.das_tiers.value(gpu, std::string());
    cfg.start = o.das_quality ? dasvis::Tier(o.das_quality - 1) : known == "low" ? dasvis::Tier::low : known == "high" ? dasvis::Tier::high : dasvis::Tier::medium;
    cfg.auto_tier = o.das_quality == 0;
    cfg.max_tier = o.das_battery && on_battery() ? dasvis::Tier::medium : dasvis::Tier::high;
    if (wallpaper) cfg.start = cfg.max_tier = dasvis::Tier::low, cfg.auto_tier = false;
    cfg.reduce_flash = o.das_reduce_flash;
    cfg.auto_advance = o.auto_advance;
    cfg.milkdrop = o.das_milkdrop && !wallpaper;
    cfg.user_dir = das_scenes_dir();
    if (const auto c = o.das_costs.find(gpu); c != o.das_costs.end() && c->is_object())
        for (const auto& [k, ms] : c->items())
            if (ms.is_number()) cfg.costs[k] = ms.get<float>();
    return cfg;
}

// MARK: Other screens and the wallpaper

void View::das_open_screens() {
    screens_.clear();
    if (vis_.opt.das_screens != 2 || GetSystemMetrics(SM_CMONITORS) < 2) return;
    struct Find {
        HMONITOR mine;
        std::vector<RECT> others;
    } find{MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), {}};
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT r, LPARAM lp) -> BOOL {
            auto* f = reinterpret_cast<Find*>(lp);
            if (m != f->mine) f->others.push_back(*r);
            return TRUE;
        },
        LPARAM(&find));
    for (const RECT& r : find.others) {
        auto s = std::make_unique<DasWindow>(g_.d3d(), nullptr, r, das_config(false));
        if (s->ok()) screens_.push_back(std::move(s));
    }
    perf_log(std::format("DasVis: {} other screen(s) of {}", screens_.size(), find.others.size()));
}

void View::wall_restart() {
    wall_.reset();
    if (vis_.opt.das_wallpaper && !fullscreen_) SetTimer(hwnd_, kWallTimer, 100, nullptr);
}

void View::wall_tick() {
    if (!vis_.opt.das_wallpaper || fullscreen_) return wall_.reset();  // off, or full screen has the visualizer (it starts again after)
    const auto now = clock_t_::now();
    const bool playing = player_.playing();
    if (playing) audio_at_ = now;
    // Paused: on battery, or with a full-screen app or game (or a maximised window) in front. The picture stays, no frames.
    const bool paused = on_battery() || desktop_hidden();
    if (paused != wall_paused_) wall_paused_ = paused, perf_log(paused ? "wallpaper: paused (on battery, or the desktop is covered)" : "wallpaper: running");
    const bool live = playing || now - audio_at_ < std::chrono::seconds(4);
    if (wall_ && (wall_->generation != g_.generation() || !wall_->ok())) wall_.reset();  // the device was rebuilt, or the desktop restarted
    if (wall_) {  // the desktop changed size (a screen added, the resolution)
        RECT r{};
        if (GetClientRect(GetParent(wall_->hwnd()), &r) && (r.right != wall_->size().cx || r.bottom != wall_->size().cy)) wall_.reset();
    }
    if (!wall_ && g_.d3d() && !paused) {
        const HWND worker = desktop_worker();
        if (!worker) return perf_log("wallpaper: no desktop window to draw into"), void(SetTimer(hwnd_, kWallTimer, 5000, nullptr));
        wall_ = std::make_unique<DasWindow>(g_.d3d(), worker, RECT{}, das_config(true));
        wall_->generation = g_.generation();
        wall_frames_ = 0, wall_at_ = {};
        bt_output_ = player::default_output_is_bluetooth();
        perf_log(wall_->ok() ? std::format("wallpaper: {} x {}, {} tier", wall_->size().cx, wall_->size().cy, dasvis::tier_name(wall_->vis().tier()))
                             : "wallpaper: couldn't start");
        if (!wall_->ok()) return wall_.reset(), void(SetTimer(hwnd_, kWallTimer, 5000, nullptr));
    }
    if (!wall_ || paused || !live) return void(SetTimer(hwnd_, kWallTimer, paused ? 1000 : 250, nullptr));
    // 30 frames a second at most: the timer comes back in ~31 ms (the clock's 15.6 ms steps), or 33 with a finer clock.
    SetTimer(hwnd_, kWallTimer, 30, nullptr);
    const float dt = wall_at_ == clock_t_::time_point{} ? 1 / 30.f : std::chrono::duration<float>(now - wall_at_).count();
    wall_at_ = now;
    frame_dt_ = std::clamp(dt, 1 / 60.f, 0.25f);
    const auto back = wall_->back();
    if (!back) return;
    const dasvis::Inputs in = das_inputs(4);  // a few ms of the GPU a frame: it runs while you work
    das_dress(wall_->vis(), wall_->shown, in, true);
    wall_->vis().render(in, back.Get());
    wall_->present();
    if (++wall_frames_ % 300 == 0)
        perf_log(std::format("wallpaper: {}, {:.0f} %, GPU {:.2f} ms a frame, {:.1f} fps", wall_->vis().scene_name(), wall_->vis().scale() * 100,
                             wall_->vis().gpu_ms(), 1 / frame_dt_));
}

// MARK: Video export

namespace {

std::optional<std::filesystem::path> save_video_dialog(HWND owner, const std::wstring& name) {
    Microsoft::WRL::ComPtr<IFileSaveDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return std::nullopt;
    const COMDLG_FILTERSPEC types[] = {{L"MP4 video", L"*.mp4"}};
    dlg->SetFileTypes(1, types);
    dlg->SetDefaultExtension(L"mp4");
    dlg->SetFileName(name.c_str());
    dlg->SetTitle(L"Export DasVis video");
    Microsoft::WRL::ComPtr<IShellItem> videos;
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_Videos, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&videos)))) dlg->SetFolder(videos.Get());
    Microsoft::WRL::ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dlg->Show(owner)) || FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return std::nullopt;
    std::filesystem::path out(path);
    CoTaskMemFree(path);
    return out;
}

}  // namespace

void View::export_video(const std::string& id, int preset) {
    if (video_) return show_toast(L"A video is already being made");
    const auto tr = store_.row(id);
    if (!tr || !tr->state || !tr->state->local_path) return;
    static constexpr int kSizes[][3] = {{1280, 720, 30}, {1920, 1080, 60}, {2560, 1440, 60}, {3840, 2160, 60}};
    std::string artists;
    for (const auto& a : tr->track.artists) artists += (artists.empty() ? "" : ", ") + a;
    std::wstring name = widen(artists.empty() ? tr->track.title : artists + " - " + tr->track.title) + L" (DasVis).mp4";
    for (wchar_t& c : name)
        if (wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    const auto out = save_video_dialog(hwnd_, name);
    if (!out) return;

    dasvis::VideoOptions o;
    o.width = kSizes[preset][0], o.height = kSizes[preset][1], o.fps = kSizes[preset][2];
    const player::VisOptions vo = player::VisOptions::from_json(Settings::current().extra.value("visualizer", json::object()));
    o.intensity = vo.das_intensity;
    o.bpm = tr->bpm().value_or(0);
    if (!tr->camelot().empty()) o.camelot = tr->camelot();
    o.song_id = id;  // the same seed as in the app: the same look
    std::string genre = tr->genre;
    std::transform(genre.begin(), genre.end(), genre.begin(), [](char c) { return char(std::tolower(static_cast<unsigned char>(c))); });
    if (const auto g = vo.das_taste.find(genre.empty() ? "any" : genre); g != vo.das_taste.end() && g->is_object())
        for (const auto& [scene, w] : g->items())
            if (w.is_number()) o.taste[scene] = w.get<float>();
    // The title card: drawn once here (it needs the app's fonts) and handed over as pixels.
    {
        D3D11_TEXTURE2D_DESC td{768, 192, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE};
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex, staging;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
        int letters = 0;
        if (g_.d3d() && SUCCEEDED(g_.d3d()->CreateTexture2D(&td, nullptr, &tex)) && !tr->track.title.empty() &&
            g_.draw_title_card(tex.Get(), widen(tr->track.title), widen(artists), &letters)) {
            td.Usage = D3D11_USAGE_STAGING, td.BindFlags = 0, td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            g_.d3d()->GetImmediateContext(&ctx);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(g_.d3d()->CreateTexture2D(&td, nullptr, &staging)) && (ctx->CopyResource(staging.Get(), tex.Get()), true) &&
                SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                o.title.resize(768 * 192 * 4);
                for (UINT y = 0; y < 192; ++y) std::memcpy(&o.title[size_t(y) * 768 * 4], static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch, 768 * 4);
                ctx->Unmap(staging.Get(), 0);
                o.title_w = 768, o.title_h = 192, o.title_letters = letters, o.title_share = Gfx::kTitleShare;
            }
        }
    }
    video_ = std::make_unique<VideoJob>();
    video_->out = *out;
    show_toast(L"Making the video… you can keep using WreckBox");
    video_->thread = std::thread([this, job = video_.get(), o, file = std::filesystem::path(widen(*tr->state->local_path)), track = tr->track, id,
                                  hwnd = hwnd_]() mutable {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);  // the app stays smooth while it works
        if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {  // the cover (WIC)
            if (const auto art = store_.ensure_artwork(track))
                if (const auto bmp = decode_image(*art, 512); bmp && SUCCEEDED(bmp->GetSize(reinterpret_cast<UINT*>(&o.cover_w), reinterpret_cast<UINT*>(&o.cover_h)))) {
                    o.cover.resize(size_t(o.cover_w) * size_t(o.cover_h) * 4);
                    if (FAILED(bmp->CopyPixels(nullptr, UINT(o.cover_w) * 4, UINT(o.cover.size()), o.cover.data()))) o.cover.clear();
                }
            CoUninitialize();
        }
        const auto map = map_for(id, file, o.bpm);
        job->error = dasvis::export_video(file, job->out, o, map.get(), [job, hwnd](double p) {
            job->progress = p;
            InvalidateRect(hwnd, nullptr, FALSE);
            return !job->cancel.load();
        });
        job->finished = true;
        InvalidateRect(hwnd, nullptr, FALSE);
    });
}

float View::video_status(const Rect& r) {
    if (!video_) return 0;
    if (video_->finished) {
        const std::string err = video_->error;
        const auto out = video_->out;
        video_.reset();  // joins: the thread has finished
        if (err.empty()) {
            show_toast(L"Video saved: " + out.filename().wstring());
            show_in_folder(narrow(out.wstring()));
        } else if (err != "cancelled") {
            show_toast(L"The video couldn't be made: " + widen(err));
        }
        return 0;
    }
    const float p = float(video_->progress.load());
    g_.fill_round(r.inset(0, 4), 10, theme::hover);
    g_.fill_round(Rect{r.l + 10, r.b - 10, r.l + 10 + (r.w() - 20) * p, r.b - 7}, 2, lilac);
    g_.icon(icon::video, r.l + 18, r.t + 18, 14, lilac);
    g_.text(std::format(L"Video {:.0f} %", p * 100), Rect{r.l + 36, r.t + 6, r.r - 70, r.t + 30}, {Font::ui, 13, 500, text});
    const Rect cancel{r.r - 68, r.t + 6, r.r - 6, r.t + 30};
    g_.text(L"Cancel", cancel, {Font::ui, 12.5f, 600, ui_.hover(cancel) ? text : text3, Align::center});
    ui_.click(cancel, [this] {
        if (video_) video_->cancel = true;
    });
    return r.h();
}

void View::das_lights() {
    const auto& o = vis_.opt;
    if (!o.das_lights || o.das_wled.empty()) return;
    if (das_wled_for_ != o.das_wled && !wled_busy_) {  // a new address: look it up (a name can take seconds)
        das_wled_for_ = o.das_wled;
        wled_to_.reset();
        wled_busy_ = true;
        auto found = std::make_shared<std::optional<sockaddr_in>>();
        jobs_.run([host = o.das_wled, found] { *found = lights::resolve(host); },
                  [this, host = o.das_wled, found] {
                      wled_busy_ = false;
                      if (das_wled_for_ == host) wled_to_ = *found;
                      if (!*found) show_toast(L"Room lights: couldn't find " + widen(host));
                  });
    }
    // At most 40 frames a second: plenty for lights, and easy on a small WLED controller's Wi-Fi.
    const auto now = clock_t_::now();
    if (!wled_to_ || now - wled_at_ < std::chrono::milliseconds(25)) return;
    wled_at_ = now;
    if (!wled_) wled_ = std::make_unique<lights::Wled>();
    wled_->send(*wled_to_, dasvis::led_frame(das_->lights(), o.das_wled_leds));
}

void View::vj_apply() {
    std::vector<json> todo;
    {
        std::lock_guard lock(vj_->m);
        todo.swap(vj_->commands);
    }
    for (const json& c : todo) {
        const std::string cmd = c.value("cmd", "");
        if (cmd == "next") das_->next();
        else if (cmd == "prev") das_->previous();
        else if (cmd == "lock") das_->lock(!das_->locked());
        else if (cmd == "like" || cmd == "skip") das_rate(cmd == "like");
        else if (cmd == "scene") das_->select(size_t(std::max(0, c.value("value", 0))));
        else if (cmd == "drop") das_->drop_now();
        else if (cmd == "shock") das_->shockwave();
        else if (cmd == "camera") das_->set_camera(c.value("x", 0.f), c.value("y", 0.f));
        else if (cmd == "intensity") vis_.opt.das_intensity = std::clamp(c.value("value", 1), 0, 2), save_vis();
        if (cmd != "camera") audio_at_ = clock_t_::now();  // seen even while paused
        vj_card_ = false;  // a phone is on the deck: the code has done its job
    }
    json st = {{"open", true}, {"scene", das_->scene_name()}, {"scenes", das_->scene_names()}, {"locked", das_->locked()},
               {"intensity", vis_.opt.das_intensity}};
    if (das_map_) {
        const auto s = das_map_->at(das_song_t_ + das_->map_offset());
        st["status"] = s.to_drop < 16 && (s.section == dasvis::Section::build || s.gap) ? std::format("Build-up · drop in {} bars", int(std::ceil(s.to_drop)))
                                                                                         : std::string(dasvis::section_name(s.section));
    }
    std::lock_guard lock(vj_->m);
    vj_->state = st.dump();
}

// The deck's QR code, top right over the picture: the phone opens the page (same Wi-Fi). Gone when a phone uses it.
void View::vj_card(float W, float H) {
    const std::string url = sync_ ? sync_->vj_url() : "";
    const Rect card = Rect::xywh(W - 316, 70, 280, 340);
    g_.fill_round(card, 16, argb(0xE0141219));
    g_.text(L"Phone VJ deck", Rect{card.l + 20, card.t + 14, card.r - 20, card.t + 40}, {Font::ui, 15, 600, text});
    if (url.empty()) {
        g_.paragraph(L"This PC isn't on a local network the phone can reach.", card.inset(20, 60, 20, 20), {Font::ui, 13, 400, text2});
        return;
    }
    const Rect box = Rect::xywh(card.l + 30, card.t + 48, 220, 220);
    const qrcodegen::QrCode code = qrcodegen::QrCode::encodeText(url.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
    g_.cached(L"qr:" + widen(url), box, [&](Gfx& g) {
        g.fill(box, white);
        const float cell = (box.r - box.l - 20) / float(code.getSize());
        for (int cy = 0; cy < code.getSize(); ++cy)
            for (int cx = 0; cx < code.getSize(); ++cx)
                if (code.getModule(cx, cy)) g.fill(Rect::xywh(box.l + 10 + float(cx) * cell, box.t + 10 + float(cy) * cell, cell + 0.5f, cell + 0.5f), black);
    });
    g_.paragraph(L"Scan with your phone's camera, on the same Wi-Fi. Anyone with this code can control the picture.",
                 Rect{card.l + 20, card.t + 278, card.r - 20, card.b - 8}, {Font::ui, 12, 400, text2});
    (void)H;
}

// F / X: your taste for this genre. A like makes the scene come up more (up to 4×), a skip less (down to 0.15×) and
// changes it now. Kept in the settings, per genre.
void View::das_rate(bool like) {
    if (!das_ || !das_->ok()) return;
    const std::string scene = das_->scene_name();
    float& w = das_taste_.try_emplace(scene, 1.f).first->second;
    w = like ? std::min(4.f, w * 1.5f) : std::max(0.15f, w * 0.6f);
    vis_.opt.das_taste[das_genre_.empty() ? "any" : das_genre_][scene] = std::round(double(w) * 100) / 100;
    save_vis();
    const std::wstring genre = das_genre_.empty() ? L"" : L" in " + wide(das_genre_);
    show_toast(like ? std::format(L"More {}{}", wide(scene), genre) : std::format(L"Less {}{}", wide(scene), genre));
    if (!like) das_->skip();
    audio_at_ = clock_t_::now();
}

// Ctrl+Shift+D: the song map over the picture. The whole song (its parts, the drops, where it is now), then the 16 s
// around now (beats, bars, kicks), and the numbers.
void View::das_dev_view(float W) {
    const float l = 48, r = W - 48, top = 64;
    g_.fill_round(Rect{l - 12, top - 12, r + 12, top + 150}, 14, argb(0xC0101014));
    const auto& m = das_map_;
    if (!m) {
        g_.text(das_map_busy_ ? L"Analysing the song…" : L"No song map (radio, or a file that couldn't be read)", Rect{l, top, r, top + 24},
                {Font::ui, 14, 600, text2});
        return;
    }
    const double t = das_song_t_ + das_->map_offset();
    const auto colour = [](dasvis::Section s) {
        switch (s) {
            case dasvis::Section::build: return peach;
            case dasvis::Section::drop: return argb(0xFFF07070);
            case dasvis::Section::breakdown: return light_blue;
            case dasvis::Section::chorus: return lilac;
            default: return argb(0xFF505060);
        }
    };
    // The whole song.
    const float sy = top, sh = 26;
    const auto x_of = [&](double s) { return l + float(s / std::max(1.0, m->duration)) * (r - l); };
    for (const auto& p : m->parts) {
        const Rect pr{x_of(p.start), sy, std::max(x_of(p.start) + 1, x_of(p.end) - 1), sy + sh};
        g_.fill(pr, with_alpha(colour(p.kind), 0.35f + 0.5f * p.energy));
        g_.text(wide(dasvis::section_name(p.kind)), pr.inset(4, 0), {Font::ui, 11, 600, text});
    }
    for (const double d : m->drops) g_.fill(Rect{x_of(d) - 1, sy - 4, x_of(d) + 1, sy + sh + 4}, white);
    g_.fill(Rect{x_of(t) - 1.5f, sy - 6, x_of(t) + 1.5f, sy + sh + 6}, lilac);
    // 16 s around now.
    const float zy = sy + sh + 16, zh = 44, mid = (l + r) / 2;
    const auto z_of = [&](double s) { return mid + float((s - t) / 8) * (r - l) / 2; };
    for (size_t i = 0; i < m->beats.size(); ++i) {
        const float x = z_of(m->beats[i]);
        if (x < l || x > r) continue;
        const long rel = long(i) - m->downbeat;
        const bool down = rel % 4 == 0, phrase = rel % 64 == 0;
        g_.fill(Rect{x - (phrase ? 1.5f : 0.75f), zy + (down ? 0 : 18), x + (phrase ? 1.5f : 0.75f), zy + zh}, phrase ? peach : down ? white : text3);
    }
    for (const double k : m->hits[dasvis::SongMap::kick]) {
        const float x = z_of(k);
        if (x >= l && x <= r) g_.fill_circle(x, zy + zh + 6, 2.5f, argb(0xFFF07070));
    }
    g_.fill(Rect{mid - 1, zy - 4, mid + 1, zy + zh + 12}, lilac);
    const auto st = m->at(t);
    g_.text(std::format(L"{:.1f} BPM · bar {} · {} ({:.0f} %) · build {:.2f} · drop {:.2f} · next drop in {} · offset {:+.0f} ms", m->bpm, st.bar + 1,
                        wide(dasvis::section_name(st.section)), st.section_t * 100, st.build, st.drop,
                        st.to_drop < 99 ? std::format(L"{:.1f} bars", st.to_drop) : std::wstring(L"—"), das_->map_offset() * 1000),
            Rect{l, zy + zh + 16, r, zy + zh + 38}, {Font::ui, 12.5f, 500, text2});
}

void View::vis_click() {
    mode_before_click_ = vis_.opt.mode;
    if (das_ && das_->ok() && vis_.opt.mode == VisOptions::Mode::dasvis) {
        das_->next();
        audio_at_ = clock_t_::now();
        return;
    }
    if (milk_ && milk_->ok() && vis_.opt.mode == VisOptions::Mode::milkdrop) {
        milk_->next();
        audio_at_ = clock_t_::now();
        return;
    }
    // Winamp: a click steps through spectrum → scope → both → off (from the fallback bars, to the scope)
    vis_.opt.mode = VisOptions::Mode((int(vis_.opt.mode) + 1) % 4);
    save_vis();
}

void View::visualizer_screen() {
    const auto t0 = clock_t_::now();
    if (frame_at_ != clock_t_::time_point{})
        frame_dt_ = frame_dt_ * 0.9f + 0.1f * std::clamp(std::chrono::duration<float>(t0 - frame_at_).count(), 0.004f, 0.1f);
    frame_at_ = t0;
    const float W = g_.width(), H = g_.height();
    const bool want_das = vis_.opt.mode == VisOptions::Mode::dasvis;
    if (!want_das) das_free();  // switched away: the GPU and memory go back
    if (want_das && das_screen()) {
        if (das_dev_) das_dev_view(W);
        if (vj_card_) vj_card(W, H);
        ui_.click(Rect{0, 0, W, H}, [this] { vis_click(); }, [this] { vis_menu(); });
        if (const float a = overlay_alpha(); a > 0) vis_overlay(Rect{0, 0, W, H}, a);
        return;
    }
    const bool want_milk = vis_.opt.mode == VisOptions::Mode::milkdrop;
    if (want_milk && !milk_ && !milk_tried_) milk_start();
    milk_poll();
    if (!want_milk && milk_) {  // switched to the bars: free the GPU; switching back builds it again
        milk_.reset();
        g_.clear_stream();
        milk_w_ = milk_h_ = 0;
        milk_tried_ = false;
    }
    if (want_milk && milk_pending_.valid()) {  // still starting: a black frame, polled again on the next tick
        g_.fill(Rect{0, 0, W, H}, black);
        vis_settling_ = true;
        ui_.click(Rect{0, 0, W, H}, [] {}, [this] { vis_menu(); });
        if (const float a = overlay_alpha(); a > 0) vis_overlay(Rect{0, 0, W, H}, a);
        return;
    }
    if (want_milk && milk_ && milk_->ok()) {
        milk_screen(W, H);
        ui_.click(Rect{0, 0, W, H}, [this] { vis_click(); }, [this] { vis_menu(); });
        if (const float a = overlay_alpha(); a > 0) vis_overlay(Rect{0, 0, W, H}, a);
        // Too slow for this PC (frames average over 14 ms for ~1.5 s, so one preset switch's shader compile doesn't
        // count): one quality step down, not saved.
        const float ms = std::chrono::duration<float, std::milli>(clock_t_::now() - t0).count();
        frame_ms_ = frame_ms_ * 0.9f + ms * 0.1f;
        slow_frames_ = frame_ms_ > 14 && !vis_settling_ ? slow_frames_ + 1 : 0;
        if (slow_frames_ > 90 && milk_quality_ > kQualities[0]) {
            milk_quality_ = milk_quality_ > kQualities[1] ? kQualities[1] : kQualities[0];
            perf_log(std::format("MilkDrop: frames average {:.1f} ms, dropping to {}p", frame_ms_, milk_quality_));
            slow_frames_ = 0;
        }
        return;
    }
    const float dt = vis_at_ == clock_t_::time_point{} ? 0.016f : std::chrono::duration<float>(t0 - vis_at_).count();
    vis_at_ = t0;
    // What's audible right now, or silence while paused so the bars fall away.
    player::AudioOutput* out = player_.output();
    if (out && player_.playing()) out->tap_until(heard_end(*out, 1), tap_.data(), tap_.size());  // shown next refresh
    else std::fill(tap_.begin(), tap_.end(), 0.f);
    vis_.set_band_count(size_t(W / 14));
    vis_.update(tap_.data(), dt);
    vis_settling_ = !player_.playing() && (*std::max_element(vis_.peaks().begin(), vis_.peaks().end()) > 0);

    const bool classic = vis_.opt.classic;
    g_.fill(Rect{0, 0, W, H}, classic ? black : bg);
    if (!classic) g_.fill_radial(Rect{0, 0, W, H}, {W * 0.5f, H * 1.1f}, std::max(W, H), argb(0x30BB96DA), argb(0x00BB96DA));
    const Rect area{48, 56, W - 48, H - 56};
    using Mode = VisOptions::Mode;
    const Mode mode = want_milk || want_das ? Mode::spectrum : vis_.opt.mode;  // no MilkDrop / DasVis here: the Winamp bars
    if (mode == Mode::spectrum || mode == Mode::both) draw_spectrum(area);
    if (mode == Mode::scope || mode == Mode::both) draw_scope(area, mode == Mode::both);
    if (mode == Mode::off)
        g_.text(L"Visualizer off — click to turn it back on", Rect{0, 0, W, H}, {Font::ui, 15, 400, text3, Align::center});
    ui_.click(Rect{0, 0, W, H}, [this] { vis_click(); }, [this] { vis_menu(); });
    if (const float a = overlay_alpha(); a > 0) vis_overlay(Rect{0, 0, W, H}, a);

    // Weak PCs: if frames take long, tick at 30 fps instead of 60.
    const float ms = std::chrono::duration<float, std::milli>(clock_t_::now() - t0).count();
    frame_ms_ = frame_ms_ * 0.95f + ms * 0.05f;
}

void View::draw_spectrum(const Rect& r) {
    const auto& bars = vis_.bars();
    const auto& peaks = vis_.peaks();
    const size_t n = bars.size();
    const float slot = r.w() / float(n), w = std::max(1.f, slot * 0.72f);
    const bool classic = vis_.opt.classic;
    const auto lo = classic ? kWinampLow : light_blue, mid = classic ? kWinampMid : peach, hi = classic ? kWinampHigh : lilac;
    using Bars = VisOptions::Bars;
    for (size_t i = 0; i < n; ++i) {
        const float x = r.l + float(i) * slot + (slot - w) / 2, h = bars[i] * r.h();
        if (h >= 1) {
            const Rect bar{x, r.b - h, x + w, r.b};
            switch (vis_.opt.bars) {
                case Bars::normal: g_.fill_span(bar, {0, r.b}, {0, r.t}, {lo, mid, hi}); break;     // colour by height on screen
                case Bars::fire: g_.fill_span(bar, {0, r.b}, {0, r.b - h}, {lo, mid, hi}); break;   // every bar's tip is hottest
                case Bars::line: g_.fill(bar, ramp(lo, mid, hi, bars[i])); break;                     // one colour per bar
            }
        }
        if (vis_.opt.peaks && peaks[i] > 0) {
            const float y = r.b - peaks[i] * r.h();
            g_.fill(Rect{x, y - 3, x + w, y}, classic ? kWinampPeak : white);
        }
    }
}

void View::draw_scope(const Rect& r, bool over_bars) {
    const auto& wave = vis_.wave();
    const size_t points = std::min(wave.size(), size_t(r.w() / 2));  // a point every 2 DIPs
    if (points < 2) return;
    const float cy = (r.t + r.b) / 2, amp = r.h() / 2 * 0.95f;
    const bool classic = vis_.opt.classic;
    const float alpha = over_bars ? 0.85f : 1.f;
    auto colour = [&](float v) {
        const float t = std::min(1.f, std::fabs(v) * 2.5f);
        return faded(classic ? mix(kWinampScopeLow, white, t) : mix(light_blue, lilac, t), alpha);
    };
    float px = 0, py = 0;
    using Scope = VisOptions::Scope;
    for (size_t i = 0; i < points; ++i) {
        const float v = std::clamp(wave[i * wave.size() / points], -1.f, 1.f);
        const float x = r.l + r.w() * float(i) / float(points - 1), y = cy - v * amp;
        switch (vis_.opt.scope) {
            case Scope::dots: g_.fill(Rect{x - 1, y - 1, x + 1.5f, y + 1.5f}, colour(v)); break;
            case Scope::lines:
                if (i) g_.line(px, py, x, y, colour(v), 2);
                break;
            case Scope::solid: g_.line(x, cy, x, y, colour(v), 2); break;
        }
        px = x, py = y;
    }
}

// The player bar over the picture: hint line and exit at the top; along the bottom, edge to edge, one glass bar with the
// song, the transport, seek and volume, the preset controls and, on the right, the "Up next" list. It fades as one layer.
// Its hide button (or H) leaves only a "Show player" button.
void View::vis_overlay(const Rect& r, float a) {
    const bool milk = milk_ && milk_->ok() && vis_.opt.mode == VisOptions::Mode::milkdrop;
    const bool das = das_ && das_->ok() && vis_.opt.mode == VisOptions::Mode::dasvis;
    if (a < 1) g_.push_opacity(a);
    // The keys: a glass bar centred at the top, each key in a keycap and then what it does. Most useful first; when the
    // screen is too narrow, the ones before the last two (options, exit) drop out rather than the line being cut.
    using Hint = std::pair<const wchar_t*, const wchar_t*>;
    std::vector<Hint> hints = das    ? std::vector<Hint>{{L"N", L"Next scene"}, {L"P", L"Previous"}, {L"L", L"Lock"}, {L"F", L"Like"}, {L"X", L"Skip"},
                                                         {L"M", L"MilkDrop"}, {L"Space", L"Pause"}, {L"H", L"Hide player"}, {L"Right-click", L"Options"}, {L"Esc", L"Exit"}}
                              : milk ? std::vector<Hint>{{L"N", L"Next preset"}, {L"P", L"Previous"}, {L"R", L"Random"}, {L"L", L"Lock"}, {L"D", L"DasVis"},
                                                         {L"Space", L"Pause"}, {L"H", L"Hide player"}, {L"Right-click", L"Options"}, {L"Esc", L"Exit"}}
                                     : std::vector<Hint>{{L"Click", L"Next style"}, {L"V", L"Look"}, {L"D", L"DasVis"}, {L"M", L"MilkDrop"}, {L"Space", L"Pause"},
                                                         {L"H", L"Hide player"}, {L"Right-click", L"Options"}, {L"Esc", L"Exit"}};
    const TextStyle key_st{Font::ui, 11.5f, 700, text}, label_st{Font::ui, 12.5f, 500, text2};
    const float pad = 16, gap = 18, cap_pad = 7, max_w = r.w() - 2 * 96;  // clear of the exit button
    const auto item_w = [&](const Hint& h) { return g_.measure(h.first, key_st) + 2 * cap_pad + 7 + g_.measure(h.second, label_st); };
    const auto bar_w = [&] {
        float w = 2 * pad - gap;
        for (const Hint& h : hints) w += item_w(h) + gap;
        return w;
    };
    while (hints.size() > 2 && bar_w() > max_w) hints.erase(hints.end() - 3);
    const Rect bar = Rect::xywh(r.l + (r.w() - bar_w()) / 2, r.t + 15, bar_w(), 34);
    g_.fill_round(bar, 17, argb(0xC0141418));
    g_.stroke_round(bar, 17, glass_border);
    float hx = bar.l + pad;
    for (const Hint& h : hints) {
        const float kw = g_.measure(h.first, key_st) + 2 * cap_pad;
        const Rect cap = Rect::xywh(hx, bar.t + 7, kw, 20);
        g_.fill_round(cap, 5, argb(0x24FFFFFF));
        g_.stroke_round(cap, 5, argb(0x1AFFFFFF));
        g_.text(h.first, Rect{cap.l + cap_pad, cap.t, cap.r, cap.b}, key_st);
        g_.text(h.second, Rect{cap.r + 7, bar.t, cap.r + 7 + g_.measure(h.second, label_st) + 2, bar.b}, label_st);
        hx += item_w(h) + gap;
    }
    if (das && !das_->user_errors().empty())  // your scenes: the first error of each that didn't compile
        g_.paragraph(L"Your scenes: " + wide(das_->user_errors()), Rect{r.l + 48, r.t + 60, r.r - 100, r.t + 210}, {Font::ui, 12.5f, 500, peach});
    ui_.icon_button(Rect::xywh(r.r - 64, r.t + 14, 36, 36), icon::exit_full, 14, [this] { set_fullscreen(false); }, false, false,
                    L"Exit full screen (Esc)");

    const player::Item* cur = player_.current();
    if (cur && !vis_.opt.panel) {
        const std::wstring label = L"Show player";
        const float w = ui_.pill_width(label, icon::chevron_up);
        ui_.pill((r.l + r.r - w) / 2, r.b - 16 - 36, label, icon::chevron_up, Ui::Pill::glass, [this] { set_vis_panel(true); });
    } else if (cur) {
        const auto& items = player_.queue().items();
        const size_t first_next = size_t(player_.queue().index() + 1), upcoming = first_next < items.size() ? items.size() - first_next : 0;
        const bool up = upcoming > 0 && r.w() >= 1100;
        const float uw = 300, ph = 236;
        const Rect panel{r.l + 16, r.b - 16 - ph, r.r - 16, r.b - 16};
        ui_.glass(panel, 24, false, true);
        ui_.click(panel, [] {});  // clicks on the panel stay on it
        const Rect box = panel.inset(20);
        const Rect in{box.l + 4, box.t, up ? box.r - uw : box.r - 4, box.b};  // the player; Up next takes the rest
        ui_.icon_button(Rect::xywh(in.r - 32, in.t, 32, 32), icon::chevron_down, 13, [this] { set_vis_panel(false); }, false, false,
                        L"Hide the player (H)");

        // Row 1: cover, title, artist, BPM and key.
        const auto row = cur->track_id ? store_.row(*cur->track_id) : std::nullopt;
        const Rect cover = Rect::xywh(in.l, in.t, 96, 96);
        if (row) ui_.artwork(row->track, cover, 16);
        else {
            g_.fill_gradient(cover, 16, {with_alpha(lilac, 0.45f), with_alpha(light_blue, 0.2f)});
            g_.icon(player::is_url(cur->location) ? icon::globe : icon::music, cover.l + 48, cover.t + 48, 30, with_alpha(white, 0.5f));
        }
        const float tx = cover.r + 18;
        const auto d = player_.display();
        g_.text(wide(d.title), Rect{tx, in.t, in.r - 44, in.t + 34}, {Font::ui, 26, 600, text});
        g_.text(wide(d.subtitle), Rect{tx, in.t + 34, in.r - 44, in.t + 58}, {Font::ui, 15, 400, text2});
        if (row) {
            float x = tx;
            const float cy = in.t + 80;
            ui_.bpm_readout(x, cy, row->bpm(), false, 17);
            x += 52;
            ui_.key_badge(x, cy, row->camelot());
        }

        // Row 2: transport, seek, volume — the same pieces as the player bar.
        const float cy2 = in.t + 96 + 12 + 20;
        transport(Rect::xywh(in.l, cy2 - 20, 124, 40));
        volume(Rect{in.r - 120, cy2 - 16, in.r, cy2 + 16});
        seek_bar(Rect{in.l + 124 + 16, cy2 - 10, in.r - 120 - 16, cy2 + 10});

        // Row 3: the preset or scene (◀ name ▶, random, lock), the DasVis / MilkDrop / Bars switch, the equalizer.
        const float cy3 = cy2 + 20 + 12 + 16;
        float x = in.r;
        eq_x_ = x - 16;
        bar_top_ = panel.t;
        ui_.icon_button(Rect::xywh(x - 32, cy3 - 16, 32, 32), icon::equalizer, 14, [this] { eq_open_ = !eq_open_; },
                        eq_open_ || player_.equalizer().enabled || player_.normalize(), false, L"Equalizer and volume normalizer");
        x -= 32 + 8;
        using Mode = VisOptions::Mode;
        const Mode mode = vis_.opt.mode;
        const bool want_milk = mode == Mode::milkdrop, bars = mode != Mode::milkdrop && mode != Mode::dasvis;
        for (const Mode m : {winamp_mode_, Mode::milkdrop, Mode::dasvis}) {  // right to left: Bars, MilkDrop, DasVis
            const bool is_bars = m != Mode::milkdrop && m != Mode::dasvis;
            const std::wstring label = is_bars ? L"Bars" : m == Mode::milkdrop ? L"MilkDrop" : L"DasVis";
            const bool on = is_bars ? bars : m == mode;
            x -= ui_.chip_width(label) + (on ? 12 : 0);
            ui_.chip(x, cy3 - 15, label, std::nullopt, on, false, [this, m, on] {
                if (!on) vis_use(m);
            });
            x -= 6;
        }
        if (das) {
            float l = in.l;
            ui_.icon_button(Rect::xywh(l, cy3 - 16, 32, 32), icon::chevron_left, 13, [this] { das_->previous(); }, false, false, L"Previous scene (P)");
            const float name_l = l + 36, buttons_w = 2 * 36;
            const float name_r = std::max(name_l, x - 8 - buttons_w);
            const std::wstring name = wide(das_->scene_name());
            const TextStyle ns{Font::ui, 13.5f, 600, text};
            const float name_w = std::min(g_.measure(name, ns) + 2, name_r - name_l);
            g_.text(name, Rect{name_l, cy3 - 12, name_l + name_w, cy3 + 12}, ns);
            l = name_l + name_w + 4;
            ui_.icon_button(Rect::xywh(l, cy3 - 16, 32, 32), icon::chevron_right, 13, [this] { das_->next(); }, false, false, L"Next scene (N)");
            ui_.icon_button(Rect::xywh(l + 36, cy3 - 16, 32, 32), icon::lock, 13, [this] { das_->lock(!das_->locked()); }, das_->locked(), false,
                            das_->locked() ? L"Unlock this scene (L)" : L"Lock this scene (L)");
            const auto liked = das_taste_.find(das_->scene_name());
            const bool loved = liked != das_taste_.end() && liked->second > 1;
            ui_.icon_button(Rect::xywh(l + 72, cy3 - 16, 32, 32), loved ? icon::heart_fill : icon::heart, 13, [this] { das_rate(true); }, loved, false,
                            L"Like this scene (F): more of it for this genre");
            // What this PC runs it at, quietly: the tier and the render size.
            const std::wstring how = std::format(L"{} · {:.0f} %", wide(dasvis::tier_name(das_->tier())), das_->scale() * 100);
            float hx = l + 112;
            if (!das_status_.empty() && hx + 200 < x) {  // the song map's word on where the song is
                const TextStyle ss{Font::ui, 13, 600, das_status_.starts_with(L"Build") ? peach : lilac};
                const float sw = g_.measure(das_status_, ss) + 2;
                g_.text(das_status_, Rect{hx, cy3 - 12, hx + sw, cy3 + 12}, ss);
                hx += sw + 14;
            }
            if (hx + 90 < x) g_.text(how, Rect{hx, cy3 - 12, x - 8, cy3 + 12}, {Font::ui, 12, 500, text3});
        } else if (milk) {
            float l = in.l;
            ui_.icon_button(Rect::xywh(l, cy3 - 16, 32, 32), icon::chevron_left, 13, [this] { milk_->previous(); }, false, false, L"Previous preset (P)");
            const float name_l = l + 36, buttons_w = 3 * 36;
            const float name_r = std::max(name_l, x - 8 - buttons_w);
            std::wstring name = wide(milk_->preset_name());
            const TextStyle ns{Font::ui, 13.5f, 600, text};
            const float name_w = std::min(g_.measure(name, ns) + 2, name_r - name_l);  // the buttons sit right after the name
            g_.text(name, Rect{name_l, cy3 - 12, name_l + name_w, cy3 + 12}, ns);
            l = name_l + name_w + 4;
            ui_.icon_button(Rect::xywh(l, cy3 - 16, 32, 32), icon::chevron_right, 13, [this] { milk_->next(); }, false, false, L"Next preset (N)");
            ui_.icon_button(Rect::xywh(l + 36, cy3 - 16, 32, 32), icon::shuffle, 13, [this] { milk_->random(); }, false, false, L"Random preset (R)");
            ui_.icon_button(Rect::xywh(l + 72, cy3 - 16, 32, 32), icon::lock, 13, [this] { milk_->lock(!milk_->locked()); }, milk_->locked(), false,
                            milk_->locked() ? L"Unlock this preset (L)" : L"Lock this preset (L)");
        } else if (mode == Mode::dasvis) {
            g_.text(L"DasVis couldn't start on this graphics card — showing the Winamp bars", Rect{in.l, cy3 - 12, std::max(in.l, x - 8), cy3 + 12},
                    {Font::ui, 13, 600, peach});
        } else if (want_milk) {
            g_.text(milk_pending_.valid() ? L"Loading MilkDrop…" : L"MilkDrop needs OpenGL 3.3 — showing the Winamp bars",
                    Rect{in.l, cy3 - 12, std::max(in.l, x - 8), cy3 + 12}, {Font::ui, 13, 600, milk_pending_.valid() ? text2 : peach});
        }

        // Up next: the next five items in the queue, each a click to play.
        if (up) {
            const Rect card{in.r + 12, panel.t, panel.r, panel.b};
            g_.fill(Rect{card.l, card.t + 20, card.l + 1, card.b - 20}, glass_border);  // the divider
            ui_.dot_label(L"Up next", card.l + 20, card.t + 28, text3);
            for (size_t i = 0; i < std::min<size_t>(5, upcoming); ++i) {
                const player::Item& it = items[first_next + i];
                const Rect rr = Rect::xywh(card.l + 10, card.t + 48 + float(i) * 34, card.w() - 20, 34);
                if (ui_.hover(rr)) g_.fill_round(rr, 10, hover);
                g_.text(wide(it.title), Rect{rr.l + 10, rr.t + 1, rr.r - 10, rr.t + 18}, {Font::ui, 13, 600, text});
                g_.text(wide(it.artist), Rect{rr.l + 10, rr.t + 17, rr.r - 10, rr.t + 33}, {Font::ui, 11.5f, 400, text3});
                ui_.click(rr, [this, n = first_next + i] { player_.jump(n); });
            }
        }
    }
    if (a < 1) g_.pop_opacity();
    if (eq_open_) eq_panel();  // always at full strength: the overlay stays up while it's open
}

void View::vis_use(VisOptions::Mode m) {
    using Mode = VisOptions::Mode;
    const bool engine = m == Mode::milkdrop || m == Mode::dasvis;
    if (vis_.opt.mode == m && !engine) return;
    if (vis_.opt.mode == m) {
        vis_.opt.mode = winamp_mode_;  // M on MilkDrop, D on DasVis: back to the bars
    } else {
        if (vis_.opt.mode != Mode::milkdrop && vis_.opt.mode != Mode::dasvis) winamp_mode_ = vis_.opt.mode;
        vis_.opt.mode = m;
    }
    audio_at_ = clock_t_::now();
    save_vis();
}

// MARK: Options menu

void View::vis_menu() {
    auto& o = vis_.opt;
    enum : UINT { kMode = 100, kBars = 200, kScope = 300, kBarFall = 400, kPeakFall = 500, kPeaks = 600, kLook = 700, kExit = 800, kQuality = 900, kAdvance = 1000, kLock = 1100, kFolder = 1200, kPulse = 1300, kCuts = 1400, kPresets = 1500, kSense = 1600, kSync = 1700, kDasIntensity = 1800, kDasQuality = 1900, kDas = 2000, kDasScreens = 2100 };
    auto radio = [](HMENU m, UINT id, const wchar_t* label, bool on) { AppendMenuW(m, MF_STRING | (on ? MF_CHECKED : 0), id, label); };
    HMENU m = CreatePopupMenu();
    const wchar_t* modes[] = {L"Spectrum analyser", L"Oscilloscope", L"Both", L"Off"};
    radio(m, kMode + 5, L"DasVis\tD", o.mode == VisOptions::Mode::dasvis);
    radio(m, kMode + 4, L"MilkDrop\tM", o.mode == VisOptions::Mode::milkdrop);
    for (int i = 0; i < 4; ++i) radio(m, kMode + i, modes[i], int(o.mode) == i);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    HMENU das = CreatePopupMenu(), das_int = CreatePopupMenu(), das_q = CreatePopupMenu();
    const wchar_t* intensities[] = {L"Chill", L"Normal", L"Insane"};
    for (int i = 0; i < 3; ++i) radio(das_int, kDasIntensity + i, intensities[i], o.das_intensity == i);
    const wchar_t* das_qualities[] = {L"Auto (measured on this PC)", L"Low (old PCs)", L"Medium", L"High"};
    for (int i = 0; i < 4; ++i) radio(das_q, kDasQuality + i, das_qualities[i], o.das_quality == i);
    if (das_ && das_->ok()) {
        const std::wstring now = std::format(L"Now: {} at {:.0f} %, {:.1f} ms a frame", wide(dasvis::tier_name(das_->tier())), das_->scale() * 100, das_->gpu_ms());
        AppendMenuW(das_q, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(das_q, MF_STRING | MF_GRAYED, 0, now.c_str());
    }
    AppendMenuW(das, MF_POPUP, UINT_PTR(das_int), L"Intensity");
    AppendMenuW(das, MF_POPUP, UINT_PTR(das_q), L"Quality");
    radio(das, kDas + 0, L"Save battery (30 fps on battery)", o.das_battery);
    radio(das, kDas + 1, L"Reduce flashing", o.das_reduce_flash);
    radio(das, kDas + 2, L"Use the fast graphics card", o.das_fast_gpu);
    radio(das, kDas + 5, L"Lock this scene\tL", das_ && das_->locked());
    AppendMenuW(das, MF_STRING | (das_ && das_->ok() ? 0 : MF_GRAYED), kDas + 6, L"Like this scene\tF");
    AppendMenuW(das, MF_STRING | (das_ && das_->ok() ? 0 : MF_GRAYED), kDas + 7, L"Skip this scene\tX");
    radio(das, kDas + 8, L"Phone VJ deck…", vj_card_);
    radio(das, kDas + 9, L"Spout output (OBS, Resolume…)", spout_ != nullptr);
    radio(das, kDas + 10, L"Live wallpaper (behind the desktop icons)", o.das_wallpaper);
    radio(das, kDas + 11, L"Lyrics (looked up on LRCLIB.net)", o.das_lyrics);
    radio(das, kDas + 12, L"MilkDrop presets as a scene (~60 MB more while it shows)", o.das_milkdrop);
    if (GetSystemMetrics(SM_CMONITORS) > 1) {
        HMENU screens = CreatePopupMenu();
        const wchar_t* ways[] = {L"This screen", L"All screens, as one picture", L"Each screen its own scene"};
        for (int i = 0; i < 3; ++i) radio(screens, kDasScreens + i, ways[i], o.das_screens == i);
        AppendMenuW(das, MF_POPUP, UINT_PTR(screens), L"Screens");
    }
    AppendMenuW(das, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(das, MF_STRING, kDas + 3, L"Open my scenes folder…");
    AppendMenuW(das, MF_STRING | (das_ && das_->ok() ? 0 : MF_GRAYED), kDas + 4, L"Reload my scenes\tF5");
    AppendMenuW(m, MF_POPUP, UINT_PTR(das), L"DasVis");
    HMENU quality = CreatePopupMenu(), advance = CreatePopupMenu();
    const wchar_t* qualities[] = {L"540p (lightest)", L"720p", L"1080p (sharpest)"};
    const wchar_t* advances[] = {L"Off", L"Every 15 s", L"Every 30 s", L"Every 60 s"};
    for (int i = 0; i < 3; ++i) radio(quality, kQuality + i, qualities[i], o.quality == kQualities[i]);
    for (int i = 0; i < 4; ++i) radio(advance, kAdvance + i, advances[i], o.auto_advance == kAdvances[i]);
    AppendMenuW(m, MF_POPUP, UINT_PTR(quality), L"MilkDrop quality");
    AppendMenuW(m, MF_POPUP, UINT_PTR(advance), L"Next preset / scene automatically");
    radio(m, kLock, L"Lock this preset\tL", milk_ && milk_->locked());
    HMENU pulse = CreatePopupMenu(), presets = CreatePopupMenu(), sense = CreatePopupMenu(), sync = CreatePopupMenu();
    const wchar_t* pulses[] = {L"Off", L"Subtle", L"Strong"};
    const wchar_t* senses[] = {L"Low", L"Normal", L"High"};
    for (int i = 0; i < 3; ++i) radio(pulse, kPulse + i, pulses[i], o.pulse == i);
    for (int i = 0; i < 3; ++i) radio(sense, kSense + i + 1, senses[i], o.beat_sensitivity == i + 1);
    radio(presets, kPresets + 0, L"Beat-heavy (no Fractal, Hypnotic)", !o.all_presets);
    radio(presets, kPresets + 1, L"All", o.all_presets);
    const std::wstring now_sync = std::format(L"Now {:+} ms{}", o.sync_ms, bt_output_ ? L" + 180 ms Bluetooth" : L"");
    AppendMenuW(sync, MF_STRING | MF_GRAYED, 0, now_sync.c_str());
    AppendMenuW(sync, MF_STRING, kSync + 0, L"Pictures earlier (−25 ms)\t[");
    AppendMenuW(sync, MF_STRING, kSync + 1, L"Pictures later (+25 ms)\t]");
    AppendMenuW(sync, MF_STRING, kSync + 2, L"Reset");
    AppendMenuW(m, MF_POPUP, UINT_PTR(pulse), L"Beat pulse");
    radio(m, kCuts, L"Change preset on drops", o.hard_cuts);
    AppendMenuW(m, MF_POPUP, UINT_PTR(sense), L"Beat sensitivity");
    AppendMenuW(m, MF_POPUP, UINT_PTR(presets), L"Presets");
    AppendMenuW(m, MF_POPUP, UINT_PTR(sync), L"Sync with the sound");
    AppendMenuW(m, MF_STRING, kFolder, L"Open presets folder…");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    HMENU bars = CreatePopupMenu(), scope = CreatePopupMenu(), bar_fall = CreatePopupMenu(), peak_fall = CreatePopupMenu(), look = CreatePopupMenu();
    const wchar_t* bar_styles[] = {L"Normal", L"Fire", L"Line"};
    const wchar_t* scope_styles[] = {L"Dots", L"Lines", L"Solid"};
    const wchar_t* speeds[] = {L"Slowest", L"Slower", L"Medium", L"Faster", L"Fastest"};
    for (int i = 0; i < 3; ++i) radio(bars, kBars + i, bar_styles[i], int(o.bars) == i);
    for (int i = 0; i < 3; ++i) radio(scope, kScope + i, scope_styles[i], int(o.scope) == i);
    for (int i = 0; i < 5; ++i) radio(bar_fall, kBarFall + i, speeds[i], o.bar_falloff == i);
    for (int i = 0; i < 5; ++i) radio(peak_fall, kPeakFall + i, speeds[i], o.peak_falloff == i);
    radio(look, kLook + 0, L"Classic Winamp", o.classic);
    radio(look, kLook + 1, L"WreckBox", !o.classic);
    AppendMenuW(m, MF_POPUP, UINT_PTR(bars), L"Analyser bars");
    AppendMenuW(m, MF_POPUP, UINT_PTR(bar_fall), L"Analyser falloff");
    AppendMenuW(m, MF_POPUP, UINT_PTR(peak_fall), L"Peak falloff");
    radio(m, kPeaks, L"Show peaks", o.peaks);
    AppendMenuW(m, MF_POPUP, UINT_PTR(scope), L"Oscilloscope style");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_POPUP, UINT_PTR(look), L"Look\tV");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kExit, L"Exit full screen\tEsc");
    POINT pt;
    GetCursorPos(&pt);
    const UINT cmd = UINT(TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr));
    DestroyMenu(m);  // destroys the submenus too
    if (!cmd) return;
    if (cmd == kExit) return set_fullscreen(false);
    const int i = int(cmd % 100);
    switch (cmd / 100 * 100) {
        case kMode: o.mode = VisOptions::Mode(i); break;
        case kBars: o.bars = VisOptions::Bars(i); break;
        case kScope: o.scope = VisOptions::Scope(i); break;
        case kBarFall: o.bar_falloff = i; break;
        case kPeakFall: o.peak_falloff = i; break;
        case kPeaks: o.peaks = !o.peaks; break;
        case kLook: o.classic = i == 0; break;
        case kQuality: o.quality = milk_quality_ = kQualities[i]; break;
        case kAdvance:
            o.auto_advance = kAdvances[i];
            if (milk_) milk_->set_auto_advance(o.auto_advance);
            if (das_) das_->set_auto_advance(o.auto_advance);
            break;
        case kDasIntensity: o.das_intensity = i; break;
        case kDasQuality:
            o.das_quality = i;
            das_free();  // made again with the new tier on the next frame
            break;
        case kDas:
            if (i == 0) o.das_battery = !o.das_battery, das_free();
            else if (i == 1) {
                o.das_reduce_flash = !o.das_reduce_flash;
                if (das_) das_->set_reduce_flash(o.das_reduce_flash);
            } else if (i == 2) o.das_fast_gpu = !o.das_fast_gpu;  // the next paint rebuilds the device on the other GPU
            else if (i == 3) {
                dasvis::prepare_user_dir(das_scenes_dir());
                ShellExecuteW(hwnd_, L"open", das_scenes_dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return;
            } else if (i == 4) return das_reload();
            else if (i == 5 && das_) return das_->lock(!das_->locked());
            else if (i == 6 || i == 7) return das_rate(i == 6);
            else if (i == 9) {
                if (spout_) spout_.reset();
                else spout_ = std::make_unique<dasvis::SpoutSender>(), show_toast(L"Sharing as Spout sender \"WreckBox DasVis\"");
                return;
            } else if (i == 10) {
                o.das_wallpaper = !o.das_wallpaper;  // starts when full screen closes
                show_toast(o.das_wallpaper ? L"Live wallpaper: on when you leave full screen" : L"Live wallpaper: off");
            } else if (i == 12) {
                o.das_milkdrop = !o.das_milkdrop;
                das_free();  // made again with (or without) the scene on the next frame
                if (!o.das_milkdrop) milk_restart();  // projectM's memory back
            } else if (i == 11) {
                o.das_lyrics = !o.das_lyrics;
                das_lyrics_for_.clear(), das_lyrics_.reset();  // looked up now, or let go
                show_toast(o.das_lyrics ? L"Lyrics: on (a .lrc next to the song, else LRCLIB.net)" : L"Lyrics: off");
            } else if (i == 8) {  // the deck's page is on the phone-sync server: start it if it isn't running
                if (!vj_card_ && sync_ && !sync_->running() && !sync_->start()) return show_toast(L"Couldn't start the phone server (port in use?)");
                vj_card_ = !vj_card_;
                return;
            }
            break;
        case kLock:
            if (milk_) milk_->lock(!milk_->locked());
            return;
        case kPulse: o.pulse = i; break;
        case kCuts:
            o.hard_cuts = !o.hard_cuts;
            if (milk_) milk_->set_hard_cuts(o.hard_cuts);
            break;
        case kSense:
            o.beat_sensitivity = i;
            if (milk_) milk_->set_beat_sensitivity(float(i));
            break;
        case kPresets:
            if (o.all_presets != (i == 1)) {
                o.all_presets = i == 1;
                milk_restart();  // a different set of folders: build the playlist again (about a second)
            }
            break;
        case kDasScreens:
            o.das_screens = i;
            save_vis();
            set_fullscreen(false), set_fullscreen(true);  // the window spans the screens, or the others get theirs
            return;
        case kSync:
            o.sync_ms = i == 2 ? 0 : std::clamp(o.sync_ms + (i ? 25 : -25), -300, 300);
            milk_heard_ = -1;
            break;
        case kFolder: {
            std::error_code ec;
            std::filesystem::create_directories(user_presets_dir(), ec);
            ShellExecuteW(hwnd_, L"open", user_presets_dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
    }
    save_vis();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

}  // namespace wb::ui
