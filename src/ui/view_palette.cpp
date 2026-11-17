// The command palette: Ctrl+K (or the top bar's search pill), type, Enter. It finds pages, playlists, songs and a few
// actions by name (ranked by ui/palette_match.h) and runs the chosen one. It is driven from the keyboard, so it opens,
// closes and moves its choice at once: nothing here animates.
#include "ui/view.h"

#include <dwmapi.h>

#include <algorithm>
#include <format>

#include "ui/palette_match.h"

namespace wb::ui {

using namespace theme;

namespace {

constexpr float kRowH = 40;      // one result
constexpr size_t kMostRows = 9;  // results in view before the list scrolls
// The library's search returns songs in library order, not by how well they match: a few hundred are scored and the
// best kept, so a song titled exactly what was typed isn't lost behind the first thirty that merely contain it.
constexpr size_t kSongsLooked = 300, kSongsShown = 30;
constexpr auto kField = argb(0xFF1E1E20);        // the search field's fill...
constexpr COLORREF kFieldRef = RGB(30, 30, 32);  // ...and the same colour for the native text box over it
constexpr const wchar_t* kCue = L"Search songs, playlists and actions";

}  // namespace

void View::open_palette() {
    // One pop-over at a time, and what was typed into a dialog isn't thrown away for this one.
    if (fullscreen_ || url_open_ || bug_open_ || slsk_query_open_) return;
    eq_open_ = false;
    // Made the first time (hidden; the next paint places and shows it) and focused now, so typing lands in it at once.
    const HWND box = edit(kPaletteBox, Rect{}, false, kCue, "", kFieldRef);
    SetWindowTextW(box, L"");  // before palette_open_: emptying the box isn't a new query
    palette_open_ = true;
    palette_build();
    SetFocus(box);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void View::close_palette() {
    if (!palette_open_) return;
    palette_open_ = false;
    palette_items_.clear();
    SetFocus(hwnd_);  // the keyboard is the window's again; the box is hidden after the next paint
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void View::palette_changed() {
    if (!palette_open_) return;
    palette_build();
    InvalidateRect(hwnd_, nullptr, FALSE);  // EN_CHANGE doesn't repaint by itself
}

void View::palette_build() {
    using Kind = PaletteItem::Kind;
    const std::string query = edit_text(kPaletteBox);
    const bool typed = !query_words(query).empty();
    // Everything that matches, in the order equal scores keep: pages, playlists, actions, songs.
    std::vector<PaletteItem> found;
    std::vector<int> scores;
    auto add = [&](PaletteItem item, int score) {
        found.push_back(std::move(item));
        scores.push_back(score);
    };

    struct PageEntry {
        Page page;
        const char* name;
        const wchar_t* glyph;
    };
    static constexpr PageEntry pages[] = {
        {Page::shelf, "Shelf", icon::home},
        {Page::all, "All tracks", icon::music},
        {Page::downloaded, "In my crate", icon::check_circle},
        {Page::missing, "Missing", icon::circle},
        {Page::ignored, "Ignored", icon::block},
        {Page::on_pc, "My folders", icon::folder},
        {Page::recent, "Recently added", icon::sparkle},
        {Page::web, "Web player", icon::globe},
        {Page::queue, "Download queue", icon::queue},
        {Page::soulseek, "Soulseek sync", icon::download},
        {Page::phone, "Sync to phone", icon::phone},
        {Page::settings, "Settings", icon::settings},
    };
    auto there = [&](Page p) {
        if (!lib_) return p == Page::settings;  // with no library yet, every other page is the welcome screen
        if (p == Page::queue || p == Page::soulseek) return slsk_ != nullptr;
        if (p == Page::phone) return sync_ && tunnel_;
        return true;
    };
    for (const auto& p : pages)
        if (const int s = match_score(query, p.name); s && there(p.page))
            add({Kind::page, wide(p.name), L"", p.glyph, [this, to = p.page] { go(to); }}, s);

    if (typed && lib_)
        for (const auto& pl : lib_->playlists)
            if (const int s = match_score(query, pl.name)) {
                const size_t n = pl.track_ids.size();
                add({Kind::playlist, wide(pl.name), n == 1 ? std::wstring(L"1 song") : std::format(L"{} songs", n), icon::list,
                     [this, name = pl.name] { go(Page::playlist, name); }},
                    s);
            }

    struct ActionEntry {
        const char* name;
        const wchar_t* glyph;
        std::wstring detail;
        std::function<void()> run;
        bool there = true;
    };
    const auto busy = store_.busy();
    const Motion motion = motion_setting();
    auto set_motion_to = [&](const char* name, Motion m, const wchar_t* glyph, const wchar_t* note) {
        return ActionEntry{name, glyph, motion == m ? L"Current" : L"", [this, m, note] {
                               set_motion(m);
                               show_toast(note);  // the palette is gone: say it took
                           }};
    };
    const ActionEntry actions[] = {
        {"Rescan and analyse", icon::scan, busy ? wide(*busy) : std::wstring(),
         [this] {
             if (const auto b = store_.busy()) return show_toast(wide(*b));  // one job at a time: say what is running instead
             jobs_.run([this] { store_.rescan(); });
         },
         lib_ != nullptr},
        // What the player's Open menu has, each straight to its dialog: a menu at the pointer is out of place after Enter.
        {"Open files", icon::add, L"", [this] { open_files(false); }},
        {"Open a folder", icon::folder, L"", [this] { open_files(true); }},
        {"Open URL", icon::globe, L"", [this] { url_open_ = true; }},
        {"Report a bug", icon::bug, L"",
         [this] {
             // The report takes its screenshot as it opens: put what was behind the palette on screen first.
             RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
             DwmFlush();
             open_bug_report();
         }},
        {"Full-screen visualizer", icon::visualizer, L"", [this] { set_fullscreen(true); }},
        set_motion_to("Motion: full", Motion::full, icon::sparkle, L"Full motion"),
        set_motion_to("Motion: reduced", Motion::reduced, icon::sparkle, L"Reduced motion"),
        set_motion_to("Motion: off", Motion::off, icon::block, L"Motion off"),
        {"Check for updates", icon::sync, update_busy_ ? L"Checking…" : L"",
         [this] {
             check_updates();
             show_toast(L"Checking for updates");
         }},
    };
    for (const auto& a : actions)
        if (const int s = match_score(query, a.name); s && a.there) add({Kind::action, wide(a.name), a.detail, a.glyph, a.run}, s);

    if (typed && lib_) {
        auto ids = store_.row_ids(ListFilter::all, std::nullopt, query);
        if (ids.size() > kSongsLooked) ids.resize(kSongsLooked);
        std::vector<PaletteItem> songs;
        std::vector<int> song_scores;
        for (const auto& id : ids) {
            const auto t = store_.track(id);
            if (!t) continue;
            const std::string artist = t->artist();
            // A song's name is its title, or its artist and title together. The library found it (by those or its
            // album), so it is listed even when neither matches, after the songs that do.
            song_scores.push_back(std::max({match_score(query, t->title), match_score(query, artist + " " + t->title), kAnyMatch}));
            songs.push_back({Kind::song, wide(t->title.empty() ? t->file_name : t->title), wide(artist), icon::music, [this, id] {
                                 // All tracks keeps its search and filters when it is already open: they could hide the song.
                                 if (page_ == Page::all && (!search_text_.empty() || mix_.active())) {
                                     SetWindowTextW(search_, L"");  // → search_changed()
                                     mix_ = {};
                                 }
                                 go(Page::all);
                                 focus(id);
                                 reveal_id_ = id;  // the list scrolls to it once its rows are known
                             }});
        }
        const auto best = rank_by_score(song_scores);
        for (size_t k = 0; k < best.size() && k < kSongsShown; ++k) add(std::move(songs[best[k]]), song_scores[best[k]]);
    }

    palette_items_.clear();
    for (const size_t i : rank_by_score(scores)) palette_items_.push_back(std::move(found[i]));
    palette_choice_ = 0;
    palette_scroll_ = 0;
}

void View::palette_run(size_t i) {
    if (i >= palette_items_.size()) return;
    const auto run = palette_items_[i].run;  // a copy: closing lets the results go
    close_palette();  // first, so a pop-over or dialog the action opens isn't under it
    run();
}

void View::palette_show_choice() {
    const float top = float(palette_choice_) * kRowH, view = float(palette_rows_) * kRowH;
    if (top < palette_scroll_) palette_scroll_ = top;
    else if (top + kRowH > palette_scroll_ + view) palette_scroll_ = top + kRowH - view;
}

bool View::palette_key(WPARAM vk) {
    const bool ctrl = GetKeyState(VK_CONTROL) & 0x8000;
    const size_t n = palette_items_.size(), step = std::max<size_t>(1, palette_rows_);
    switch (vk) {
        case VK_ESCAPE: close_palette(); break;
        case VK_RETURN: palette_run(palette_choice_); break;
        case VK_UP:
        case VK_DOWN:  // past either end to the other
            if (n) palette_choice_ = (palette_choice_ + (vk == VK_UP ? n - 1 : 1)) % n;
            palette_show_choice();
            break;
        case VK_PRIOR:
            palette_choice_ = palette_choice_ >= step ? palette_choice_ - step : 0;
            palette_show_choice();
            break;
        case VK_NEXT:
            if (n) palette_choice_ = std::min(palette_choice_ + step, n - 1);
            palette_show_choice();
            break;
        default:
            if (vk == 'K' && ctrl) close_palette();
            // Anything else is typing, which belongs in the box (focus can stray to the window, e.g. after switching apps).
            else if (const auto it = edits_.find(kPaletteBox); it != edits_.end() && GetFocus() != it->second.hwnd) SetFocus(it->second.hwnd);
    }
    return true;  // it has the keyboard while it's open
}

void View::palette() {
    const float W = g_.width(), H = g_.height();
    g_.fill(Rect{0, 0, W, H}, argb(0x99000000));
    ui_.click(Rect{0, 0, W, H}, [this] { close_palette(); });  // a click outside closes it

    // The panel hangs near the top so the results can grow down from the box: as many rows as fit, nine at most.
    const float top = 96, field_h = 44, footer_h = 34;
    const float chrome = 12 + field_h + 8 + 8 + 1 + footer_h;
    palette_rows_ = std::clamp(size_t(std::max(0.f, H - top - 24 - chrome) / kRowH), size_t(3), kMostRows);
    const size_t n = palette_items_.size();
    const float list_h = n ? float(std::min(n, palette_rows_)) * kRowH : 56;
    const float pw = std::min(640.f, W - 32);
    const Rect p = Rect::xywh((W - pw) / 2, top, pw, chrome + list_h);
    g_.shadow(p, 20, 24, 0.6f);
    ui_.glass(p, 20, false, true);  // solid: what's behind doesn't show through the text
    ui_.click(p, [] {});            // clicks on the panel stay on it

    // The box: a native text box over a drawn field of its colour.
    const Rect field{p.l + 12, p.t + 12, p.r - 12, p.t + 12 + field_h};
    g_.fill_round(field, 12, kField);
    g_.icon(icon::search, field.l + 22, (field.t + field.b) / 2, 15, text2);
    const HWND box = edit(kPaletteBox, Rect{field.l + 42, field.t + 13, field.r - 14, field.b - 13}, false, kCue, "", kFieldRef);
    g_.stroke_round(field, 12, GetFocus() == box ? glass_border : hairline);
    // It has the keyboard while it's open: focus that strayed to the window comes back (only while this app is in front).
    if (GetFocus() != box && GetActiveWindow() == hwnd_) SetFocus(box);

    const Rect results{p.l + 8, field.b + 8, p.r - 8, field.b + 8 + list_h};
    if (!n) {
        g_.text(L"Nothing matches", results, {Font::ui, 13, 400, text2, Align::center});
    } else {
        const float content = float(n) * kRowH, most = std::max(0.f, content - results.h());
        palette_scroll_ = std::clamp(palette_scroll_, 0.f, most);
        const float right = most > 0 ? results.r - 12 : results.r;  // the scrollbar's lane stays clear
        static constexpr const wchar_t* kinds[] = {L"Page", L"Playlist", L"Action", L"Song"};  // PaletteItem::Kind's order
        const TextStyle name_st{Font::ui, 13.5f, 500, text}, detail_st{Font::ui, 12.5f, 400, text2};
        const TextStyle kind_st{Font::ui, 12, 400, text2, Align::right};
        ui_.push_clip(results);
        for (size_t i = size_t(palette_scroll_ / kRowH); i < n; ++i) {
            const Rect r = Rect::xywh(results.l, results.t + float(i) * kRowH - palette_scroll_, right - results.l, kRowH);
            if (r.t >= results.b) break;
            const PaletteItem& item = palette_items_[i];
            const bool chosen = i == palette_choice_;
            const Rect lit = r.inset(0, 2);
            if (chosen) {
                g_.fill_round(lit, 10, selected);
                g_.stroke_round(lit, 10, with_alpha(lilac, 0.75f), 1.5f);
            } else if (ui_.hover(r)) {
                g_.fill_round(lit, 10, hover);
            }
            const float cy = (r.t + r.b) / 2;
            g_.icon(item.glyph, r.l + 22, cy, 14, chosen ? text : text2);
            const std::wstring kind = kinds[size_t(item.kind)];
            const float kind_l = r.r - 14 - g_.measure(kind, kind_st) - 2;
            g_.text(kind, Rect{kind_l, r.t, r.r - 14, r.b}, kind_st);
            // The name first; the quieter part gets what's left of the line (the name at most 70 % of it).
            const float x = r.l + 44, room = std::max(0.f, kind_l - 16 - x);
            const float name_w = item.detail.empty() ? room : std::min(g_.measure(item.name, name_st) + 2, room * 0.7f);
            g_.text(item.name, Rect{x, r.t, x + name_w, r.b}, name_st);
            if (!item.detail.empty() && room - name_w > 30) g_.text(item.detail, Rect{x + name_w + 10, r.t, kind_l - 16, r.b}, detail_st);
            ui_.click(r, [this, i] { palette_run(i); });
        }
        ui_.pop_clip();
        ui_.scroll_area(results, palette_scroll_, content);  // after the rows, so its bar is drawn over them
    }

    const float line_y = results.b + 8;
    g_.line(p.l + 1, line_y, p.r - 1, line_y, hairline);
    g_.text(L"Enter to open, Esc to close", Rect{p.l + 20, line_y, p.r - 20, p.b}, {Font::ui, 12, 400, text2});
}

}  // namespace wb::ui
