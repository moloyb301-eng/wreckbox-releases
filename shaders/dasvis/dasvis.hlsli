// DasVis shader library: what every DasVis shader can read, and shared helpers. Shader Model 4 (Direct3D feature level
// 10_0), so it runs on graphics cards from about 2008 on.
//
// Writing your own scene: a .hlsl file in the scenes folder (right-click → DasVis → Open my scenes folder) like this:
//
//     #define DASVIS_SCENE
//     #include "dasvis.hlsli"
//     float3 scene(float2 uv, float2 p) {         // uv: 0–1 across the screen; p: centred, x scaled by the aspect
//         return PAL(length(p) - BEATS * 0.25) * (0.3 + KICK);
//     }
//
// It's compiled when DasVis opens and on F5; errors show on screen. The values are linear HDR (1 = white before the
// tone mapping); PREV() is the last frame, for feedback effects. Everything here is our own code (MIT, like the app).

cbuffer DasVis : register(b0) {
    float4 u_time;   // time (s, since DasVis opened), dt (s), seed (0–1, from the song), quality (0–1)
    float4 u_res;    // this pass's target size in pixels, and 1 / size
    float4 u_out;    // output size in pixels, aspect (w / h), transition (0→1 into a new scene; 1 = none)
    float4 u_beat;   // beat phase (0→1, 0 on the beat), bar phase (0→1 across 4 beats), beats (count), bpm
    float4 u_drums;  // kick, snare, hats (envelopes 0–1), kick_hit (1 on the frame of a kick)
    float4 u_tone;   // bass, lead, energy (0–1), live (1: no song map yet)
    float4 u_look;   // intensity (0.5 chill, 1, 1.6 insane), flash budget (0–1), exposure, has cover (0 / 1)
    float4 u_post;   // bloom mix, bloom levels, grain, ring (spectrum ring overlay, 0–1)
    float4 u_key;    // key tint (rgb, linear), chromatic aberration
    float4 u_pal[5]; // the song's palette, dark → light (linear rgb)
    float4 u_pass;   // per pass: the source's texel size (xy)
    float4 u_song;   // phrase phase (0→1 across 16 bars), section (0 intro, 1 verse, 2 build-up, 3 drop, 4 breakdown,
                     // 5 chorus, 6 outro), progress through it (0→1), build (0→1 up to the drop)
    float4 u_drop;   // drop (1 on its first beat, gone a bar later), bars to the next drop (99: none), gap (1 in the
                     // quiet bars before a drop), the part's energy (0–1)
    float4 u_hits;   // kick, snare, hats hit in this frame (1 / 0), seconds since the last kick
    float4 u_sim;    // the built-in scenes' simulations: grid size and two values of their own (0 in your scenes)
    float4 u_dir;    // the director: transition kind (0 crossfade, 1 wipe, 2 zoom through, 3 shatter, 4 glitch), the
                     // drop's events (bits: 1 shockwave, 2 white-out, 4 cover shatter), beat sigils (0 / 1)
    float4 u_drift;  // four slow waves (0–1, tens of seconds each) from the song's seed: let a parameter wander
    float4 u_ext;    // travel (distance flown: faster with energy, kicks and drops), kicks so far, seconds into the
                     // song, bars since the last drop (99: none yet)
    float4 u_title;  // the title card: its texture's width / height, letters, the title line's share of its height, on
    float4 u_deck;   // the phone deck: camera turn and zoom (−1…1, 0 at rest), a shockwave (1→0 over 0.6 s), spare
    float4 u_lyric;  // a lyric line (in the title card's texture): its start and end (song seconds), on, spare
};

