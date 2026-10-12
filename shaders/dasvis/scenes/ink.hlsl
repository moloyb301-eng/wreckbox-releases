// Ink Ocean: dye in the song's colours on a fluid (Stam's stable fluids) solved by pixel shaders on a grid 128 cells
// tall on old PCs, 256 on fast ones. Each kick fires jets of ink from three places that move on every beat; the lead
// keeps a current going round; the bass turns the whole ocean slowly; the drop fires all five colours at once from a
// ring. The dye is the picture itself: the last frame, carried along by the flow and fading slowly.
//
// Entry points: main (the scene: t_sim is the flow), and the fluid's passes, run in this order each frame:
// ps_velocity (moves the flow along itself, adds the jets and keeps the swirls), ps_divergence, ps_jacobi (8–16 times),
// ps_gradient (takes the pressure out, so the flow neither bunches up nor thins out).
// u_sim: the grid's size in cells. The flow is in cells a second (y down).
#define DASVIS_SCENE
#include "dasvis.hlsli"

// The jets at p (p's space): their push (screen heights / s²) and the dye they add (linear rgb / s).
void jets(float2 p, out float2 push, out float3 dye) {
    push = 0, dye = 0;
    uint b = uint(max(BEATS, 0)) * 8u + uint(SEED * 65536) * 977u;
    [unroll] for (uint i = 0; i < 3; ++i) {  // the kick's: three places, new on every beat
        float2 at = (float2(hashu(b + i * 3u), hashu(b + i * 3u + 1u)) - 0.5) * float2(ASPECT * 0.75, 0.75);
        float a = hashu(b + i * 3u + 2u) * TAU;
        float g = exp(-dot(p - at, p - at) * 400);
        push += float2(cos(a), sin(a)) * g * 14 * KICK * INTENSITY;
        dye += PAL(0.35 + 0.2 * float(i) + 0.1 * hashu(b + i)) * g * 7 * KICK;
    }
    // The lead's current: a jet going round the middle.
    float t = TIME * 0.35 + SEED * TAU;
    float2 at = float2(cos(t) * (0.15 + 0.2 * DRIFT.x) * ASPECT, sin(t * 1.3) * (0.15 + 0.15 * DRIFT.y));
    float g = exp(-dot(p - at, p - at) * 250);
    push += float2(-sin(t), cos(t)) * g * (0.6 + 4 * LEAD) * INTENSITY;
    dye += PAL(0.85 + 0.1 * sin(TIME * 0.2)) * g * 4 * LEAD;
    push += float2(-p.y, p.x) * BASS * 0.8;  // the bass: the whole ocean turns (a rigid turn adds no pressure)
    if (DROP > 0) {  // the drop: five jets on a ring, one in each of the palette's colours
        [unroll] for (uint k = 0; k < 5; ++k) {
            float ka = float(k) * TAU / 5 + SEED * TAU;
            float2 kp = float2(cos(ka), sin(ka)) * 0.3;
            float kg = exp(-dot(p - kp, p - kp) * 300);
            push += float2(-sin(ka), cos(ka)) * kg * 18 * DROP;
            dye += u_pal[k].rgb * kg * 12 * DROP * (k == 0 ? 3 : 1);  // the darkest colour needs more to show
        }
    }
}

float3 scene(float2 uv, float2 p) {
    float2 flow = t_sim.SampleLevel(s_clamp, uv, 0).xy;
    float3 c = PREV(uv - flow * DT / u_sim.xy) * exp(-DT * (0.3 + 0.6 * (1 - ENERGY)));
    float2 push;
    float3 dye;
    jets(p, push, dye);
    return c + dye * DT;
}

// MARK: The fluid

float2 F(int2 c) { return t_src.Load(int3(clamp(c, 0, int2(u_sim.xy) - 1), 0)).xy; }  // the flow at a cell (walls: the edge)
float curl(int2 c) { return (F(c + int2(1, 0)).y - F(c - int2(1, 0)).y - F(c + int2(0, 1)).x + F(c - int2(0, 1)).x) * 0.5; }

float4 ps_velocity(VsOut v) : SV_Target {
    int2 c = int2(v.pos.xy);
    float2 flow = t_src.SampleLevel(s_clamp, v.uv - F(c) * DT * u_res.zw, 0).xy;  // carried along by itself
    // Vorticity confinement: puts back the small swirls the grid smooths away.
    float2 g = float2(abs(curl(c + int2(1, 0))) - abs(curl(c - int2(1, 0))), abs(curl(c + int2(0, 1))) - abs(curl(c - int2(0, 1))));
    g /= length(g) + 1e-4;
    flow += float2(g.y, -g.x) * curl(c) * u_sim.y * 0.12 * DT;
    float2 push;
    float3 dye;
    jets((v.uv - 0.5) * float2(ASPECT, 1), push, dye);
    flow += push * u_sim.y * DT;
    flow *= exp(-DT * 0.7);  // it calms down when the music does
    return float4(clamp(flow, -u_sim.y, u_sim.y), 0, 0);
}

// The pressure texture: pressure (r), the flow's divergence (g). t_src: the flow; t_sim: last frame's pressure.
float4 ps_divergence(VsOut v) : SV_Target {
    int2 c = int2(v.pos.xy);
    float d = (F(c + int2(1, 0)).x - F(c - int2(1, 0)).x + F(c + int2(0, 1)).y - F(c - int2(0, 1)).y) * 0.5;
    return float4(t_sim.Load(int3(c, 0)).r * 0.8, d, 0, 0);  // last frame's pressure is a good start
}

float P(Texture2D t, int2 c) { return t.Load(int3(clamp(c, 0, int2(u_sim.xy) - 1), 0)).r; }

float4 ps_jacobi(VsOut v) : SV_Target {  // t_src: the pressure
    int2 c = int2(v.pos.xy);
    float d = t_src.Load(int3(c, 0)).g;
    return float4((P(t_src, c + int2(1, 0)) + P(t_src, c - int2(1, 0)) + P(t_src, c + int2(0, 1)) + P(t_src, c - int2(0, 1)) - d) * 0.25, d, 0, 0);
}

float4 ps_gradient(VsOut v) : SV_Target {  // t_src: the flow; t_sim: the pressure
    int2 c = int2(v.pos.xy);
    float2 g = float2(P(t_sim, c + int2(1, 0)) - P(t_sim, c - int2(1, 0)), P(t_sim, c + int2(0, 1)) - P(t_sim, c - int2(0, 1))) * 0.5;
    return float4(F(c) - g, 0, 0);
}
