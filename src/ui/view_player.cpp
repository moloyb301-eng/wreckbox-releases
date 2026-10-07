// The player bar (port of app/lib/ui/player_bar.dart, plus volume, VLC's equalizer, Open and the visualizer), the
// equalizer pop-over, Open files / folder / URL, drag & drop, media keys and the repaint ticks while playing.
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <format>

#include "ui/view.h"

namespace wb::ui {

using namespace theme;
using player::Player;
using player::VlcEngine;

namespace {

std::wstring clock_text(int64_t ms) {
    const int64_t s = std::max<int64_t>(0, ms) / 1000;
    return s >= 3600 ? std::format(L"{}:{:02}:{:02}", s / 3600, s / 60 % 60, s % 60) : std::format(L"{}:{:02}", s / 60, s % 60);
}

// Open files… / Open folder… (folders are searched for music later, by Player::collect on a worker).
std::vector<std::string> pick(HWND owner, bool folder) {
    std::vector<std::string> out;
    Microsoft::WRL::ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
    std::wstring patterns;
    if (!folder) {
        for (const auto& e : Player::extensions()) patterns += (patterns.empty() ? L"*." : L";*.") + widen(e);
        const COMDLG_FILTERSPEC filter[] = {{L"Music and playlists", patterns.c_str()}, {L"All files", L"*.*"}};
        dlg->SetFileTypes(2, filter);
    }
    dlg->SetTitle(folder ? L"Choose folders of music" : L"Choose music to play");
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_ALLOWMULTISELECT | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    if (FAILED(dlg->Show(owner))) return out;
    Microsoft::WRL::ComPtr<IShellItemArray> items;
    DWORD n = 0;
    if (FAILED(dlg->GetResults(&items)) || FAILED(items->GetCount(&n))) return out;
    for (DWORD i = 0; i < n; ++i) {
        Microsoft::WRL::ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            out.push_back(narrow(path));
            CoTaskMemFree(path);
        }
    }
    return out;
}

std::wstring band_label(float hz) {
    return hz >= 1000 ? std::format(L"{}K", std::lround(hz / 1000)) : std::format(L"{}", std::lround(hz));
}

}  // namespace

// MARK: Bar

void View::player_bar(const Rect& r) {
    bar_top_ = r.t;
    ui_.glass(r, 20, false, true);
    const Rect in = r.inset(12, 8);
    const float cy = (in.t + in.b) / 2;
    const player::Item* cur = player_.current();
    const auto row = cur && cur->track_id ? store_.row(*cur->track_id) : std::nullopt;

    // Right: volume, equalizer, visualizer, Open.
    float x = in.r;
    auto button = [&](const wchar_t* glyph, std::wstring tip, std::function<void()> fn, bool on = false) {
        x -= 32;
        ui_.icon_button(Rect::xywh(x, cy - 16, 32, 32), glyph, 14, std::move(fn), on, false, std::move(tip));
        x -= 4;
    };
    button(icon::add, L"Open files, a folder or a radio URL (Ctrl+O)", [this] { open_menu(); });
    button(icon::visualizer, L"Visualizer, full screen (F11)", [this] { set_fullscreen(true); });
    button(icon::equalizer, L"Equalizer and volume normalizer", [this] { eq_open_ = !eq_open_; },
           eq_open_ || player_.equalizer().enabled || player_.normalize());
    eq_x_ = x + 4 + 16;
    const Rect vol = Rect::xywh(x - 4 - 120, cy - 16, 120, 32);
    volume(vol);
    const float right_l = vol.l - 12;

    // Middle: previous / play / next, then the seek bar (Flutter: info 3 : seek 4 around the controls).
    const float controls_w = 124, avail = std::max(0.f, right_l - in.l - controls_w - 24);
    const float info_r = in.l + avail * 3 / 7, ctl = info_r + 12;
    transport(Rect::xywh(ctl, cy - 20, controls_w, 40));
    seek_bar(Rect{ctl + controls_w + 12, cy - 10, right_l, cy + 10});

    // Left: cover, what's playing, BPM and key.
    const Rect cover = Rect::xywh(in.l, cy - 22, 44, 44);
    if (row) {
        ui_.artwork(row->track, cover, 8);
    } else {
        g_.fill_gradient(cover, 8, {with_alpha(lilac, 0.45f), with_alpha(light_blue, 0.2f)});
        g_.icon(cur && player::is_url(cur->location) ? icon::globe : icon::music, cover.l + 22, cover.t + 22, 16, argb(0x80FFFFFF));
    }
    float text_r = info_r;
    if (row && info_r - in.l > 300) {  // room for the readouts, as in Flutter's wide bar
        text_r = info_r - 104;
        ui_.bpm_readout(text_r + 8, cy, row->bpm(), false, 15);
        ui_.key_badge(text_r + 50, cy, row->camelot());
    }
    std::wstring title = L"Nothing playing", sub = L"Play a track, or drop music here";
    if (cur) {
        const auto d = player_.display();
        title = wide(d.title);
        sub = wide(d.subtitle);
    }
    const float tx = cover.r + 10;
    const bool err = !player_.error().empty();
    const float ty = cy - (err ? 24 : sub.empty() ? 9 : 17);
    g_.text(title, Rect{tx, ty, text_r - 4, ty + 18}, {Font::ui, 13.5f, 600, cur ? text : text2});
    g_.text(sub, Rect{tx, ty + 18, text_r - 4, ty + 34}, {Font::ui, 11.5f, 400, text2});
    if (err) g_.text(wide(player_.error()), Rect{tx, ty + 34, text_r - 4, ty + 49}, {Font::ui, 11, 600, peach});
    if (row) ui_.click(Rect{in.l, in.t, text_r, in.b}, [this, id = row->id()] { focus(id); });
}

