// The shelf, the first screen: a wall of record crates on planks, one crate per library view and per playlist, each
// holding the covers of its first songs. The crate under the pointer lifts its front record; clicking one zooms into it
// while its track list arrives.
#include <algorithm>
#include <cmath>
#include <format>

#include "ui/view.h"

namespace wb::ui {

using namespace theme;

namespace {

// A crate on the wall, in DIPs: the box with its sleeves standing in it (its name is on the box's front).
constexpr float kCrateW = 190, kCrateH = 160;
constexpr float kFaceH = 62;               // the box's front, which hides the lower part of the sleeves
constexpr float kSleeve = 128;             // the front sleeve; each one further back is a little smaller
constexpr float kGap = 30;                 // the least room between two crates
constexpr float kRowTop = 18;              // room above the crates for the front record to lift into
constexpr float kLift = 24;                // how far the front record rises under the pointer
constexpr float kRowH = kRowTop + kCrateH + 56;  // a row: its crates, the plank under them, air before the next
constexpr float kZoom = 0.38f;             // the crate opening, in seconds

float ease_out(float t) {
    t = std::clamp(t, 0.f, 1.f);
    const float u = 1 - t;
    return 1 - u * u * u;
}

}  // namespace

// MARK: Crates

std::vector<View::Crate> View::crates() const {
    std::vector<Crate> out;
    if (!lib_) return out;
    crate_cache_check();
    auto count = [](size_t n) { return std::to_wstring(n); };
    out.push_back({Page::all, std::nullopt, L"All tracks", count(lib_->tracks.size())});
    out.push_back({Page::downloaded, std::nullopt, L"In my crate", count(n_downloaded_)});
    out.push_back({Page::missing, std::nullopt, L"Missing", count(n_missing_)});
    out.push_back({Page::ignored, std::nullopt, L"Ignored", count(n_ignored_)});
    out.push_back({Page::on_pc, std::nullopt, L"My folders", count(n_on_pc_)});
    out.push_back({Page::recent, std::nullopt, L"Recently added", count(recent_.size())});
    for (const auto& p : lib_->playlists) {
        auto it = crate_have_.find(p.name);
        if (it == crate_have_.end()) it = crate_have_.emplace(p.name, store_.downloaded_in(p)).first;
        const size_t have = it->second, all = p.track_ids.size();
        out.push_back({Page::playlist, p.name, wide(p.name), std::format(L"{} of {}", have, all), all ? double(have) / double(all) : 0.0});
    }
    return out;
}

void View::crate_cache_check() const {
    // The library's identity and its counts: a download or a rescan changes which songs a library crate holds.
    size_t sig = std::hash<const void*>{}(lib_.get());
    for (const size_t v : {n_downloaded_, n_missing_, n_ignored_, n_on_pc_, recent_.size()}) sig = (sig ^ v) * 1099511628211ull;
    if (sig == crate_sig_) return;
    crate_sig_ = sig;
    crate_cache_.clear();
    crate_have_.clear();
}

const std::vector<LibraryTrack>& View::crate_songs(Page p, const std::optional<std::string>& playlist, size_t n) const {
    crate_cache_check();
    CrateSongs& c = crate_cache_[{int(p), p == Page::playlist ? playlist.value_or("") : std::string()}];
    // Found already, unless fewer were asked for than now and there may be more.
    if (!lib_ || c.asked >= n || c.tracks.size() < c.asked) return c.tracks;
    c.asked = n;
    c.tracks.clear();
    auto add = [&](const std::string& id) {
        if (c.tracks.size() >= n || std::any_of(c.tracks.begin(), c.tracks.end(), [&](const LibraryTrack& t) { return t.id == id; })) return;
        if (auto t = store_.track(id)) c.tracks.push_back(std::move(*t));
    };
    switch (p) {
        case Page::playlist:
            for (const auto& pl : lib_->playlists)
                if (playlist && pl.name == *playlist)
                    for (const auto& id : pl.track_ids) {
                        if (c.tracks.size() >= n) break;
                        add(id);
                    }
            break;
        case Page::recent:
            for (const auto& id : recent_) add(id);
            break;
        case Page::on_pc: {
            // Files, not library songs: the ones matched to a library song come first, as they have its cover.
            std::vector<LibraryTrack> plain;
            const auto ids = store_.row_ids(ListFilter::on_pc);
            for (size_t i = 0; i < ids.size() && i < 200 && c.tracks.size() < n; ++i)
                if (auto row = store_.row(ids[i])) (is_file_id(row->track.id) ? plain : c.tracks).push_back(std::move(row->track));
            for (auto& t : plain)
                if (c.tracks.size() < n) c.tracks.push_back(std::move(t));
            break;
        }
        default: {
            const ListFilter f = p == Page::downloaded ? ListFilter::downloaded
                                 : p == Page::missing  ? ListFilter::missing
                                 : p == Page::ignored  ? ListFilter::ignored
                                                       : ListFilter::all;
            for (const auto& id : store_.row_ids(f)) {
                if (c.tracks.size() >= n) break;
                add(id);
            }
        }
    }
    return c.tracks;
}

std::vector<LibraryTrack> View::crate_covers(Page p, const std::optional<std::string>& playlist, size_t n) const {
    const auto& all = crate_songs(p, playlist, n);
    return {all.begin(), all.begin() + std::ptrdiff_t(std::min(n, all.size()))};
}

// MARK: The wall

void View::shelf(const Rect& r) {
    // Drawn only for show while a crate opens (the page is already the crate's): the crate being opened stays lit, and
    // nothing here takes input (the caller made the UI inert). It is drawn in two passes then (shelf_pass_): everything
    // else, then that crate on its own, so the two can fade at their own pace.
    const bool for_show = page_ != Page::shelf;
    const bool only_opening = for_show && shelf_pass_ == ShelfPass::opening, skip_opening = for_show && shelf_pass_ == ShelfPass::rest;
    std::wstring line;
    if (lib_)
        line = std::format(L"{} {}, {} in your crate, {} missing, {} analysed", lib_->tracks.size(), lib_->tracks.size() == 1 ? L"track" : L"tracks",
                           n_downloaded_, n_missing_, n_analysed_);
    if (!only_opening) shelf_head_ = header(r, L"Shelf", line, library_actions(false));
    const float h = shelf_head_;
    if (!lib_) return;
    const auto all = crates();
    const int n = int(all.size());

    // Crates left to right, as many to a plank as fit, the planks centred on the page.
    const Rect wall{r.l, r.t + h - 8, r.r, r.b};
    const float side = 12, room = wall.w() - 2 * side;
    const int cols = std::max(1, int((room + kGap) / (kCrateW + kGap)));
    const float gap = cols > 1 ? std::min(72.f, (room - float(cols) * kCrateW) / float(cols - 1)) : 0;
    const float row_w = float(cols) * kCrateW + float(cols - 1) * gap, x0 = wall.l + (wall.w() - row_w) / 2;
    const int rows = (n + cols - 1) / cols;
    if (!for_show) {
        shelf_cols_ = cols, shelf_count_ = n;
        if (shelf_sel_ >= n) shelf_sel_ = n - 1;
        // Back from a crate (or here again after one was shown): that crate is chosen, and in view. Its ring shows only if
        // a key brought the user back; by mouse, the arrows start there but nothing shows until one is pressed.
        if (!keyboard_nav_) shelf_ring_ = false;
        if (shelf_visit_ != visit_) {
            shelf_visit_ = visit_;
            shelf_ring_ = keyboard_nav_;
            for (int i = 0; shelf_from_ && i < n; ++i)
                if (all[size_t(i)].page == shelf_from_->page && all[size_t(i)].playlist == shelf_from_->playlist) shelf_sel_ = i, shelf_reveal_ = true;
        }
        if (shelf_reveal_ && shelf_sel_ >= 0) {  // at once: the keyboard moved it. Its ring clear of the faded edges.
            const float top = 4 + float(shelf_sel_ / cols) * kRowH, want_t = top + kRowTop - 44, want_b = top + kRowTop + kCrateH + 44;
            if (want_t < shelf_scroll_) shelf_scroll_ = want_t;
            else if (want_b > shelf_scroll_ + wall.h()) shelf_scroll_ = want_b - wall.h();
        }
        shelf_reveal_ = false;
    }
    const float content = 4 + float(rows) * kRowH;
    if (!only_opening) ui_.scroll_area(wall, shelf_scroll_, content);

    const bool full = motion_now() == Motion::full;
    if (!for_show) shelf_rect_ = r;

    // Where there is more above or below, the crates go out of sight softly rather than cut through; the fade grows with
    // what's left to scroll, so the ends of the wall show it whole.
    const float more_below = std::max(0.f, content - wall.h() - shelf_scroll_);
    ui_.push_clip(wall);
    g_.push_fade(wall, std::min(28.f, shelf_scroll_), std::min(28.f, more_below));
    for (int row = 0; row < rows; ++row) {
        const float top = wall.t + 4 + float(row) * kRowH - shelf_scroll_, floor = top + kRowTop + kCrateH;
        if (top > wall.b || floor + 48 < wall.t) continue;
        // The plank: furniture, so quiet. Its top seen from above (the back edge a little shorter), a lit nose, its
        // front, and the shade it throws below.
        const float pl = wall.l, pr = wall.r;
        if (!only_opening) {
            const D2D1_POINT_2F board[4] = {{pl + 18, floor - 10}, {pr - 18, floor - 10}, {pr, floor + 8}, {pl, floor + 8}};
            g_.fill_quad(board, {argb(0x0DFFFFFF), argb(0x1CFFFFFF)});
            const D2D1_POINT_2F front[4] = {board[3], board[2], {pr - 1, floor + 19}, {pl + 1, floor + 19}};
            g_.fill_quad(front, {argb(0x0CFFFFFF), argb(0x04FFFFFF)});
            g_.line(board[3].x, board[3].y, board[2].x, board[2].y, argb(0x30FFFFFF), 1);
            g_.fill_span(Rect{front[3].x, front[3].y, front[2].x, front[3].y + 28}, front[3], {front[3].x, front[3].y + 28},
                         {argb(0x5C000000), argb(0x00000000)});
        }
        for (int c = 0; c < cols; ++c) {
            const int i = row * cols + c;
            if (i >= n) break;
            const bool opening = for_show && zoom_.to == Where{all[size_t(i)].page, all[size_t(i)].playlist};
            if ((only_opening && !opening) || (skip_opening && opening)) continue;
            // The ring only while the keyboard is in use (like a browser's :focus-visible).
            const bool ring = !for_show && keyboard_nav_ && shelf_ring_ && i == shelf_sel_;
            draw_crate(all[size_t(i)], Rect::xywh(x0 + float(c) * (kCrateW + gap), top + kRowTop, kCrateW, kCrateH), full, ring);
        }
    }
    g_.pop_fade();
    ui_.pop_clip();
}

void View::draw_crate(const Crate& c, const Rect& b, bool full, bool chosen) {
    const uint64_t id = Ui::key(c.playlist.value_or(""), uint64_t(c.page));
    const bool over = page_ == Page::shelf ? ui_.hover(b) : zoom_.on && zoom_.to == Where{c.page, c.playlist};
    // It answers the pointer: the box's edge lights, and under full motion its front record lifts and leans toward it.
    const float lit = ui_.anim(Ui::key("crate-lit", id), over ? 1.f : 0.f, 18);
    // While it opens, the crate holds the lean it had when clicked.
    D2D1_POINT_2F to{0, 0};
    if (full && page_ == Page::shelf) to = ui_.lean(b);
    else if (full && over) to = shelf_lean_;
    if (page_ == Page::shelf && over) shelf_lean_ = to;
    // The lean follows the pointer inside this one click region: while it does, paint on every pointer move.
    if (full && page_ == Page::shelf && over) pointer_frames_ = true;
    const float lean_x = ui_.anim(Ui::key("crate-lean-x", id), to.x, 14), lean_y = ui_.anim(Ui::key("crate-lean-y", id), to.y, 14);
    const float lift = full ? lit : 0.f;
    const float cx = (b.l + b.r) / 2, fb = b.b, ft = fb - kFaceH, bt = ft - 24;  // face bottom and top, the back's top

    if (lit > 0.01f) {
        if (!full) g_.fill_round(Rect{b.l - 10, b.t - 10, b.r + 10, b.b + 10}, 16, with_alpha(white, 0.06f * lit));  // no travel: a fill
        g_.fill_glow(cx, b.t + 70, 150, with_alpha(lilac, 0.10f * lit));
    }
    // Its shade on the plank.
    g_.push_transform(D2D1::Matrix3x2F::Scale(1, 0.1f, {cx, fb}));
    g_.fill_glow(cx, fb, kCrateW * 0.62f, argb(0xA0000000));
    g_.pop_transform();
    // Inside the box: the far wall and the floor, in shadow, and the rims of the back and sides catching a little light.
    const D2D1_POINT_2F inside[4] = {{b.l + 15, bt}, {b.r - 15, bt}, {b.r, ft}, {b.l, ft}};
    g_.fill_quad(inside, {argb(0xFF0B0B0E), argb(0xFF141419)});
    g_.line(inside[0].x, inside[0].y, inside[1].x, inside[1].y, argb(0x38FFFFFF), 1.5f);
    g_.line(inside[0].x, inside[0].y, inside[3].x, inside[3].y, argb(0x24FFFFFF), 1.5f);
    g_.line(inside[1].x, inside[1].y, inside[2].x, inside[2].y, argb(0x24FFFFFF), 1.5f);

    // The sleeves, back to front: each further one a little higher, smaller and dimmer, and not quite in line.
    const auto& songs = crate_songs(c.page, c.playlist, 5);
    static constexpr float kStagger[5] = {0, -5, 4, -3, 5};
    // The front one, lifted, rises a good part of the way out of the box and comes a little forward (a touch larger).
    auto sleeve = [&](int i) {
        const float k = float(i), s = kSleeve * (1 - 0.04f * k) * (i == 0 ? 1 + 0.03f * lift : 1);
        const float x = cx + kStagger[i], y = fb - 8 - 12 * k - (i == 0 ? kLift * lift : 0);
        return Rect{x - s / 2, y - s, x + s / 2, y};
    };
    for (int i = int(std::min<size_t>(songs.size(), 5)) - 1; i >= 0; --i) {
        const Rect sr = sleeve(i);
        if (i == 0) {
            // Tilted back a few degrees; it leans with the pointer (the side under it sinks).
            ui_.artwork_3d(songs[0], sr, 3, {6 - lean_y * 9, lean_x * 14});
            continue;
        }
        ui_.artwork(songs[size_t(i)], sr, 3);
        g_.fill_round(sr, 3, with_alpha(black, 0.16f + 0.1f * float(i)));
        g_.line(sr.l + 3, sr.t + 0.5f, sr.r - 3, sr.t + 0.5f, argb(0x1CFFFFFF), 1);
        // The sleeve in front shades this one just above its top edge.
        const float front_top = sleeve(i - 1).t;
        if (front_top - 10 < sr.b && front_top > sr.t)
            g_.fill_span(Rect{sr.l, std::max(sr.t, front_top - 10), sr.r, front_top}, {sr.l, front_top - 10}, {sr.l, front_top},
                         {argb(0x00000000), argb(0x73000000)});
    }
    if (songs.empty()) {
        // Nothing in it: a bare divider card stands in the box, so it reads as an empty crate rather than a missing picture.
        const float y = fb - 8 - 0.5f * kLift * lift;
        const Rect card{cx - 60, y - 80, cx + 52, y}, tab{card.l + 10, card.t - 12, card.l + 48, card.t + 4};
        g_.fill_round(card, 3, argb(0xFF202027));
        g_.line(card.l + 3, card.t + 0.5f, card.r - 3, card.t + 0.5f, argb(0x22FFFFFF), 1);
        g_.fill_round(tab, 3, argb(0xFF202027));
        g_.line(tab.l + 3, tab.t + 0.5f, tab.r - 3, tab.t + 0.5f, argb(0x22FFFFFF), 1);
    }

    // The box's front, over the sleeves' lower part: wider at the top (seen from above), its top edge lit.
    const D2D1_POINT_2F face[4] = {{b.l, ft}, {b.r, ft}, {b.r - 7, fb}, {b.l + 7, fb}};
    g_.fill_quad(face, {argb(0xFF25252D), argb(0xFF17171C)});
    const D2D1_POINT_2F rim[4] = {{b.l + 2, ft - 4}, {b.r - 2, ft - 4}, face[1], face[0]};
    g_.fill_quad(rim, {argb(0xFF3B3B45)});
    if (lit > 0.01f) {
        g_.fill_quad(rim, {with_alpha(light_blue, 0.9f * lit), with_alpha(lilac, 0.9f * lit), with_alpha(peach, 0.9f * lit)}, false);
        g_.line(face[0].x, face[0].y, face[3].x, face[3].y, with_alpha(light_blue, 0.55f * lit), 1);
        g_.line(face[1].x, face[1].y, face[2].x, face[2].y, with_alpha(peach, 0.55f * lit), 1);
    } else {
        g_.line(face[0].x, face[0].y, face[3].x, face[3].y, argb(0x14FFFFFF), 1);
        g_.line(face[1].x, face[1].y, face[2].x, face[2].y, argb(0x14FFFFFF), 1);
    }
    // Its label: the name, how many songs (a playlist: how many of them are here, and a thin line for it).
    const float tl = face[0].x + 16, tr = face[1].x - 16, ty = face[0].y;
    g_.text(c.name, Rect{tl, ty + 9, tr, ty + 29}, {Font::ui, 14, 600, text});
    g_.text(c.count, Rect{tl, ty + 29, tr, ty + 45}, {Font::ui, 12, 400, text2});
    if (c.filled) ui_.progress(Rect{tl, ty + 50, tr, ty + 52.5f}, *c.filled);

    if (chosen) g_.stroke_round(Rect{b.l - 10, b.t - 12, b.r + 10, b.b + 12}, 16, with_alpha(lilac, 0.95f), 2);
    ui_.click(b, [this, pg = c.page, pl = c.playlist, b] { open_crate(pg, pl, b); });
}

// MARK: Keyboard

bool View::shelf_key(WPARAM vk) {
    const int n = shelf_count_, cols = std::max(1, shelf_cols_);
    if (n <= 0) return false;
    const bool none = shelf_sel_ < 0 || shelf_sel_ >= n;
    int i = shelf_sel_;
    switch (vk) {
        case VK_LEFT: i = none ? 0 : std::max(0, i - 1); break;
        case VK_RIGHT: i = none ? 0 : std::min(n - 1, i + 1); break;
        case VK_UP: i = none ? 0 : i >= cols ? i - cols : i; break;
        case VK_DOWN:
            if (none) i = 0;
            else if (i + cols < n) i += cols;
            else if (i / cols < (n - 1) / cols) i = n - 1;  // the row below is shorter: its last crate
            break;
        case VK_HOME: i = 0; break;
        case VK_END: i = n - 1; break;
        case VK_RETURN: {
            if (none) return false;
            if (!shelf_ring_) {  // nothing shows as chosen yet: show it first rather than open something unseen
                shelf_ring_ = shelf_reveal_ = true;
                return true;
            }
            const auto all = crates();
            if (size_t(i) >= all.size()) return false;
            go(all[size_t(i)].page, all[size_t(i)].playlist);  // no zoom: what the keyboard does happens at once
            return true;
        }
        default: return false;
    }
    // The first of these keys shows the ring even when the choice can't move (Left on the first crate).
    const bool shown = shelf_ring_;
    shelf_ring_ = true;
    if (i == shelf_sel_ && shown) return false;
    shelf_sel_ = i;
    shelf_reveal_ = true;
    return true;
}

// MARK: Opening a crate

void View::open_crate(Page p, std::optional<std::string> playlist, const Rect& from) {
    const Where to{p, playlist};
    go(p, std::move(playlist));
    rows_arrive_ = true;
    zoom_ = {};
    if (motion_now() == Motion::full) zoom_ = {true, to, from, -1};
}

bool View::zoom_frame(const Rect& r) {
    // Every paint passes here: remember the last crate shown, so the shelf can choose it when it comes back.
    switch (page_) {
        case Page::all:
        case Page::downloaded:
        case Page::missing:
        case Page::ignored:
        case Page::on_pc:
        case Page::recent:
        case Page::playlist: shelf_from_ = here(); break;
        default: break;
    }
    zoom_page_a_ = 1;
    if (!zoom_.on) return false;
    if (zoom_.start < 0) zoom_.start = ui_.now;
    const float t = float(ui_.now - zoom_.start) / kZoom;
    if (t >= 1 || here() != zoom_.to || !lib_) {  // done, or gone somewhere else meanwhile
        zoom_.on = false;
        return false;
    }
    ui_.keep_animating();
    // The page's own short fade runs out of sight meanwhile, so it doesn't fade in a second time when the zoom ends.
    ui_.enter(arrive("page"), 0, 0.16f);

    // The shelf grows about the crate; it is only a picture now. The crates around it and the planks fade first, so the
    // one clicked stands out at once and is the last of the shelf to go. It keeps the layout it had when clicked: a song
    // chosen earlier brings the inspector back with the crate's page, and the narrower column would put the crates
    // elsewhere; the inspector fades in over it with the page (paint() in view.cpp).
    const float rest_a = 1 - ease_out(t / 0.45f), crate_a = 1 - ease_out((t - 0.1f) / 0.6f);
    if (rest_a > 0.01f || crate_a > 0.01f) {
        const float s = 1 + 0.5f * ease_out(t);
        const Rect sr = shelf_rect_.w() > 0 ? shelf_rect_ : r;
        g_.push_clip(sr);
        g_.push_transform(D2D1::Matrix3x2F::Scale(s, s, {(zoom_.from.l + zoom_.from.r) / 2, (zoom_.from.t + zoom_.from.b) / 2}));
        ui_.set_inert(true);
        for (const auto& [pass, a] : {std::pair{ShelfPass::rest, rest_a}, std::pair{ShelfPass::opening, crate_a}}) {
            if (a <= 0.01f) continue;
            shelf_pass_ = pass;
            g_.push_opacity(a);
            shelf(sr);
            g_.pop_opacity();
        }
        shelf_pass_ = ShelfPass::all;
        ui_.set_inert(false);
        g_.pop_transform();
        g_.pop_clip();
    }
    // The crate's page comes in over the last 75 %, while the crate goes, settling from a touch smaller onto where its
    // covers land.
    const float u = ease_out((t - 0.25f) / 0.75f);
    zoom_page_a_ = u;
    if (u > 0.005f) {
        const bool landed = crate_landing_.w() > 0 && crate_landing_.h() > 0;
        const D2D1_POINT_2F to = landed ? D2D1_POINT_2F{(crate_landing_.l + crate_landing_.r) / 2, (crate_landing_.t + crate_landing_.b) / 2}
                                        : D2D1_POINT_2F{r.l, r.t};
        const float s = 0.97f + 0.03f * u;
        g_.push_opacity(u);
        g_.push_transform(D2D1::Matrix3x2F::Scale(s, s, to));
        page(r);
        g_.pop_transform();
        g_.pop_opacity();
        // Native text boxes can neither fade nor scale: they appear when the zoom ends.
        for (auto& kv : edits_) kv.second.used = false;
    }
    return true;
}

}  // namespace wb::ui
