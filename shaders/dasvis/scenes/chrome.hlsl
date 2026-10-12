// Liquid Chrome: five blobs of liquid metal, one per stem (kick, snare, hats, bass, lead), each as big as its stem is
// loud, ray marched and reflecting the album cover. They drift apart through each bar and flow together on its last
// beat; the light catches their rims in the song's colours.
#define DASVIS_SCENE
#include "dasvis.hlsli"

float stem(int i) { return i == 0 ? KICK : i == 1 ? SNARE : i == 2 ? HATS : i == 3 ? BASS : LEAD; }

float3 blob(int i) {
    float fi = float(i), a = fi * TAU / 5 + TIME * (0.25 + 0.05 * fi) + SEED * TAU;
    float spread = 0.95 * (1 - 0.75 * smoothstep(0.6, 1, BAR));  // together on the bar's last beat
    return float3(cos(a) * spread * 1.2, sin(a * 1.3 + fi) * spread * 0.7, sin(a) * spread * 0.6);
}

float map(float3 p) {
    float d = 1e9;
    [unroll] for (int i = 0; i < 5; ++i) d = smin(d, length(p - blob(i)) - (0.22 + 0.32 * stem(i) * INTENSITY), 0.45);
    return d;
}

// What the chrome reflects: the cover (or the song's colours) wrapped round the sky.
float3 env(float3 r) {
    float2 e = float2(atan2(r.x, r.z) / TAU + 0.5, r.y * 0.5 + 0.5);
    float3 sky = HAS_COVER > 0.5 ? COVER(float2(e.x * 2, 1 - e.y)) : PAL(e.y * 0.8 + e.x * 0.3) * (0.2 + 0.8 * e.y);
    return sky * (0.6 + 0.6 * smoothstep(-0.2, 0.6, r.y));
}

float3 scene(float2 uv, float2 p) {
    float3 ro = float3(0, 0, -3.4);
    ro.xz = mul(ro.xz, rot(0.3 * sin(TIME * 0.11) + DRIFT.x - 0.5));
    float3 fw = normalize(-ro), rt = normalize(cross(float3(0, 1, 0), fw)), up = cross(fw, rt);
    float3 rd = normalize(fw * 1.7 + rt * p.x - up * p.y);
    float3 bg = HAS_COVER > 0.5 ? t_cover.SampleLevel(s_wrap, uv, 5).rgb : PAL(0.1 + uv.y * 0.2);
    float3 c = bg * bg * 0.18 * (1 - 0.6 * dot(p, p));
    float t = 0, d = 1;
    int steps = STEPS(32, 64);
    [loop] for (int i = 0; i < steps; ++i) {
        d = map(ro + rd * t);
        if (d < 0.001 || t > 8) break;
        t += d;
    }
    if (d < 0.01) {
        float3 pos = ro + rd * t;
        const float2 k = float2(1, -1) * 0.002;
        float3 n = normalize(k.xyy * map(pos + k.xyy) + k.yyx * map(pos + k.yyx) + k.yxy * map(pos + k.yxy) + k.xxx * map(pos + k.xxx));
        float fres = pow(1 - saturate(dot(n, -rd)), 3);
        float3 refl = env(reflect(rd, n));
        c = refl * (0.55 + 0.45 * fres) + PAL(0.85) * fres * (0.6 + 1.5 * KICK * FLASH);
        c += PAL(0.95) * pow(saturate(dot(reflect(rd, n), normalize(float3(0.5, 0.8, -0.4)))), 40) * 1.5;
    }
    return c;
}
