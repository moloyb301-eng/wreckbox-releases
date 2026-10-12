// The particle layer, which the director puts over some scenes (Feedback Classic, Neon Tunnel, Kaleido Cover): N × N particles (16k on old PCs, 65k on fast ones) whose position and
// velocity live in a float texture, moved on by a pixel shader each frame and drawn as points added to the picture (the
// feedback turns them into trails). They drift on curl noise (it swirls but never bunches up), faster with the lead;
// each kick throws a share of them off the ring; the hats make a few sparkle.
//
// Entry points: ps_update (one step: t_src is the state), vs_draw / ps_draw (the points: t_sim is the state).
// u_sim: N, N, frames since the layer was made.
#include "dasvis.hlsli"

// Where the particles are born: a ring round the middle (Feedback Classic's ring, the tunnel's far end).
float ring_r() { return 0.2 * (1 - 0.35 * BUILD) * (1 + 0.6 * DROP); }

float4 ps_update(VsOut v) : SV_Target {
    uint2 ij = uint2(v.pos.xy);
    float4 s = t_src.Load(int3(ij, 0));
    float2 pos = s.xy, vel = s.zw;
    uint id = ij.y * uint(u_sim.x) + ij.x, frame = uint(u_sim.z) * 2654435761u;
    // The flow: the curl of a noise field (divergence-free, so it swirls without bunching up), faster with the lead.
    float2 q = pos * 2.5 + float2(TIME * 0.07, SEED * 37);
    const float e = 0.02;
    float n0 = noise(q);
    float2 flow = float2(noise(q + float2(0, e)) - n0, n0 - noise(q + float2(e, 0))) / e * (0.05 + 0.2 * LEAD * INTENSITY);
    vel = lerp(vel, flow, 1 - exp(-DT * 2));
    pos += vel * DT;
    // Born again on the ring: each after ~1.5 s at random, any that left the screen, the new ones (all 0), and on a kick a
    // share of them at once, thrown outwards.
    bool kick = KICK_HIT > 0.5 && hashu(id) < 0.1 * INTENSITY;
    bool gone = hashu(id ^ frame) < DT / 1.5 || abs(pos.x) > ASPECT * 0.5 + 0.05 || abs(pos.y) > 0.55 || dot(s, s) == 0;
    if (kick || gone) {
        float a = hashu(id * 3u ^ frame) * TAU;
        float2 dir = float2(cos(a), sin(a));
        pos = dir * ring_r();
        vel = dir * (kick ? 0.5 + 0.7 * hashu(id * 7u ^ frame) : 0.03);
    }
    return float4(pos, vel);
}

struct Dot {
    float4 pos : SV_Position;
    float3 col : COLOR0;
};

Dot vs_draw(uint id : SV_VertexID) {
    uint n = uint(u_sim.x);
    float4 s = t_sim.Load(int3(id % n, id / n, 0));
    Dot o;
    o.pos = float4(s.x * 2 / ASPECT, -s.y * 2, 0, 1);
    float spark = HATS * FLASH * step(0.9, hashu(id ^ (uint(u_sim.z) / 4u * 2654435761u)));  // a few at a time, on the hats
    float crowd = u_sim.x * u_sim.x * u_res.z * u_res.w;  // particles per pixel: where there are more, each is fainter
    float bright = (0.03 + 0.5 * saturate(length(s.zw) * 2 - 0.1) + 3 * spark) * 0.08 / max(crowd, 0.08);
    o.col = PAL(0.55 + 0.45 * hashu(id) + LEAD * 0.15) * bright;
    return o;
}

float4 ps_draw(Dot d) : SV_Target { return float4(d.col, 1); }
