// Aurora: layered curtains of light over a night lake that reflects them. The lead brightens them and folds them
// further; the key sets their hue (KEY_TINT at their foot); the stars twinkle; the director keeps it for calm parts.
#define DASVIS_SCENE
#include "dasvis.hlsli"

static const float SHORE = -0.14;  // the lake's far shore (p's y, flipped)

float3 sky(float2 s) {
    float3 c = lerp(u_pal[0].rgb * 0.05 + KEY_TINT * 0.015, u_pal[0].rgb * 0.015, saturate((s.y - SHORE) * 1.5));
    float2 cell = floor(s * 240);
    float star = step(0.996, hash12(cell + SEED * 31)) * (0.6 + 0.4 * sin(TIME * 2.5 + hash12(cell + 1.3) * 40));
    c += star * 0.5 * smoothstep(SHORE + 0.05, SHORE + 0.3, s.y);
    int layers = STEPS(3, 5);
    [loop] for (int i = 0; i < layers; ++i) {
        float fi = float(i);
        float x = s.x * (0.9 + 0.25 * fi) + fi * 1.7 + SEED * 5;
        float fold = 0.12 + 0.3 * LEAD * INTENSITY + 0.12 * DRIFT.x;
        float base = SHORE + 0.12 + 0.07 * fi + fold * (fbm(float2(x * 1.3 + TIME * 0.04, fi * 3.1), 3) - 0.5);
        float hgt = 0.16 + 0.14 * fbm(float2(x * 0.7 - TIME * 0.03, fi + 9), 2);
        float t = (s.y - base) / hgt;
        float curtain = smoothstep(-0.03, 0.05, t) * exp(-max(t, 0) * 2.3);
        float rays = 0.35 + 0.65 * noise(float2(x * 48 + TIME * 0.5, fi * 3));
        float3 col = lerp(KEY_TINT * 1.6 + PAL(0.5) * 0.25, PAL(0.8 + fi * 0.05), saturate(t * 1.2));
        c += col * curtain * rays * (0.3 + 0.9 * LEAD + 0.25 * ENERGY) * (1 - 0.15 * fi);
    }
    return c;
}

float3 scene(float2 uv, float2 p) {
    float2 q = float2(p.x, -p.y);
    float hills = SHORE + 0.015 + 0.035 * fbm(float2(q.x * 3 + SEED * 7, 0.5), 3);
    float3 c;
    if (q.y >= SHORE) {
        c = q.y < hills ? u_pal[0].rgb * 0.01 : sky(q);  // the far hills, dark against the sky
    } else {  // the lake: the sky upside down, rippled and dimmer
        float depth = SHORE - q.y;
        float2 s = float2(q.x + 0.006 * sin(q.y * 150 + TIME * 1.7) * saturate(depth * 6), 2 * SHORE - q.y);
        c = (s.y < hills ? u_pal[0].rgb * 0.01 : sky(s)) * (0.5 - 0.3 * saturate(depth * 2));
    }
    return lerp(c, PREV(uv), 0.35);  // a little persistence: soft
}
