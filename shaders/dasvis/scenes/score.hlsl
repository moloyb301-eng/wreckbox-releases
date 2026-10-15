// Score: the song as a sequencer grid (src/dasvis/score.h works it out; this only draws it). Two bars of sixteenths
// across, the voices up the rows (kick at the bottom, hats at the top). Each block is a square with its voice's mark cut
// into it and turns a quarter turn when it plays; it lights while it sounds; pixels ripple out from it ring by ring.
// Flat and crisp on purpose (no trails), the opposite of the flowing scenes.
//
// t_sim: (cols × 2) × rows. Left half: voice + 1, shown, lit, quarter turns. Right half: ripple dot, ripple block,
// the ripple's voice + 1. u_sim: cols, rows, the playhead (columns).
#define DASVIS_SCENE
#include "dasvis.hlsli"

float3 tint(float voice) {  // off-white paper (SQNCR's #DDDACA) leaning towards the song's palette, a hue per voice
    return lerp(float3(0.72, 0.70, 0.60), PAL(0.12 + 0.17 * voice), 0.55);
}

float box(float2 d, float half_size) { return max(abs(d.x), abs(d.y)) - half_size; }

// The voice's mark inside a block (n: −1…1 across it, already turned): 1 where it's cut in.
float mark(int voice, float2 n) {
    if (voice == 1) return step(1.1, length(n - 1));      // kick: a quarter circle out of one corner
    if (voice == 2) return step(0, n.x + n.y);            // snare: half, on the diagonal
    if (voice == 3) return step(0.55, max(abs(n.x), abs(n.y)));  // hats: a frame
    if (voice == 4) return step(0.15, n.x);               // bass: a bar down one side
    return step(1.6, length(n + 1));                      // lead: all but a quarter circle
}

float3 scene(float2 uv, float2 p) {
    float cols = u_sim.x, rows = u_sim.y, head = u_sim.z;
    float cs = min(ASPECT * 0.88 / cols, 0.78 / rows);  // a cell, in screen heights
    float px = 1 / (RES.y * cs);                         // a pixel, in cells
    float2 g = p / cs + float2(cols, rows) * 0.5;        // cells from the grid's top left
    float3 c = 0.0052;                                   // the instrument's black (#101010)
    if (g.x < -1 || g.y < -1 || g.x > cols + 1 || g.y > rows + 1) return c;

    // The paper: a faint dot at each corner, brighter on the beats; the playhead's column glows.
    float2 corner = abs(g - round(g));
    float beat = fmod(round(g.x), 4) == 0 ? 2 : 1;
    c += 0.012 * beat * smoothstep(0.07 + px, 0.07, max(corner.x, corner.y)) * step(-0.01, g.x) * step(g.x, cols + 0.01);
    float to_head = abs(g.x - head);
    c += PAL(0.9) * (0.035 * smoothstep(1.2, 0, to_head) + (0.25 + 0.6 * BUILD) * smoothstep(0.05 + px, 0.05, to_head)) *
         step(0, g.y) * step(g.y, rows);

    if (g.x < 0 || g.y < 0 || g.x >= cols || g.y >= rows) return c;
    int2 cell = int2(floor(g.x), int(rows) - 1 - int(floor(g.y)));
    float2 d = frac(g) - 0.5;
    d.y = -d.y;  // up is up

    // Ripples: a pixel in the middle of the cell, or a whole block.
    float4 r = t_sim.Load(int3(cell.x + int(cols), cell.y, 0));
    if (r.z > 0) {
        float3 rc = tint(r.z) * 0.9;
        c += rc * r.x * smoothstep(px, -px, box(d, 0.17));
        c += rc * r.y * 0.8 * smoothstep(px, -px, box(d, 0.4));
    }

    float4 b = t_sim.Load(int3(cell, 0));
    if (b.x > 0 && b.y > 0) {
        int voice = int(b.x + 0.5);
        float2 q = mul(d, rot(b.w * PI * 0.5));                  // its turns
        float half_size = 0.42 * b.y * (1 + 0.06 * b.z);         // grows in; a little bigger while it sounds
        float inside = smoothstep(px, -px, box(q, half_size));
        float3 fill = tint(voice) * (0.3 + 1.2 * b.z * (0.5 + 0.5 * FLASH));  // lit, but not a flash: many play each second
        float3 ink = tint(voice) * 0.05;
        c = lerp(c, lerp(fill, ink, mark(voice, q / max(half_size, 1e-3))), inside);
    }
    return c;
}
