// Galaxy: a spiral galaxy of stars drawn as points (64k on old PCs, a million on fast ones). Nothing is stored: each
// star's place is worked out from its number and the time. Every star is on a flattened orbit whose tilt grows with its
// size, which is how real spiral arms form (a density wave): the arms hold their shape while the stars flow through
// them, the inner ones faster. The arms turn a step on each bar; each kick sends a density wave outwards; the bass
// lights the core; the drop is a supernova: the core flashes and a shell of stars flies outwards.
//
// Entry points: main (space: the dust and the core), vs_star / ps_star (the stars). u_sim: the number of stars.
#define DASVIS_SCENE
#include "dasvis.hlsli"

float zoom() { return 1.25 + 0.4 * DRIFT.z; }  // how close: it drifts
float squash() { return 0.62 + 0.12 * sin(TIME * 0.05 + SEED * TAU) - 0.2 * BUILD; }  // how flat it's seen
float roll() { return TIME * 0.015 + SEED * TAU; }
float arms() {  // the arms' turn: a step on each bar
    float bars = BEATS / 4;
    return (floor(bars) + smoothstep(0, 0.4, frac(bars))) * 0.3;
}

float3 scene(float2 uv, float2 p) {
    float2 g = mul(float2(p.x, p.y / squash()) / zoom(), rot(-roll()));  // the galaxy's plane
    float a = length(g);
    float dust = fbm(mul(g, rot(a * 16 + arms())) * 7 + SEED * 10, STEPS(3, 5));
    float3 c = PAL(0.3 + 0.4 * dust) * smoothstep(0.35, 0.8, dust) * exp(-a * 4) * 0.12;
    c += PAL(0.92) * 0.012 / (dot(p, p) * 10 + 0.01) * (0.35 + 0.9 * BASS + 2.5 * DROP * FLASH);  // the core
    return max(c, PREV(uv) * exp(-DT * 20));  // the stars leave short trails
}

struct Star {
    float4 pos : SV_Position;
    float3 col : COLOR0;
};

Star vs_star(uint id : SV_VertexID) {
    float r1 = hashu(id * 4u), r2 = hashu(id * 4u + 1u), r3 = hashu(id * 4u + 2u), r4 = hashu(id * 4u + 3u);
    float a = 0.015 - log(1 - 0.995 * r1) * 0.085;  // the orbit's size: crowded in the middle (an exponential disc)
    float phi = r2 * TAU - TIME * 0.05 / (a + 0.03);  // the inner stars orbit faster
    float2 g = mul(float2(cos(phi), sin(phi) * 0.7) * a, rot(a * 16 + arms() + SEED * TAU));
    // The kick's density wave: a ring running outwards that pushes the stars it passes.
    float w = SINCE_KICK * 0.5;
    g *= 1 + 0.15 * INTENSITY * exp(-SINCE_KICK * 2) * exp(-(a - w) * (a - w) * 300);
    float h = (r3 - 0.5) * 0.05 * exp(-a * 6);  // a bulge in the middle, thin at the edge
    float bright = 0.35 + 3 * pow(r4, 40);       // a few bright stars
    float3 col = PAL(lerp(0.92, 0.35, saturate(a * 3)) + 0.1 * r3);
    // The supernova: one star in ten flies out from the core on the drop.
    if (r4 > 0.9 && DROP > 0) {
        float sa = r2 * TAU, sb = r3 * 2 - 1;
        g = float2(cos(sa), sin(sa)) * sqrt(1 - sb * sb) * (1 - DROP) * 0.9;
        h = sb * (1 - DROP) * 0.4;
        bright = 2.5 * DROP * FLASH, col = PAL(0.95);
    }
    g = mul(g, rot(roll()));
    float sq = squash();
    float2 s = float2(g.x, g.y * sq + h * sqrt(1 - sq * sq)) * zoom();
    Star o;
    o.pos = float4(s.x * 2 / ASPECT, -s.y * 2, 0, 1);
    o.col = col * bright * 256 / sqrt(u_sim.x);  // more stars: each fainter
    return o;
}

float4 ps_star(Star s) : SV_Target { return float4(s.col, 1); }