// MARK: Shared by the bar and the full-screen panel

// Previous / play / next, 124 DIPs wide, centred in r vertically.
void View::transport(const Rect& r) {
    const float ctl = r.l, cy = (r.t + r.b) / 2;
    const bool has = player_.current() != nullptr;
    ui_.icon_button(Rect::xywh(ctl, cy - 18, 36, 36), icon::previous, 14, has ? std::function<void()>([this] { player_.previous(); }) : nullptr,
                    false, false, L"Previous (or back to the start)");
    ui_.icon_button(Rect::xywh(ctl + 42, cy - 20, 40, 40), player_.playing() ? icon::pause : icon::play, 16,
                    has ? std::function<void()>([this] { player_.toggle(); }) : nullptr, false, true,
                    player_.playing() ? L"Pause (Space)" : L"Play (Space)");
    ui_.icon_button(Rect::xywh(ctl + 88, cy - 18, 36, 36), icon::next, 14,
                    has && player_.queue().has_next() ? std::function<void()>([this] { player_.next(); }) : nullptr, false, false,
                    has && !player_.queue().has_next() ? L"Next (this is the last song)" : L"Next");
}

// Elapsed time, the seek slider and the length (r is 20 DIPs high); "LIVE" and the clock for a stream. Nothing if it's
// too narrow or nothing is loaded.
void View::seek_bar(const Rect& seek) {
    if (!player_.current() || seek.w() <= 120) return;
    const float cy = (seek.t + seek.b) / 2;
    const TextStyle ts{Font::dot, 11, 700, text3};
    const int64_t len = player_.length_ms(), t = player_.time_ms();
    if (len > 0 && player_.seekable()) {
        const int64_t shown = seek_drag_ ? int64_t(double(*seek_drag_) * double(len)) : t;
        g_.text(clock_text(shown), Rect{seek.l, seek.t, seek.l + 46, seek.b}, ts);
        g_.text(clock_text(len), Rect{seek.r - 46, seek.t, seek.r, seek.b}, {Font::dot, 11, 700, text3, Align::right});
        ui_.slider(Rect{seek.l + 50, seek.t, seek.r - 50, seek.b}, seek_drag_.value_or(float(double(t) / double(len))),
                   [this, len](float v, bool done) {
                       if (!done) return void(seek_drag_ = v);  // seek on release: dragging shows where it will land
                       player_.seek(int64_t(double(v) * double(len)));
                       seek_drag_.reset();
                   });
    } else {
        // A live stream: no end, nothing to seek.
        const float w = ui_.dot_label(L"Live", seek.l, cy, lilac);
        g_.text(clock_text(t), Rect{seek.l + w + 12, seek.t, seek.r, seek.b}, ts);
    }
}

