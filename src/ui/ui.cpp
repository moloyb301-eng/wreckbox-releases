#include "ui/ui.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <numbers>

namespace wb::ui {

using namespace theme;

std::wstring wide(const std::string& s) { return widen(s); }

namespace {

D2D1_COLOR_F fade(D2D1_COLOR_F c, float k) { return {c.r, c.g, c.b, c.a * k}; }

std::wstring upper(std::wstring s) {
    if (!s.empty()) CharUpperBuffW(s.data(), DWORD(s.size()));
    return s;
}

Rect intersect(const Rect& a, const Rect& b) { return {std::max(a.l, b.l), std::max(a.t, b.t), std::min(a.r, b.r), std::min(a.b, b.b)}; }

}  // namespace

// MARK: Frame & regions

void Ui::begin_frame() {
    hits_.clear();
    tips_.clear();
    scrolls_.clear();
    clips_.clear();
    art.begin_frame();
    using clock = std::chrono::steady_clock;
    static const auto start = clock::now();
    const double t = std::chrono::duration<double>(clock::now() - start).count();
    // The first paint after a rest counts as one ordinary frame, however long the rest was: a hover that starts now
    // shouldn't jump half-way because nothing had been drawn for a minute.
    dt = animating_ ? float(std::clamp(t - now, 0.0, 0.05)) : 1.f / 60;
    now = t;
    animating_ = false;
    if ((++frame_ & 255) == 0) std::erase_if(moving_, [&](const auto& kv) { return frame_ - kv.second.seen > 240; });
}

// MARK: Motion

uint64_t Ui::key(std::string_view name, uint64_t n) {
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    for (const unsigned char c : name) h = (h ^ c) * 1099511628211ull;
    return (h ^ (n + 0x9E3779B97F4A7C15ull)) * 1099511628211ull;
}

uint64_t Ui::key(const Rect& r, uint64_t n) {
    const int32_t v[4] = {int32_t(std::lround(r.l)), int32_t(std::lround(r.t)), int32_t(std::lround(r.r)), int32_t(std::lround(r.b))};
    return key(std::string_view(reinterpret_cast<const char*>(v), sizeof v), n);
}

float Ui::anim(uint64_t k, float target, float speed) {
    const auto [it, fresh] = moving_.try_emplace(k);
    Moving& m = it->second;
    // New, or not drawn in the paint before (another page, scrolled away): it is where it should be, with no travel.
    if (fresh || !motion || frame_ - m.seen > 1) {
        m.value = target;
    } else if (m.value != target) {
        m.value += (target - m.value) * (1 - std::exp(-speed * dt));
        if (std::abs(target - m.value) < 0.004f) m.value = target;
    }
    m.seen = frame_;
    if (m.value != target) animating_ = true;
    return m.value;
}

float Ui::enter(uint64_t k, float delay, float duration) {
    const auto [it, fresh] = moving_.try_emplace(k);
    Moving& m = it->second;
    if (fresh || frame_ - m.seen > 1) m.born = now;
    m.seen = frame_;
    if (!motion) return 1;
    const float t = std::clamp(float(now - m.born - double(delay)) / duration, 0.f, 1.f);
    if (t < 1) animating_ = true;
    const float u = 1 - t;
    return 1 - u * u * u;  // eased out: quick to start, soft to land
}

float Ui::press(const Rect& r) {
    if (!motion || !r.contains(press_x_, press_y_)) return 0;
    const float k = 1 - float(now - press_at_) / 0.28f;
    if (k <= 0) return 0;
    animating_ = true;
    return k;
}

D2D1_POINT_2F Ui::lean(const Rect& r) const {
    if (!hover(r) || r.w() <= 0 || r.h() <= 0) return {0, 0};
    return {std::clamp((mx - (r.l + r.r) / 2) / (r.w() / 2), -1.f, 1.f), std::clamp((my - (r.t + r.b) / 2) / (r.h() / 2), -1.f, 1.f)};
}

Rect Ui::clip_or_all() const { return clips_.empty() ? Rect{-1e6f, -1e6f, 1e6f, 1e6f} : clips_.back(); }

bool Ui::hover(const Rect& r) const { return r.contains(mx, my) && clip_or_all().contains(mx, my); }

void Ui::click(const Rect& r, std::function<void()> on_click, std::function<void()> on_right) {
    const Rect c = intersect(r, clip_or_all());
    if (c.w() > 0 && c.h() > 0) hits_.push_back({c, std::move(on_click), std::move(on_right)});
}

void Ui::push_clip(const Rect& r) {
    const Rect c = intersect(r, clip_or_all());
    clips_.push_back(c);
    g.push_clip(c);
}

void Ui::pop_clip() {
    clips_.pop_back();
    g.pop_clip();
}

void Ui::scroll_area(const Rect& r, float& offset, float content_height) {
    const float max = std::max(0.f, content_height - r.h());
    offset = std::clamp(offset, 0.f, max);
    scrolls_.push_back({r, &offset, max});
    if (max <= 0) return;
    const float track = r.h() - 8;
    const float thumb = std::max(28.f, track * r.h() / content_height);
    const float y = r.t + 4 + (track - thumb) * (offset / max);
    g.fill_round(Rect::xywh(r.r - 7, y, 4, thumb), 2, argb(hover(r) ? 0x40FFFFFF : 0x22FFFFFF));
}

void Ui::slider(const Rect& r, float value, std::function<void(float, bool)> on_change, bool vertical, float origin) {
    value = std::clamp(value, 0.f, 1.f);
    // Under the pointer the track thickens, the thumb grows and a light comes up behind it.
    const float h = anim(key(r, 11), hover(r) || (drag_.slide && r.contains(press_x_, press_y_)) ? 1.f : 0.f, 18);
    const float thumb = 5.5f + 2 * h, half = 1.5f + 0.75f * h;
    if (vertical) {
        const float cx = (r.l + r.r) / 2, y = r.b - r.h() * value, y0 = r.b - r.h() * origin;
        g.fill_round(Rect{cx - half, r.t, cx + half, r.b}, half, hairline);
        const Rect fill{cx - half, std::min(y, y0), cx + half, std::max(y, y0)};
        if (fill.h() > 0.5f) g.fill_gradient(fill, half, {peach, lilac, light_blue}, true);
        if (h > 0.01f) g.fill_glow(cx, y, 18, with_alpha(lilac, 0.35f * h));
        g.fill_circle(cx, y, thumb, white);
    } else {
        const float cy = (r.t + r.b) / 2, x = r.l + r.w() * value, x0 = r.l + r.w() * origin;
        g.fill_round(Rect{r.l, cy - half, r.r, cy + half}, half, hairline);
        const Rect fill{std::min(x, x0), cy - half, std::max(x, x0), cy + half};
        if (fill.w() > 0.5f) g.fill_span(fill, {r.l, cy}, {r.r, cy}, {light_blue, lilac, peach});
        if (h > 0.01f) g.fill_glow(x, cy, 18, with_alpha(lilac, 0.35f * h));
        g.fill_circle(x, cy, thumb, white);
    }
    if (!on_change) return;
    const Rect c = intersect(r, clip_or_all());
    if (c.w() <= 0 || c.h() <= 0) return;
    hits_.push_back({c, {}, {}, [r, vertical, fn = std::move(on_change)](float x, float y, bool done) {
                         fn(std::clamp(vertical ? (r.b - y) / r.h() : (x - r.l) / r.w(), 0.f, 1.f), done);
                     }});
}

void Ui::tip(const Rect& r, std::wstring label) {
    const Rect c = intersect(r, clip_or_all());
    if (c.w() > 0 && c.h() > 0) tips_.push_back({c, std::move(label)});
}

unsigned Ui::draw_tooltip() {
    const Tip* over = nullptr;
    for (auto it = tips_.rbegin(); it != tips_.rend() && !over; ++it)
        if (it->r.contains(mx, my)) over = &*it;
    if (!over) {
        tip_at_ = {};
        return 0;
    }
    const auto now = GetTickCount64();
    if (over->r.l != tip_at_.l || over->r.t != tip_at_.t || over->r.r != tip_at_.r || over->r.b != tip_at_.b) {
        tip_at_ = over->r;  // a new thing under the pointer: start waiting (the text may change while resting, e.g. Play → Pause)
        tip_since_ = now;
        tip_dismissed_ = false;
    }
    if (tip_dismissed_) return 0;
    if (now - tip_since_ < 500) return unsigned(500 - (now - tip_since_));
    const TextStyle st{Font::ui, 12, 500, text};
    const float w = g.measure(over->text, st) + 20, h = 26, W = g.width(), H = g.height();
    const float x = std::clamp((over->r.l + over->r.r - w) / 2, 4.f, std::max(4.f, W - w - 4));
    float y = over->r.t - h - 6;  // above (the player bar is at the bottom), else below
    if (y < 4) y = std::min(over->r.b + 6, H - h - 4);
    const Rect box = Rect::xywh(x, y, w, h);
    g.fill_round(box, 8, argb(0xFF1C1C21));
    g.stroke_round(box, 8, glass_border);
    g.text(over->text, Rect{box.l + 10, box.t, box.r, box.b}, st);
    return 0;
}

bool Ui::mouse_down(float x, float y, bool right) {
    tip_dismissed_ = true;  // a click hides the tooltip, as in Windows
    press_x_ = x, press_y_ = y, press_at_ = now;  // what was pressed gives a little (press())
    // The right-hand 10 DIPs of a scrolling area are its scrollbar: grab it.
    if (!right)
        for (auto it = scrolls_.rbegin(); it != scrolls_.rend(); ++it) {
            if (it->max <= 0 || !it->r.contains(x, y) || x < it->r.r - 10) continue;
            const float content = it->r.h() + it->max, track = it->r.h() - 8;
            const float thumb = std::max(28.f, track * it->r.h() / content);
            drag_ = {it->offset, it->max, track - thumb, y, *it->offset};
            const float thumb_y = it->r.t + 4 + (track - thumb) * (*it->offset / it->max);
            if (y < thumb_y || y > thumb_y + thumb) {  // clicked the track: jump so the thumb centres on the pointer
                *it->offset = std::clamp((y - it->r.t - 4 - thumb / 2) / (track - thumb) * it->max, 0.f, it->max);
                drag_.grab_offset = *it->offset;
            }
            return true;
        }
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (!it->r.contains(x, y)) continue;
        if (it->slide) {
            if (right) return false;
            drag_ = {};
            drag_.slide = it->slide;
            drag_.slide(x, y, false);
            return true;
        }
        auto& fn = right ? it->right : it->click;
        if (!fn) return false;
        fn();  // may change state; the caller repaints
        return true;
    }
    return false;
}

