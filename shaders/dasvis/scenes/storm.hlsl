// Storm: banks of cloud lit from inside. Each snare throws a bolt of lightning (within the flash budget) that lights
// the clouds round it; kicks flicker light deep inside them; the bass rumbles the camera like thunder; a build-up
// darkens and thickens the sky.
#define DASVIS_SCENE
#include "dasvis.hlsli"

float3 scene(float2 uv, float2 p) {
    float2 q = float2(p.x, -p.y);
    float shake = BASS * BASS * 0.012 * INTENSITY;  // thunder
    q += (float2(noise(float2(TIME * 31, 1)), noise(float2(TIME * 29, 7))) - 0.5) * shake;

    // The bolt: a new one each half beat, where the beat says, jagged at three scales, from the top down to its end.
    float slot = floor(BEATS * 2);
    float bx = (hash12(float2(slot, 1.7) + SEED) - 0.5) * ASPECT * 0.8;
    float end = -0.1 - 0.35 * hash12(float2(slot, 4.1));
    float xb = bx + (noise(float2(q.y * 6, slot)) - 0.5) * 0.14 + (noise(float2(q.y * 19, slot + 3)) - 0.5) * 0.05 +
               (noise(float2(q.y * 55, slot + 7)) - 0.5) * 0.015;
    float strike = SNARE * FLASH;
    float bolt = exp(-abs(q.x - xb) * 320) * smoothstep(end, end + 0.08, q.y) * strike;
    float2 lp = float2(bx, 0.25);  // where it lights the cloud from
    float2 kp = (float2(hash12(float2(floor(BEATS), 3.3)), hash12(float2(floor(BEATS), 5.9))) - 0.5) * float2(ASPECT, 0.6);  // a kick's flicker

    float3 c = u_pal[0].rgb * 0.02;
    int layers = STEPS(3, 5);
    [loop] for (int k = layers - 1; k >= 0; --k) {  // back to front
        float fk = float(k);
        float2 cq = q * (1.4 + 0.5 * fk) + float2(TIME * 0.02 * (fk + 1), fk * 7.3 + SEED * 11);
        float dens = smoothstep(0.35 - 0.1 * BUILD, 0.75, fbm(cq, 4) + 0.15 * q.y);
        float inner = strike * 2.5 * exp(-length(q - lp) * 2.2) + KICK * 0.6 * exp(-length(q - kp) * 4) + 0.04 + 0.05 * DRIFT.z;
        float3 cloud = lerp(u_pal[0].rgb * 0.08, PAL(0.7 + 0.08 * fk), saturate(inner)) * (0.5 + inner * 1.5);
        c = lerp(c, cloud * (1 - 0.12 * fk), dens * (0.75 - 0.1 * fk));
    }
    c += PAL(0.95) * bolt * 3 + PAL(0.9) * bolt * 0.5;
    c *= 1 - 0.35 * BUILD;
    return c;
}
