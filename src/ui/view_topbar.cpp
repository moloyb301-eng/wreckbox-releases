// The top bar: the logo (the way home to the shelf), where you are, the search pill that opens the command palette, and
// the tools that used to sit at the foot of the sidebar. Quiet on purpose: the shelf under it is the one bold thing.
#include <algorithm>
#include <format>

#include "ui/view.h"

namespace wb::ui {

using namespace theme;

void View::top_bar(const Rect& r) {
    ui_.glass(r, 16);
    const float cy = (r.t + r.b) / 2;
    const bool home = page_ == Page::shelf;

    // The logo and the wordmark. Away from the shelf they are a way back to it, and light up under the pointer.
    const float tile = 30, lx = r.l + (r.h() - tile) / 2;
    const TextStyle mark{Font::dot, 16, 700, text, Align::left, 2};
    const float mark_x = lx + tile + 10, mark_w = g_.measure(L"WRECKBOX", mark);
    const Rect brand{lx - 4, cy - 19, mark_x + mark_w + 8, cy + 19};
    if (!home) {
        const float lit = ui_.anim(Ui::key("top-home"), ui_.hover(brand) ? 1.f : 0.f, 16);
        if (lit > 0.01f) g_.fill_round(brand, 12, with_alpha(white, 0.06f * lit));
        ui_.click(brand, [this] { go(Page::shelf); });
    }
    {
        // The record on the tile turns while music plays (paint() turns spin_).
        const Rect t = Rect::xywh(lx, cy - tile / 2, tile, tile);
        g_.fill_gradient(t, tile * 0.225f, {argb(0xFFF1ECE4), argb(0xFFDCD5CA)});
        const float rs = tile * 0.78f, rx = t.l + (tile - rs) / 2, ry = t.t + (tile - rs) / 2;
        g_.push_transform(D2D1::Matrix3x2F::Rotation(spin_, {t.l + tile / 2, t.t + tile / 2}));
        ui_.logo(rx, ry, rs, false);
        g_.pop_transform();
    }
    g_.text(L"WRECKBOX", Rect{mark_x, r.t, mark_x + mark_w + 2, r.b}, mark);
    float x = brand.r + 8;
    g_.fill(Rect{x, cy - 10, x + 1, cy + 10}, hairline);
    x += 11;
    // Where the steps start when the back button is there (right after it: its circle already has room around the
    // glyph). The search pill is sized against this on every page, so it keeps one width as you go from page to page.
    const float steps_l = x + 32;

    // The tools, right to left from the bar's end. The one whose page is open is lit.
    float bx = r.r - 8;
    const auto tool = [&](const wchar_t* glyph, std::wstring tip, std::function<void()> fn, bool on) {
        bx -= 32;
        const Rect b = Rect::xywh(bx, cy - 16, 32, 32);
        ui_.icon_button(b, glyph, 14, std::move(fn), on, false, std::move(tip));
        bx -= 4;
        return b;
    };
    tool(icon::bug, L"Report a bug", [this] { open_bug_report(); }, bug_open_);
    tool(icon::settings, L"Settings", [this] { go(Page::settings); }, page_ == Page::settings);
    tool(icon::phone, L"Sync to phone", [this] { go(Page::phone); }, page_ == Page::phone);
    tool(icon::download, L"Soulseek sync", [this] { go(Page::soulseek); }, page_ == Page::soulseek);
    // How many songs are waiting to download: a badge up and right of the queue's glyph, clear of it (the glyph is about
    // 14 by 10 about the button's centre). While it shows, the queue button stands a little further from the next one,
    // so even "99+" stays off that button's glyph; the room doesn't change with the count, so nothing shifts as it does.
    if (n_priority_ > 0) bx -= 6;
    const Rect queue = tool(icon::queue, n_priority_ ? std::format(L"Download queue, {} waiting", n_priority_) : std::wstring(L"Download queue"),
                            [this] { go(Page::queue); }, page_ == Page::queue);
    tool(icon::globe, L"Web player", [this] { go(Page::web); }, page_ == Page::web);
    if (n_priority_ > 0) {
        const std::wstring n = n_priority_ > 99 ? std::wstring(L"99+") : std::to_wstring(n_priority_);
        const TextStyle bs{Font::ui, 10, 700, argb(0xFF141418), Align::center};
        const Rect badge = Rect::xywh((queue.l + queue.r) / 2 + 5, cy - 19, std::max(15.f, g_.measure(n, bs) + 8), 15);
        g_.fill_round(badge, 7.5f, lilac);
        g_.text(n, badge, bs);
    }

    // The search pill opens the command palette (so does Ctrl+K). When the bar is short of room it gives up width
    // before anything else does, then its Ctrl+K hint; the tools always stay.
    const TextStyle word_st{Font::ui, 13, 500, text2}, key_st{Font::ui, 11, 600, text2, Align::center};
    const float word_w = g_.measure(L"Search", word_st), cap_w = g_.measure(L"Ctrl+K", key_st) + 12;
    const float bare = 34 + word_w + 14, full = 34 + word_w + 12 + cap_w + 7;  // the narrowest it may be, and with the hint
    const float pill_r = bx - 6;
    // Room kept for the steps before the pill may have its full width, and while a video is being made for its chip
    // too (about 144: the chip is drawn at its own width, and the steps get what is left).
    const bool video = video_ && !video_->finished;
    const float keep = video ? 374.f : 230.f;
    const float pill_w = std::clamp(pill_r - steps_l - keep, bare, 220.f);
    const Rect pill{pill_r - pill_w, cy - 16, pill_r, cy + 16};
    const float lit = ui_.anim(Ui::key("top-search"), ui_.hover(pill) ? 1.f : 0.f, 16);
    g_.fill_round(pill, 16, glass_fill);
    if (lit > 0.01f) g_.fill_round(pill, 16, with_alpha(white, 0.045f * lit));
    g_.stroke_round(pill, 16, with_alpha(white, 0.10f + 0.10f * lit));
    g_.icon(icon::search, pill.l + 18, cy, 13, text2);
    const bool hint = pill_w >= full;
    g_.text(L"Search", Rect{pill.l + 34, pill.t, hint ? pill.r - 7 - cap_w - 8 : pill.r - 12, pill.b},
            {Font::ui, 13, 500, with_alpha(white, 0.6f + 0.34f * lit)});
    if (hint) {
        const Rect cap{pill.r - 7 - cap_w, cy - 9, pill.r - 7, cy + 9};
        g_.fill_round(cap, 6, argb(0x0FFFFFFF));
        g_.stroke_round(cap, 6, hairline);
        g_.text(L"Ctrl+K", cap, key_st);
    }
    ui_.click(pill, [this] { open_palette(); });

    // A video being made shows as a chip just left of the pill.
    const float chip_r = pill.l - 8, chip_w = video_status(Rect{steps_l, cy - 16, chip_r, cy + 16});
    const float steps_r = (chip_w > 0 ? chip_r - chip_w : pill.l) - 14;

    // Where you are: the shelf, then the page you are on. The shelf step takes you back to it; the page's name is cut
    // short with an ellipsis rather than run into the pill.
    const TextStyle now_st{Font::ui, 13.5f, 600, text};
    if (home) {
        g_.text(L"Shelf", Rect{x + 8, r.t, steps_r, r.b}, now_st);
        return;
    }
    ui_.icon_button(Rect::xywh(x, cy - 16, 32, 32), icon::chevron_left, 12, [this] { back(); }, false, false, L"Back");
    x += 32;
    std::wstring name;
    switch (page_) {
        case Page::all: name = L"All tracks"; break;
        case Page::downloaded: name = L"In my crate"; break;
        case Page::missing: name = L"Missing"; break;
        case Page::ignored: name = L"Ignored"; break;
        case Page::on_pc: name = L"My folders"; break;
        case Page::recent: name = L"Recently added"; break;
        case Page::playlist: name = playlist_ ? wide(*playlist_) : L"All tracks"; break;
        case Page::web: name = L"Web player"; break;
        case Page::queue: name = L"Download queue"; break;
        case Page::soulseek: name = L"Soulseek sync"; break;
        case Page::phone: name = L"Sync to phone"; break;
        case Page::settings: name = L"Settings"; break;
        case Page::shelf: break;
    }
    const TextStyle was_st{Font::ui, 13.5f, 500, text2};
    const Rect shelf{x, cy - 16, x + g_.measure(L"Shelf", was_st) + 16, cy + 16};
    const float lit_shelf = ui_.anim(Ui::key("top-shelf"), ui_.hover(shelf) ? 1.f : 0.f, 16);
    if (lit_shelf > 0.01f) g_.fill_round(shelf, 16, with_alpha(white, 0.07f * lit_shelf));
    g_.text(L"Shelf", Rect{shelf.l + 8, r.t, shelf.r, r.b}, {Font::ui, 13.5f, 500, with_alpha(white, 0.6f + 0.34f * lit_shelf)});
    ui_.click(shelf, [this] { go(Page::shelf); });
    g_.icon(icon::chevron_right, shelf.r + 6, cy + 0.5f, 9, text3);  // a mark between the steps, not something to read
    const Rect at{shelf.r + 18, r.t, steps_r, r.b};
    g_.text(name, at, now_st);
    if (g_.measure(name, now_st) > at.w()) ui_.tip(at, name);  // cut short: the whole name when the pointer rests on it
}

}  // namespace wb::ui
