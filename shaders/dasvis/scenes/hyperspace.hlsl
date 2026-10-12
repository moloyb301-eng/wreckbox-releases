// Hyperspace: a starfield rushing past, the stars stretching into streaks. Faster with the energy and each kick; a
// build-up stretches them longer and longer; the drop is the jump to light speed (long streaks, a flash at the
// centre).
#define DASVIS_SCENE
#include "dasvis.hlsli"

float3 scene(float2 uv, float2 p) {
    float r = length(p), a = atan2(p.y, p.x) / TAU + 0.5;
    float stretch = 0.015 + 0.6 * BUILD * BUILD + 1.8 * DROP;
    float3 c = 0;
    int layers = STEPS(3, 6);
    [loop] for (int k = 0; k < layers; ++k) {
        float cells = 70 + 41 * float(k);
        float ca = a * cells + float(k) * 0.37;
        float cell = floor(ca);
        float h = hash12(float2(cell, float(k) * 13.1 + SEED * 7));
        float d = frac(h * 9.1 + TRAVEL * (0.15 + 0.2 * h));   // 0 far → 1 passing you
        float rho = 0.025 / max(1 - d, 0.025);                  // further out as it nears
        float len = rho * stretch + 0.003;
        float along = smoothstep(rho - len, rho, r) * smoothstep(rho + 0.004, rho, r);
        float across = abs(frac(ca) - 0.5) / cells * TAU * r;  // in p's units
        float s = along * smoothstep(0.0015 + 0.004 * d, 0, across) * smoothstep(0, 0.25, d);
        c += PAL(0.55 + 0.45 * h) * s * (0.5 + 2 * d);
    }
    c += PAL(0.95) * 0.008 / (r * r * 40 + 0.008) * (0.25 + 0.5 * BASS + 3 * DROP * FLASH);  // the light ahead
    return max(c, PREV(0.5 + (uv - 0.5) * 0.985) * exp(-DT * (12 - 8 * DROP)));
}
