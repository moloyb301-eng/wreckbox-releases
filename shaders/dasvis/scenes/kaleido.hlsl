// Kaleido Cover: a kaleidoscope of the album art (or of the song's colours when there's no cover). It turns a step on
// each beat, the number of mirrors changes every 8 bars, the bass zooms it, the snare lights the seams, kicks light
// the centre, the hats glint across it, the lead brightens it.
#define DASVIS_SCENE
#include "dasvis.hlsli"

float3 scene(float2 uv, float2 p) {
    float n = 6 + 2 * floor(frac(SEED * 5.3 + floor(BEATS / 32) * 0.37) * 4);  // 6–12 mirrors, new every 8 bars
    float step_ = floor(BEATS) + smoothstep(0, 0.3, BEAT);                       // eased turn on each beat
    float spin = step_ * PI / n * 0.5;
    float r = length(p) * (1 - 0.18 * BASS * INTENSITY) * (1 + 0.6 * BUILD) * (1 - 0.3 * DROP);  // more mirrors in view as it builds
    float a = atan2(p.y, p.x) + spin;
    float seg = TAU / n;
    float m = abs(frac(a / seg) - 0.5) * seg;  // the mirrored angle, 0 … seg / 2
    float2 k = float2(cos(m), sin(m)) * r;
    float2 cuv = 0.5 + k * (0.65 + 0.4 * DRIFT.w) + (0.06 + 0.14 * DRIFT.z) * float2(sin(TIME * 0.11), cos(TIME * 0.07));  // drifting across the cover
    float3 img;
    if (HAS_COVER > 0.5) img = COVER(cuv);
    else {  // no cover: marbled noise in the song's colours, with dark veins for contrast
        float g = fbm(cuv * 5 + TIME * 0.08, STEPS(3, 6));
        img = PAL(g * 0.7 + TIME * 0.01) * smoothstep(0.25, 0.65, g) * 0.9;
    }
    float3 c = img * (0.5 + 0.9 * LEAD);

    float d = r * sin(min(m, seg * 0.5 - m));  // distance to the nearest mirror line
    c += PAL(BEATS * 0.05) * smoothstep(0.005, 0, d) * (0.35 + 1.6 * SNARE * FLASH);
    c *= 1 + 0.8 * KICK * FLASH * exp(-r * 5);
    // The hats: tiny glints scattered over the pattern (mirrored with it), each catching the light on a hat.
    float2 cell = floor(k * 46);
    float g = hash12(cell + floor(BEATS * 2) * 7.31);
    float2 f = frac(k * 46) - 0.5;
    c += PAL(0.95) * step(0.965, g) * smoothstep(0.35, 0, length(f)) * HATS * 2.5;
    return lerp(c, PREV(0.5 + (uv - 0.5) * 0.99), 0.25);  // a little feedback for softness
}
