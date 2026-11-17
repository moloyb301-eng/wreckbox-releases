// Shell: the window's layout, page headers, going from page to page and back, the keyboard. The top bar is in
// view_topbar.cpp, the shelf in view_shelf.cpp, track lists and the inspector in view_tracks.cpp.
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
            if (id == kUrlBox || id == kPaletteBox || in_bug_report) SendMessageW(GetParent(h), WM_KEYDOWN, wp, lp);  // closes the dialog too
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
        // The palette's box keeps the typing; moving through the results, choosing one and Ctrl+K go to the app.
        if (id == kPaletteBox && (wp == VK_UP || wp == VK_DOWN || wp == VK_RETURN || wp == VK_PRIOR || wp == VK_NEXT ||
                                  (wp == 'K' && (GetKeyState(VK_CONTROL) & 0x8000)))) {
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
    // The record on the logo and in the player bar turns while music plays (33⅓ rpm), easing up to speed and down. Only
    // while the window is in front: behind other windows it waits where it is, and asks for no frames.
    // Timed by the clock, not by ui_.dt: dt is capped for easing, and the 33 ms frames here would make it turn at half speed.
    const bool may_turn = player_.playing() && backdrop_moving();
    const float turning = ui_.anim(Ui::key("record-speed"), may_turn ? 1.f : 0.f, 2.5f);
    if (turning > 0.001f && spin_at_ >= 0) {
        spin_ = float(std::fmod(double(spin_) + 200.0 * double(turning) * std::min(ui_.now - spin_at_, 0.25), 360.0));
        backdrop_wants_frames_ = true;
    }
    spin_at_ = turning > 0.001f || may_turn ? ui_.now : -1;
    pointer_frames_ = false;  // a page sets it again if something on it follows the pointer

    // The window, top to bottom: the top bar, the page (with the inspector beside it when a song is chosen), and the
    // player floating over the page's foot.
    const float top_h = 48, page_l = 22, bar_h = 66;
    const float page_t = 10 + top_h;
    top_bar(Rect{10, 10, W - 10, page_t});
    const auto focus = store_.focus();
    const bool tools = page_ == Page::shelf || page_ == Page::web || page_ == Page::queue || page_ == Page::soulseek || page_ == Page::phone ||
                       page_ == Page::settings;
    const bool inspect = focus && !tools && lib_;
    const bool overlay = W < 1320;
    const float column_r = inspect && !overlay ? W - 340 : W;
    const bool banner = update_visible();
    if (banner) update_banner(Rect{page_l, page_t + 6, column_r - 10, page_t + 50});
    {
        const Rect pr{page_l, banner ? page_t + 46 : page_t, column_r - 22, H - 10 - bar_h - 10};
        // A crate opening or closing draws its own frames. Otherwise the page is simply there, after a short fade
        // (no slide: the same page arrives many times a day).
        if (!zoom_frame(pr)) {
            // None after a key: what the keyboard does happens at once. (Asked for either way, so that a click on the
            // page later doesn't find the fade unseen and start it then.)
            const float fade = ui_.enter(arrive("page"), 0, 0.16f);
            const float in = keyboard_nav_ ? 1.f : fade;
            if (in < 1) g_.push_opacity(in);
            page(pr);
            if (in < 1) g_.pop_opacity();
        }
    }
    {
        // The player: an island centred under the page, no wider than it needs to be.
        const float room = column_r - 10 - page_l, w = std::min(room, 1040.f), cx = page_l + room / 2;
        player_bar(Rect{cx - w / 2, H - 10 - bar_h, cx + w / 2, H - 10});
    }
    web_sync();
    if (inspect) {
        // While a crate opens, the inspector (a song chosen earlier) comes in with the crate's page, not before it.
        const bool fading = zoom_page_a_ < 1;
        if (fading) g_.push_opacity(zoom_page_a_);
        inspector(Rect{W - 340, page_t + 8, W - 10, H - 10}, overlay);
        if (fading) g_.pop_opacity();
    }
    if (eq_open_ || url_open_ || bug_open_ || slsk_query_open_ || palette_open_) {
        // Native text boxes would sit on top of a pop-over: hide them while one is open (the dialogs' own boxes excepted).
        for (auto& [id, e] : edits_)
            if (id != kUrlBox && id != kBugTitle && id != kBugBody && id != kSlskQuery && id != kPaletteBox) e.used = false;
        if (eq_open_) eq_panel();
        if (url_open_) url_dialog();
        if (bug_open_) bug_dialog();
        if (slsk_query_open_) slsk_query_dialog();
        if (palette_open_) palette();
    }
    draw_toast();
    if (const UINT due = ui_.draw_tooltip()) SetTimer(hwnd_, kTooltipTimer, due + 10, nullptr);
    hide_unused_edits();
    update_timer();
    schedule_motion();
}