// Mute button and volume slider: 120 DIPs wide, 32 high.
void View::volume(const Rect& r) {
    const float cy = (r.t + r.b) / 2;
    ui_.icon_button(Rect::xywh(r.l, cy - 16, 32, 32), player_.muted() || player_.volume() == 0 ? icon::mute : icon::volume, 14,
                    [this] { player_.set_muted(!player_.muted()); }, false, false, player_.muted() ? L"Unmute" : L"Mute");
    const Rect vol{r.l + 36, cy - 10, r.r, cy + 10};
    ui_.slider(vol, player_.muted() ? 0.f : player_.volume(), [this](float v, bool done) {
        if (player_.muted() && v > 0) player_.set_muted(false);
        player_.set_volume(v, done);
    });
    ui_.tip(vol, player_.muted() ? L"Volume (muted)" : std::format(L"Volume {}%", std::lround(player_.volume() * 100)));
}

// MARK: Equalizer

void View::eq_panel() {
    const float W = g_.width(), H = g_.height();
    ui_.click(Rect{0, 0, W, H}, [this] { eq_open_ = false; });  // a click outside closes it
    const float pw = 600, ph = 318;
    const float l = std::clamp(eq_x_ - pw + 60, 10.f, W - pw - 10);
    const Rect p = Rect::xywh(l, bar_top_ - ph - 10, pw, ph);
    ui_.glass(p, 20, false, true);
    ui_.click(p, [] {});  // clicks on the panel stay on it
    const Rect in = p.inset(20, 18);
    const auto eq = player_.equalizer();

    ui_.dot_label(L"Equalizer", in.l, in.t + 15, text);
    float x = in.l + 112;
    x += ui_.chip(x, in.t, eq.enabled ? L"On" : L"Off", std::nullopt, eq.enabled, false, [this] {
        auto e = player_.equalizer();
        e.enabled = !e.enabled;
        if (e.bands.empty()) e.bands.assign(VlcEngine::band_frequencies().size(), 0.f);
        player_.set_equalizer(e);
    }) + 8;
    const auto names = VlcEngine::preset_names();
    const bool flat = eq.preamp == 12 && std::all_of(eq.bands.begin(), eq.bands.end(), [](float b) { return b == 0; });
    const std::wstring preset = eq.preset >= 0 && size_t(eq.preset) < names.size() ? wide(names[size_t(eq.preset)]) : flat ? L"Flat" : L"Custom";
    x += ui_.pill(x, in.t - 2, preset, icon::down, Ui::Pill::glass, [this] { preset_menu(); }) + 8;
    ui_.pill(x, in.t - 2, L"Reset", icon::undo, Ui::Pill::glass, [this] {
        player::Equalizer e;
        e.enabled = player_.equalizer().enabled;
        e.bands.assign(VlcEngine::band_frequencies().size(), 0.f);
        player_.set_equalizer(e);
    });
    const std::wstring norm = L"Normalize volume";
    const float nw = ui_.chip_width(norm) + (player_.normalize() ? 12 : 0);
    ui_.chip(in.r - nw, in.t, norm, std::nullopt, player_.normalize(), true, [this] { player_.set_normalize(!player_.normalize()); });

    // Sliders: the preamp (shown as real gain, VLC's 12 = 0 dB), then the ten bands, −20…+20 dB with 0 in the middle.
    const auto freqs = VlcEngine::band_frequencies();
    const float top = in.t + 64, bottom = in.b - 22, col = (in.w() - 24) / float(freqs.size() + 1);
    const float k = eq.enabled ? 1.f : 0.45f;
    auto column = [&](float cx, float db, const std::wstring& label, std::function<void(float, bool)> on) {
        g_.text(std::format(L"{:+.1f}", db), Rect{cx - 30, top - 26, cx + 30, top - 10}, {Font::dot, 10, 700, with_alpha(text2, 0.6f * k), Align::center});
        ui_.slider(Rect{cx - 12, top, cx + 12, bottom}, (db + 20) / 40, std::move(on), true, 0.5f);
        g_.text(label, Rect{cx - 30, bottom + 6, cx + 30, bottom + 20}, {Font::dot, 10, 700, text3, Align::center});
    };
    auto change_eq = [this](std::function<void(player::Equalizer&)> change, bool done) {
        auto e = player_.equalizer();
        if (e.bands.size() != VlcEngine::band_frequencies().size()) e.bands.assign(VlcEngine::band_frequencies().size(), 0.f);
        e.enabled = true;
        e.preset = -1;
        change(e);
        player_.set_equalizer(e, done);  // applies live while dragging; saved on release
    };
    const float snap = 0.5f;
    column(in.l + col / 2, eq.preamp - 12, L"PRE", [change_eq, snap](float v, bool done) {
        change_eq([&](player::Equalizer& e) { e.preamp = std::clamp(std::round((v * 40 - 20) / snap) * snap + 12, -20.f, 20.f); }, done);
    });
    g_.line(in.l + col + 12, top, in.l + col + 12, bottom, hairline);
    for (size_t i = 0; i < freqs.size(); ++i) {
        const float db = i < eq.bands.size() ? eq.bands[i] : 0.f;
        column(in.l + 24 + col * (float(i) + 1.5f), db, band_label(freqs[i]), [change_eq, i, snap](float v, bool done) {
            change_eq([&](player::Equalizer& e) { e.bands[i] = std::round((v * 40 - 20) / snap) * snap; }, done);
        });
    }
}

