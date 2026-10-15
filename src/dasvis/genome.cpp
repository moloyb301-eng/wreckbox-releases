#include "dasvis/genome.h"

#include <algorithm>
#include <cstddef>

namespace wb::dasvis {

namespace {

float rnd(uint32_t seed, uint32_t salt) {  // 0–1, depends only on the seed and the salt (splitmix64)
    uint64_t z = (uint64_t(seed) << 32 | salt) + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return float(double((z ^ (z >> 31)) >> 11) / double(1ull << 53));
}

// Each number: its range, and where it lives in the genome.
struct Range {
    float Genome::*field;
    float lo, hi;
};
constexpr Range kRanges[] = {
    {&Genome::mirrors, 3, 9},   {&Genome::density, 1.5f, 7}, {&Genome::edge, 0.02f, 0.18f}, {&Genome::core, 0.15f, 0.6f},
    {&Genome::grain, 0, 1},     {&Genome::hue, 0, 1},        {&Genome::spread, 0.15f, 1},   {&Genome::speed, 0.5f, 1.6f},
    {&Genome::glow, 0.2f, 1.2f},
};

}  // namespace

Genome make_genome(uint32_t seed) {
    Genome g;
    const float pick = rnd(seed, 1);  // cells most often: the "zooming into skin" look
    g.genus = pick < 0.4f ? Genome::cells : pick < 0.7f ? Genome::organelles : Genome::tunnel;
    // One to three warps; the first is always there, the others less often.
    for (int i = 0; i < 3; ++i) {
        const bool on = i == 0 ? rnd(seed, 10) < 0.85f : rnd(seed, 10 + uint32_t(i)) < 0.45f - 0.15f * float(i);
        g.warps[i] = on ? Genome::Warp(1 + std::min(int(Genome::kWarps) - 2, int(rnd(seed, 20 + uint32_t(i)) * (Genome::kWarps - 1)))) : Genome::none;
        g.amount[i] = 0.2f + 0.8f * rnd(seed, 30 + uint32_t(i));
    }
    for (size_t i = 0; i < std::size(kRanges); ++i) {
        const Range& r = kRanges[i];
        g.*r.field = r.lo + (r.hi - r.lo) * rnd(seed, 40 + uint32_t(i));
    }
    g.mirrors = float(int(g.mirrors + 0.5f));
    return g;
}

Genome mutate(const Genome& g, uint32_t seed, float amount) {
    Genome m = g;
    for (size_t i = 0; i < std::size(kRanges); ++i) {
        const Range& r = kRanges[i];
        if (r.field == &Genome::mirrors || r.field == &Genome::hue) continue;  // a count, and the colours: they stay
        const float to = r.lo + (r.hi - r.lo) * rnd(seed, 100 + uint32_t(i));
        m.*r.field = std::clamp(g.*r.field + (to - g.*r.field) * amount, r.lo, r.hi);
    }
    for (int i = 0; i < 3; ++i) m.amount[i] = std::clamp(g.amount[i] + (rnd(seed, 200 + uint32_t(i)) - 0.5f) * amount, 0.2f, 1.f);
    return m;
}

void ease(Genome& on, const Genome& to, float k) {
    for (const Range& r : kRanges)
        if (r.field != &Genome::mirrors) on.*r.field += (to.*r.field - on.*r.field) * k;
    for (int i = 0; i < 3; ++i) on.amount[i] += (to.amount[i] - on.amount[i]) * k;
}

void pack(const Genome& g, float out[4][4]) {
    const float v[4][4] = {{float(g.genus), float(g.warps[0]), float(g.warps[1]), float(g.warps[2])},
                           {g.amount[0], g.amount[1], g.amount[2], g.mirrors},
                           {g.density, g.edge, g.core, g.grain},
                           {g.hue, g.spread, g.speed, g.glow}};
    std::copy(&v[0][0], &v[0][0] + 16, &out[0][0]);
}

}  // namespace wb::dasvis
