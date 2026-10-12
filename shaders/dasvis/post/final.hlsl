// The final pass, at the screen's size: upscale the scene (rendered smaller on slower PCs) with a light sharpen,
// chromatic aberration, the bloom, exposure and tone mapping, a vignette, the overlays (spectrum ring, beat sigils,
// title card), the drop's events (shockwave, cover shatter, white-out), film grain, then gamma.
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
    o.rgb += PAL(0.7) * smoothstep(0.003 + aa, 0, abs(r - R)) * (0.9 + 1.2 * SNARE * FLASH);  // the rim, flashing on the snare
    return o;
}

// The cover shatter, a drop's event: the cover, in the middle, breaks into 16 shards that fly at you on the drop, come
// back together over the next 8 bars and fade. For each shard, the pixel is moved back to where it came from on the
// cover; it's that shard's if the shard's seed is the nearest there. Premultiplied (alpha = what it covers).
float4 shatter(float2 pd) {
    float b = SINCE_DROP;
    float spread = smoothstep(0, 0.6, b) * (1 - smoothstep(2, 8, b));
    float fade = smoothstep(0, 0.03, b) * (1 - smoothstep(7, 9, b));
    const float S = 0.25;  // half the cover's height on screen
    if (fade <= 0 || length(pd) > S * 1.5 * (1 + 2 * spread) + spread) return 0;
    float2 seed[16];
    [unroll] for (uint k = 0; k < 16u; ++k) {
        float2 ck = float2(k & 3u, k >> 2);
        seed[k] = (ck + 0.2 + 0.6 * float2(hash12(ck + 1.3), hash12(ck + 4.1))) / 4;
    }
    float4 o = 0;
    [loop] for (uint j = 0; j < 16u; ++j) {
        float h = hash12(seed[j] * 17);
        float2 centre = (seed[j] - 0.5) * 2 * S;
        float2 off = normalize(centre + 1e-3) * spread * (0.35 + 0.5 * h);
        float sc = 1 + spread * (0.4 + 0.8 * h);  // nearer you
        float2 cuv = mul(pd - centre - off, rot(-spread * (h - 0.5) * 2)) / sc / (2 * S) + seed[j];
        float best = 9;
        uint near = 0;
        [unroll] for (uint m = 0; m < 16u; ++m) {
            float dd = distance(cuv, seed[m]);
            if (dd < best) best = dd, near = m;
        }
        if (o.a == 0 && near == j && all(cuv >= 0) && all(cuv <= 1)) o = float4(COVER(cuv) * 1.1, 1);
    }
    return o * fade;
}

// Beat sigils: a ring, triangle or hexagon stamped where the kick says, growing and fading over 0.6 s.
float sides(float2 q, float r, float n) {
    float a = atan2(q.x, q.y), seg = TAU / n;
    return cos(floor(0.5 + a / seg) * seg - a) * length(q) - r;
}

float sigil(float2 pd) {
    float age = SINCE_KICK / 0.6;
    if (age >= 1) return 0;
    uint k = uint(u_ext.y);
    float h1 = hashu(k * 3u), h2 = hashu(k * 3u + 1u), h3 = hashu(k * 3u + 2u);
    float2 q = mul(pd - (float2(h1, h2) - 0.5) * float2(ASPECT * 0.75, 0.65), rot(h3 * TAU + age * 0.6));
    float r = 0.05 + 0.13 * (1 - pow(1 - age, 3));
    float d = h3 < 0.34 ? abs(length(q) - r) : abs(sides(q, r, h3 < 0.67 ? 3 : 6));
    return smoothstep(0.0045, 0.0012, d) * (1 - age) * (1 - age);
}

