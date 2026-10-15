// Journey: a generated world (src/dasvis/genome.*), different every time. Cells, organelles or a tunnel, seen through
// up to three warps of space (twist, kaleidoscope, fold, fluid flow, log-polar spiral, lens, ripples), zooming in for
// ever: as one level of the world grows to twice its size, the next one down fades in beneath it, so it never ends and
// never jumps. The zoom flies faster with the music and the tempo and jolts on each kick; the cells' seeds wander, the
// nuclei swell on the kick, the membranes shimmer with the hats.
#define DASVIS_SCENE
#include "dasvis.hlsli"

#define GENUS int(u_gen[0].x + 0.5)
#define DENSITY u_gen[2].x
#define EDGE u_gen[2].y
#define CORE u_gen[2].z
#define GRAIN u_gen[2].w
#define HUE u_gen[3].x
#define SPREAD u_gen[3].y
#define SPEED u_gen[3].z
#define GLOW u_gen[3].w

float2 warp(float2 p, int kind, float a, float t) {
    float r = length(p);
    [branch] if (kind == 1) {  // twist: turns more towards the edge, and slowly over time
        p = mul(p, rot(a * 2.5 * r + t * 0.2));
    } else if (kind == 2) {  // mirror: a kaleidoscope
        float seg = TAU / u_gen[1].w, ang = atan2(p.y, p.x) + t * 0.05;
        ang = abs(frac(ang / seg + 0.5) - 0.5) * seg;
        p = float2(cos(ang), sin(ang)) * r;
    } else if (kind == 3) {  // fold: over itself, twice, as fractals are
        p = abs(p) - 0.35 * a;
        p = mul(p, rot(0.5 + a + t * 0.03));
        p = abs(p) - 0.18 * a;
    } else if (kind == 4) {  // flow: a fluid noise warp
        p += a * 0.5 * (float2(fbm(p * 1.3 + t * 0.12, 3), fbm(p * 1.3 - t * 0.1 + 4.2, 3)) - 0.5);
    } else if (kind == 5) {  // spiral: log-polar, so the world winds into a spiral
        float2 lp = float2(log(r + 1e-3) * 0.6, atan2(p.y, p.x) / PI);
        p = lerp(p, mul(lp, rot(a * 0.8)) * 1.6, a);
    } else if (kind == 6) {  // bulge: a lens, pushing out or pinching in
        p *= pow(max(r, 1e-3), a * 0.9 - 0.35);
    } else if (kind == 7) {  // wave: ripples
        p += a * 0.12 * sin(p.yx * 5 + t * 1.3);
    }
    return p;
}

// Cells: the nearest and second-nearest wandering seeds (F1, F2) and the nearest one's cell.
void voronoi(float2 x, float t, out float f1, out float f2, out float2 id) {
    float2 n = floor(x), f = frac(x);
    f1 = 8, f2 = 8, id = n;
    [unroll] for (int j = -1; j <= 1; ++j) {
        [unroll] for (int i = -1; i <= 1; ++i) {
            float2 g = float2(i, j), c = n + g;
            float2 o = 0.5 + 0.38 * sin(t * (0.5 + hash12(c)) + TAU * float2(hash12(c + 3.1), hash12(c + 7.7)));
            float2 r = g + o - f;
            float d = dot(r, r);
            if (d < f1) {
                f2 = f1, f1 = d, id = c;
            } else {
                f2 = min(f2, d);
            }
        }
    }
    f1 = sqrt(f1), f2 = sqrt(f2);
}

float3 cells(float2 q, float layer, float t) {
    float f1, f2;
    float2 id;
    voronoi(q + layer * 7.31, t, f1, f2, id);
    float h = hash12(id + layer * 17.3);
    float wall = f2 - f1;  // 0 on the membrane
    float mem = smoothstep(EDGE * 0.6 + 0.025, EDGE * 0.2, wall);  // a crisp membrane
    float nuc = smoothstep(CORE * 0.42, CORE * 0.42 - 0.06, f1 * (1 - 0.25 * KICK * INTENSITY));
    float g = fbm(q * 3.1 + h * 9, STEPS(2, 4));
    // The cytoplasm: dark, grainy, in shadow just inside the wall (relief), lighter towards the middle.
    float3 c = PAL(HUE + h * SPREAD) * (0.06 + 0.45 * lerp(0.6, g, GRAIN)) * smoothstep(EDGE * 0.5, EDGE + 0.22, wall);
    c = lerp(c, PAL(HUE + 0.5 + h * 0.25) * (1.1 + 1.2 * KICK * FLASH) * (0.6 + 0.4 * g), nuc);
    c += PAL(HUE + 0.8 + 0.1 * h) * mem * (0.5 + GLOW) * (0.8 + 1.4 * HATS);
    return c;
}

