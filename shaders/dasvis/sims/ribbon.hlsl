// The ribbon layer, which the director puts over some scenes: the waveform as a glowing ribbon twisting through
// space, added to the picture (the feedback leaves it a trail). One triangle strip of 1,024 vertices, worked out from
// their number; t_sim is the waveform. The kick brightens it; the bass swells it.
#include "dasvis.hlsli"

struct Rib {
    float4 pos : SV_Position;
    float3 col : COLOR0;
    float v : TEXCOORD0;  // across the ribbon, −1 … 1
};

Rib vs_ribbon(uint id : SV_VertexID) {
    uint i = id / 2u;
    float side = (id & 1u) ? 1 : -1;
    float s = float(i) / 511;
    float w = t_sim.Load(int3(i, 0, 0)).r;
    // A path through space: a long S along x, the wave lifting it, turning slowly about two axes.
    float3 P = float3((s - 0.5) * 2.6, w * (0.12 + 0.12 * BASS) + 0.12 * sin(s * TAU + TIME * 0.4), 0.25 * sin(s * PI * 2 + TIME * 0.3 + SEED * TAU));
    float tw = s * TAU + TIME * 0.8;  // the twist
    P += float3(0, cos(tw), sin(tw)) * 0.03 * side;
    P.xz = mul(P.xz, rot(0.35 * sin(TIME * 0.13) + 0.3 * (DRIFT.w - 0.5)));
    P.yz = mul(P.yz, rot(0.25 * sin(TIME * 0.09)));
    float z = 1.8 + P.z;
    Rib o;
    o.pos = float4(P.x / z * 1.6 / ASPECT, P.y / z * 1.6 * 2, 0, 1);
    float ends = smoothstep(0, 0.12, s) * smoothstep(1, 0.88, s);
    o.col = PAL(s * 0.5 + TIME * 0.03 + 0.4) * ends * (0.35 + 1.2 * KICK * FLASH);
    o.v = side;
    return o;
}

float4 ps_ribbon(Rib r) : SV_Target { return float4(r.col * (1 - r.v * r.v), 1); }
