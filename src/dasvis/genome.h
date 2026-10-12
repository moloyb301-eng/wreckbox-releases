// A generated world for the Journey scene (shaders/dasvis/scenes/journey.hlsl): what it's made of (its genus), how space
// is bent before it's drawn (up to three warps), and the numbers that shape it. Drawn from a seed (a new one for each
// world, and each play) and nudged a little each phrase, so it evolves while you watch. Every genome is valid: each
// number is drawn and kept inside a range that looks right.
#pragma once
#include <cstdint>

namespace wb::dasvis {

struct Genome {
    enum Genus : uint8_t { cells, organelles, tunnel, kGenera };
    // twist: turns more towards the edge; mirror: a kaleidoscope; fold: folded over itself (as fractals are); flow: a
    // fluid noise warp; spiral: log-polar (anything becomes a spiral tunnel); bulge: a lens; wave: ripples.
    enum Warp : uint8_t { none, twist, mirror, fold, flow, spiral, bulge, wave, kWarps };
    Genus genus = cells;
    Warp warps[3] = {};
    float amount[3] = {};  // each warp's strength, 0.2–1
    float mirrors = 6;     // for `mirror`: 3–9
    float density = 4;     // features across the screen's height, 1.5–7
    float edge = 0.08f;    // membrane / wall line width, 0.02–0.18
    float core = 0.3f;     // nucleus / blob core size, 0.15–0.6
    float grain = 0.5f;    // texture inside, 0–1
    float hue = 0;         // where on the palette it starts, 0–1
    float spread = 0.5f;   // how much of the palette it uses, 0.15–1
    float speed = 1;       // how fast it zooms and churns, 0.5–1.6
    float glow = 0.5f;     // edge glow, 0.2–1.2
};

Genome make_genome(uint32_t seed);
// The same genus and warps, each number moved up to `amount` of its range (0–1) towards a new one, kept in range.
Genome mutate(const Genome& g, uint32_t seed, float amount);
// Eases `on` towards `to` by k (0–1): the numbers only (genus and warps switch with a new world, not mid-way).
void ease(Genome& on, const Genome& to, float k);
// The shader's u_gen[4].
void pack(const Genome& g, float out[4][4]);

}  // namespace wb::dasvis