void Ui::mouse_up() {
    if (drag_.slide) drag_.slide(mx, my, true);
    drag_ = {};
}

bool Ui::mouse_drag(float x, float y) {
    if (drag_.slide) {
        drag_.slide(x, y, false);
        return true;
    }
    if (!drag_.offset) return false;
    if (drag_.track > 0) *drag_.offset = std::clamp(drag_.grab_offset + (y - drag_.grab_y) / drag_.track * drag_.max, 0.f, drag_.max);
    return true;
}

bool Ui::wheel(float x, float y, float delta) {
    for (auto it = scrolls_.rbegin(); it != scrolls_.rend(); ++it) {
        if (!it->r.contains(x, y) || it->max <= 0) continue;
        *it->offset = std::clamp(*it->offset - delta, 0.f, it->max);
        return true;
    }
    return false;
}

int Ui::hit_index(float x, float y) const {
    for (int i = int(hits_.size()) - 1; i >= 0; --i)
        if (hits_[size_t(i)].r.contains(x, y)) return i;
    // Regions with only a tooltip (a disabled button) count too, so moving onto one repaints and starts its tooltip.
    for (int i = int(tips_.size()) - 1; i >= 0; --i)
        if (tips_[size_t(i)].r.contains(x, y)) return int(hits_.size()) + i;
    return -1;
}

