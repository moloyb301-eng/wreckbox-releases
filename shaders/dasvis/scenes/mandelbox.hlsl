// Mandelbox Flight: flying round a Mandelbox fractal, ray marched, glossy, with ambient occlusion and fog. The bass
// changes its fold; the camera's path changes on each 16-bar phrase; the snare catches the edges; the key tints the
// fog. Heavy: the director runs it where it fits (Medium and High tiers, or Low if it measured fast enough).
#define DASVIS_SCENE
#include "dasvis.hlsli"

float de(float3 p, out float trap) {
    float s = 2.0 + 0.15 * DRIFT.x - 0.1 * BASS;  // the fold scale
    float4 z = float4(p, 1);
    trap = 1e9;
    int it = STEPS(6, 10);
    [loop] for (int i = 0; i < it; ++i) {
        z.xyz = clamp(z.xyz, -1, 1) * 2 - z.xyz;  // box fold
        float r2 = dot(z.xyz, z.xyz);
        z *= clamp(1 / r2, 1, 4);                 // sphere fold
        z = z * float4(s, s, s, abs(s)) + float4(p, 1);
        trap = min(trap, r2);
    }
    return (length(z.xyz) - abs(s - 1)) / abs(z.w) - pow(abs(s), float(1 - it));  // the standard estimate (never overshoots)
}

float de(float3 p) {
    float t;
    return de(p, t);
}

float3 scene(float2 uv, float2 p) {
    // The camera: round the fractal (it fills a box about 6 across), closing in and pulling back; a new path each phrase.
    float ph = floor(BEATS / 64) + SEED * 17;
    float ang = TRAVEL * 0.04 + hash12(float2(ph, 1)) * TAU;
    float dist = 13 + 3.5 * sin(TRAVEL * 0.027 + ph);
    float3 ro = float3(cos(ang) * dist, (hash12(float2(ph, 2)) - 0.5) * 8 + sin(TRAVEL * 0.023) * 2, sin(ang) * dist);
    float3 fw = normalize(-ro + 1.5 * float3(sin(TRAVEL * 0.07), cos(TRAVEL * 0.04), cos(TRAVEL * 0.05)));
    float3 rt = normalize(cross(float3(0, 1, 0), fw)), up = cross(fw, rt);
    float3 rd = normalize(fw * 1.4 + rt * p.x - up * p.y);

    float t = 0.1, trap = 0, d = 1;
    int steps = STEPS(40, 90), i = 0;
    [loop] for (i = 0; i < steps; ++i) {
        d = de(ro + rd * t, trap);
        if (d < 0.001 * t || t > 36) break;
        t += d * 0.85;
    }
    float3 fog = KEY_TINT * 0.12 + u_pal[0].rgb * 0.06;
    float3 c = fog * (0.5 + 0.5 * BASS) * (1 - 0.5 * dot(p, p));
    if (t < 36 && d < 0.01 * t) {
        float3 pos = ro + rd * t;
        const float2 k = float2(1, -1) * 0.0015 * t;  // the normal: four samples round the point (a tetrahedron)
        float3 n = normalize(k.xyy * de(pos + k.xyy) + k.yyx * de(pos + k.yyx) + k.yxy * de(pos + k.yxy) + k.xxx * de(pos + k.xxx));
        float3 l = normalize(float3(0.6, 0.8, -0.3));
        float ao = 0.35 + 0.65 * (1 - float(i) / float(steps));  // fewer steps: more open
        float dif = saturate(dot(n, l));
        float spec = pow(saturate(dot(reflect(rd, n), l)), 32);
        float3 base = PAL(0.3 + 0.6 * saturate(trap * 0.6) + 0.1 * DRIFT.y);
        float rim = pow(1 - saturate(dot(n, -rd)), 3);
        float3 lit = base * (0.25 + 1.6 * dif) * ao + PAL(0.95) * spec * (0.8 + 2.5 * SNARE * FLASH) + PAL(0.7) * rim * 0.5;
        c = lerp(fog, lit, exp(-max(t - 8, 0) * 0.06));
    }
    return c;
}
