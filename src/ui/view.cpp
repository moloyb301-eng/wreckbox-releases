// Shell, sidebar, headers, Home and the placeholder pages. Track lists and the inspector are in view_tracks.cpp.
#include "ui/view.h"

#include <commctrl.h>

#include <algorithm>
#include <format>

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
    const float W = g_.width(), H = g_.height();
    ui_.begin_frame();
    if (fullscreen_) {
        visualizer_screen();
        draw_toast();
        if (const UINT due = ui_.draw_tooltip()) SetTimer(hwnd_, kTooltipTimer, due + 10, nullptr);
        hide_unused_edits();
        update_timer();
        return;
    }

    g_.fill(Rect{0, 0, W, H}, bg);
    g_.fill_radial(Rect{0, 0, W, H}, {W * 0.95f, H * 0.05f}, std::min(W, H) * 1.3f, argb(0x29BB96DA), argb(0x00BB96DA));

    sidebar(Rect{10, 10, 252, H - 10});
    const auto focus = store_.focus();
    const bool tools = page_ == Page::queue || page_ == Page::soulseek || page_ == Page::phone || page_ == Page::settings;
    const bool inspect = focus && !tools && lib_;
    const bool overlay = W < 1320;
    // The page column: the page, and the player bar under it (Flutter: Column(page, PlayerBar)).
    const float column_r = inspect && !overlay ? W - 340 : W, bar_h = 66;
    const bool banner = update_visible();
    if (banner) update_banner(Rect{252 + 22, 10, column_r - 10, 54});
    page(Rect{252 + 22, banner ? 56.f : 0.f, column_r - 22, H - 10 - bar_h - 10});
    player_bar(Rect{252 + 22, H - 10 - bar_h, column_r - 10, H - 10});
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
}

// MARK: Sidebar

