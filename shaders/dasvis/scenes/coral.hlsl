// Coral: reaction-diffusion (Gray-Scott) growing on a grid 128 cells tall on old PCs, 256 on fast ones (finer coral, the
// same speed across the screen), 2–4 steps a frame, lit as relief in the song's colours. Loud parts grow mazes, calm
// parts split into spots; each kick seeds new growth in three places; a patch dies back on each bar and grows again;
// the drop sends out a wave that clears a ring; the light turns a step on each bar; the snare catches the ridges; the
// hats glint on them.
//
// Entry points: main (the scene: t_sim is the grid), ps_step (one step: t_src is the grid).
// u_sim: the grid's size in cells; z = 1 on the first frame (it seeds a dozen places to start from).
#define DASVIS_SCENE
#include "dasvis.hlsli"

float2 S(int2 c) {  // the grid wraps round, so growth carries on over the edges
    uint2 n = uint2(u_sim.xy);
    return t_src.Load(int3(uint2(c + int2(n)) % n, 0)).xy;
}

float4 ps_step(VsOut v) : SV_Target {
    int2 c = int2(v.pos.xy);
    float2 s = S(c);
    float2 lap = (S(c + int2(1, 0)) + S(c - int2(1, 0)) + S(c + int2(0, 1)) + S(c - int2(0, 1))) * 0.2 +
                 (S(c + int2(1, 1)) + S(c - int2(1, 1)) + S(c + int2(1, -1)) + S(c - int2(1, -1))) * 0.05 - s;
    // Feed and kill: from spots (calm) to coral mazes (loud).
    float e = saturate(LIVE > 0.5 ? ENERGY : PART_ENERGY);
    float feed = lerp(0.0367, 0.0545, e), kill = lerp(0.0649, 0.062, e);
    float r = s.x * s.y * s.y;
    s += float2(lap.x - r + feed * (1 - s.x), 0.5 * lap.y + r - (feed + kill) * s.y);
    // New growth: three seeds on a kick, a dozen to start with.
    uint seeds = u_sim.z > 0.5 ? 12u : (KICK_HIT > 0.5 ? 3u : 0u);
    uint b = uint(max(BEATS, 0)) * 64u + uint(SEED * 65536) * 977u;
    [loop] for (uint i = 0; i < seeds; ++i) {
        float2 at = float2(hashu(b + i * 2u), hashu(b + i * 2u + 1u)) * u_sim.xy;
        if (length(float2(c) - at) < 6) s = float2(0.5, 1);
    }
    // So it never sets solid: a patch dies back at the start of each bar (and grows again), and the drop sends a wave
    // out from the middle that clears a ring.
    uint bar = uint(max(BEATS, 0) / 4) * 8u + uint(SEED * 65536);
    float2 hole = float2(hashu(bar), hashu(bar + 1u)) * u_sim.xy;
    if (BAR < 0.04 && length(float2(c) - hole) < u_sim.y * 0.12) s = float2(1, 0);
    if (DROP > 0 && abs(length(float2(c) - u_sim.xy * 0.5) - (1 - DROP) * u_sim.x * 0.6) < 4) s = float2(1, 0);
    return float4(saturate(s), 0, 0);
}

float G(float2 uv) { return t_sim.SampleLevel(s_clamp, uv, 0).y; }

float3 scene(float2 uv, float2 p) {
    float2 t = 1 / u_sim.xy;
    float v = G(uv);
    float2 g = float2(G(uv + float2(t.x, 0)) - G(uv - float2(t.x, 0)), G(uv + float2(0, t.y)) - G(uv - float2(0, t.y)));
    float3 n = normalize(float3(-g * 4, 0.25));
    float bars = BEATS / 4;
    float la = (floor(bars) + smoothstep(0, 0.5, frac(bars))) * 0.9 + SEED * TAU;  // the light turns a step each bar
    float3 l = normalize(float3(cos(la), sin(la), 0.9));
    float dif = saturate(dot(n, l));
    float spec = pow(saturate(reflect(-l, n).z), 24);
    float m = smoothstep(0.1, 0.3, v);  // coral (1) or the water around it (0)
    float3 base = lerp(u_pal[0].rgb * 0.25 + 0.01, PAL(0.3 + 0.5 * v + 0.15 * uv.x + 0.25 * DRIFT.y + LEAD * 0.15), m);
    float3 c = base * (0.25 + 1.1 * dif) * (0.85 + 0.4 * BASS);
    c += PAL(0.95) * spec * m * (0.4 + 1.6 * SNARE * FLASH);
    // The hats: glints on the ridges.
    float2 cell = floor(uv * u_sim.xy / 3);
    c += PAL(0.95) * step(0.97, hashu(uint(cell.x) * 7919u + uint(cell.y) * 104729u + uint(max(BEATS, 0) * 2) * 31u)) * spec * HATS * 6;
    return c;
}
