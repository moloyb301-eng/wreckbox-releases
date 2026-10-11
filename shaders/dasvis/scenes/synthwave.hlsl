// Synthwave: an outrun sun over a neon grid floor, with mountains made from the spectrum on the horizon. The grid
// scrolls a step on each beat; the sun's stripes pulse with the bass; a build-up heats the horizon; the drop flares
// the sun; the kick brightens the grid.
#define DASVIS_SCENE
#include "dasvis.hlsli"

static const float HORIZON = -0.02;  // up from the middle (p's y, flipped)

float3 scene(float2 uv, float2 p) {
    float2 q = float2(p.x, -p.y);
    float3 c;
    if (q.y > HORIZON) {
        float t = q.y - HORIZON;
        c = lerp(PAL(0.75) * 0.45, u_pal[0].rgb * 0.04, saturate(t * 2.4));      // the sky: hot at the horizon
        c += PAL(0.85) * 0.1 * exp(-t * 7) * (1 + 1.5 * BUILD);
        // The sun, its lower half cut by stripes that thicken with the bass.
        float2 s = q - float2(0, 0.16 + 0.02 * DRIFT.x);
        float r = length(s);
        float stripes = s.y < 0.03 ? step(0.32 + 0.25 * BASS, frac(-s.y * 26 + BEATS * 0.25)) : 1;
        float3 sun = lerp(PAL(0.95), PAL(0.6), saturate(0.5 - s.y * 4)) * (1.3 + 1.2 * BASS + 3 * DROP * FLASH);
        c = lerp(c, sun, smoothstep(0.205, 0.198, r) * stripes);
        c += PAL(0.9) * 0.015 / (r * r * 7 + 0.015) * (0.4 + BASS + 3 * DROP * FLASH);  // its glow; the drop flares it
        // The mountains: the spectrum's peaks, mirrored, a dark range with a lit rim.
        float x = abs(q.x) / (ASPECT * 0.5);
        float m = 0.015 + 0.17 * pow(saturate(SPEC_PEAK(0.04 + x * 0.6)), 1.5) * smoothstep(0.08, 0.4, x) + 0.025 * noise(float2(q.x * 9, SEED * 9));
        c = lerp(c, u_pal[0].rgb * 0.06, step(t, m));
        c += PAL(0.55) * smoothstep(0.004, 0, abs(t - m)) * 1.3;
    } else {
        // The floor: a grid in perspective, a step further on each beat.
        float d = HORIZON - q.y;
        float z = 0.16 / max(d, 1e-3);
        float scroll = floor(BEATS) + smoothstep(0, 0.35, BEAT);
        float2 g = float2(q.x * z * 1.6, z + scroll * 0.5);
        float2 w = fwidth(g);
        float2 a = abs(frac(g + 0.5) - 0.5);
        float grid = max(smoothstep(w.x * 1.5, 0, a.x), smoothstep(w.y * 1.5, 0, a.y)) * saturate(1 - max(w.x, w.y) * 2);  // fades where lines crowd
        c = u_pal[0].rgb * 0.03 + PAL(0.6 + 0.1 * DRIFT.y) * grid * (0.9 + 1.6 * KICK * FLASH) * exp(-z * 0.04);
        c += PAL(0.9) * 0.02 / (q.x * q.x * 30 + d * 3 + 0.02) * (0.3 + BASS);  // the sun on the floor
    }
    return c;
}