// MARK: Widgets

void Ui::glass(const Rect& r, float radius, bool smart, bool solid) {
    // A sheet of dark glass lit from above: a faint sheen from its top-left corner, and an edge that catches the light
    // at the top and fades down the sides.
    if (solid) {
        g.fill_round(r, radius, bg_raised);  // opaque: without a backdrop blur, see-through glass would show ghosting
        g.fill_gradient(r, radius, {argb(0x12FFFFFF), argb(0x03FFFFFF)}, true);
    } else if (smart) {
        g.fill_gradient(r, radius, {with_alpha(light_blue, 0.16f), with_alpha(peach, 0.14f), with_alpha(lilac, 0.18f)});
    } else {
        g.fill_gradient(r, radius, {argb(0x16FFFFFF), argb(0x07FFFFFF), argb(0x05FFFFFF)}, true);
    }
    if (smart) g.stroke_round(r, radius, with_alpha(lilac, 0.45f));
    else g.stroke_gradient(r, radius, {argb(0x3CFFFFFF), argb(0x14FFFFFF), argb(0x0CFFFFFF)}, 1, true);
}

float Ui::card(const Rect& r, float radius, uint64_t k, bool interactive) {
    const float h = interactive ? anim(k, hover(r) ? 1.f : 0.f, 14) : 0.f;
    if (h > 0.01f) g.shadow(r, radius, 16 * h, 0.55f * h);
    glass(r, radius);
    if (h > 0.01f) {
        // The pointer carries a light across the card, and the card's edge takes the accent colours.
        g.push_clip(r.inset(1));
        g.fill_glow(mx, my, std::max(r.w(), r.h()) * 0.9f, with_alpha(white, 0.085f * h));
        g.pop_clip();
        g.stroke_gradient(r, radius, {with_alpha(light_blue, 0.75f * h), with_alpha(lilac, 0.75f * h), with_alpha(peach, 0.75f * h)}, 1.3f);
    }
    return h;
}

