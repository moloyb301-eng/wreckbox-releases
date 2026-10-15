#include "dasvis/score.h"

#include <algorithm>
#include <cmath>

namespace wb::dasvis {

namespace {

uint32_t mix(uint32_t x) {  // a well-spread hash of a whole number
    x ^= x >> 16, x *= 0x7FEB352Du, x ^= x >> 15, x *= 0x846CA68Bu, x ^= x >> 16;
    return x;
}

// Each voice's rows, bottom to top: the kick at the bottom, the hats at the top.
constexpr int kLo[Score::kVoices] = {0, 6, 13, 3, 9}, kHi[Score::kVoices] = {3, 9, 16, 6, 13};
// How long a block stays lit when it plays (s): the drums are short, the bass and lead ring on.
constexpr double kHold[Score::kVoices] = {0.16, 0.2, 0.12, 0.4, 0.5};
constexpr size_t kMaxRipples = 900;

}  // namespace

float Score::rand() {
    rng_ ^= rng_ << 13, rng_ ^= rng_ >> 17, rng_ ^= rng_ << 5;
    return float(rng_ >> 8) / 16777216.f;
}

void Score::reset(uint32_t seed, double t) {
    salt_ = mix(seed | 1u);
    rng_ = salt_ | 1u;
    style_ = Style(mix(salt_ ^ 0x5C0Eu) % kStyles);
    cells_.assign(cells_.size(), Cell{});
    pending_.clear(), ripples_.clear();
    now_ = t;
    strike_ = true;
}

size_t Score::blocks() const {
    return size_t(std::count_if(cells_.begin(), cells_.end(), [](const Cell& c) { return c.voice >= 0; }));
}

bool Score::free_for(int col, int row, int voice) const {
    const Cell& c = cells_[size_t(row * kCols + col)];
    return c.voice < 0 || c.voice == voice || c.gone_at >= 0;
}

int Score::place_near(int col, int row, int lo, int hi, int voice) {
    for (int d = 0; d < hi - lo; ++d)
        for (const int r : {row + d, row - d})
            if (r >= lo && r < hi && free_for(col, r, voice)) return r;
    return -1;
}

void Score::place(Voice voice, double at, double when, float strength) {
    long page = long(std::floor(at / 2));
    int col = int(std::floor((at / 2 - double(page)) * kCols + 0.5));
    if (col >= kCols) col = 0, ++page;
    const int lo = kLo[voice], hi = kHi[voice], size = hi - lo;
    const uint32_t h = mix(salt_ ^ uint32_t(voice) * 0x9E3779B9u ^ uint32_t(col) * 0x85EBCA6Bu);
    std::vector<std::pair<int, int>> want;  // (col, row)
    switch (style_) {
    case city: {  // anywhere in its rows; a loud one is a bigger block
        const int r = place_near(col, lo + int(h % uint32_t(size)), lo, hi, voice);
        if (r < 0) break;
        want.push_back({col, r});
        if (strength > 0.7f && (h >> 8) % 4 == 0) want.push_back({col + 1 < kCols ? col + 1 : col - 1, r});
        break;
    }
    case gagaku: {  // the drums and bass run across on their own line; the snare and lead stand straight up
        const int len = strength > 0.7f ? 3 : 2;
        if (voice == snare || voice == lead) {
            const int r0 = lo + int((salt_ >> (voice * 4)) % uint32_t(std::max(1, size - len + 1)));
            for (int k = 0; k < len && r0 + k < hi; ++k) want.push_back({col, r0 + k});
        } else {
            const int r = lo + int((salt_ >> (voice * 3)) % uint32_t(size));
            for (int k = 0; k < len && col + k < kCols; ++k) want.push_back({col + k, r});
        }
        break;
    }
    case koto: {  // escalators: each sixteenth a row further, up for the high voices, down for the low
        const int dir = voice == kick || voice == bass ? -1 : 1, offset = int(salt_ >> (voice * 3)) & 15;
        const int r = lo + (((dir * col + offset) % size) + size) % size;
        if (free_for(col, r, voice)) want.push_back({col, r});
        break;
    }
    case ma: {  // a few tall stacks, far apart, on the beats: only the kick, snare and bass build them
        if (voice == hats || voice == lead) break;
        col = (int(std::lround(col / 8.0)) * 8 + int(salt_ % 4) * 2) % kCols;
        const int base = voice == kick ? 0 : voice == bass ? 2 : 5;
        const int height = 2 + int(std::lround(strength * 6)) + (voice == snare ? 2 : 0);
        for (int k = 0; k < height && base + k < kRows; ++k) want.push_back({col, base + k});
        break;
    }
    default: break;
    }
    if (want.empty()) return;
    Hit hit{when, strength, int(voice), {}};
    const bool ahead = when > now_ + 0.05;  // placed before it plays: the page is written in, left to right
    for (const auto& [c, r] : want) {
        if (c < 0 || c >= kCols || r < 0 || r >= kRows || !free_for(c, r, voice)) continue;
        Cell& cell = cells_[size_t(r * kCols + c)];
        if (cell.voice != int(voice) || cell.gone_at >= 0) cell = Cell{int(voice), now_ + (ahead ? 0.008 * c : 0.0)};
        cell.page = std::max(cell.page, page);
        hit.cells.push_back(r * kCols + c);
    }
    if (!hit.cells.empty()) pending_.push_back(std::move(hit));
}

void Score::throw_rings(int cell, int voice, float strength, double t0) {
    if (ripples_.size() >= kMaxRipples) return;
    const int cx = cell % kCols, cy = cell / kCols;
    const int rings = 1 + int(std::lround(strength * 2)) + (style_ == ma ? 1 : 0);
    const int per = std::max(2, int(std::lround(2 + strength * 3.2f)));
    const float blocks = style_ == koto ? 0.f : style_ == ma ? 0.55f : 0.32f;
    std::vector<int> ring;
    for (int f = 1; f <= rings; ++f) {
        ring.clear();
        for (int y = cy - f; y <= cy + f; ++y)
            for (int x = cx - f; x <= cx + f; ++x)
                if (std::max(std::abs(x - cx), std::abs(y - cy)) == f && x >= 0 && x < kCols && y >= 0 && y < kRows) ring.push_back(y * kCols + x);
        for (int k = 0; k < per && !ring.empty(); ++k) {
            const size_t i = size_t(rand() * float(ring.size())) % ring.size();
            const double born = t0 + (f - 1) * 0.045, fade = born + 0.1 + rand() * 0.12;
            ripples_.push_back({ring[i], voice, rand() < blocks, born, fade, fade + 0.18 + rand() * 0.3});
            ring.erase(ring.begin() + std::ptrdiff_t(i));
        }
    }
}

void Score::fire(const Hit& h, double t) {
    for (const int i : h.cells) {
        Cell& c = cells_[size_t(i)];
        if (c.voice != h.voice) continue;
        c.lit_from = h.when, c.lit_until = h.when + kHold[h.voice] * (0.7 + 0.6 * h.strength);
        ++c.turns, c.turned_at = h.when;
    }
    if (t - h.when < 0.25) throw_rings(h.cells.front(), h.voice, h.strength, h.when);  // not for hits a seek jumped over
}

void Score::update(double bars, double t) {
    now_ = t;
    page_ = long(std::floor(bars / 2));
    head_ = (bars / 2 - double(page_)) * kCols;
    std::erase_if(pending_, [&](const Hit& h) {
        if (h.when > t) return false;
        fire(h, t);
        return true;
    });
    for (int i = 0; i < kCols * kRows; ++i) {
        Cell& c = cells_[size_t(i)];
        if (c.voice < 0) continue;
        if (c.gone_at < 0 && c.page < page_ && (i % kCols < head_ - 0.75 || c.page < page_ - 1)) c.gone_at = t;  // not played this time round
        if (c.gone_at >= 0 && t - c.gone_at > 0.3) c = Cell{};
    }
    if (strike_ && blocks() > 0) {  // the first page flashes in, column by column, each block with a small ring
        strike_ = false;
        int k = 0;
        for (int col = 0; col < kCols; ++col)
            for (int row = 0; row < kRows; ++row)
                if (Cell& c = at(col, row); c.voice >= 0) {
                    c.lit_from = t + 0.15 + 0.018 * k++, c.lit_until = c.lit_from + 0.12;
                    throw_rings(row * kCols + col, c.voice, 0.2f, c.lit_from);
                }
    }
    std::erase_if(ripples_, [t](const Ripple& r) { return r.dead < t; });
}

void Score::burst(double t) {
    for (int i = 0; i < kCols * kRows; ++i) {
        Cell& c = cells_[size_t(i)];
        if (c.voice < 0 || c.gone_at >= 0) continue;
        c.lit_from = t + 0.01 * (i % kCols), c.lit_until = c.lit_from + 0.3;
        if (rand() < 0.4f) throw_rings(i, c.voice, 1, c.lit_from);
    }
}

void Score::write(float* out, double t) const {
    const auto ease = [](double x) {
        x = std::clamp(x, 0.0, 1.0);
        return float(x * x * (3 - 2 * x));
    };
    std::fill(out, out + size_t(kCols * 2 * kRows * 4), 0.f);
    for (int row = 0; row < kRows; ++row)
        for (int col = 0; col < kCols; ++col) {
            const Cell& c = cells_[size_t(row * kCols + col)];
            if (c.voice < 0) continue;
            float* o = out + size_t((row * kCols * 2 + col) * 4);
            const float shown = ease((t - c.shown_at) / 0.12) * (c.gone_at < 0 ? 1.f : 1 - ease((t - c.gone_at) / 0.3));
            const float lit = t < c.lit_from ? 0.f : t < c.lit_until ? 1.f : std::max(0.f, float(1 - (t - c.lit_until) / 0.15));
            o[0] = float(c.voice + 1), o[1] = shown, o[2] = lit;
            o[3] = c.turns > 0 ? float(c.turns - 1) + ease((t - c.turned_at) / 0.12) : 0.f;
        }
    for (const Ripple& r : ripples_) {
        const float v = t < r.born ? 0.f : t <= r.fade ? 1.f : float(1 - (t - r.fade) / std::max(0.001, r.dead - r.fade));
        float* o = out + size_t(((r.cell / kCols) * kCols * 2 + kCols + r.cell % kCols) * 4);
        float& slot = o[r.block ? 1 : 0];
        if (v > slot) slot = v, o[2] = float(r.voice + 1);
    }
}

}  // namespace wb::dasvis