// MARK: Header

float View::header(const Rect& r, const std::wstring& title, const std::wstring& subtitle, const std::vector<Action>& actions) {
    float y = r.t + 18;  // no label above the title: the top bar already says where this is
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
        case Page::shelf: return shelf(r);
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
    const float h = header(r, L"Web player", L"Spotify and YouTube Music's own players. Sign in once; it remembers you.", actions);
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
    if (eq_open_ || url_open_ || bug_open_ || slsk_query_open_ || palette_open_) return web_->hide();  // a pop-over sits on top of it
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
    const float h = header(r, title, subtitle);
    const Rect box{r.l, r.t + h, r.r, r.t + h + 110};
    ui_.glass(box);
    g_.text(L"Not built yet", Rect{box.l + 22, box.t + 22, box.r - 22, box.t + 48}, {Font::ui, 17, 600, text});
    g_.text(L"This screen arrives in " + phase + L" of the plan (docs/PLAN.md). The original build still has it meanwhile.",
            Rect{box.l + 22, box.t + 52, box.r - 22, box.t + 80}, {Font::ui, 13, 400, text2});
}

// MARK: Behaviour

void View::go(Page p, std::optional<std::string> playlist) {
    const bool same_list = page_ == p && playlist_ == playlist;
    if (!same_list) history_.went(here(), Where{p, playlist}, Where{});
    page_ = p;
    playlist_ = std::move(playlist);
    if (!same_list) {
        ++visit_;  // what's on the new page arrives afresh
        rows_arrive_ = false;  // open_crate() sets it again when the visit came out of a crate
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

bool View::back() {
    if (fullscreen_ || palette_open_) return false;
    const auto to = history_.back(here(), Where{});
    if (!to) return false;
    go(to->page, to->playlist);
    return true;
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
    keyboard_nav_ = false;  // the mouse again: keyboard rings go away
    return ui_.mouse_down(x, y, right);
}

bool View::key(WPARAM vk) {
    if (fullscreen_) return vis_key(vk);
    keyboard_nav_ = true;
    const bool ctrl = GetKeyState(VK_CONTROL) & 0x8000;
    if (palette_open_) return palette_key(vk);  // it has the keyboard while it's open
    if (vk == 'K' && ctrl) {
        open_palette();
        return true;
    }
    if (player_key(vk)) return true;
    if (vk == 'F' && ctrl) {
        // A list's own search box where there is one; anywhere else, the palette searches everything.
        if (IsWindowVisible(search_)) SetFocus(search_);
        else open_palette();
        return true;
    }
    if (page_ == Page::shelf && shelf_key(vk)) return true;
    if (vk == VK_F5) {
        if (lib_ && !store_.busy()) jobs_.run([this] { store_.rescan(); });
        return true;
    }
    if (vk == 'A' && ctrl && !ids_.empty() && page_ != Page::shelf) {  // select every song in the list
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
    if (vk == VK_ESCAPE || vk == VK_BACK) return back();  // nothing left to dismiss: back to the page before
    if (vk == VK_RETURN && store_.focus() && page_ != Page::shelf) {
        play_track(*store_.focus());
        return true;
    }
    if ((vk == VK_UP || vk == VK_DOWN) && !ids_.empty() && page_ != Page::shelf) {
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
