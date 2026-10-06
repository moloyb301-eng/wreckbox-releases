// The full-screen visualizer: MilkDrop presets (player::MilkDrop, drawn from its pixels) by default, or Winamp 2 style —
// spectrum bars (normal / fire / line) with falling peak caps, the oscilloscope (dots / lines / solid), both, or off, in
// Classic Winamp colours or WreckBox pastel. Without OpenGL 3.3 it shows the Winamp bars. Click: next preset (Winamp
// modes: step through them), right-click has every option, N / P / R / L / M / V keys; Esc, F11 or a double-click leave.
// The bar maths is player::Visualizer (unit-tested); this file draws it and runs the screen.
#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>

#include "model/paths.h"
#include "model/settings.h"
#include "ui/view.h"

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

constexpr int kQualities[] = {540, 720, 1080};
constexpr int kAdvances[] = {0, 15, 30, 60};

std::wstring clock_text(int64_t ms) {
    const int64_t s = std::max<int64_t>(0, ms) / 1000;
    return s >= 3600 ? std::format(L"{}:{:02}:{:02}", s / 3600, s / 60 % 60, s % 60) : std::format(L"{}:{:02}", s / 60, s % 60);
}

}  // namespace

// MARK: Window

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
    } else {
        if (milk_pending_.valid()) milk_pending_.wait();  // left before it finished starting
        milk_pending_ = {};
        milk_.reset();  // free the GPU and its memory
        g_.clear_stream();
        tap_cursor_ = 0;
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
            if (milk_ && milk_->ok()) {
                if (vk == 'N') milk_->next();
                else if (vk == 'P') milk_->previous();
                else if (vk == 'R') milk_->random();
                else milk_->lock(!milk_->locked());
                audio_at_ = clock_t_::now();  // show the change even while paused
            }
            return true;
        case 'M': vis_use_milk(vis_.opt.mode != VisOptions::Mode::milkdrop); return true;
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
    cfg.preset_dirs = {base / L"presets", user_presets_dir()};
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
        if (playing && out) n = out->take_tap(tap_cursor_, pcm_.data(), pcm_.size());
        else if (!playing) std::fill(pcm_.begin(), pcm_.begin() + 512, 0.f), n = 512;
        const int h = milk_quality_, w = int(std::lround(h * W / std::max(1.f, H))) & ~1;
        if ((w != milk_w_ || h != milk_h_) && milk_->resize(w, h)) milk_w_ = w, milk_h_ = h;
        if (milk_->ok()) {
            const auto f = milk_->render(pcm_.data(), n);
            if (f) g_.upload_stream(f.bgra, f.width, f.height);
        }
    }
    if (!g_.draw_stream(Rect{0, 0, W, H}, true)) g_.fill(Rect{0, 0, W, H}, black);
}