// The title card at a song's start: the title and artist (drawn by the app with Direct2D into t_sim: coverage in r,
// the letter's number × coverage in g), lower left. The letters arrive one each half beat with a flash, hold, then
// dissolve into sparks. Premultiplied.
float4 title_card(float2 uv) {
    float T = u_ext.z;
    float h = 0.17, w = h * u_title.x / ASPECT;
    float2 tuv = (uv - float2(0.05, 0.56)) / float2(w, h);  // lower left, clear of the player panel
    if (u_title.w < 0.5 || T > 10 || any(tuv < 0) || any(tuv > 1)) return 0;
    bool top = tuv.y < u_title.z;
    uint tw, th;
    t_sim.GetDimensions(tw, th);
    float2 near = t_sim.Load(int3(tuv * float2(tw, th), 0)).rg;
    float idx = round(near.g / max(near.r, 1e-3) * 255);
    float arrive = saturate((T * BPM / 60 - 1 - idx * 0.5) * 3);
    float cov = t_sim.SampleLevel(s_clamp, tuv, 0).r * arrive * (1 + 1.5 * (1 - arrive) * arrive * 4);  // a flash as it lands
    float dis = saturate((T - 7) / 2.5);  // the dissolve: grains drop out, sparking as they go
    float g = hash12(floor(uv * float2(ASPECT, 1) * 260));
    float keep = step(dis, g), spark = step(g, dis) * step(dis - 0.06, g);
    float3 col = top ? lerp(1, PAL(0.95), 0.2) : lerp(1, PAL(0.8), 0.55);
    return float4(col * cov * keep + PAL(0.95) * cov * spark * 1.5, cov * keep);
}

// A lyric line in kinetic type (drawn by the app into t_sim like the title card), centred low: the letters pop in one
// after another as the line starts, each lit in the palette as it's sung, the line punches on the kick, then dissolves
// into sparks as it ends. Premultiplied.
float4 lyric(float2 uv) {
    float T = u_ext.z, t0 = u_lyric.x, t1 = u_lyric.y;
    if (T < t0 - 0.05 || T > t1 + 0.5) return 0;
    float h = 0.2 * (1 + 0.06 * KICK * FLASH), w = h * u_title.x / ASPECT;
    if (w > 0.94) h *= 0.94 / w, w = 0.94;
    float2 tuv = (uv - float2(0.5, 0.77)) / float2(w, h) + float2(0.5, 0.3);  // the line sits in the texture's top 60 %
    if (any(tuv < 0) || any(tuv > 1)) return 0;
    uint tw, th;
    t_sim.GetDimensions(tw, th);
    float2 near = t_sim.Load(int3(tuv * float2(tw, th), 0)).rg;
    float idx = round(near.g / max(near.r, 1e-3) * 255), n = max(u_title.y, 1), dur = max(t1 - t0, 0.5);
    float arrive = saturate((T - t0 - idx / n * min(dur * 0.3, 1.2)) * 9);
    float sung = saturate(((T - t0) / (dur * 0.8) * n - idx) * 2);
    float cov = t_sim.SampleLevel(s_clamp, tuv, 0).r * arrive * (1 + 6 * arrive * (1 - arrive));
    float dis = saturate((T - t1) / 0.5);
    float g = hash12(floor(uv * float2(ASPECT, 1) * 260));
    float keep = step(dis, g), spark = step(g, dis) * step(dis - 0.08, g);
    float3 col = lerp(0.5, lerp(1, PAL(0.9), 0.45), sung) * (1 + 0.5 * sung * KICK * FLASH);
    return float4(col * cov * keep + PAL(0.95) * cov * spark * 1.5, cov * keep);
}