void Ui::artwork_3d(const LibraryTrack& t, const Rect& r, float radius, D2D1_POINT_2F tilt, float opacity, float depth) {
    if (ID2D1Bitmap* bmp = art.get(t, r.w())) return g.image_3d(bmp, r, radius, tilt.x, tilt.y, opacity, depth);
    artwork(t, r, radius, opacity);
}

void Ui::ring(float cx, float cy, float radius, double value, float width) {
    const float v = float(std::clamp(value, 0.0, 1.0));
    g.stroke_circle(cx, cy, radius, hairline, width);
    if (v < 0.004f) return;
    // Three arcs in the accent colours, end to end, so the ring shades from blue to peach as it fills.
    const float sweep = 360 * v;
    const D2D1_COLOR_F cols[3] = {light_blue, lilac, peach};
    for (int i = 0; i < 3; ++i) {
        const float a0 = sweep * float(i) / 3, a1 = sweep * float(i + 1) / 3;
        g.arc(cx, cy, radius, a0, a1 - a0 + (i < 2 ? 1.5f : 0), cols[i], width);
    }
    const float end = sweep * 3.14159265f / 180;
    g.fill_glow(cx + radius * std::sin(end), cy - radius * std::cos(end), width * 3.2f, with_alpha(peach, 0.5f));
}

float Ui::dot_label(const std::wstring& s, float x, float cy, D2D1_COLOR_F c, float size) {
    const TextStyle st{Font::dot, size, 700, c, Align::left, 1.6f};
    const std::wstring u = upper(s);
    const float w = g.measure(u, st);
    g.text(u, Rect{x, cy - size, x + w + 2, cy + size}, st);
    return w;
}

float Ui::pill_width(const std::wstring& label, const wchar_t* icon) {
    return 28 + (icon ? 22 : 0) + g.measure(label, {Font::ui, 13, 600});
}

