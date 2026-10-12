// Ridge Lines: the last few seconds of the spectrum as ridges in 3D (Joy Division's Unknown Pleasures), flowing away
// from you towards a sun on the horizon at dusk. Each ridge is a line with a dark curtain under it, drawn from the back
// to the front so the nearer ones hide what's behind them: no ray marching, so it's cheap on any PC. The bass lights the
// sun; the hats sparkle on the ridge tops; a build-up lifts the camera; the lead colours the newest ridges.
//
// Entry points: main (the sky, the sun and the ground), vs_ridge / ps_ridge (the ridges: t_sim is the spectrum's
// history, one row each 1/30 s). u_sim: ridges to draw, points along each, the newest row, how far it's moved (0–1).
#define DASVIS_SCENE
#include "dasvis.hlsli"

static const float HORIZON = 0.3;  // where the horizon is (up from the middle, in half screen heights)
static const uint HISTORY = 128;   // rows in the history

float3 sky(float2 uv) {
    float2 s = float2((uv.x - 0.5) * ASPECT, 0.5 - uv.y) * 2 - float2(0, HORIZON);  // from the sun, in half heights
    float3 c = lerp(PAL(0.75) * 0.3, u_pal[0].rgb * 0.04, sqrt(saturate(s.y * 2)));  // dusk: bright at the horizon
    c += PAL(0.9) * smoothstep(0.26, 0.25, length(s)) * (0.6 + 2.2 * BASS);          // the sun
    c += PAL(0.85) * 0.03 / (dot(s, s) * 4 + 0.03) * (0.4 + BASS);                   // its glow
    return s.y < 0 ? u_pal[0].rgb * 0.02 + PAL(0.85) * 0.08 * exp(s.y * 12) : c;    // the ground, lit at the horizon
}

float3 scene(float2 uv, float2 p) { return sky(uv); }

struct Ridge {
    float4 pos : SV_Position;
    float3 col : COLOR0;
    float v : TEXCOORD0;    // 0 on the line, 1 at the curtain's foot
    float fog : TEXCOORD1;
};

Ridge vs_ridge(uint id : SV_VertexID) {
    uint rows = uint(u_sim.x), segs = uint(u_sim.y);
    uint quad = id / 6u, corner = id - quad * 6u;
    uint row = quad / segs, seg = quad - row * segs;  // row 0: the farthest, drawn first
    bool right = corner == 1u || corner == 4u || corner == 5u, foot = corner == 2u || corner == 3u || corner == 5u;
    uint age = rows - 1u - row;                       // 0: the newest
    uint hist = (uint(u_sim.z) + HISTORY - age) % HISTORY;
    float x = float(seg + (right ? 1u : 0u)) / float(segs) * 2 - 1;
    // The lows in the middle, the highs out at the sides (45 Hz – 4 kHz), three bins averaged (smoother at the bottom,
    // where the FFT is coarser than the bins); quiet is flat, so the peaks stand out.
    int bin = 16 + int(abs(x) * 170);
    float level = (t_sim.Load(int3(bin - 1, hist, 0)).r + t_sim.Load(int3(bin, hist, 0)).r + t_sim.Load(int3(bin + 1, hist, 0)).r) / 3;
    level = pow(saturate((level - 0.3) / 0.7), 1.5);
    float y = foot ? -0.03 : level * (0.3 + 0.2 * DRIFT.w) * smoothstep(1, 0.3, abs(x)) * sqrt(INTENSITY);
    float z = 0.3 + (float(age) + u_sim.w) * 0.09;
    float cam = 0.3 + 0.35 * BUILD;
    Ridge o;
    o.pos = float4(x * 1.8 * 1.2 / z / ASPECT, HORIZON + (y - cam) / z * 1.2, 0, 1);
    float spark = step(0.97, hashu(seg * 7919u + hist * 104729u)) * HATS * FLASH * 4;
    o.col = PAL(0.45 + 0.25 * abs(x) + 0.004 * float(age) + LEAD * 0.15 * exp(-float(age) * 0.1)) * (0.5 + 2.5 * level + spark);
    o.v = foot ? 1 : 0;
    o.fog = smoothstep(0.3, 1, float(age) / float(rows));
    return o;
}

float4 ps_ridge(Ridge r) : SV_Target {
    float edge = 1 - smoothstep(0, fwidth(r.v) * 1.5, r.v);
    float3 c = lerp(u_pal[0].rgb * 0.02, r.col, edge);
    return float4(lerp(c, sky(r.pos.xy * u_res.zw), r.fog), 1);
}
