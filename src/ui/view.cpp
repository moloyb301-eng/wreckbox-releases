// Shell, sidebar, headers, Home and the placeholder pages. Track lists and the inspector are in view_tracks.cpp.
#include "ui/view.h"
#include "model/paths.h"
#include "model/settings.h"

#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <numeric>

#include "../res/resource.h"

namespace wb::ui {

using namespace theme;

namespace {

// Keys inside a text box that belong to the app: Tab moves between fields, Esc leaves the box, and in the search box
// ↑/↓ move through the list and F5 rescans.
LRESULT CALLBACK edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    const int id = GetDlgCtrlID(h);
    const bool in_bug_report = id == kBugTitle || id == kBugBody || id == kSlskQuery;  // dialogs that Esc closes
    if (msg == WM_KEYDOWN) {
        if (wp == VK_TAB) {
            PostMessageW(GetParent(h), WM_APP_TAB, WPARAM(id), (GetKeyState(VK_SHIFT) & 0x8000) ? 1 : 0);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            SetFocus(GetParent(h));
            if (id == kUrlBox || in_bug_report) SendMessageW(GetParent(h), WM_KEYDOWN, wp, lp);  // closes the dialog too
            return 0;
        }
        if ((id == kUrlBox || id == kSlskQuery) && wp == VK_RETURN) {
            SendMessageW(GetParent(h), WM_KEYDOWN, wp, lp);
            return 0;
        }
        if (id == kSearchBox && (wp == VK_UP || wp == VK_DOWN || wp == VK_F5)) {
            SendMessageW(GetParent(h), WM_KEYDOWN, wp, lp);
            return 0;
        }
    }
    if (msg == WM_CHAR && (wp == VK_TAB || wp == VK_ESCAPE || (wp == VK_RETURN && id != kBugBody))) return 0;  // no beep (Enter is a new line in the bug text)
    return DefSubclassProc(h, msg, wp, lp);
}

}  // namespace

View::View(HWND hwnd, LibraryStore& store, Jobs& jobs, Ui& ui, player::Player& player)
    : hwnd_(hwnd), store_(store), jobs_(jobs), ui_(ui), g_(ui.g), player_(player) {
    // GDI needs its own copy of Urbanist for the text boxes (DirectWrite's private collection isn't visible to GDI).
    if (HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_FONT_URBANIST), MAKEINTRESOURCEW(10))) {
        DWORD n = 0;
        font_handle_ = AddFontMemResourceEx(LockResource(LoadResource(nullptr, res)), SizeofResource(nullptr, res), nullptr, &n);
    }
    dpi_changed();
    search_ = edit(kSearchBox, Rect{}, false, L"Search artist, title, album");
    vis_.opt = player::VisOptions::from_json(Settings::current().extra.value("visualizer", json::object()));
    wall_restart();  // the live wallpaper, if it's on
}

View::~View() {
    if (edit_font_) DeleteObject(edit_font_);
    for (const auto& [c, b] : edit_brushes_) DeleteObject(b);
    if (font_handle_) RemoveFontMemResourceEx(font_handle_);
}