#define TIME u_time.x
#define DT u_time.y
#define SEED u_time.z
#define QUALITY u_time.w
#define RES u_res.xy
#define ASPECT u_out.z
#define TRANSITION u_out.w
#define BEAT u_beat.x
#define BAR u_beat.y
#define BEATS u_beat.z
#define BPM u_beat.w
// The stems: from the song map when it's ready (exact timing, each separated from the rest of the mix), else live.
#define KICK u_drums.x
#define SNARE u_drums.y
#define HATS u_drums.z
#define KICK_HIT u_drums.w
#define SNARE_HIT u_hits.y
#define HATS_HIT u_hits.z
#define SINCE_KICK u_hits.w   // seconds: for things that travel after each kick
#define BASS u_tone.x         // the bass line (harmonic, under 250 Hz)
#define LEAD u_tone.y         // the lead / vocals (harmonic, 300 Hz – 4 kHz)
#define ENERGY u_tone.z
#define LIVE u_tone.w
#define INTENSITY u_look.x
#define FLASH u_look.y        // multiply bright flashes by this: it's 0 once 3 have happened this second
#define EXPOSURE u_look.z
#define HAS_COVER u_look.w
#define KEY_TINT u_key.rgb
// From the song map (all 0 / 99 until the song has been analysed, or for radio: LIVE is then 1).
#define PHRASE u_song.x
#define SECTION u_song.y
#define SECTION_T u_song.z
#define BUILD u_song.w      // tighten things as it rises: the drop lands when it reaches 1
#define DROP u_drop.x       // let go: 1 on the drop's first beat, fading over the bar
#define TO_DROP u_drop.y
#define GAP u_drop.z        // the breath before the drop: hold back
#define PART_ENERGY u_drop.w
#define DRIFT u_drift       // float4, each 0–1, changing slowly: a long section keeps evolving
#define EVENT int(u_dir.y + 0.5)
#define TRAVEL u_ext.x      // for flights: steady, faster with the music
#define SINCE_DROP u_ext.w  // bars

Texture2D t_src : register(t0);    // scenes: the last frame; post passes: their input
Texture2D t_cover : register(t1);  // the album cover (when HAS_COVER)
Texture2D t_spec : register(t2);   // 256 × 1: spectrum now (r), slowly falling peaks (g)
Texture2D t_wave : register(t3);   // 512 × 1: the waveform, −1…1
Texture2D t_bloom : register(t4);  // the final pass: the bloom
Texture2D t_sim : register(t5);    // the built-in scenes: their simulation (the fluid's flow, the reaction-diffusion …)
SamplerState s_clamp : register(s0);
SamplerState s_wrap : register(s1);

static const float PI = 3.14159265;
static const float TAU = 6.2831853;

float3 PREV(float2 uv) { return t_src.SampleLevel(s_clamp, uv, 0).rgb; }
float3 COVER(float2 uv) { float3 c = t_cover.SampleLevel(s_wrap, uv, 0.5).rgb; return c * c; }  // to linear (≈ gamma 2)
float SPEC(float x) { return t_spec.SampleLevel(s_clamp, float2(x, 0.5), 0).r; }       // x: 0 lows → 1 highs
float SPEC_PEAK(float x) { return t_spec.SampleLevel(s_clamp, float2(x, 0.5), 0).g; }
float WAVE(float x) { return t_wave.SampleLevel(s_clamp, float2(x, 0.5), 0).r; }

// The palette, wrapping smoothly through its five colours as t goes 0 → 1.
// The palette comes from the album cover (or the song's key when there's no colourful cover).
float3 PAL(float t) {
    t = frac(t) * 5;
    int i = int(t);
    return lerp(u_pal[i].rgb, u_pal[i == 4 ? 0 : i + 1].rgb, smoothstep(0, 1, frac(t)));
}

// Loop counts that follow the quality knob: lo on old PCs, hi on fast ones.
int STEPS(int lo, int hi) { return int(lerp(float(lo), float(hi), QUALITY) + 0.5); }

