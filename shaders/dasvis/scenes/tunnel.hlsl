// Neon Tunnel: flying down an endless tunnel of glowing rings and panel seams, one ring per beat. Worked out in polar
// space (no ray marching), so it's light enough for any PC. Each kick sends a pulse of light racing down the tunnel; the
// snare flashes the seams; the hats flicker the panels; the bass lights the far end; the lead shifts the ring colours;
// the camera leans with the bar.
#define DASVIS_SCENE
#include "dasvis.hlsli"

float3 scene(float2 uv, float2 p) {
    float sides = 6 + 2 * floor(frac(SEED * 13.7) * 4);  // 6, 8, 10 or 12 sides: this song's tunnel
    p = mul(p, rot((0.08 + 0.3 * DRIFT.y) * sin(TIME * 0.27) + 0.12 * sin(BAR * TAU)));
    float a01 = atan2(p.y, p.x) / TAU + 0.5;
    float cell = frac(a01 * sides);              // across one face, 0 → 1
    float seg = TAU / sides;
    float ang = (cell - 0.5) * seg;              // from the face's middle
    float r = max(length(p) * cos(ang), 1e-3);   // distance to the face, so the walls are flat
    float z = 0.3 / r * (1 + 0.7 * BUILD);       // depth into the tunnel; narrower through a build-up
    float u = z + BEATS;                         // travel: one ring per beat
    float d = 0.5 - abs(frac(u) - 0.5);          // distance to the nearest ring
    float ring = smoothstep(fwidth(u) * 1.5 + 0.035, 0, d);
    float seam = smoothstep(0.86, 1.0, abs(cell - 0.5) * 2);
    float idx = floor(u);
    float face = floor(a01 * sides);
    float fog = exp(-z * 0.11);

    // The kick's pulse: a bright band that starts at the camera and races away (6 depth units a second), fading.
    float zp = 0.35 + SINCE_KICK * 6;
    float pulse = max(exp(-SINCE_KICK * 2.5) * exp(-abs(z - zp) * 2.5), DROP);  // the drop lights every ring

    float3 col = PAL(idx * 0.137 + SEED + LEAD * 0.12);
    float flicker = 0.5 + noise(float2(idx * 3.1 + face * 7.7, TIME * 6)) * HATS * 2;
    float3 c = col * (ring * (0.7 + 2.2 * pulse * FLASH) + seam * (0.35 + 1.4 * SNARE * FLASH)) * fog * (0.75 + 0.6 * ENERGY);
    c += col * 0.05 * fog * flicker;                                  // the panels
    c += PAL(0.5 + TIME * 0.01) * 0.35 * exp(-length(p) * 7) * (0.4 + BASS);  // the far end
    // Speed trails: the last frame, pushed outwards (flying forward), fading fast.
    c = max(c, PREV(0.5 + (uv - 0.5) * 0.993) * exp(-DT * 14));
    return c;
}