void View::dpi_changed() {
    if (edit_font_) DeleteObject(edit_font_);
    edit_font_ = CreateFontW(-MulDiv(13, int(g_.dpi()), 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Urbanist");
    for (auto& [id, e] : edits_) {
        SendMessageW(e.hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(edit_font_), TRUE);
        e.px = {};
    }
}

HBRUSH View::edit_colors(HDC dc, HWND box) {
    COLORREF fill = RGB(20, 20, 22);
    for (const auto& [id, e] : edits_)
        if (e.hwnd == box) fill = e.bg;
    SetTextColor(dc, RGB(240, 240, 240));
    SetBkColor(dc, fill);
    HBRUSH& b = edit_brushes_[fill];
    if (!b) b = CreateSolidBrush(fill);
    return b;
}

HWND View::edit(int id, const Rect& r, bool password, const wchar_t* cue, const std::string& initial, COLORREF fill, bool multiline) {
    Edit& e = edits_[id];
    e.bg = fill;
    if (!e.hwnd) {
        e.hwnd = CreateWindowExW(0, L"EDIT", widen(initial).c_str(), WS_CHILD | (multiline ? ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN : ES_AUTOHSCROLL) | (password ? ES_PASSWORD : 0), 0, 0, 10, 10, hwnd_,
                                 reinterpret_cast<HMENU>(INT_PTR(id)), nullptr, nullptr);
        SendMessageW(e.hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(edit_font_), FALSE);
        SendMessageW(e.hwnd, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue));
        SetWindowSubclass(e.hwnd, edit_proc, 1, 0);
        return e.hwnd;  // created hidden; placed on the next paint
    }
    if (r.w() <= 0) return e.hwnd;
    e.used = true;
    const float k = g_.dpi() / 96.f;
    const RECT px{LONG(r.l * k), LONG(r.t * k), LONG(r.r * k), LONG(r.b * k)};
    if (!EqualRect(&px, &e.px)) {
        MoveWindow(e.hwnd, px.left, px.top, px.right - px.left, px.bottom - px.top, TRUE);
        e.px = px;
    }
    if (!IsWindowVisible(e.hwnd)) ShowWindow(e.hwnd, SW_SHOWNA);
    return e.hwnd;
}

std::string View::edit_text(int id) const {
    const auto it = edits_.find(id);
    if (it == edits_.end()) return "";
    std::wstring buf(size_t(GetWindowTextLengthW(it->second.hwnd)) + 1, L'\0');
    buf.resize(size_t(GetWindowTextW(it->second.hwnd, buf.data(), int(buf.size()))));
    return narrow(buf);
}

void View::hide_unused_edits() {
    for (auto& [id, e] : edits_) {
        if (!e.used && IsWindowVisible(e.hwnd)) {
            if (GetFocus() == e.hwnd) SetFocus(hwnd_);
            ShowWindow(e.hwnd, SW_HIDE);
        }
        e.used = false;
    }
}

void View::tab(int from_id, bool back) {
    std::vector<int> visible;
    for (const auto& [id, e] : edits_)
        if (IsWindowVisible(e.hwnd)) visible.push_back(id);
    if (visible.empty()) return;
    auto at = std::find(visible.begin(), visible.end(), from_id);
    const size_t i = at == visible.end() ? 0 : size_t(at - visible.begin());
    const size_t next = back ? (i + visible.size() - 1) % visible.size() : (i + 1) % visible.size();
    SetFocus(edits_[visible[next]].hwnd);
    SendMessageW(edits_[visible[next]].hwnd, EM_SETSEL, 0, -1);
}

void View::search_changed() {
    wchar_t buf[256]{};
    GetWindowTextW(search_, buf, 256);
    search_text_ = buf;
    list_scroll_ = 0;
    dirty_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void View::set_status(const std::string& s) {
    {
        std::lock_guard lock(status_m_);
        status_ = s;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);  // safe from any thread
}

std::string View::status() const {
    std::lock_guard lock(status_m_);
    return status_;
}

// MARK: Paint

void View::paint() {
    // While a long job reports progress, the store changes many times a second; rebuilding lists that often would
    // waste the CPU the job needs, so refresh at most every 750 ms then (a timer brings the final state in).
    if (dirty_) {
        if (!store_.busy() || GetTickCount64() - refreshed_at_ > 750) refresh();
        else SetTimer(hwnd_, kRefreshTimer, 800, nullptr);
    }
    log_memory();
    const float W = g_.width(), H = g_.height();
    ui_.begin_frame();
    ui_.motion = motion_now() != Motion::off;
    if (fullscreen_) {
        visualizer_screen();
        web_sync();
        draw_toast();
        if (const UINT due = ui_.draw_tooltip()) SetTimer(hwnd_, kTooltipTimer, due + 10, nullptr);
        hide_unused_edits();
        update_timer();
        return;
    }

    backdrop(W, H);
    // The record on the logo and in the player bar turns while music plays (33⅓ rpm), easing up to speed and down.
    const float turning = ui_.anim(Ui::key("record-speed"), player_.playing() && motion_now() == Motion::full ? 1.f : 0.f, 2.5f);
    if (turning > 0.001f) {
        spin_ = std::fmod(spin_ + 200.f * turning * ui_.dt, 360.f);
        backdrop_wants_frames_ = true;
    }

    sidebar(Rect{10, 10, 252, H - 10});
    const auto focus = store_.focus();
    const bool tools = page_ == Page::web || page_ == Page::queue || page_ == Page::soulseek || page_ == Page::phone || page_ == Page::settings;
    const bool inspect = focus && !tools && lib_;
    const bool overlay = W < 1320;
    // The page column: the page, and the player bar under it (original: Column(page, PlayerBar)).
    const float column_r = inspect && !overlay ? W - 340 : W, bar_h = 66;
    const bool banner = update_visible();
    if (banner) update_banner(Rect{252 + 22, 10, column_r - 10, 54});
    {
        // A page arrives: it fades in and settles upward into place.
        const float in = ui_.enter(arrive("page"), 0, 0.30f);
        if (in < 1) {
            g_.push_opacity(in);
            g_.push_transform(D2D1::Matrix3x2F::Translation(0, (1 - in) * 16));
        }
        page(Rect{252 + 22, banner ? 56.f : 0.f, column_r - 22, H - 10 - bar_h - 10});
        if (in < 1) {
            g_.pop_transform();
            g_.pop_opacity();
        }
    }
    player_bar(Rect{252 + 22, H - 10 - bar_h, column_r - 10, H - 10});
    web_sync();
    if (inspect) inspector(Rect{W - 340, 10, W - 10, H - 10}, overlay);
    if (eq_open_ || url_open_ || bug_open_ || slsk_query_open_) {
        // Native text boxes would sit on top of a pop-over: hide them while one is open (the dialogs' own boxes excepted).
        for (auto& [id, e] : edits_)
            if (id != kUrlBox && id != kBugTitle && id != kBugBody && id != kSlskQuery) e.used = false;
        if (eq_open_) eq_panel();
        if (url_open_) url_dialog();
        if (bug_open_) bug_dialog();
        if (slsk_query_open_) slsk_query_dialog();
    }
    draw_toast();
    if (const UINT due = ui_.draw_tooltip()) SetTimer(hwnd_, kTooltipTimer, due + 10, nullptr);
    hide_unused_edits();
    update_timer();
    schedule_motion();
}

// MARK: Sidebar

void View::sidebar(const Rect& r) {
    ui_.glass(r);
    {
        // The logo: its tile, and on it the record, which turns while music plays.
        const Rect tile = Rect::xywh(r.l + 18, r.t + 24, 34, 34);
        g_.fill_gradient(tile, 34 * 0.225f, {argb(0xFFF1ECE4), argb(0xFFDCD5CA)});
        const float rs = 34 * 0.78f, rx = tile.l + (34 - rs) / 2, ry = tile.t + (34 - rs) / 2;
        g_.push_transform(D2D1::Matrix3x2F::Rotation(spin_, {tile.l + 17, tile.t + 17}));
        ui_.logo(rx, ry, rs, false);
        g_.pop_transform();
    }
    g_.text(L"WRECKBOX", Rect{r.l + 62, r.t + 24, r.r, r.t + 58}, {Font::dot, 17, 700, text, Align::left, 2});

    const float vh = video_status(Rect{r.l + 8, r.b - 52 - 40, r.r - 8, r.b - 52});  // a video being made: above "Report a bug"
    const Rect area{r.l + 8, r.t + 72, r.r - 8, r.b - 52 - vh};
    // Report a bug: pinned to the bottom of the sidebar, under the scrolling list.
    {
        const Rect br{r.l + 8, r.b - 46, r.r - 8, r.b - 12};
        if (ui_.hover(br)) g_.fill_round(br, 10, theme::hover);
        g_.icon(icon::bug, br.l + 18, br.t + 17, 15, peach);
        g_.text(L"Report a bug", Rect{br.l + 36, br.t, br.r, br.b}, {Font::ui, 13.5f, 500, peach});
        ui_.click(br, [this] { open_bug_report(); });
    }
    ui_.push_clip(area);
    float y = area.t - side_scroll_;
    uint64_t n = 0;
    auto item = [&](Page p, const std::wstring& title, const wchar_t* glyph, std::optional<size_t> count,
                    std::optional<std::string> pl = std::nullopt) {
        const Rect ir{area.l, y, area.r, y + 34};
        const bool is_selected = page_ == p && playlist_ == pl;
        const float lit = ui_.anim(Ui::key("side-item", n++), !is_selected && ui_.hover(ir) ? 1.f : 0.f, 18);
        if (is_selected) {
            // The marker glides from the item that was selected to this one, rather than jumping.
            const float gy = ui_.anim(Ui::key("side-marker"), ir.t + side_scroll_, 20) - side_scroll_;
            const Rect m{ir.l, gy, ir.r, gy + 34};
            g_.fill_gradient(m, 10, {with_alpha(lilac, 0.26f), with_alpha(light_blue, 0.10f), argb(0x12FFFFFF)});
            g_.stroke_gradient(m, 10, {with_alpha(lilac, 0.55f), argb(0x18FFFFFF)}, 1);
            g_.fill_glow(m.l + 2, gy + 17, 26, with_alpha(lilac, 0.45f));
            g_.fill_gradient(Rect{m.l, gy + 8, m.l + 3, gy + 26}, 1.5f, {light_blue, lilac, peach}, true);
        } else if (lit > 0.01f) {
            g_.fill_round(ir, 10, with_alpha(white, 0.06f * lit));
        }
        g_.icon(glyph, ir.l + 18 + 2 * lit, ir.t + 17, 15, is_selected ? lilac : with_alpha(white, 0.36f + 0.4f * lit));
        float right = ir.r - 10;
        if (count) {
            const std::wstring c = std::to_wstring(*count);
            const TextStyle st{Font::dot, 11, 700, text3};
            const float cw = g_.measure(c, st);
            g_.text(c, Rect{right - cw, ir.t, right, ir.b}, st);
            right -= cw + 8;
        }
        g_.text(title, Rect{ir.l + 36 + 2 * lit, ir.t, right, ir.b},
                {Font::ui, 13.5f, is_selected ? 600 : 500, is_selected ? text : with_alpha(white, 0.60f + 0.34f * lit)});
        ui_.click(ir, [this, p, pl] { go(p, pl); });
        y += 36;
    };
    auto section = [&](const wchar_t* s) {
        y += 12;
        ui_.dot_label(s, area.l + 12, y + 8);
        y += 24;
    };
    item(Page::home, L"Home", icon::home, std::nullopt);
    item(Page::web, L"Web player", icon::globe, std::nullopt);
    section(L"Library");
    item(Page::all, L"All tracks", icon::list, lib_ ? std::optional(lib_->tracks.size()) : std::nullopt);
    item(Page::downloaded, L"In my crate", icon::check_circle, n_downloaded_);
    item(Page::missing, L"Missing", icon::circle, n_missing_);
    item(Page::ignored, L"Ignored", icon::block, n_ignored_);
    item(Page::on_pc, L"My folders", icon::folder, n_on_pc_);
    section(L"Playlists");
    if (lib_)
        for (const auto& p : lib_->playlists) item(Page::playlist, wide(p.name), p.collaborative ? icon::people : icon::music, p.track_ids.size(), p.name);
    section(L"Tools");
    item(Page::queue, L"Download queue", icon::queue, n_priority_ ? std::optional(n_priority_) : std::nullopt);
    item(Page::soulseek, L"Soulseek sync", icon::download, std::nullopt);
    item(Page::phone, L"Sync to phone", icon::phone, std::nullopt);
    item(Page::settings, L"Settings", icon::settings, std::nullopt);
    ui_.pop_clip();
    ui_.scroll_area(area, side_scroll_, y + side_scroll_ - area.t + 8);
}

// MARK: Header

float View::header(const Rect& r, const std::wstring& eyebrow, const std::wstring& title, const std::wstring& subtitle,
                   const std::vector<Action>& actions) {
    float y = r.t + 34;
    ui_.dot_label(eyebrow, r.l, y + 7);
    y += 20;
    float right = r.r;
    for (auto it = actions.rbegin(); it != actions.rend(); ++it) right -= it->w + 10;
    g_.text(title, Rect{r.l, y, right, y + 42}, {Font::ui, 34, 500, text});
    y += 42;
    if (!subtitle.empty()) {
        g_.text(subtitle, Rect{r.l, y, right, y + 18}, {Font::ui, 13, 400, text2});
        y += 18;
    }
    float x = right + 10;
    for (const auto& a : actions) {  // bottom-aligned with the title block, like the original Row(crossAxisAlignment.end)
        a.draw(x, y - 36);
        x += a.w + 10;
    }
    return y + 16 - r.t;
}

std::vector<View::Action> View::library_actions(bool write_tags) {
    std::vector<Action> out;
    const auto busy = store_.busy();
    if (write_tags) {
        const std::wstring label = L"Write tags to files";
        out.push_back({ui_.pill_width(label, icon::tag), [this, label, busy](float x, float y) {
                           ui_.pill(x, y, label, icon::tag, Ui::Pill::smart,
                                    busy ? std::function<void()>{} : [this] { jobs_.run([this] { store_.write_tags(); }); });
                       }});
    }
    if (busy) {
        const std::wstring b = wide(*busy);
        out.push_back({g_.measure(b, {Font::ui, 12, 400}), [this, b](float x, float y) {
                           g_.text(b, Rect{x, y, x + 400, y + 34}, {Font::ui, 12, 400, text2});
                       }});
    }
    const std::wstring label = L"Rescan & analyse";
    out.push_back({ui_.pill_width(label, icon::scan), [this, label, busy](float x, float y) {
                       ui_.pill(x, y, label, icon::scan, Ui::Pill::glass,
                                busy ? std::function<void()>{} : [this] { jobs_.run([this] { store_.rescan(); }); });
                   }});
    return out;
}

// MARK: Pages

void View::page(const Rect& r) {
    if (!lib_ && loading_) {
        g_.text(L"Loading your library…", r, {Font::ui, 14, 400, text2, Align::center});
        return;
    }
    if (const auto err = store_.load_error()) {
        // Shown on every page until fixed: the store refuses to save state it couldn't read.
        g_.fill_round(Rect{r.l, r.t + 10, r.r, r.t + 40}, 10, with_alpha(peach, 0.12f));
        g_.text(wide(*err), Rect{r.l + 12, r.t + 10, r.r - 12, r.t + 40}, {Font::ui, 12.5f, 600, peach});
    }
    if (!lib_ && page_ != Page::settings) return welcome(r);
    switch (page_) {
        case Page::home: return home(r);
        case Page::web: return web_page(r);
        case Page::settings: return settings_page(r);
        case Page::soulseek: return soulseek_page(r);
        case Page::phone: return phone_page(r);
        case Page::queue: return queue_page(r);
        default: return track_page(r);
    }
}

void View::welcome(const Rect& r) {
    const float cx = (r.l + r.r) / 2, cy = (r.t + r.b) / 2;
    ui_.logo(cx - 48, cy - 150, 96);
    g_.text(L"Welcome to WreckBox", Rect{r.l, cy - 36, r.r, cy}, {Font::ui, 28, 500, text, Align::center});
    g_.text(L"Start by importing your Spotify and YouTube playlists (CSV files).", Rect{r.l, cy + 4, r.r, cy + 26},
            {Font::ui, 14, 400, text2, Align::center});
    const std::wstring label = L"Set up";
    const float w = ui_.pill_width(label, icon::arrow_right);
    ui_.pill(cx - w / 2, cy + 44, label, icon::arrow_right, Ui::Pill::primary, [this] { go(Page::settings); });
}

// MARK: Web player

void View::web_page(const Rect& r) {
    if (!web_) {
        web_ = std::make_unique<WebPlayer>(hwnd_, paths::settings_dir() / L"web");
        web_->on_playing = [this](WebPlayer::Site) {
            if (player_.playing()) player_.toggle();  // the site plays: the app pauses
        };
        web_->on_changed = [this] { InvalidateRect(hwnd_, nullptr, FALSE); };
    }
    std::vector<Action> actions;
    for (const auto site : {WebPlayer::spotify, WebPlayer::youtube}) {
        const std::wstring label = web_->playing(site) ? std::wstring(WebPlayer::name(site)) + L" \u266A" : WebPlayer::name(site);
        actions.push_back({ui_.chip_width(label) + 12, [this, label, site](float x, float y) {
                               ui_.chip(x, y + 6, label, std::nullopt, web_site_ == site, false, [this, site] { web_site_ = site; });
                           }});
    }
    const std::wstring reload = L"Reload";
    actions.push_back({ui_.pill_width(reload, icon::sync), [this, reload](float x, float y) {
                           ui_.pill(x, y, reload, icon::sync, Ui::Pill::glass, [this] { web_->reload(web_site_); });
                       }});
    const float h = header(r, L"Listen", L"Web player", L"Spotify and YouTube Music's own players. Sign in once; it remembers you.", actions);
    const Rect box{r.l, r.t + h, r.r, r.b};
    ui_.glass(box, 16);
    const std::wstring& err = web_->error();
    if (!err.empty()) {
        g_.paragraph(err, box.inset(24, 24), {Font::ui, 14, 400, text2});
        web_->hide();
        return;
    }
    if (web_->loading(web_site_))
        g_.text(L"Opening " + std::wstring(WebPlayer::name(web_site_)) + L"…", box, {Font::ui, 14, 400, text2, Align::center});
    if (eq_open_ || url_open_ || bug_open_ || slsk_query_open_) return web_->hide();  // a pop-over sits on top of it
    const float k = g_.dpi() / 96.f;
    const Rect in = box.inset(6, 6);
    web_->show(web_site_, RECT{LONG(in.l * k), LONG(in.t * k), LONG(in.r * k), LONG(in.b * k)});
}

void View::web_sync() {
    if (!web_) return;
    const bool playing = player_.playing();
    if (playing && !was_playing_) web_->pause();  // the app plays: the sites pause
    was_playing_ = playing;
    web_->set_muted(player_.muted());
    if (page_ != Page::web || fullscreen_) web_->hide();
}

void View::placeholder(const Rect& r, const std::wstring& title, const std::wstring& subtitle, const std::wstring& phase) {
    const float h = header(r, L"Tools", title, subtitle);
    const Rect box{r.l, r.t + h, r.r, r.t + h + 110};
    ui_.glass(box);
    g_.text(L"Not built yet", Rect{box.l + 22, box.t + 22, box.r - 22, box.t + 48}, {Font::ui, 17, 600, text});
    g_.text(L"This screen arrives in " + phase + L" of the plan (docs/PLAN.md). The original build still has it meanwhile.",
            Rect{box.l + 22, box.t + 52, box.r - 22, box.t + 80}, {Font::ui, 13, 400, text2});
}

void View::home(const Rect& r) {
    ui_.push_clip(Rect{r.l - 22, r.t, r.r + 22, r.b});  // headers sit flush; shadows of cards may overhang slightly
    const float top = r.t - home_scroll_;
    const size_t total = lib_ ? lib_->tracks.size() : 0;
    float y = top + header(Rect{r.l, top, r.r, r.b}, L"Home", L"Your crate",
                           std::format(L"{} tracks from {} Spotify playlists", total, lib_ ? lib_->playlists.size() : 0), library_actions(false));

    // The top of the page: the cover flow on the left, four stat tiles (two by two) on the right.
    const float gap = 12, hero_h = 300, fw = std::floor(r.w() * 0.58f);
    const float tw = (r.w() - fw - 2 * gap) / 2, th = (hero_h - gap) / 2;
    // Something arriving: it fades in and rises into place, `delay` seconds after the page did. Returns how far in it
    // is; call landed() after drawing it.
    auto arriving = [&](std::string_view what, uint64_t i, float delay) {
        const float in = ui_.enter(arrive(what, i), delay, 0.5f);
        if (in < 1) {
            g_.push_opacity(in);
            g_.push_transform(D2D1::Matrix3x2F::Translation(0, (1 - in) * 22));
        }
        return in;
    };
    auto landed = [&](float in) {
        if (in < 1) {
            g_.pop_transform();
            g_.pop_opacity();
        }
    };
    // A card rises a little toward the pointer when it's lit.
    auto lift = [&](const Rect& c, float lit) {
        g_.push_transform(D2D1::Matrix3x2F::Scale(1 + 0.02f * lit, 1 + 0.02f * lit, {(c.l + c.r) / 2, (c.t + c.b) / 2}) *
                          D2D1::Matrix3x2F::Translation(0, -4 * lit));
    };

    auto stat = [&](int i, const std::wstring& label, size_t value, const std::wstring& detail, std::optional<double> share, D2D1_COLOR_F tint) {
        const Rect t = Rect::xywh(r.l + fw + gap + float(i % 2) * (tw + gap), y + float(i / 2) * (th + gap), tw, th);
        const float in = arriving("stat", uint64_t(i), 0.12f + 0.05f * float(i));
        const float lit = ui_.anim(Ui::key("stat-lit", uint64_t(i)), ui_.hover(t) ? 1.f : 0.f, 14);
        lift(t, lit);
        ui_.card(t, 20, Ui::key("stat-card", uint64_t(i)));
        // A light of the tile's own colour in its corner, brighter when lit.
        ui_.push_clip(t.inset(1));
        g_.fill_glow(t.r - 26, t.t + 18, 120, with_alpha(tint, 0.16f + 0.14f * lit));
        ui_.pop_clip();
        ui_.dot_label(label, t.l + 18, t.t + 26);
        // The number counts up as the tile arrives.
        const float counted = ui_.enter(arrive("stat-count", uint64_t(i)), 0.1f + 0.05f * float(i), 0.9f);
        g_.text(std::to_wstring(size_t(std::llround(double(value) * double(counted)))), Rect{t.l + 18, t.t + 40, t.r - 18, t.t + 82},
                {Font::dot, 34, 700, text});
        if (share) {
            // How much of the library it is, as a ring that fills.
            const float filled = ui_.enter(arrive("stat-ring", uint64_t(i)), 0.25f, 1.1f);
            ui_.ring(t.r - 44, t.t + 62, 22, *share * double(filled), 5);
            g_.text(std::format(L"{}%", std::lround(*share * 100)), Rect{t.r - 66, t.t + 52, t.r - 22, t.t + 72}, {Font::dot, 11, 700, text2, Align::center});
        }
        g_.text(detail, Rect{t.l + 18, t.t + 96, t.r - 18, t.t + 114}, {Font::ui, 12, 400, text2});
        g_.pop_transform();
        landed(in);
    };
    const double of_library = total ? 1.0 / double(total) : 0;
    stat(0, L"Tracks", total, L"across all playlists", std::nullopt, light_blue);
    stat(1, L"In crate", n_downloaded_, total ? std::format(L"{}% of your library", (n_downloaded_ * 100 + total / 2) / total) : L"",
         std::optional(double(n_downloaded_) * of_library), lilac);
    stat(2, L"Missing", n_missing_, std::format(L"{} ignored", n_ignored_), std::nullopt, peach);
    stat(3, L"Analysed", n_analysed_, L"BPM · key · energy", std::optional(double(n_analysed_) * of_library), light_blue);

    // The cover flow: the songs added most recently, as cards standing in a row in space. The one in the middle faces
    // you; the others turn away to each side and overlap, like records in a crate. Click one to bring it forward.
    {
        const Rect flow{r.l, y, r.l + fw, y + hero_h};
        const float in = arriving("flow", 0, 0.04f);
        ui_.glass(flow, 24);
        ui_.dot_label(L"Recently added", flow.l + 20, flow.t + 24, text2, 12);
        std::vector<LibraryTrack> items;
        for (const auto& id : recent_) {
            if (items.size() == 15) break;
            if (auto t = store_.track(id)) items.push_back(std::move(*t));
        }
        const int n = int(items.size());
        if (n) {
            flow_sel_ = std::clamp(flow_sel_, 0, n - 1);
            const float pos = ui_.anim(Ui::key("flow-pos"), float(flow_sel_), 8);
            const float fcx = (flow.l + flow.r) / 2, fcy = flow.t + 128, size = 164;
            ui_.push_clip(flow.inset(1));
            g_.fill_glow(fcx, fcy + size * 0.66f, size * 1.7f, with_alpha(lilac, 0.20f));  // light on the floor under the middle one
            std::vector<int> order(size_t(n), 0);
            std::iota(order.begin(), order.end(), 0);
            std::sort(order.begin(), order.end(), [&](int a, int b) { return std::abs(float(a) - pos) > std::abs(float(b) - pos); });  // far ones first
            for (const int i : order) {
                const float d = float(i) - pos, a = std::abs(d);
                if (a > 5.2f) continue;
                const float fore = 1 - std::min(a, 1.f);                      // 1 in the middle, 0 from the first neighbour out
                const float off = a < 1 ? a * 132 : 132 + (a - 1) * 54;       // the neighbours stand close together
                const float s = size * (0.84f + 0.16f * fore), cx = fcx + (d < 0 ? -off : off);
                const Rect c = Rect::xywh(cx - s / 2, fcy - s / 2, s, s);
                D2D1_POINT_2F tilt{0, std::clamp(d, -1.f, 1.f) * 62};
                if (fore > 0.6f) {  // the one in front also leans toward the pointer
                    const auto lean = ui_.lean(c);
                    tilt.x += ui_.anim(Ui::key("flow-lean-x"), -lean.y * 11, 10) * fore;
                    tilt.y += ui_.anim(Ui::key("flow-lean-y"), lean.x * 11, 10) * fore;
                    g_.shadow(c.inset(10), 14, 26 * fore, 0.85f * fore);
                }
                const float seen = std::clamp(5.2f - a, 0.f, 1.f) * (0.50f + 0.50f * fore);
                ui_.artwork_3d(items[size_t(i)], c, 14, tilt, seen, 2.6f);
                if (fore > 0.6f) {
                    if (ui_.hover(c)) {  // a light crosses the front cover with the pointer
                        ui_.push_clip(c);
                        g_.fill_glow(ui_.mx, ui_.my, 120, with_alpha(white, 0.14f));
                        ui_.pop_clip();
                    }
                    ui_.click(c, [this, id = items[size_t(i)].id] { focus(id); });
                } else {
                    ui_.click(Rect::xywh(cx - s * 0.30f, c.t, s * 0.60f, s), [this, i] { flow_sel_ = i; });
                }
            }
            ui_.pop_clip();

            // Under it: the song in front (its name changes with a quick fade), a play button if it's on this
            // computer, the way to the next and the one before, and where you are in the row.
            const LibraryTrack& front = items[size_t(flow_sel_)];
            const float named = ui_.enter(arrive("flow-name", uint64_t(flow_sel_) + 1), 0, 0.28f);
            g_.push_opacity(named);
            g_.text(wide(front.title), Rect{flow.l + 70, fcy + size / 2 + 12, flow.r - 70, fcy + size / 2 + 34}, {Font::ui, 15.5f, 600, text, Align::center});
            g_.text(wide(front.artist()), Rect{flow.l + 70, fcy + size / 2 + 34, flow.r - 70, fcy + size / 2 + 52}, {Font::ui, 12, 400, text2, Align::center});
            g_.pop_opacity();
            if (player_.can_play(front.id)) {
                const Rect pb = Rect::xywh(fcx + size / 2 - 48, fcy + size / 2 - 48, 38, 38);
                ui_.icon_button(pb, icon::play, 14, [this, id = front.id] { play_track(id); }, false, true, L"Play");
            }
            ui_.icon_button(Rect::xywh(flow.l + 16, fcy - 18, 36, 36), icon::chevron_left, 13,
                            flow_sel_ > 0 ? std::function<void()>([this] { --flow_sel_; }) : nullptr, false, false, L"Newer");
            ui_.icon_button(Rect::xywh(flow.r - 52, fcy - 18, 36, 36), icon::chevron_right, 13,
                            flow_sel_ < n - 1 ? std::function<void()>([this] { ++flow_sel_; }) : nullptr, false, false, L"Older");
            const float dots_w = float(n) * 10 - 4, dy = flow.b - 18;
            const float mark = ui_.anim(Ui::key("flow-dot"), float(flow_sel_), 10);
            for (int i = 0; i < n; ++i) g_.fill_circle(fcx - dots_w / 2 + 3 + float(i) * 10, dy, 2, with_alpha(white, 0.18f));
            g_.fill_round(Rect::xywh(fcx - dots_w / 2 + mark * 10 - 3, dy - 3, 12, 6), 3, lilac);
        } else {
            g_.text(L"Songs you add will stand here.", flow, {Font::ui, 13, 400, text3, Align::center});
        }
        landed(in);
    }
    y += hero_h + 22;

    // Playlists: each card carries a small fan of its covers, which opens when the card is lit.
    ui_.dot_label(L"Playlists", r.l, y + 7, text2, 12);
    y += 26;
    float x = r.l;
    if (lib_) {
        const float cw = 262, ch = 88;
        uint64_t i = 0;
        for (const auto& p : lib_->playlists) {
            if (x + cw > r.r + 1) {
                x = r.l;
                y += ch + 14;
            }
            const Rect card = Rect::xywh(x, y, cw, ch);
            const float in = arriving("playlist", i, 0.26f + 0.035f * float(std::min<uint64_t>(i, 12)));
            const float lit = ui_.anim(Ui::key("playlist-lit", i), ui_.hover(card) ? 1.f : 0.f, 14);
            lift(card, lit);
            ui_.card(card, 20, Ui::key("playlist-card", i));
            // Up to three covers from the playlist, fanned like cards in a hand.
            {
                std::vector<LibraryTrack> covers;
                for (const auto& id : p.track_ids) {
                    if (covers.size() == 3) break;
                    if (auto t = store_.track(id)) covers.push_back(std::move(*t));
                }
                const float px0 = x + 44, py0 = y + ch / 2 + 2;
                for (int j = int(covers.size()) - 1; j >= 0; --j) {
                    const float spread = float(j) * (6 + 5 * lit);
                    g_.push_transform(D2D1::Matrix3x2F::Rotation(spread * 1.6f, {px0, py0 + 34}) * D2D1::Matrix3x2F::Translation(spread * 0.9f, 0));
                    const Rect c = Rect::xywh(px0 - 24, py0 - 26, 48, 48);
                    g_.shadow(c, 8, 5, 0.6f);
                    ui_.artwork(covers[size_t(j)], c, 8, j == 0 ? 1.f : 0.85f - 0.15f * float(j));
                    g_.pop_transform();
                }
                if (covers.empty()) g_.icon(p.collaborative ? icon::people : icon::music, px0, py0, 20, text3);
            }
            const float tl = x + 108;
            g_.text(wide(p.name), Rect{tl, y + 14, x + cw - 14 - 18 * lit, y + 34}, {Font::ui, 14, 600, text});
            if (lit > 0.01f) g_.icon(icon::arrow_right, x + cw - 22 - 6 * (1 - lit), y + 24, 12, with_alpha(lilac, lit));  // "open": slides in
            const size_t n = p.track_ids.size(), have = store_.downloaded_in(p);
            g_.text(n == 1 ? L"1 track" : std::format(L"{} tracks", n), Rect{tl, y + 38, tl + 90, y + 54}, {Font::ui, 11.5f, 400, text2});
            g_.text(std::format(L"{}/{}", have, n), Rect{tl + 40, y + 38, x + cw - 14, y + 54}, {Font::dot, 11, 700, text3, Align::right});
            ui_.progress(Rect{tl, y + 66, x + cw - 14, y + 69}, n ? double(have) / double(n) : 0, arrive("playlist-bar", i));
            g_.pop_transform();
            landed(in);
            ui_.click(card, [this, name = p.name] { go(Page::playlist, name); });
            x += cw + 14;
            ++i;
        }
        y += ch;
    }
    y += 30;
    ui_.pop_clip();
    const float before = home_scroll_;
    ui_.scroll_area(r, home_scroll_, y - top);
    if (home_scroll_ != before) InvalidateRect(hwnd_, nullptr, FALSE);  // clamped: draw again at the valid offset
}

// MARK: Behaviour

void View::go(Page p, std::optional<std::string> playlist) {
    const bool same_list = page_ == p && playlist_ == playlist;
    page_ = p;
    playlist_ = std::move(playlist);
    if (!same_list) {
        ++visit_;  // what's on the new page arrives afresh
        // A new list starts fresh, like the original TrackListView getting a new key.
        SetWindowTextW(search_, L"");
        search_text_.clear();
        mix_ = {};
        sort_ = Sort::none;
        asc_ = true;
        list_scroll_ = 0;
        sync_message_.clear();
        clear_selection();
        confirm_free_ = false;
    }
    dirty_ = true;
}

void View::focus(const std::optional<std::string>& id) {
    store_.set_focus(id);
    insp_scroll_ = 0;
}

ListFilter View::list_filter() const {
    switch (page_) {
        case Page::downloaded: return ListFilter::downloaded;
        case Page::missing: return ListFilter::missing;
        case Page::ignored: return ListFilter::ignored;
        case Page::on_pc: return ListFilter::on_pc;
        default: return ListFilter::all;
    }
}

bool View::mouse_down(float x, float y, bool right, WPARAM keys) {
    if (GetFocus() == search_) SetFocus(hwnd_);  // clicking anywhere else leaves the search box
    mods_ = keys;
    return ui_.mouse_down(x, y, right);
}

bool View::key(WPARAM vk) {
    if (fullscreen_) return vis_key(vk);
    if (player_key(vk)) return true;
    const bool ctrl = GetKeyState(VK_CONTROL) & 0x8000;
    if (vk == 'F' && ctrl) {
        if (IsWindowVisible(search_)) SetFocus(search_);
        return false;
    }
    if (vk == VK_F5) {
        if (lib_ && !store_.busy()) jobs_.run([this] { store_.rescan(); });
        return true;
    }
    if (vk == 'A' && ctrl && !ids_.empty() && page_ != Page::home) {  // select every song in the list
        selected_.clear();
        selected_.insert(ids_.begin(), ids_.end());
        sel_dirty_ = true, confirm_bulk_ = false;
        return true;
    }
    if (vk == VK_ESCAPE && !selected_.empty()) {
        clear_selection();
        return true;
    }
    if (vk == VK_ESCAPE && store_.focus()) {
        focus(std::nullopt);
        return true;
    }
    if (vk == VK_RETURN && store_.focus() && page_ != Page::home) {
        play_track(*store_.focus());
        return true;
    }
    if ((vk == VK_UP || vk == VK_DOWN) && !ids_.empty() && page_ != Page::home) {
        const auto f = store_.focus();
        const auto at = f ? std::find(ids_.begin(), ids_.end(), *f) : ids_.end();
        size_t i = at == ids_.end() ? 0 : size_t(at - ids_.begin());
        if (at != ids_.end()) i = vk == VK_UP ? (i ? i - 1 : 0) : std::min(i + 1, ids_.size() - 1);
        focus(ids_[i]);
        // Keep the focused row on screen.
        const float top = 6 + float(i) * 54;
        if (top < list_scroll_) list_scroll_ = top - 6;
        else if (top + 54 > list_scroll_ + list_view_h_) list_scroll_ = top + 54 + 6 - list_view_h_;
        return true;
    }
    return false;
}

}  // namespace wb::ui