float3 organelles(float2 q, float layer, float t) {
    float field = 0, tint = 0;
    int n = STEPS(6, 11);
    [loop] for (int i = 0; i < n; ++i) {
        float hi = hashu(uint(layer + 64) * 31u + uint(i)), hj = hashu(uint(i) * 7u + 3u + uint(layer + 64));
        float2 c = (0.35 + 0.65 * hi) * float2(sin(t * (0.25 + 0.4 * hj) + i * 2.4 + layer), cos(t * (0.2 + 0.3 * hi) + i * 1.7));
        float rad = (0.12 + 0.2 * hj) * CORE * 2.2 * (1 + 0.35 * KICK * INTENSITY * step(0.5, hj));
        float d = dot(q - c, q - c) + 1e-4;
        float w = rad * rad / d;
        field += w, tint += w * hi;
    }
    tint /= field;
    float body = smoothstep(0.95, 1.15, field);
    float rim = smoothstep(EDGE * 2 + 0.05, 0, abs(field - 1));
    float g = fbm(q * 4 + layer * 3.7, STEPS(2, 4));
    float3 c = PAL(HUE + tint * SPREAD) * body * (0.15 + 0.6 * lerp(0.6, g, GRAIN)) * (0.4 + 0.6 * saturate(field - 1));
    c += PAL(HUE + 0.7 + tint * 0.3) * rim * (0.5 + GLOW) * (0.8 + 1.2 * HATS);
    c += PAL(HUE + 0.15) * 0.04 * fbm(q * 1.5 - t * 0.05, 3);  // the cytoplasm around them
    return c;
}

float3 tunnel(float2 p, float t, float travel) {
    float r = length(p) + 1e-3, a = abs(atan2(p.y, p.x)) / PI;  // mirrored: no seam
    float2 tq = float2(a * DENSITY * 1.5, 0.45 / r + travel);
    float f1, f2;
    float2 id;
    voronoi(tq, t, f1, f2, id);
    float h = hash12(id);
    float wall = f2 - f1;
    float3 c = PAL(HUE + h * SPREAD + tq.y * 0.05) * (0.2 + 0.5 * lerp(0.6, fbm(tq * 3, 3), GRAIN)) * smoothstep(0, 0.3, wall);
    c += PAL(HUE + 0.8) * smoothstep(EDGE + 0.05, EDGE * 0.3, wall) * GLOW * (0.6 + 1.4 * HATS);
    float ring = frac(0.45 / r + travel - SINCE_KICK * 2);  // each kick sends a ring down the tunnel
    c += PAL(HUE + 0.5) * smoothstep(0.04, 0, abs(ring - 0.5)) * exp(-SINCE_KICK * 3) * 1.5 * FLASH;
    return c * smoothstep(0.02, 0.35, r);  // into the dark
}

float3 scene(float2 uv, float2 p) {
    float tempo = clamp(BPM / 125, 0.6, 1.7);
    float t = TIME * SPEED * 0.6;
    float travel = TRAVEL * SPEED * tempo * 0.45 + 0.06 * KICK * INTENSITY;  // the kick jolts it forward
    float2 q = mul(p, rot(TIME * 0.02 * (SEED - 0.5)));
    [unroll] for (int k = 0; k < 3; ++k) q = warp(q, int(u_gen[0][k + 1] + 0.5), u_gen[1][k], t);
    float3 c;
    [branch] if (GENUS == 2) {
        c = tunnel(q, t, travel);
    } else {
        // The endless zoom: level L at its size, level L + 1 (twice as fine) fading in under it as L grows to double.
        float z = travel * 0.5, f = frac(z), level = floor(z);
        float2 a = q * DENSITY / exp2(f), b = a * 2;
        float mix_in = smoothstep(0.55, 1, f);  // one level crisp most of the time; the next comes in as it doubles
        [branch] if (GENUS == 1) c = organelles(a * 0.45, level, t);
        else c = cells(a, level, t);
        [branch] if (mix_in > 0) {  // only drawn while it's fading in: about half the cost
            float3 cb;
            [branch] if (GENUS == 1) cb = organelles(b * 0.45, level + 1, t);
            else cb = cells(b, level + 1, t);
            c = lerp(c, cb, mix_in);
        }
    }
    return c * (1 + 0.5 * DROP);
}