float4 main(VsOut v) : SV_Target {
    float2 d = v.uv - 0.5, uv = v.uv;
    // The phone deck's camera: turn (zoomed in enough that no edge shows) and zoom in, or a fisheye pinch out.
    if (any(u_deck.xy != 0)) {
        float2 q = mul(d * float2(ASPECT, 1), rot(u_deck.x * 0.45));
        float zoom = (1 + 0.55 * abs(u_deck.x)) * (1 + 0.6 * max(u_deck.y, 0));
        q /= zoom * (1 + 1.4 * max(-u_deck.y, 0) * dot(q, q));
        uv = q / float2(ASPECT, 1) + 0.5;
        d = uv - 0.5;
    }
    float ca = u_key.w;
    // A shockwave from the deck (shake the phone): the same ring as the drop's, on its own clock.
    if (u_deck.z > 0) {
        float2 pd = d * float2(ASPECT, 1);
        float x = (length(pd) - (1 - u_deck.z) * 1.2) / 0.05;
        float band = exp(-x * x) * u_deck.z;
        uv -= normalize(pd + 1e-5) / float2(ASPECT, 1) * band * 0.04;
        ca += band * 0.03;
    }
    // The drop's shockwave: a ring of refraction running out from the middle over the bar, splitting the colours.
    if ((EVENT & 1) && DROP > 0) {
        float2 pd = d * float2(ASPECT, 1);
        float x = (length(pd) - (1 - DROP) * 1.2) / 0.05;
        float band = exp(-x * x) * DROP;
        uv -= normalize(pd + 1e-5) / float2(ASPECT, 1) * band * 0.04;
        ca += band * 0.03;
    }
    float3 c;
    if (ca > 0) {
        c.r = t_src.SampleLevel(s_clamp, uv - d * ca, 0).r;
        c.g = t_src.SampleLevel(s_clamp, uv, 0).g;
        c.b = t_src.SampleLevel(s_clamp, uv + d * ca, 0).b;
    } else {
        c = t_src.SampleLevel(s_clamp, uv, 0).rgb;
    }
    if (QUALITY > 0.5) {  // sharpen: the scene was drawn smaller than the screen
        float2 t = u_pass.xy;
        float3 nb = t_src.SampleLevel(s_clamp, uv + float2(t.x, 0), 0).rgb + t_src.SampleLevel(s_clamp, uv - float2(t.x, 0), 0).rgb +
                    t_src.SampleLevel(s_clamp, uv + float2(0, t.y), 0).rgb + t_src.SampleLevel(s_clamp, uv - float2(0, t.y), 0).rgb;
        c = max(c + (c - nb * 0.25) * 0.4, 0);
    }
    float3 b = t_bloom.SampleLevel(s_clamp, v.uv, 0).rgb / u_post.y;
    c = lerp(c, b, u_post.x) + b * 0.12 * ENERGY;
    if ((EVENT & 4) && HAS_COVER > 0.5 && SINCE_DROP < 9) {
        float4 sh = shatter(d * float2(ASPECT, 1));
        c = c * (1 - sh.a * 0.92) + sh.rgb * 0.92;
    }
    c = aces(c * EXPOSURE * (1 + 0.6 * DROP * FLASH) * (1 - 0.55 * GAP));  // the drop punches; the breath before it dims
    c = lerp(c, dot(c, float3(0.2126, 0.7152, 0.0722)), 0.45 * BUILD * BUILD);  // the colour drains as a build-up rises
    c *= 1 - (0.8 + 1.4 * BUILD) * dot(d, d);  // vignette, closing in through a build-up
    if (EVENT & 2)  // the white-out: to white over the last beat before the drop, then back (within the flash budget)
        c = lerp(c, 1, 0.85 * FLASH * max(TO_DROP < 0.25 ? 1 - TO_DROP * 4 : 0, DROP * DROP * DROP));
    if (u_post.w > 0) {
        float4 o = ring(d * float2(ASPECT, 1));
        c = c * (1 - o.a * u_post.w) + o.rgb * u_post.w;
    }
    if (u_dir.z > 0.5) c += PAL(hashu(uint(u_ext.y) * 3u)) * sigil(d * float2(ASPECT, 1)) * 0.8;
    float4 tc = u_lyric.z > 0.5 ? lyric(v.uv) : title_card(v.uv);
    c = c * (1 - tc.a * 0.92) + tc.rgb * 0.92;
    c += (hash12(v.pos.xy + frac(TIME * 7.3) * 157) - 0.5) * u_post.z;
    return float4(pow(saturate(c), 1 / 2.2), 1);
}