void View::preset_menu() {
    HMENU m = CreatePopupMenu();
    const auto names = VlcEngine::preset_names();
    for (size_t i = 0; i < names.size(); ++i)
        AppendMenuW(m, MF_STRING | (int(i) == player_.equalizer().preset ? MF_CHECKED : 0), i + 1, wide(names[i]).c_str());
    POINT pt;
    GetCursorPos(&pt);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
    if (cmd > 0) player_.set_equalizer(VlcEngine::preset(cmd - 1));
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// MARK: Open

void View::open_menu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Open files…\tCtrl+O");
    AppendMenuW(m, MF_STRING, 2, L"Open folder…");
    AppendMenuW(m, MF_STRING, 3, L"Open URL…\tCtrl+U");
    if (player_.active()) {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, 4, L"Stop");
    }
    POINT pt;
    GetCursorPos(&pt);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
    if (cmd == 1 || cmd == 2) play_paths(pick(hwnd_, cmd == 2));
    if (cmd == 3) url_open_ = true;
    if (cmd == 4) player_.stop();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void View::url_dialog() {
    const float W = g_.width(), H = g_.height();
    g_.fill(Rect{0, 0, W, H}, argb(0x99000000));
    ui_.click(Rect{0, 0, W, H}, [this] { url_open_ = false; });
    const Rect p = Rect::xywh((W - 540) / 2, (H - 196) / 2, 540, 196);
    ui_.glass(p, 22, false, true);
    ui_.click(p, [] {});
    const Rect in = p.inset(24, 22);
    g_.text(L"Open a stream or radio URL", Rect{in.l, in.t, in.r, in.t + 24}, {Font::ui, 18, 600, text});
    g_.text(L"Internet radio, any stream VLC plays, or a playlist link (.m3u, .pls, .xspf).", Rect{in.l, in.t + 26, in.r, in.t + 44},
            {Font::ui, 12.5f, 400, text2});
    const Rect field{in.l, in.t + 56, in.r, in.t + 96};
    constexpr COLORREF fill = RGB(30, 30, 32);
    const HWND h = edit(kUrlBox, field.inset(14, 11), false, L"https://…", "", fill);
    g_.fill_round(field, 14, argb(0xFF1E1E20));
    g_.stroke_round(field, 14, GetFocus() == h ? with_alpha(lilac, 0.6f) : hairline);
    if (GetFocus() != h && GetWindowTextLengthW(h) == 0) SetFocus(h);
    const float bw = ui_.pill_width(L"Play", icon::play), cw = ui_.pill_width(L"Cancel");
    ui_.pill(in.r - bw, in.b - 34, L"Play", icon::play, Ui::Pill::primary, [this] { open_url(); });
    ui_.pill(in.r - bw - 10 - cw, in.b - 34, L"Cancel", nullptr, Ui::Pill::glass, [this] { url_open_ = false; });
}