void View::sidebar(const Rect& r) {
    ui_.glass(r);
    ui_.logo(r.l + 18, r.t + 24, 34);
    g_.text(L"WRECKBOX", Rect{r.l + 62, r.t + 24, r.r, r.t + 58}, {Font::dot, 17, 700, text, Align::left, 2});

    const Rect area{r.l + 8, r.t + 72, r.r - 8, r.b - 52};
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
    auto item = [&](Page p, const std::wstring& title, const wchar_t* glyph, std::optional<size_t> count,
                    std::optional<std::string> pl = std::nullopt) {
        const Rect ir{area.l, y, area.r, y + 34};
        const bool is_selected = page_ == p && playlist_ == pl;
        if (is_selected) g_.fill_round(ir, 10, theme::selected);
        else if (ui_.hover(ir)) g_.fill_round(ir, 10, theme::hover);
        g_.icon(glyph, ir.l + 18, ir.t + 17, 15, is_selected ? text : text3);
        float right = ir.r - 10;
        if (count) {
            const std::wstring c = std::to_wstring(*count);
            const TextStyle st{Font::dot, 11, 700, text3};
            const float cw = g_.measure(c, st);
            g_.text(c, Rect{right - cw, ir.t, right, ir.b}, st);
            right -= cw + 8;
        }
        g_.text(title, Rect{ir.l + 36, ir.t, right, ir.b}, {Font::ui, 13.5f, is_selected ? 600 : 500, is_selected ? text : text2});
        ui_.click(ir, [this, p, pl] { go(p, pl); });
        y += 36;
    };
    auto section = [&](const wchar_t* s) {
        y += 12;
        ui_.dot_label(s, area.l + 12, y + 8);
        y += 24;
    };
    item(Page::home, L"Home", icon::home, std::nullopt);
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
    for (const auto& a : actions) {  // bottom-aligned with the title block, like the Flutter Row(crossAxisAlignment.end)
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

void View::placeholder(const Rect& r, const std::wstring& title, const std::wstring& subtitle, const std::wstring& phase) {
    const float h = header(r, L"Tools", title, subtitle);
    const Rect box{r.l, r.t + h, r.r, r.t + h + 110};
    ui_.glass(box);
    g_.text(L"Not built yet", Rect{box.l + 22, box.t + 22, box.r - 22, box.t + 48}, {Font::ui, 17, 600, text});
    g_.text(L"This screen arrives in " + phase + L" of the plan (docs/PLAN.md). The Flutter build still has it meanwhile.",
            Rect{box.l + 22, box.t + 52, box.r - 22, box.t + 80}, {Font::ui, 13, 400, text2});
}

void View::home(const Rect& r) {
    ui_.push_clip(Rect{r.l - 22, r.t, r.r + 22, r.b});  // headers sit flush; shadows of cards may overhang slightly
    const float top = r.t - home_scroll_;
    const size_t total = lib_ ? lib_->tracks.size() : 0;
    float y = top + header(Rect{r.l, top, r.r, r.b}, L"Home", L"Your crate",
                           std::format(L"{} tracks from {} Spotify playlists", total, lib_ ? lib_->playlists.size() : 0), library_actions(false));

    // Stat tiles
    const float gap = 12, tw = (r.w() - 3 * gap) / 4, th = 138;
    auto stat = [&](int i, const std::wstring& label, const std::wstring& value, const std::wstring& detail, std::optional<double> progress) {
        const Rect t = Rect::xywh(r.l + float(i) * (tw + gap), y, tw, th);
        ui_.glass(t, 20);
        ui_.dot_label(label, t.l + 18, t.t + 26);
        g_.text(value, Rect{t.l + 18, t.t + 40, t.r - 18, t.t + 82}, {Font::dot, 34, 700, text});
        float dy = t.t + 90;
        if (progress) {
            ui_.progress(Rect{t.l + 18, dy, t.r - 18, dy + 4}, *progress);
            dy += 10;
        }
        g_.text(detail, Rect{t.l + 18, dy, t.r - 18, dy + 18}, {Font::ui, 12, 400, text2});
    };
    stat(0, L"Tracks", std::to_wstring(total), L"across all playlists", std::nullopt);
    stat(1, L"In crate", std::to_wstring(n_downloaded_), total ? std::format(L"{}% of your library", (n_downloaded_ * 100 + total / 2) / total) : L"",
         total ? std::optional(double(n_downloaded_) / double(total)) : std::optional(0.0));
    stat(2, L"Missing", std::to_wstring(n_missing_), std::format(L"{} ignored", n_ignored_), std::nullopt);
    stat(3, L"Analysed", std::to_wstring(n_analysed_), L"BPM · key · energy", std::nullopt);
    y += th + 22;

    // Recently added — as many as fit the width.
    ui_.dot_label(L"Recently added", r.l, y + 7, text2, 12);
    y += 26;
    float x = r.l;
    for (const auto& id : recent_) {
        if (x + 160 > r.r) break;
        const auto t = store_.track(id);
        if (!t) continue;
        const Rect card = Rect::xywh(x, y, 160, 210);
        ui_.glass(card, 20);
        if (ui_.hover(card)) g_.fill_round(card, 20, theme::hover);
        ui_.artwork(*t, Rect::xywh(x + 10, y + 10, 140, 140), 16);
        g_.text(wide(t->title), Rect{x + 10, y + 156, x + 150, y + 176}, {Font::ui, 13, 600, text});
        g_.text(wide(t->artist()), Rect{x + 10, y + 176, x + 150, y + 194}, {Font::ui, 11.5f, 400, text2});
        ui_.click(card, [this, id] { focus(id); });
        x += 174;
    }
    y += 210 + 22;

    // Playlists
    ui_.dot_label(L"Playlists", r.l, y + 7, text2, 12);
    y += 26;
    x = r.l;
    if (lib_) {
        for (const auto& p : lib_->playlists) {
            if (x + 196 > r.r + 1) {
                x = r.l;
                y += 86;
            }
            const Rect card = Rect::xywh(x, y, 196, 72);
            ui_.glass(card, 20);
            if (ui_.hover(card)) g_.fill_round(card, 20, theme::hover);
            g_.text(wide(p.name), Rect{x + 12, y + 10, x + 184, y + 30}, {Font::ui, 14, 600, text});
            const size_t n = p.track_ids.size(), have = store_.downloaded_in(p);
            g_.text(n == 1 ? L"1 track" : std::format(L"{} tracks", n), Rect{x + 12, y + 34, x + 120, y + 50}, {Font::ui, 11.5f, 400, text2});
            g_.text(std::format(L"{}/{}", have, n), Rect{x + 100, y + 34, x + 184, y + 50}, {Font::dot, 11, 700, text3, Align::right});
            ui_.progress(Rect{x + 12, y + 57, x + 184, y + 60}, n ? double(have) / double(n) : 0);
            ui_.click(card, [this, name = p.name] { go(Page::playlist, name); });
            x += 210;
        }
        y += 72;
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
        // A new list starts fresh, like the Flutter TrackListView getting a new key.
        SetWindowTextW(search_, L"");
        search_text_.clear();
        mix_ = {};
        sort_ = Sort::none;
        asc_ = true;
        list_scroll_ = 0;
        sync_message_.clear();
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

bool View::mouse_down(float x, float y, bool right) {
    if (GetFocus() == search_) SetFocus(hwnd_);  // clicking anywhere else leaves the search box
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