float Ui::pill(float x, float y, const std::wstring& label, const wchar_t* icon, Pill style, std::function<void()> on_click) {
    const float w = pill_width(label, icon), h = 34;
    const Rect r = Rect::xywh(x, y, w, h);
    const float k = on_click ? 1.f : 0.45f;  // disabled
    // Lit: it glows and its edge takes the accent colours. Pressed: it gives a little, then springs back.
    const float lit = on_click ? anim(key(r, 21), hover(r) ? 1.f : 0.f, 16) : 0.f;
    const float squeeze = on_click ? press(r) : 0.f;
    const bool moved = squeeze > 0 || lit > 0.01f;
    if (moved) g.push_transform(D2D1::Matrix3x2F::Scale(1 + 0.025f * lit - 0.06f * squeeze, 1 + 0.025f * lit - 0.06f * squeeze, {(r.l + r.r) / 2, (r.t + r.b) / 2}));
    if (lit > 0.01f)
        g.fill_glow((r.l + r.r) / 2, (r.t + r.b) / 2, w * 0.75f, with_alpha(style == Pill::primary ? white : lilac, (style == Pill::primary ? 0.16f : 0.20f) * lit));
    switch (style) {
        case Pill::primary: g.fill_round(r, h / 2, fade(white, k)); break;
        case Pill::smart:
            g.fill_gradient(r, h / 2, {fade(with_alpha(light_blue, 0.3f + 0.15f * lit), k), fade(with_alpha(peach, 0.25f + 0.15f * lit), k),
                                       fade(with_alpha(lilac, 0.3f + 0.15f * lit), k)});
            break;
        case Pill::glass: g.fill_gradient(r, h / 2, {fade(argb(0x1CFFFFFF), k), fade(argb(0x0AFFFFFF), k)}, true); break;
    }
    if (style == Pill::primary) g.stroke_round(r, h / 2, fade(white, k));
    else if (style == Pill::smart) g.stroke_round(r, h / 2, fade(with_alpha(lilac, 0.7f), k));
    else g.stroke_gradient(r, h / 2, {fade(argb(0x36FFFFFF), k), fade(argb(0x10FFFFFF), k)}, 1, true);
    if (lit > 0.01f && style != Pill::primary)
        g.stroke_gradient(r, h / 2, {with_alpha(light_blue, 0.8f * lit), with_alpha(lilac, 0.8f * lit), with_alpha(peach, 0.8f * lit)}, 1.2f);
    const auto fg = fade(style == Pill::primary ? black : text, k);
    float tx = x + 14;
    if (icon) {
        g.icon(icon, tx + 7.5f, y + h / 2, 14, fg);
        tx += 22;
    }
    g.text(label, Rect{tx, y, x + w, y + h}, {Font::ui, 13, 600, fg});
    if (moved) g.pop_transform();
    if (on_click) click(r, std::move(on_click));
    return w;
}

float Ui::chip_width(const std::wstring& label, std::optional<int> count) {
    float w = 24 + g.measure(label, {Font::ui, 12.5f, 600});
    if (count) w += 6 + g.measure(std::to_wstring(*count), {Font::dot, 11, 700});
    return w;
}

float Ui::chip(float x, float y, const std::wstring& label, std::optional<int> count, bool on, bool smart, std::function<void()> on_click) {
    const float w = chip_width(label, count) + (on ? 12 : 0), h = 30;
    const Rect r = Rect::xywh(x, y, w, h);
    const float lit = on_click && !on ? anim(key(r, 22), hover(r) ? 1.f : 0.f, 16) : 0.f;
    if (on) g.fill_round(r, h / 2, white);
    else if (smart) g.fill_gradient(r, h / 2, {with_alpha(light_blue, 0.22f), with_alpha(lilac, 0.22f)});
    else g.fill_gradient(r, h / 2, {argb(0x18FFFFFF), argb(0x08FFFFFF)}, true);
    g.stroke_round(r, h / 2, on ? white : smart ? with_alpha(lilac, 0.6f) : hairline);
    if (lit > 0.01f) {
        g.fill_round(r, h / 2, with_alpha(white, 0.06f * lit));
        g.stroke_gradient(r, h / 2, {with_alpha(light_blue, 0.7f * lit), with_alpha(lilac, 0.7f * lit), with_alpha(peach, 0.7f * lit)}, 1.1f);
    }
    const auto fg = on ? black : smart ? text : text2;
    float tx = x + 12;
    if (on) {
        g.fill_circle(tx + 3, y + h / 2, 3, lilac);
        tx += 12;
    }
    const float lw = g.measure(label, {Font::ui, 12.5f, 600});
    g.text(label, Rect{tx, y, tx + lw + 1, y + h}, {Font::ui, 12.5f, 600, fg});
    if (count) g.text(std::to_wstring(*count), Rect{tx + lw + 6, y, x + w, y + h}, {Font::dot, 11, 700, fade(fg, 0.6f)});
    if (on_click) click(r, std::move(on_click));
    return w;
}

