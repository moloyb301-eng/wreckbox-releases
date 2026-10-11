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
        saved_style_ = GetWindowLongW(hwnd_, GWL_STYLE);
        GetWindowPlacement(hwnd_, &saved_place_);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(hwnd_, GWL_STYLE, saved_style_ & ~WS_OVERLAPPEDWINDOW);
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
        case 'M': vis_use(VisOptions::Mode::milkdrop); return true;
        case 'D': vis_use(VisOptions::Mode::dasvis); return true;
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

void View::milk_screen(float W, float H) {
    const auto now = clock_t_::now();
    const bool playing = player_.playing();
    if (playing) audio_at_ = now;
    // Paused: keep rendering silence for 4 s so the picture calms down, then hold the last frame (no timer, 0% CPU).
    const bool live = playing || now - audio_at_ < std::chrono::seconds(4);
    vis_settling_ = !playing && live;
    if (live) {
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
        const int h = milk_quality_, w = int(std::lround(h * W / std::max(1.f, H))) & ~1;
        if ((w != milk_w_ || h != milk_h_) && milk_->resize(w, h)) milk_w_ = w, milk_h_ = h;
        if (milk_->ok()) {
            const auto f = milk_->render(pcm_.data(), n);
            if (f) g_.upload_stream(f.bgra, f.width, f.height);
        }
    }
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
    const float fps = vis_.opt.das_battery && on_battery() ? 30.f : 60.f;  // a 120 Hz screen still gets 60
    return UINT(std::max(1.f, std::round(das_hz_ / fps)));
}

std::filesystem::path View::das_scenes_dir() const { return paths::settings_dir() / L"dasvis" / L"scenes"; }

void View::das_free() {
    if (!das_) return;
    das_.reset();
    das_cover_for_.clear();
}

void View::das_reload() {
    if (!das_ || !das_->ok()) return;
    const std::string errors = das_->reload_user_scenes();
    show_toast(errors.empty() ? std::format(L"{} scenes", das_->scene_count()) : L"Your scenes have errors (shown at the top)");
}

