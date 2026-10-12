// Feedback Classic: MilkDrop's soul in HDR. Each frame is the last one warped (zoomed in, turned, swirled) and faded,
// with new ink drawn on top: the waveform as a glowing ring. The bass pulls the zoom, each beat turns it a step, the
// hats make it shimmer, kicks thicken and brighten the ring, the snare flashes a wider ring around it, the lead turns
// the colours.
#define DASVIS_SCENE
#include "dasvis.hlsli"

float3 scene(float2 uv, float2 p) {
    float r = length(p);
    float on_beat = 1 - smoothstep(0, 0.35, BEAT);  // 1 on the beat, gone a third of the way through it
    float dir = frac(SEED * 7.3) < 0.5 ? 1 : -1;     // which way this song turns
    // Per-second rates times DT, so 30 and 60 fps look alike.
    // A build-up pulls the zoom harder; the drop throws the turn the other way for a moment.
    float zoom = exp(-DT * (0.3 + 1.2 * BASS * INTENSITY + 1.5 * BUILD));
    float turn = dir * DT * (0.1 + 1.6 * on_beat * INTENSITY - 6 * DROP);
    float swirl = DT * (0.25 + 0.7 * DRIFT.x) * sin(TIME * 0.21 + SEED * TAU) * r;
    float2 q = mul(p, rot(turn + swirl)) * zoom;
    int oct = STEPS(2, 4);
    float2 n = float2(fbm(p * 2 + TIME * 0.15, oct), fbm(p * 2 - TIME * 0.15 + 7.1, oct)) - 0.5;
    q += n * DT * (0.05 + 0.4 * HATS);
    // Trails last about half a second (less when it's quiet), so the newest ink stays the brightest thing on screen.
    float3 c = PREV(q / float2(ASPECT, 1) + 0.5) * exp(-DT * (2.2 + 2.0 * (1 - ENERGY)));

    // The ring: the waveform round a circle (mirrored, so it closes without a seam). max(), not +, so a ring drawn every
    // frame never builds up past its own brightness.
    float a = atan2(p.y, p.x) / TAU + 0.5;
    float w = WAVE(abs(a * 2 - 1));
    float rr = (0.2 + 0.07 * w + 0.05 * KICK * INTENSITY) * (1 - 0.35 * BUILD) * (1 + 0.6 * DROP);  // tightens, then bursts
    float width = 0.002 + 0.006 * KICK;
    float ring = smoothstep(width + 0.003, width * 0.3, abs(r - rr));
    float3 ink = PAL(a * 0.5 + TIME * 0.05 + BEATS * 0.03 + LEAD * 0.2) * (1.4 + 2.5 * KICK * FLASH);
    c = max(c, ink * ring);
    // The snare: a thin outer ring that flashes and is carried outwards by the zoom.
    float outer = smoothstep(0.004, 0.0012, abs(r - rr * 1.55)) * SNARE * FLASH;
    c = max(c, PAL(0.85 + a * 0.3) * 1.8 * outer);
    // A small core that breathes with the lead.
    c = max(c, PAL(0.9 + TIME * 0.02) * (0.15 + 0.6 * LEAD) * 0.02 / (r * r * 80 + 0.02));
    return c;
}