float Ui::key_badge(float x, float cy, const std::string& camelot, bool unsure, bool large) {
    const float size = large ? 22.f : 13.f;
    if (camelot.empty()) {
        g.text(L"–", Rect{x, cy - size, x + size * 2, cy + size}, {Font::dot, size, 700, text3});
        return size;
    }
    const auto c = theme::camelot(camelot);
    const std::wstring s = wide(camelot) + (unsure ? L"?" : L"");
    const TextStyle st{Font::dot, size, 700, c};
    const float tw = g.measure(s, st), px = large ? 12.f : 8.f, h = size * 1.1f + (large ? 10.f : 6.f);
    const Rect r = Rect::xywh(x, cy - h / 2, tw + px * 2, h);
    g.fill_round(r, h / 2, with_alpha(c, 0.16f));
    g.stroke_round(r, h / 2, with_alpha(c, 0.35f));
    g.text(s, Rect{x + px, r.t, r.r, r.b}, st);
    return r.w();
}

void Ui::bpm_readout(float x, float cy, std::optional<double> bpm, bool unsure, float size) {
    const std::wstring s = bpm ? std::to_wstring(std::lround(*bpm)) : L"–";
    const TextStyle st{Font::dot, size, 700, bpm ? text : text3};
    const float w = g.measure(s, st);
    g.text(s, Rect{x, cy - size, x + w + 1, cy + size}, st);
    if (unsure) g.text(L"?", Rect{x + w, cy - size * 0.7f, x + w + size, cy + size * 0.5f}, {Font::dot, size * 0.7f, 700, peach});
}

void Ui::energy_meter(float x, float bottom, std::optional<double> value, float height) {
    const int level = value ? int(std::ceil(*value * 5)) : 0;
    const float k = value ? 1.f : 0.5f;
    for (int i = 0; i < 5; ++i) {
        const float h = height * (0.5f + float(i) * 0.125f);
        const Rect bar = Rect::xywh(x + float(i) * 6, bottom - h, 4, h);
        if (i < level) g.fill_gradient(bar, 1.5f, {fade(light_blue, k), fade(peach, k), fade(lilac, k)}, true);
        else g.fill_round(bar, 1.5f, fade(hairline, k));
    }
}

void Ui::status_dot(float cx, float cy, TrackStatus s, float size) {
    switch (s) {
        case TrackStatus::downloaded:
            g.fill_circle(cx, cy, size / 2, lilac);
            g.icon(icon::check, cx, cy, size * 0.6f, black);
            break;
        case TrackStatus::missing: {
            const float r = size / 2 - 1;
            g.stroke_circle(cx, cy, r, text3, 1.3f, float(2 * std::numbers::pi * r / 24));  // 12 dashes, like the original painter
            break;
        }
        case TrackStatus::ignored: g.icon(icon::block, cx, cy, size * 0.9f, text3); break;
    }
}

void Ui::artwork(const LibraryTrack& t, const Rect& r, float radius, float opacity) {
    if (ID2D1Bitmap* bmp = art.get(t, r.w())) {
        g.image(bmp, r, radius, opacity);
        return;
    }
    const auto c = tint(t.album.value_or(t.id));
    g.fill_gradient(r, radius, {with_alpha(c, 0.55f * opacity), with_alpha(c, 0.15f * opacity)});
    g.icon(icon::music, (r.l + r.r) / 2, (r.t + r.b) / 2, r.w() * 0.35f, with_alpha(white, 0.35f * opacity));
}