void View::open_url() {
    std::string url = edit_text(kUrlBox);
    url.erase(0, url.find_first_not_of(" \t"));
    url.erase(url.find_last_not_of(" \t") + 1);
    if (url.empty()) return;
    if (!player::is_url(url)) url = "http://" + url;  // "radio.example/stream" as typed
    url_open_ = false;
    SetWindowTextW(edits_[kUrlBox].hwnd, L"");
    SetFocus(hwnd_);
    play_paths({url});
}

void View::play_paths(std::vector<std::string> paths) {
    if (paths.empty()) return;
    // Searching folders, reading tags and fetching radio playlists touch the disk / network: a worker does it.
    auto items = std::make_shared<std::vector<player::Item>>();
    jobs_.run([items, paths = std::move(paths)] { *items = Player::collect(paths); },
              [this, items] {
                  if (items->empty()) player_.fail("Nothing there that WreckBox can play.");
                  else player_.play_items(std::move(*items));
              });
}

void View::drop(HDROP files) {
    std::vector<std::string> paths;
    const UINT n = DragQueryFileW(files, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < n; ++i) {
        std::wstring p(DragQueryFileW(files, i, nullptr, 0) + 1, L'\0');
        p.resize(DragQueryFileW(files, i, p.data(), UINT(p.size())));
        paths.push_back(narrow(p));
    }
    DragFinish(files);
    play_paths(std::move(paths));
}

void View::play_track(const std::string& id) {
    const player::Item* cur = player_.current();
    if (cur && cur->track_id == id) player_.toggle();
    else player_.play(id, ids_);
}

// MARK: Keys & ticks

bool View::app_command(int cmd) {
    switch (cmd) {
        case APPCOMMAND_MEDIA_PLAY_PAUSE: player_.toggle(); return true;
        case APPCOMMAND_MEDIA_PLAY:
            if (!player_.playing()) player_.toggle();
            return true;
        case APPCOMMAND_MEDIA_PAUSE:
            if (player_.playing()) player_.toggle();
            return true;
        case APPCOMMAND_MEDIA_NEXTTRACK: player_.next(); return true;
        case APPCOMMAND_MEDIA_PREVIOUSTRACK: player_.previous(); return true;
        case APPCOMMAND_MEDIA_STOP: player_.stop(); return true;
        default: return false;
    }
}

bool View::player_key(WPARAM vk) {
    const bool ctrl = GetKeyState(VK_CONTROL) & 0x8000;
    if (slsk_query_open_) {
        if (vk == VK_RETURN) slsk_query_submit();
        if (vk == VK_ESCAPE) close_slsk_query();
        return vk == VK_RETURN || vk == VK_ESCAPE;
    }
    if (bug_open_) {  // the dialog's text boxes take Esc themselves; this is for a click on the page first
        if (vk == VK_ESCAPE) close_bug_report();
        return vk == VK_ESCAPE;
    }
    if (url_open_) {
        if (vk == VK_RETURN) open_url();
        if (vk == VK_ESCAPE) url_open_ = false;
        return vk == VK_RETURN || vk == VK_ESCAPE;
    }
    if (vk == VK_ESCAPE && eq_open_) {
        eq_open_ = false;
        return true;
    }
    if (ctrl && vk == 'O') return play_paths(pick(hwnd_, false)), true;
    if (ctrl && vk == 'U') {
        url_open_ = true;
        return true;
    }
    if (vk == VK_SPACE && player_.active()) return player_.toggle(), true;
    if (vk == VK_F11) return set_fullscreen(true), true;
    return false;
}

void View::update_timer() {
    UINT want = 0;
    // Full screen and moving: no timer. The window paints again after each frame, and in full screen each frame waits for
    // the display's refresh (Gfx::set_vsync), so frames land on it: 60 fps, or 30 when a frame takes longer than one
    // refresh. (WM_TIMER ticks every 15.6 ms, which judders against 60 Hz.) Paused and settled: a still frame, nothing.
    vsync_loop_ = fullscreen_ && (player_.playing() || vis_settling_);
    if (!vsync_loop_ && player_.playing()) want = 250;  // the seek bar's clock
    if (want == timer_ms_) return;
    if (want) SetTimer(hwnd_, kPlayerTimer, want, nullptr);
    else KillTimer(hwnd_, kPlayerTimer);
    timer_ms_ = want;
}

}  // namespace wb::ui