void View::vis_click() {
    mode_before_click_ = vis_.opt.mode;
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
    const float W = g_.width(), H = g_.height();
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
    if (out && player_.playing()) out->tap(tap_.data(), tap_.size());
    else std::fill(tap_.begin(), tap_.end(), 0.f);
    vis_.set_band_count(size_t(W / 14));
    vis_.update(tap_.data(), dt);
    vis_settling_ = !player_.playing() && (*std::max_element(vis_.peaks().begin(), vis_.peaks().end()) > 0);

    const bool classic = vis_.opt.classic;
    g_.fill(Rect{0, 0, W, H}, classic ? black : bg);
    if (!classic) g_.fill_radial(Rect{0, 0, W, H}, {W * 0.5f, H * 1.1f}, std::max(W, H), argb(0x30BB96DA), argb(0x00BB96DA));
    const Rect area{48, 56, W - 48, H - 56};
    using Mode = VisOptions::Mode;
    const Mode mode = want_milk ? Mode::spectrum : vis_.opt.mode;  // no MilkDrop here: the Winamp bars instead
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

// The player panel over the picture: hint line and exit at the top; at the bottom a glass card with the song, the
// transport, seek and volume, and the preset controls, and next to it the "Up next" list. It fades as one layer.
void View::vis_overlay(const Rect& r, float a) {
    const bool milk = milk_ && milk_->ok() && vis_.opt.mode == VisOptions::Mode::milkdrop;
    if (a < 1) g_.push_opacity(a);
    g_.fill_span(Rect{r.l, r.t, r.r, r.t + 80}, {0, r.t}, {0, r.t + 80}, {argb(0xA0000000), argb(0x00000000)});  // keeps the hint legible
    g_.text(milk ? L"Click / N: next preset     P: previous     R: random     L: lock     M: bars     Right-click: options     Space: pause     Esc: exit"
                 : L"Click: spectrum · scope · both · off     M: MilkDrop     Right-click: options     V: look     Space: pause     Esc: exit",
            Rect{r.l + 48, r.t + 18, r.r - 100, r.t + 44}, {Font::ui, 12.5f, 500, text2});
    ui_.icon_button(Rect::xywh(r.r - 64, r.t + 14, 36, 36), icon::exit_full, 14, [this] { set_fullscreen(false); }, false, false,
                    L"Exit full screen (Esc)");

    const player::Item* cur = player_.current();
    if (cur) {
        const auto& items = player_.queue().items();
        const size_t first_next = size_t(player_.queue().index() + 1), upcoming = first_next < items.size() ? items.size() - first_next : 0;
        const bool up = upcoming > 0 && r.w() >= 1100;
        const float gap = 12, uw = 260, ph = 236;
        const float pw = std::min(760.f, r.w() - 48 - (up ? gap + uw : 0));
        const float left = (r.w() - pw - (up ? gap + uw : 0)) / 2;
        const Rect panel = Rect::xywh(r.l + left, r.b - 24 - ph, pw, ph);
        ui_.glass(panel, 24, false, true);
        ui_.click(panel, [] {});  // clicks on the panel stay on it
        const Rect in = panel.inset(20);

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
        g_.text(wide(d.title), Rect{tx, in.t, in.r, in.t + 34}, {Font::ui, 26, 600, text});
        g_.text(wide(d.subtitle), Rect{tx, in.t + 34, in.r, in.t + 58}, {Font::ui, 15, 400, text2});
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

        // Row 3: the preset (◀ name ▶, random, lock), the MilkDrop / Bars switch, the equalizer.
        const float cy3 = cy2 + 20 + 12 + 16;
        float x = in.r;
        eq_x_ = x - 16;
        bar_top_ = panel.t;
        ui_.icon_button(Rect::xywh(x - 32, cy3 - 16, 32, 32), icon::equalizer, 14, [this] { eq_open_ = !eq_open_; },
                        eq_open_ || player_.equalizer().enabled || player_.normalize(), false, L"Equalizer and volume normalizer");
        x -= 32 + 8;
        const bool want_milk = vis_.opt.mode == VisOptions::Mode::milkdrop;
        for (const bool is_milk : {false, true}) {  // right to left: Bars, then MilkDrop
            const std::wstring label = is_milk ? L"MilkDrop" : L"Bars";
            const bool on = is_milk == want_milk;
            x -= ui_.chip_width(label) + (on ? 12 : 0);
            ui_.chip(x, cy3 - 15, label, std::nullopt, on, false, [this, is_milk] { vis_use_milk(is_milk); });
            x -= 6;
        }
        if (milk) {
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
        } else if (want_milk) {
            g_.text(milk_pending_.valid() ? L"Loading MilkDrop…" : L"MilkDrop needs OpenGL 3.3 — showing the Winamp bars",
                    Rect{in.l, cy3 - 12, std::max(in.l, x - 8), cy3 + 12}, {Font::ui, 13, 600, milk_pending_.valid() ? text2 : peach});
        }

        // Up next: the next five items in the queue, each a click to play.
        if (up) {
            const Rect card = Rect::xywh(panel.r + gap, panel.t, uw, ph);
            ui_.glass(card, 24, false, true);
            ui_.click(card, [] {});
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

void View::vis_use_milk(bool milk) {
    using Mode = VisOptions::Mode;
    if (milk == (vis_.opt.mode == Mode::milkdrop)) return;
    if (milk) winamp_mode_ = vis_.opt.mode, vis_.opt.mode = Mode::milkdrop;
    else vis_.opt.mode = winamp_mode_;
    audio_at_ = clock_t_::now();
    save_vis();
}

// MARK: Options menu

void View::vis_menu() {
    auto& o = vis_.opt;
    enum : UINT { kMode = 100, kBars = 200, kScope = 300, kBarFall = 400, kPeakFall = 500, kPeaks = 600, kLook = 700, kExit = 800, kQuality = 900, kAdvance = 1000, kLock = 1100, kFolder = 1200 };
    auto radio = [](HMENU m, UINT id, const wchar_t* label, bool on) { AppendMenuW(m, MF_STRING | (on ? MF_CHECKED : 0), id, label); };
    HMENU m = CreatePopupMenu();
    const wchar_t* modes[] = {L"Spectrum analyser", L"Oscilloscope", L"Both", L"Off"};
    radio(m, kMode + 4, L"MilkDrop\tM", o.mode == VisOptions::Mode::milkdrop);
    for (int i = 0; i < 4; ++i) radio(m, kMode + i, modes[i], int(o.mode) == i);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    HMENU quality = CreatePopupMenu(), advance = CreatePopupMenu();
    const wchar_t* qualities[] = {L"540p (lightest)", L"720p", L"1080p (sharpest)"};
    const wchar_t* advances[] = {L"Off", L"Every 15 s", L"Every 30 s", L"Every 60 s"};
    for (int i = 0; i < 3; ++i) radio(quality, kQuality + i, qualities[i], o.quality == kQualities[i]);
    for (int i = 0; i < 4; ++i) radio(advance, kAdvance + i, advances[i], o.auto_advance == kAdvances[i]);
    AppendMenuW(m, MF_POPUP, UINT_PTR(quality), L"MilkDrop quality");
    AppendMenuW(m, MF_POPUP, UINT_PTR(advance), L"Next preset automatically");
    radio(m, kLock, L"Lock this preset\tL", milk_ && milk_->locked());
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
            break;
        case kLock:
            if (milk_) milk_->lock(!milk_->locked());
            return;
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