bool View::das_screen() {
    using dasvis::Tier;
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
        dasvis::Config cfg;
        const std::string known = o.das_tiers.value(gpu, std::string());
        cfg.start = o.das_quality ? Tier(o.das_quality - 1) : known == "low" ? Tier::low : known == "high" ? Tier::high : Tier::medium;
        cfg.auto_tier = o.das_quality == 0;
        cfg.max_tier = o.das_battery && on_battery() ? Tier::medium : Tier::high;
        cfg.reduce_flash = o.das_reduce_flash;
        cfg.auto_advance = o.auto_advance;
        cfg.user_dir = das_scenes_dir();
        das_ = std::make_unique<dasvis::DasVis>(g_.d3d(), cfg);
        das_gen_ = g_.generation();
        das_feat_ = {};
        das_tier_saved_ = false;
        das_frames_ = 0;
        perf_log(das_->ok() ? std::format("DasVis started on {}: {} scenes, {} tier{}", gpu, das_->scene_count(), dasvis::tier_name(das_->tier()),
                                          das_->user_errors().empty() ? "" : "; your scenes have errors")
                            : "DasVis unavailable: " + das_->error());
    }
    if (!das_->ok()) return false;

    const auto back = g_.back_buffer();
    if (!back) return false;
    g_.flush();  // anything Direct2D queued goes under the picture, not over it
    if (!live) das_->redraw(back.Get());  // paused and settled: the same picture, nothing moves on
    else {
        player::AudioOutput* out = player_.output();
        static_assert(player::Visualizer::kFft == dasvis::Features::kFft);
        if (playing && out) out->tap_until(heard_end(*out, 1), tap_.data(), tap_.size());  // on screen at the next refresh
        else std::fill(tap_.begin(), tap_.end(), 0.f);
        const player::Item* cur = player_.current();
        const auto row = cur && cur->track_id ? store_.row(*cur->track_id) : std::nullopt;
        das_feat_.update(tap_.data(), frame_dt_, row && row->bpm() ? *row->bpm() : 0);
        dasvis::Inputs in;
        in.f = &das_feat_;
        in.dt = frame_dt_;
        in.song = cur ? (cur->track_id ? *cur->track_id : cur->location) : std::string();
        if (row && !row->camelot().empty()) in.camelot = row->camelot();
        in.intensity = o.das_intensity;
        in.budget_ms = 0.75f * 1000.f * float(frame_interval()) / das_hz_;
        // The cover: decoded off the UI thread at 512 px, handed over when it's ready (if the song is still on).
        if (in.song != das_cover_for_) {
            das_cover_for_ = in.song;
            das_->set_cover(nullptr, 0, 0);
            if (row) {
                auto pixels = std::make_shared<std::vector<uint8_t>>();
                auto size = std::make_shared<std::pair<UINT, UINT>>();
                jobs_.run(
                    [this, t = row->track, pixels, size] {
                        const auto file = store_.ensure_artwork(t);
                        const auto bmp = file ? decode_image(*file, 512) : nullptr;
                        if (!bmp || FAILED(bmp->GetSize(&size->first, &size->second))) return;
                        pixels->resize(size_t(size->first) * size->second * 4);
                        bmp->CopyPixels(nullptr, size->first * 4, UINT(pixels->size()), pixels->data());
                    },
                    [this, song = in.song, pixels, size] {
                        if (das_ && das_cover_for_ == song && !pixels->empty()) das_->set_cover(pixels->data(), int(size->first), int(size->second));
                    });
            }
        }
        das_->render(in, back.Get());
        // Remember the measured tier for this graphics card, so the next visit starts there.
        if (!das_tier_saved_ && o.das_quality == 0 && das_->tier_settled()) {
            das_tier_saved_ = true;
            std::string t = dasvis::tier_name(das_->tier());
            std::transform(t.begin(), t.end(), t.begin(), [](char c) { return char(std::tolower(c)); });
            if (o.das_tiers.value(gpu, std::string()) != t) o.das_tiers[gpu] = t, save_vis();
        }
        if (++das_frames_ % 300 == 0)
            perf_log(std::format("DasVis: {}, {} tier at {:.0f} %, GPU {:.2f} ms a frame (budget {:.1f}), every {} refresh(es) of {:.0f} Hz, flash budget {:.2f}",
                                 das_->scene_name(), dasvis::tier_name(das_->tier()), das_->scale() * 100, das_->gpu_ms(), in.budget_ms,
                                 frame_interval(), das_hz_, das_->flash_budget()));
    }
    return true;
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
    g_.fill_span(Rect{r.l, r.t, r.r, r.t + 80}, {0, r.t}, {0, r.t + 80}, {argb(0xA0000000), argb(0x00000000)});  // keeps the hint legible
    g_.text(das    ? L"Click / N: next scene     P: previous     L: lock     D: bars     F5: reload my scenes     H: hide player     Right-click: options     Esc: exit"
            : milk ? L"Click / N: next preset     P: previous     R: random     L: lock     M: bars     H: hide player     Right-click: options     Space: pause     Esc: exit"
                   : L"Click: spectrum · scope · both · off     M: MilkDrop     D: DasVis     H: hide player     Right-click: options     V: look     Esc: exit",
            Rect{r.l + 48, r.t + 18, r.r - 100, r.t + 44}, {Font::ui, 12.5f, 500, text2});
    if (das && !das_->user_errors().empty())  // your scenes: the first error of each that didn't compile
        g_.paragraph(L"Your scenes: " + wide(das_->user_errors()), Rect{r.l + 48, r.t + 48, r.r - 100, r.t + 200}, {Font::ui, 12.5f, 500, peach});
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
            // What this PC runs it at, quietly: the tier and the render size.
            const std::wstring how = std::format(L"{} · {:.0f} %", wide(dasvis::tier_name(das_->tier())), das_->scale() * 100);
            const float hx = l + 76;
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
    enum : UINT { kMode = 100, kBars = 200, kScope = 300, kBarFall = 400, kPeakFall = 500, kPeaks = 600, kLook = 700, kExit = 800, kQuality = 900, kAdvance = 1000, kLock = 1100, kFolder = 1200, kPulse = 1300, kCuts = 1400, kPresets = 1500, kSense = 1600, kSync = 1700, kDasIntensity = 1800, kDasQuality = 1900, kDas = 2000 };
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
