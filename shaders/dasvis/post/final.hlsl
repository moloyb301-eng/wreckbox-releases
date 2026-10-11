// The final pass, at the screen's size: upscale the scene (rendered smaller on slower PCs) with a light sharpen,
// chromatic aberration, the bloom, exposure and tone mapping, a vignette, the spectrum ring, film grain, then gamma.
#include "dasvis.hlsli"

// The spectrum ring: bars round a spinning disc of the cover. Premultiplied colour (alpha = what it covers).
float4 ring(float2 p) {
    float r = length(p);
    float x = abs(atan2(p.x, -p.y)) / PI;  // 0 at the top, 1 at the bottom, the same on both sides
    float R = 0.15 + 0.012 * KICK;
    const float bars = 36;
    float slot = frac(x * bars), bin = (floor(x * bars) + 0.5) / bars * 0.8;  // the lower 80 % of the spectrum
    float len = 0.015 + 0.15 * SPEC(bin) * INTENSITY;
    float base = R + 0.012, aa = fwidth(r) * 1.5;
    float across = smoothstep(0.36, 0.32, abs(slot - 0.5));
    float bar = across * smoothstep(base - aa, base, r) * smoothstep(base + len + aa, base + len, r);
    float cap_r = base + 0.015 + 0.15 * SPEC_PEAK(bin) * INTENSITY;
    float cap = across * smoothstep(0.004 + aa, 0.004, abs(r - cap_r));
    float3 col = PAL(x * 0.6 + TIME * 0.02);
    float4 o = float4(col * (bar * (0.9 + 0.8 * KICK * FLASH) + cap * 0.9), 0);  // bars glow over the picture
    float disc = smoothstep(R, R - aa, r);
    // The cover, turning; without one, a dark disc lit softly from the middle in the song's colour.
    float3 cover = HAS_COVER > 0.5 ? COVER(mul(p / (R * 2), rot(TIME * 0.25)) + 0.5) : u_pal[0].rgb * (0.05 + 0.25 * (1 - r / R)) * (1 + KICK);
    o += float4(cover * disc, disc);
    o.rgb += PAL(0.7) * smoothstep(0.003 + aa, 0, abs(r - R)) * 0.9;  // the rim
    return o;
}

float4 main(VsOut v) : SV_Target {
    float2 d = v.uv - 0.5;
    float ca = u_key.w;
    float3 c;
    if (ca > 0) {
        c.r = t_src.SampleLevel(s_clamp, v.uv - d * ca, 0).r;
        c.g = t_src.SampleLevel(s_clamp, v.uv, 0).g;
        c.b = t_src.SampleLevel(s_clamp, v.uv + d * ca, 0).b;
    } else {
        c = t_src.SampleLevel(s_clamp, v.uv, 0).rgb;
    }
    if (QUALITY > 0.5) {  // sharpen: the scene was drawn smaller than the screen
        float2 t = u_pass.xy;
        float3 nb = t_src.SampleLevel(s_clamp, v.uv + float2(t.x, 0), 0).rgb + t_src.SampleLevel(s_clamp, v.uv - float2(t.x, 0), 0).rgb +
                    t_src.SampleLevel(s_clamp, v.uv + float2(0, t.y), 0).rgb + t_src.SampleLevel(s_clamp, v.uv - float2(0, t.y), 0).rgb;
        c = max(c + (c - nb * 0.25) * 0.4, 0);
    }
    float3 b = t_bloom.SampleLevel(s_clamp, v.uv, 0).rgb / u_post.y;
    c = lerp(c, b, u_post.x) + b * 0.12 * ENERGY;
    c = aces(c * EXPOSURE);
    c *= 1 - 0.8 * dot(d, d);  // vignette
    if (u_post.w > 0) {
        float4 o = ring(d * float2(ASPECT, 1));
        c = c * (1 - o.a * u_post.w) + o.rgb * u_post.w;
    }
    c += (hash12(v.pos.xy + frac(TIME * 7.3) * 157) - 0.5) * u_post.z;
    return float4(pow(saturate(c), 1 / 2.2), 1);
}