float hash12(float2 p) {
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float hashu(uint n) {  // 0–1 from a whole number (a particle's, a beat's): good at any size, where hash12 isn't (PCG)
    n = n * 747796405u + 2891336453u;
    n = ((n >> ((n >> 28u) + 4u)) ^ n) * 277803737u;
    return float((n >> 22u) ^ n) * 2.3283064e-10;
}

float noise(float2 p) {  // value noise, 0–1
    float2 i = floor(p), f = frac(p), u = f * f * (3 - 2 * f);
    return lerp(lerp(hash12(i), hash12(i + float2(1, 0)), u.x), lerp(hash12(i + float2(0, 1)), hash12(i + 1), u.x), u.y);
}

float fbm(float2 p, int octaves) {  // fractal noise, about 0–1
    float s = 0, a = 0.5;
    [loop] for (int i = 0; i < octaves; ++i) {
        s += a * noise(p);
        p = mul(p, float2x2(1.6, 1.2, -1.2, 1.6));
        a *= 0.5;
    }
    return s;
}

float smin(float a, float b, float k) {  // a smooth union of two distances
    float h = saturate(0.5 + 0.5 * (b - a) / k);
    return lerp(b, a, h) - k * h * (1 - h);
}

float2x2 rot(float a) {
    float c = cos(a), s = sin(a);
    return float2x2(c, -s, s, c);
}

float3 aces(float3 x) {  // filmic tone mapping (Narkowicz's fit of ACES)
    return saturate(x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14));
}

struct VsOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

#ifdef DASVIS_SCENE
float3 scene(float2 uv, float2 p);

// Into a new scene, the way the director chose: the last picture (PREV, which keeps whatever hasn't been replaced yet)
// gives way to the new one `c`. Only the new scene is drawn: the old one is its last frames, so a change costs nothing.
float3 into(float2 uv, float3 c) {
    float t = TRANSITION;
    int kind = int(u_dir.x + 0.5);
    float3 o;
    if (kind == 0) {  // crossfade (calm to calm)
        o = lerp(PREV(uv), c, smoothstep(0, 1, t));
    } else if (kind == 1) {  // wipe: along a noise pattern, with a soft edge
        float n = fbm(uv * float2(ASPECT, 1) * 2.5 + SEED * 9, 3);
        o = lerp(PREV(uv), c, smoothstep(n - 0.06, n + 0.06, lerp(-0.1, 1.0, t)));
    } else if (kind == 2) {  // zoom through: it rushes past you
        o = lerp(PREV(0.5 + (uv - 0.5) * (1 - 0.12 * t)), c, t * t);
    } else if (kind == 3) {  // shatter: shards of the last picture, each going at its own moment, edges lit as they crack
        float2 q = uv * float2(ASPECT, 1) * 6, cell = floor(q), id = cell;
        float d1 = 9, d2 = 9;
        [unroll] for (int y = -1; y <= 1; ++y) {
            [unroll] for (int x = -1; x <= 1; ++x) {
                float2 at = cell + float2(x, y);
                float d = distance(q, at + float2(hash12(at), hash12(at + 7.7)));
                if (d < d1) {
                    d2 = d1;
                    d1 = d;
                    id = at;
                } else {
                    d2 = min(d2, d);
                }
            }
        }
        float gone = step(hash12(id + 3.1) * 0.85, t);
        float3 old = PREV(0.5 + (uv - 0.5) * (1 - 0.03 * t));  // flying at you
        o = lerp(old + PAL(0.95) * smoothstep(0.08, 0, d2 - d1) * 1.5 * FLASH * t, c, gone);
    } else {  // glitch: bands of the last picture, shifted and split into red, green and blue, cutting over band by band
        float r = hash12(float2(floor(uv.y * 24), floor(TIME * 20)));
        float sh = (r - 0.5) * 0.06;
        float3 g = float3(PREV(uv + float2(sh + 0.01, 0)).r, PREV(uv + float2(sh, 0)).g, PREV(uv + float2(sh - 0.01, 0)).b);
        o = r < t ? c : g;
    }
    return o;
}

float4 main(VsOut v) : SV_Target {
    float2 p = (v.uv - 0.5) * float2(ASPECT, 1);
    float3 c = scene(v.uv, p);
    if (TRANSITION < 1) c = into(v.uv, c);
    // One bad pixel (NaN or infinity: all exponent bits set) must not spread through the feedback. A bit test, as the
    // compiler assumes floats are never NaN and would drop isnan().
    if (any((asuint(c) & 0x7F800000) == 0x7F800000)) c = 0;
    return float4(clamp(c, 0, 64), 1);
}
#endif