// The WreckBox logo: a 32×32 pixel vinyl record (same drawing as the Mac app's PixelRecord).
void Ui::logo(float x, float y, float size, bool with_tile) {
    // ~800 little squares: drawn once into a bitmap, then reused.
    g.cached(std::format(L"logo{}{}", size, with_tile ? 1 : 0), Rect::xywh(x, y, size, size), [x, y, size, with_tile](Gfx& gg) {
        float rs = size, rx = x, ry = y;
        if (with_tile) {
            gg.fill_gradient(Rect::xywh(x, y, size, size), size * 0.225f, {argb(0xFFF1ECE4), argb(0xFFDCD5CA)});
            rs = size * 0.78f;
            rx = x + (size - rs) / 2;
            ry = y + (size - rs) / 2;
        }
        const float px = rs / 32;
        for (int cy = 0; cy < 32; ++cy) {
            for (int cx = 0; cx < 32; ++cx) {
                const float dx = float(cx) - 15.5f, dy = float(cy) - 15.5f;
                const float r = std::sqrt(dx * dx + dy * dy);
                const float angle = float(std::atan2(dy, dx) * 180 / std::numbers::pi);
                uint32_t c = 0;
                if (r < 1.6f) continue;
                if (r < 3.6f) c = 0xFFBB96DA;
                else if (r < 5.6f) c = 0xFFEFAF86;
                else if (r < 6.3f) c = 0xFF050506;
                else if (r < 14.6f) {
                    const bool glint = (angle > -150 && angle < -118) || (angle > 30 && angle < 62);
                    const bool band = int(std::floor((r - 6.3f) / 1.7f)) % 2 == 0;
                    c = glint ? (band ? 0xFF4A4A56 : 0xFF5C5C6A) : (band ? 0xFF15151A : 0xFF202027);
                } else if (r < 15.6f) c = 0xFF34343E;
                else continue;
                gg.fill(Rect::xywh(rx + float(cx) * px, ry + float(cy) * px, px + 0.5f, px + 0.5f), argb(c));
            }
        }
    });
}

void Ui::icon_button(const Rect& r, const wchar_t* glyph, float size, std::function<void()> on_click, bool on, bool filled,
                     std::wstring tooltip) {
    if (!tooltip.empty()) tip(r, std::move(tooltip));
    const float cx = (r.l + r.r) / 2, cy = (r.t + r.b) / 2, rad = std::min(r.w(), r.h()) / 2;
    const float k = on_click ? 1.f : 0.4f;
    const float lit = on_click ? anim(key(r, 23), hover(r) ? 1.f : 0.f, 16) : 0.f;
    const float squeeze = on_click ? press(r) : 0.f;
    const float s = 1 + 0.08f * lit - 0.12f * squeeze;
    if (filled) {
        // The play button: a halo in the accent colours comes up around it.
        if (lit > 0.01f) g.fill_glow(cx, cy, rad * 2.3f, with_alpha(lilac, 0.45f * lit));
        g.fill_circle(cx, cy, rad * s, fade(white, k));
    } else {
        if (on) g.fill_circle(cx, cy, rad, with_alpha(lilac, 0.22f));
        if (lit > 0.01f) g.fill_circle(cx, cy, rad * s, with_alpha(white, 0.09f * lit));
    }
    g.icon(glyph, cx, cy, size * (1 + 0.06f * lit - 0.1f * squeeze), filled ? black : fade(on ? lilac : text, k));
    if (on_click) click(r, std::move(on_click));
}

void Ui::progress(const Rect& r, double value, uint64_t k) {
    g.fill_round(r, r.h() / 2, hairline);
    // With a name it fills when it first appears and follows smoothly when the number behind it changes. Without
    // one (a bar in a scrolling list, which has no steady identity) it is simply drawn.
    float v = float(std::clamp(value, 0.0, 1.0));
    if (k) v = anim(k, v, 6) * enter(k ^ 0x5bd1e995, 0.1f, 0.7f);
    if (v <= 0) return;
    const Rect fill{r.l, r.t, r.l + std::max(r.h(), r.w() * v), r.b};
    g.fill_span(fill, {r.l, r.t}, {r.r, r.t}, {light_blue, lilac, peach});
}

}  // namespace wb::ui
