# DasVis — WreckBox's own visualizer: the build plan

Status: **plan, nothing built yet** (2026-10-11; revised the same day: runs on old PCs, low RAM and CPU). It sits next
to MilkDrop (`docs/MILKDROP-PLAN.md`) and doesn't replace it.

## 1. What and why

MilkDrop (projectM) is from 2001:
- **Colour:** 8-bit, so gradients band.
- **Audio:** only three bands (bass / mid / treble), read from the last few milliseconds.
- **Looks:** 2D only, and the preset changes at random times.
- **Cost:** heavy for what it shows. Its per-vertex equations run on the CPU: **26–30 % of one core** here at 720p,
  plus a 4 ms GPU read-back, and **~60 MB** more memory while it's open (`docs/DESIGN.md`).

WreckBox can do much better, because **the songs are files on your PC.** DasVis analyses the whole song before it
plays:
- It knows where every beat, bar, build-up and drop is, and what the kick, snare, hats, bass and vocals are doing.
- It can **anticipate**. In a build-up the picture tightens, and on the drop's first beat it lets go.

It draws everything on the GPU:
- HDR with bloom
- particles
- fluid
- fractals
- motion blur

An **auto-director** cuts between scenes on phrase boundaries, the way a real VJ does.

**The goal, in one line:** every frame looks like a music video someone edited to this exact song, and it runs on a
ten-year-old laptop using less CPU and memory than MilkDrop.

### Why that's possible: what makes it look good is cheap

What makes DasVis better than MilkDrop is mostly **brains, not GPU power:**
- knowing the song (the beats, the sections, the drops)
- smooth springs instead of jitter
- cuts on phrases
- palettes taken from the cover

All of that runs once per song or costs microseconds a frame. The expensive GPU effects (fractals, a million
particles) are the **top layer**, scaled to what each PC can do. An old PC gets the same director, timing and
palettes, so the same "edited to the song" feel; it just gets simpler scenes and a lower internal resolution.

### What makes the animation *good* (the rules every scene follows)

1. **Nothing jumps. Everything springs.**
   - Audio drives motion through envelopes with a fast attack and a slow release, and through critically damped
     springs.
   - Raw amplitude never moves anything directly: that's the jitter that makes most visualizers look cheap.
2. **Movement locks to the beat's phase, not its loudness.** The camera travels one ring per beat and turns on the
   bar, so it moves smoothly *with* the music.
3. **Anticipation and release.** Build-ups compress (speed, saturation, field of view, a riser glow). Drops release
   (flash, shockwave, a new scene).
4. **Contrast over time.** Breakdowns are calm and sparse. Drops are dense and bright. When everything is loud all the
   time, nothing is.
5. **Each song has its own look.** A seed made from the track's id picks the palette offsets, camera path and scene
   order. The same song looks like itself every time, and different songs look different.
6. **Film finish on every frame:**
   - HDR colour, bloom, ACES tone mapping
   - a touch of grain, chromatic aberration and vignette
   - motion blur from the feedback loop
   
   All of it is cheap enough for the lowest tier.
7. **Smooth or it doesn't ship.** Dynamic resolution keeps the frame time. A stutter is worse than a slightly softer
   picture.
8. **Safe flashing.** A limiter keeps full-screen brightness flashes to 3 a second at most (WCAG 2.3.1). *Reduce
   flashing* lowers them further.
9. **Light on every PC** (§4). Every pass has a cost class and a quality knob. Nothing runs on the CPU per pixel or per
   vertex. Every resource is freed when full screen closes.

## 2. Where it starts from (today's code)

| Piece | Today | DasVis uses it for |
|---|---|---|
| `player/milkdrop.*` | projectM in an OpenGL pbuffer; pixels read back (4 ms a frame) and drawn as a Direct2D bitmap | Kept as its own mode, and later as one layer inside DasVis |
| `ui/gfx.*` | `ID2D1HwndRenderTarget` | Phase 0 moves it onto a D3D11 device, so DasVis's texture is drawn with **no read-back** |
| `AudioOutput::tap_until` / `heard_index` | The exact samples heard while a frame is on screen (beat sync, phase 5c) | The live audio feed, already in sync |
| `player::Visualizer` | 2,048-point FFT for the Winamp bars | The same FFT feeds DasVis, so nothing is computed twice |
| `wb::decode_mono` (22.05 kHz mono) and `wb::tempo` / `key` | BPM and key analysis | The song map builds on them |
| `ArtworkCache`, the album colours | Covers and their colours | Cover textures and the palette |
| `View` full-screen screen | The panel, keys, menu, Bars / MilkDrop chips; it stops drawing when paused and settled | A third chip: **DasVis**; the same pause rule |
| `sync_server` (phone over Wi-Fi) | Phone sync | Phase 7: the phone as a VJ deck |

The dev PC (GTX 1650 Ti + Intel UHD, i5-10300H) is a **new-ish laptop**. Old PCs are covered by the tiers in §4 and
the tests in §12.

## 3. Decisions (defaults I've taken; any of them can change)

| Question | Default | Why |
|---|---|---|
| Graphics API | **Direct3D 11 at feature level 10_0: Shader Model 4, pixel and vertex shaders only, no compute shaders** | Runs on every GPU since about 2008: Intel HD 2000 onward, GeForce 8, Radeon HD 2000. Simulations (particles, fluid, reaction-diffusion) run as pixel shaders on ping-pong textures, the classic way, which is just as fast at these sizes. One code path for every PC. |
| Colour format | **R11G11B10_FLOAT** (4 bytes a pixel) for HDR targets | HDR at half the memory of RGBA16F; on integrated graphics GPU memory *is* your RAM |
| Shaders | **HLSL**, compiled at build time with `fxc` (`ps_4_0` / `vs_4_0`) into `.cso` files next to the exe. Your own scenes compile at run time with `d3dcompiler_47.dll` (part of Windows 10), which is loaded **only if you have your own scenes**. | No compiler in memory normally; the build fails if a shader doesn't compile |
| Stems (kick / snare / hats / bass / vocals) | **DSP pseudo-stems:** harmonic–percussive separation plus band splits, streamed through the song (§6) | A few seconds of one background core a song, under 2 MB of memory, no new dependencies |
| When songs are analysed | **Only while DasVis is open:** the current song at once (it plays *Live* for those few seconds), then the next one in the queue | Never costs anything if you don't use DasVis |
| Default visualizer | **MilkDrop until DasVis has 12+ scenes and the director**, then DasVis (done in phase 6: 13 scenes) | Don't swap in something half-finished |
| Graphics card | On laptops with two GPUs, DasVis asks for the **fast one** while full screen is open, and goes back when it closes | Battery life the rest of the time |
| On battery | **30 fps cap** and one tier lower (setting: *Full / Save battery*) | Half the power for a picture that still moves smoothly |
| No usable GPU (Remote Desktop, a broken driver) | The **Winamp bars**, as MilkDrop does today | Software rendering would burn the CPU |
| Third-party shader code | **None copied from Shadertoy** (its default licence is CC BY-NC-SA, which is not OK to ship). Our own code, or MIT sources (Ashima noise, AMD FSR 1's sharpen), with licences in `licenses\` | It's distributed |
| New dependencies | **None** for phases 0–7 | D3D11, DXGI, d3dcompiler and Media Foundation are part of Windows |

## 4. Runs on any PC: tiers and budgets

### Tiers, measured rather than guessed

DasVis doesn't look up your GPU's name. It measures:
1. It starts in **Medium** and times every pass on the GPU (timestamp queries, read back three frames late so it never
   stalls).
2. After 2 seconds it settles on a tier, and keeps adjusting the render scale within that tier as it runs.
3. The result is remembered per graphics card in `dasvis.json`, so the next start is right from the first frame.

| Tier | Typical PC | Output | Internal render | Frame rate | Scenes allowed | Particles | Fluid / RD grid | Bloom |
|---|---|---|---|---|---|---|---|---|
| **Low** | Intel HD 4000 / HD 520, GT 710; 2 cores, 4 GB | up to 1080p | 33–60 % of output | 30–60 | Light + Medium (Medium at low quality) | 16k–64k | 128² | 3 levels, quarter res |
| **Medium** | Intel UHD 620, Vega 8, GTX 750–1050 | up to 1440p | 50–85 % | 60 | all; Heavy at low quality | 256k | 256² | 5 levels |
| **High** | GTX 1650 Ti and up | up to 4K | 70–90 % | 60 (or display rate) | all | 1M | 512² | 6 levels |

**Below Low** (no feature level 10_0, or Low can't hold 25 fps at 33 % scale): the Winamp bars, with a one-line note.

### Budgets: what DasVis may cost, measured on top of the app with the visualizer closed

| | Low tier (old PC) | High tier (this PC) | MilkDrop today, for comparison |
|---|---|---|---|
| **CPU, per frame** | < 0.5 ms | < 0.5 ms | |
| **CPU, overall** | < 5 % of one core at 60 fps | < 3 % of one core | 26–30 % of one core |
| **App memory (private RAM) while open** | **≤ +20 MB** | ≤ +30 MB | about +60 MB |
| **GPU memory** | ≤ 24 MB | ≤ 128 MB | |
| **Song analysis** | < 15 s of one core, at background priority | < 5 s | none |
| **After closing full screen** | back to where it was | back to where it was | back to where it was |

### How it stays light

**CPU**
- Nothing per pixel or per vertex runs on the CPU. A frame's CPU work is:
  - one shared FFT
  - ~20 envelopes and springs
  - a song map lookup (binary search)
  - the director's decision (a few comparisons)
  - one 256-byte constant buffer upload
- Frames are paced by the display's refresh (a vsync'd present with a frame latency of 1), so the CPU sleeps between
  frames and never busy-waits.
- Drawing stops when it should:
  - **paused:** once the picture settles
  - **minimized or covered:** right away (the same rules as MilkDrop)
- Song analysis runs on one background thread at the lowest priority (`THREAD_MODE_BACKGROUND_BEGIN`), one song at a
  time, so playback and the UI never feel it, even on two cores.

**Memory**
- **Few targets.** The render targets are:
  - two feedback buffers (ping-pong)
  - one scratch buffer
  - the bloom chain (a third the size of one target)
  - the simulation grids, which exist only while their scene is on screen
- **Small format.** R11G11B10 is 4 bytes a pixel. At Low tier, 1080p output rendered at 50 % is 960×540: about 2 MB a
  target.
- **Streamed analysis.**
  - Harmonic–percussive separation needs only a sliding window of 17 spectra, not the whole song's spectrogram
    (~70 KB, not ~40 MB).
  - The only big buffer is the decoded song (~20 MB for 4 minutes), held for a few seconds.
  - Option, if that's still too much on 4 GB PCs: decode in 30-second chunks.
- **Small song maps.** Envelopes are stored as bytes at 43 per second, with beats and sections as short lists: about
  60 KB a song. It's read from disk when the song starts; only the current song and the next stay in memory.
- **Covers** go up as one 512² texture with mipmaps (~1.4 MB). The cover's 5-colour palette is computed from a 64²
  thumbnail.
- **No compiler** loaded unless you have your own scenes; shipped shaders are precompiled `.cso` files (a few hundred
  KB in all).

**GPU** (quality without the cost)
- **Feedback is the best value in graphics.**
  - It's one texture read per pixel, and gives motion trails, tunnels and smears for free. MilkDrop proves how far it
    goes.
  - Most scenes are built on it, with a few cheap shapes or noise on top.
- **One quality knob for everything.** Every shader reads `quality` (0–1) and scales its loops: ray-march steps,
  noise octaves, blur taps. One shader serves Low and High. There are never two versions to keep in step.
- **Render low, finish high.**
  - Scenes draw at the internal resolution.
  - Bloom works at quarter resolution.
  - Grain, vignette and the final sharpen (FSR 1's RCAS, one cheap pass) run at full output, so edges and text stay
    crisp.
- **Simulations scale by grid size, not by code.** Particles live in a texture (16k = 128², 1M = 1024²), as do the
  fluid and reaction-diffusion grids.
- **Heavy scenes** (fractals, chrome) render at a lower scale and are given more steps only when the measured frame
  time has room.

## 5. Architecture

```
                         ┌──────── only while DasVis is open, background thread, low priority ────────┐
 current / next song ──► │ decode_mono → streaming STFT → HPSS (17-frame window) → pseudo-stems       │
                         │ → onsets → events → beat grid → bars / phrases → sections → drops          │
                         │ → song map  (cache: _cache\dasvis\maps\<id>.bin, ~60 KB)                   │
                         └───────────────────────────────────┬────────────────────────────────────────┘
                                                             ▼
 AudioOutput::tap_until ─► Features (the shared FFT,  ─► Uniforms (one 256-byte cbuffer)
   (what's heard now)       envelopes, springs) +         beat/bar/phrase phase, stems,
                            SongMap lookup                anticipation, palette, quality …
                                                             ▼
                         Director: which scene(s) fit the tier and the moment; transitions on phrase boundaries
                                                             ▼
 GPU frame (D3D11, FL 10_0, pixel shaders): simulations (ping-pong textures) → world layer → feedback warp
   → mid layer (particles) → overlay → transition blend → bloom (¼ res) → tone map + finish + flash limiter
   → upscale + sharpen → swap-chain back buffer
                                                             ▼
 Direct2D (same device): the glass player panel drawn on top → Present (paced by the refresh)
```

### Files

```
src/dasvis/                 library wbdasvis: no UI dependencies (like player/milkdrop)
  dasvis.h / .cpp           public API: DasVis(cfg), frame(features, dt) → draws into a texture; next / prev / lock;
                            tier measurement and the render scale
  gpu.h / .cpp              D3D11 helpers: targets, ping-pong pairs, timestamp queries, .cso loading, hot reload
  features.h / .cpp         live audio features from the tap (FFT bands, envelopes, springs, beat PLL)
  songmap.h / .cpp          streaming analysis → SongMap (HPSS + pseudo-stems inside); binary cache with a version
  director.h / .cpp         scene choice within the tier, transitions, intensity, likes / skips
  post.h / .cpp             bloom, tone map, finish, flash limiter, upscale + sharpen
shaders/dasvis/
  dasvis.hlsli              the uniform block + shared library (noise, SDF, palettes, easing, ray march, colour)
  post/*.hlsl
  sims/*.hlsl               particles (state textures), fluid, reaction-diffusion: all pixel shaders
  scenes/<name>/scene.json + *.hlsl
src/ui/view_visualizer.cpp  + DasVis mode, chip, keys, menu, HUD line
tests/dasvis_tests.cpp
cmake/dasvis.cmake          fxc compile of every .hlsl → <exe>\dasvis\*.cso (+ sources, for hot reload)
```

`DasVis` exists only while full screen is open in DasVis mode, as MilkDrop does today. When it closes, every GPU
resource is released, and so are the song maps and the analysis thread's buffers.

## 6. The song map (the brain)

Built **only while DasVis is open:**
- The current song is analysed at once. It plays *Live* (§6, "Using it") for those few seconds, then switches over
  smoothly.
- The next song in the queue follows on the same background thread.

It's cached in `_cache\dasvis\maps\<track id>.bin` (about 60 KB a song), with a version number so better analysis
rebuilds old maps. A song is analysed once, ever.

### The steps (one streaming pass through the song)

1. **Decode:** `decode_mono` (22.05 kHz), the existing decoder.
2. **STFT:** 2,048-sample window, 512 hop (23 ms), log-magnitude, computed frame by frame.
3. **HPSS** (harmonic–percussive separation, Fitzgerald 2010), over a sliding window of 17 frames:
   - A median over the 17 frames along time keeps what's *harmonic*.
   - A median over 17 bins along frequency keeps what's *percussive*.
   - Soft masks split the frame in two.
4. **Pseudo-stems**, each an onset envelope (half-wave rectified spectral flux) plus a list of events (adaptive
   threshold, peak picking). Envelopes are stored as bytes:

   | Stem | Source | Range |
   |---|---|---|
   | kick | percussive | 40–120 Hz |
   | snare / clap | percussive | 150–400 Hz + 1.5–4 kHz |
   | hats / cymbals | percussive | 6–11 kHz |
   | bass | harmonic | 40–250 Hz (envelope plus a rough pitch) |
   | lead / vocals | harmonic | 300 Hz–4 kHz (envelope plus chroma) |

5. **Beat grid:**
   - The BPM comes from the existing `tempo()`.
   - Beat times come from dynamic-programming beat tracking (Ellis 2007) on the combined onset envelope.
   - **Downbeats:** the beat offset (0–3) whose kick and bass onsets are strongest, bar by bar.
   - **Phrases:** 8 and 16 bars from the first strong downbeat.
6. **Energy:** loudness per beat (RMS, K-weighted like the LUFS measure), smoothed over 4 beats.
7. **Sections:**
   - Beat-synchronous features (32 log-mel bands + 12 chroma, a few KB) go into a self-similarity matrix, and Foote
     novelty (a 16-beat checkerboard kernel) finds the section boundaries. A 4-minute song at 128 BPM is about 500
     beats: a 1 MB matrix for a moment.
   - Boundaries snap to the nearest phrase start.
   - Each section is labelled from its energy and density:

     | Label | What it looks like |
     |---|---|
     | `intro` / `outro` | quiet, at the ends |
     | `breakdown` | low energy, few kicks |
     | `build` | energy rising, snare / hat density rising, high-frequency riser energy rising |
     | `drop` | a big energy jump after a build, or the loudest section |
     | `verse` / `chorus` | the rest, by energy |
8. **Drops:**
   - A drop is the start of a `drop` section, scored by how big the jump is.
   - The two bars before a drop often go quiet (the "silence before the drop"). That gap is marked, so the visuals can
     hold their breath too.

### Using it while the song plays

**Position.** The heard position = the playing position of the sample on screen now. It comes from the same
`heard_index` that MilkDrop's beat sync uses, so the map and the picture can't drift apart. A seek just looks up the
new position.

**Live mode.** Internet radio, or a song still being analysed, runs **live-only**:
- a phase-locked loop on the live kick onsets gives the beat phase
- energy-only sections, and no anticipation

The HUD says *Live* in that case.

## 7. The uniform block (what every shader can read)

Like Shadertoy's built-ins, plus the music. One 256-byte constant buffer, updated once a frame:

```hlsl
cbuffer DasVis : register(b0) {
    float  time, dt;              // seconds since the scene started; frame time
    float2 res;                   // internal render size in pixels (after dynamic resolution)
    float  quality;               // 0–1: scales loops (march steps, octaves, taps). Low tier ≈ 0.3, High = 1
    float  seed;                  // from the track id: this song's look
    float  bpm;
    float  beat;                  // 0→1 across each beat (sawtooth); beat_i = whole beats so far
    float  bar, phrase;           // 0→1 across each bar / 16-bar phrase
    float  beat_i, bar_i;
    float  kick, snare, hats, bass, lead;   // envelopes 0–1: fast attack, ~150–400 ms release
    float  kick_hit;              // 1 on the frame of a kick, else 0 (for spawning)
    float  energy;                // section-smoothed loudness 0–1
    float  section, section_t;    // section kind (enum) and 0→1 progress through it
    float  build;                 // 0 outside a build, rises to 1 at the drop
    float  drop;                  // 1 on the drop's first beat, decays over a bar
    float  to_drop;               // bars until the next drop (99 = none)
    float  live;                  // 1 when there's no song map
    float3 key_tint;              // from the key: Camelot wheel → hue; minor cooler, major warmer
    float3 pal[5];                // from the cover art (k-means in Oklab), sorted dark → light
    float  intensity;             // the user's Chill 0.5 / Normal 1 / Insane 1.6
    float  flash_budget;          // what the limiter still allows this second (0–1)
};
Texture2D spectrum;   // 256 log bins (R) and smoothed (G): 2 KB
Texture2D wave;       // 512 samples, trigger-aligned
Texture2D history;    // the last 128 spectra (a scrolling spectrogram: terrain, ridges): 64 KB
Texture2D cover;      // album art, 512² with mipmaps
Texture2D prev;       // last frame's composite (feedback)
Texture2D noise;      // 128² tiling blue noise (3D noise is computed, not stored)
```

`dasvis.hlsli` holds the shared library, all our own code or MIT:
- noise: simplex, curl, fbm, Voronoi
- SDF primitives and operators, a ray marcher with a `quality`-scaled step count
- Oklab colour, cosine palettes, ACES
- easing curves and hashes

## 8. Scenes: the launch set

Scene layers:
- **World:** fills the screen.
- **Mid:** particles and shapes, drawn additively over the world.
- **Overlay:** rings, titles and type.
- **Event:** one-shot moments, used at drops.

Each scene has a `scene.json` (name, layer, tags, energy range, **cost**, passes, resources) and one or more `.hlsl`
files. Shaders read the uniforms directly; there's no mapping language to learn.

**Cost classes** (measured on the Intel UHD at 720p, `quality` 0.3):

| Class | Measured cost | Where it runs |
|---|---|---|
| **L** (light) | < 2 ms | everywhere |
| **M** (medium) | < 5 ms | Low tier at low quality, everything else fully |
| **H** (heavy) | anything more | Medium and High tiers; on Low only if the measured frame time allows it at 33 % scale |

**A Low-tier PC still gets 15 of the 21 scenes.**

### World

| # | Scene | Cost | Look | What the music does |
|---|---|---|---|---|
| 1 | **Feedback Classic** | L | MilkDrop's soul in HDR: a warp field (zoom, rotation, swirl) over the last frame, with waveforms drawn in | bass = zoom pull, beat = rotation step, hats = shimmer, drop = the warp inverts for a bar |
| 2 | **Neon Tunnel** | L | Endless tunnel of glowing rings and panels (worked out in polar space, no ray marching) | one ring per beat; kick sends a light pulse down the tunnel; build narrows it and speeds up; drop shatters the walls into particles (event 19) |
| 3 | **Mandelbox Flight** | H | Fractal fly-through, glossy, with ambient occlusion and fog | bass changes the fold scale; the camera path changes on each phrase; snare = specular glints; key tint lights the fog |
| 4 | **Ink Ocean** | M | Fluid simulation with dye in the cover's colours | kick = radial pressure burst; lead = curl streams following the melody; drop = all five palette dyes at once |
| 5 | **Ridge Lines** | L | The *Unknown Pleasures* ridges in 3D from the spectrogram history (line strips, not ray marching), flown over at dusk | the bass lights the sun on the horizon; hats sparkle the ridge tops; build lifts the camera |
| 6 | **Aurora** | M | Layered curtains over a night lake with reflections | lead brightens and folds the curtains; key sets the hue; calm sections only |
| 7 | **Coral** | M | Reaction-diffusion growing organic patterns, lit as relief | energy moves feed / kill between "spots" and "maze"; kicks seed new growth |
| 8 | **Synthwave** | L | Outrun sun, neon grid floor, mountains made from the spectrum | grid scrolls on the beat; sun stripes pulse with bass; drop = the sun flares |
| 9 | **Liquid Chrome** | H | Ray-marched metaballs reflecting the cover art | one blob per stem, size from its envelope; they merge on the bar |
| 10 | **Galaxy** | M | Spiral arms of particles (16k on Low, 1M on High) | arms turn on the bar; kick = a density wave; drop = supernova (core flash, outward shell) |
| 11 | **Kaleido Cover** | L | Kaleidoscope of the album art, 6–12 segments | segments change on the phrase; rotation on the beat; bass zooms |
| 12 | **Storm** | H | Clouds lit from inside, procedural lightning | snare = a bolt (within the flash budget); bass = thunder rumble on the camera |
| 13 | **Hyperspace** | L | Starfield, then streaks | build stretches the stars; drop = jump to light speed |

### Mid and overlay

| # | Layer | Cost | What |
|---|---|---|---|
| 14 | **Particles** | L–M | 16k–1M particles on a curl-noise field: kicks emit, hats sparkle, lead steers the flow; coloured from the palette |
| 15 | **Spectrum Ring** | L | A circle of bars round a spinning disc of the cover, with peak caps (Winamp's bars, grown up) |
| 16 | **Ribbon** | L | The waveform as a glowing 3D ribbon trailing through space |
| 17 | **Title Card** | L | At each song's start: the title and artist in kinetic type (drawn once with DirectWrite into a small texture), letters arriving on the beats, then dissolving into particles |
| 18 | **Beat Sigils** | L | Geometric marks (rings, triangles) that stamp on kicks and fade out |

### Event (one-shots at drops)

| # | Event | Cost | What |
|---|---|---|---|
| 19 | **Cover Shatter** | M | The cover breaks into Voronoi shards that fly at the camera on the drop and reassemble over the next phrase |
| 20 | **Shockwave** | L | A refraction ring from the centre, plus chromatic split |
| 21 | **White-out** | L | Bloom to white over one beat, then the new scene fades in from it (within the flash budget) |

## 9. The director

1. **Tags.** Each scene declares:
   - its layer
   - the energy it suits (calm / mid / peak)
   - a tempo range
   - its cost class; its measured cost on *this* PC is kept in `dasvis.json`
2. **Picking.** At each **section boundary** (snapped to a phrase), it picks a world scene for the new section's energy,
   plus mid and overlay layers.
   - Only scenes **that fit the frame budget on this PC** are picked: world + layers + post must fit, at the current
     render scale.
   - The pick is weighted-random from the track seed, and never repeats one of the last 4.
   - Drops get a *hero* world and an event:
     - heroes: Tunnel, Galaxy, Hyperspace, plus Mandelbox where it fits
     - events: Shatter, Shockwave, White-out
   - Breakdowns get calm worlds (Aurora, Ink Ocean, Ridge Lines).
3. **Transitions** happen on the phrase's downbeat and last 1–4 beats:

   | Transition | When |
   |---|---|
   | crossfade | calm to calm |
   | noise-luma wipe | default |
   | zoom-through (into the feedback) | build to drop |
   | shatter | drops |
   | glitch (pixel-sort, RGB split) | genre tags: dubstep, DnB |

   - Two worlds render at once only during a transition, and the render scale dips for those beats.
   - On Low tier, a transition with a heavy scene on either side becomes a **cut through the white-out**: one world at
     a time.
4. **Within a section,** scene parameters drift from the track seed, so a long section keeps evolving and doesn't loop.
5. **You:**
   - **N / P:** next / previous scene
   - **L:** lock
   - **❤ (F):** like this scene
   - **Skip (X)**
   
   Likes and skips change that scene's weight for the current genre; they're saved in `dasvis.json` in the settings
   folder.
6. **Intensity:** **Chill / Normal / Insane**. It scales how many layers run, particle counts, cut frequency and how
   far the drop goes, always within the tier's budget.

## 10. The phases (each one ships as a release: 0.7.4, 0.7.5, …)

Every phase has to pass the **Low-tier check** (§12) before it ships, not just look good on this PC.

### Phase 0: the foundation (no visible change) ✅ 2026-10-11
- `Gfx` moved from `ID2D1HwndRenderTarget` to **a D3D11 device (feature level 11_1 down to 10_0) + a DXGI swap chain
  + `ID2D1DeviceContext`** (`src/ui/gfx.*`).
  - `ID2D1DeviceContext` *is an* `ID2D1RenderTarget`, so `Gfx`'s drawing code didn't change.
  - Device loss uses the same recreate path (the bitmap generation counter); so do DPI changes and going into or out of
    full screen.
- **Changed from the plan while building it:**
  - **The swap chain uses the bit-block-transfer model** (`DXGI_SWAP_EFFECT_DISCARD`), not flip model. It's what
    Direct2D's window target used, so the `EDIT` child windows, PrintWindow and the beat-sync timing are unchanged.
    Flip model (waitable, a frame less latency) can come in phase 1, for the full-screen window only, once beat sync
    can be re-measured with it.
  - **The fast-GPU switch moved to phase 1.** Nothing uses it until DasVis exists; full screen already rebuilds the
    device, so phase 1 only has to pick the adapter there.
- **Checks, measured:**
  - **Memory walk** (`build/dev/memtest.ps1`, 287-track copy, private MB, old → new):

    | Step | Old | New |
    |---|---|---|
    | Home, playing | 42.6 | 42.6 |
    | All tracks | 48.3 | 49.0 |
    | Scrolled (covers) | 55.1 | 55.5 |
    | Details open | 53.5 | 53.7 |
    | Full-screen MilkDrop | 110.5 | 111.6 |
    | Back from full screen | 43.4 | 37.0 |
    | In use again | 50.4 | 47.9 |

    The first try cost +40 MB in full screen: the device was kept across full screen, and with it every cached cover
    and text layout. Going into and out of full screen now rebuilds it, as the old target was rebuilt.
  - **First paint:** median 119 ms old, 124 ms new over 10 launches each (both with occasional 300 ms outliers):
    within noise. Creating the D3D11 device takes ~45 ms, as it did inside the old target.
  - **Screenshots:** Home, All tracks and Soulseek are pixel-identical to the old build; Settings differs only at the
    scrollbar's end (content still settling). Resizing the window redraws correctly.
  - **Full screen:** a steady 16.6 ms a frame (60 fps), vsync-paced.
  - **Feature level 10_0** (`WRECKBOX_FL10=1`) draws the same pixels; **WARP** (`WRECKBOX_SOFTWARE=1`, "Microsoft Basic
    Render Driver") works.
  - All test suites pass.
  - Still to do by you: listen to MilkDrop with real music; beat sync should feel the same as before.

### Phase 1: the engine and the first three scenes ✅ 2026-10-11
**Built** (`src/dasvis/`, `shaders/dasvis/`, `cmake/dasvis.cmake`, `tests/dasvis_tests.cpp`):
- **Features** (`features.*`):
  - kick / snare / hats onsets from spectral flux, each with an envelope
  - bass / lead / energy levels with automatic gain, and a −60 dB floor so near-silence can't fire onsets
  - a beat clock: the library's BPM when known, otherwise from the kicks; its phase is pulled onto the kicks, never
    runs backwards, and re-locks if it's half a beat off
  - the spectrum and waveform textures
- **Controllers** (`control.*`): the flash limiter (WCAG counting with an 80 ms reference, so slow fades aren't
  flashes) and the tier and scale controller.
- **Renderer** (`dasvis.*`):
  - scene → bloom (dual-filter, no threshold, mixed in) → final pass at the screen's size: sharpen, chromatic
    aberration, ACES, vignette, the spectrum ring, grain, gamma
  - the final pass draws **straight into the window's back buffer**, so DasVis keeps no full-screen texture
  - a 16 × 16 brightness read-back, two frames late, for the limiter
  - timestamp queries for the tier controller
  - the render size re-made only in 5 % steps, with the picture carried over (no black frame)
- **Scenes:** Feedback Classic, Neon Tunnel (with the spectrum ring), Kaleido Cover; your own scenes from
  `%APPDATA%\local.wreckbox\wreckbox\dasvis\scenes` (an example and `dasvis.hlsli` are written there), F5 reloads.
- **UI:**
  - the DasVis / MilkDrop / Bars chips, and **D**
  - N / P / L / click for scenes
  - the scene's name with "High · 85 %" on the panel
  - the menu's DasVis submenu: Intensity, Quality (Auto / Low / Medium / High, with what it's running at), Save
    battery, Reduce flashing, Use the fast graphics card, Open my scenes folder, Reload my scenes
- **Settings** under `visualizer.dasvis`; the measured tier is remembered per graphics card.

**Changed from the plan while building it:**
- **Shaders are built into the exe** (`fxc` → headers), not `.cso` files: nothing to ship or load, and the tests get them
  too. Only your own scenes are compiled at run time.
- **No scene.json yet:** a scene is one `.hlsl` file. The tags the director needs come in phase 5.
- **The final pass draws into the window's buffer** instead of a texture Direct2D draws: 8 MB less at 1080p (33 MB at
  4K) and one full-screen copy less a frame. Paused, `redraw()` repeats only the final pass.
- **"Use the fast graphics card" is off by default.** Measured: on the GeForce, DasVis is 2–3 ms a frame but the app
  uses **144 MB**, because NVIDIA's driver alone is ~70 MB. On the Intel UHD it runs the High tier at 7–8 ms and the
  app uses **64–69 MB**. GeForce Experience also shows its "Alt+Z" notice when an app starts on the GeForce.
- **Frame pacing:** 60 fps even on a 120 / 144 Hz screen (every n-th refresh), 30 fps on battery.

**Measured** (dev PC, 1920 × 1080, muted test music):

| | Result | Budget (§4) |
|---|---|---|
| **Low-tier check:** Intel UHD, feature level 10_0, 2 cores, Low | **5.1 %** of one core for the whole app (audio, UI and DasVis); **+19.1 MB** while open; 3.3 ms GPU at 60 % scale | < 5 %, ≤ +20 MB |
| Auto on the same | picked **High at 85 %**: 7.2–7.4 ms GPU, 8 % CPU, +25.9 MB | ≤ +30 MB |
| Back after closing | 38.4 MB (from 38.8 before opening) | back to where it was |
| MilkDrop, for comparison | 26–30 % CPU, ~+65 MB | |
| Memory walk (MilkDrop path) | unchanged: 108–115 MB in full screen over 3 runs; 134 once (a heavy random preset) | ±3 MB |

- **Tests:**
  - features: 22–24 of 24 kicks found, no kicks in hats or silence, tempo within 2 BPM, the clock on every kick
    after 6 s
  - limiter: ≤ 3 flashes in any second against a 7.5 Hz strobe, ≤ 1 with Reduce flashing; steady light and slow fades
    untouched
  - tier controller: weak, middling and strong fake GPUs settle under budget without see-sawing
  - every scene rendered on WARP at Direct3D 11 and at feature level 10_0 (not black or white, moving, the feedback
    bounded, `redraw()` identical)
  - your scenes: one that compiles and one whose error is reported
  - All 12 suites pass.
- **Still to do by you:** watch it with real music. The look and timing were checked on test music and screenshots,
  not by eye with songs you know.

**As planned:**
- **GPU choice** (moved from phase 0): `IDXGIFactory6::EnumAdapterByGpuPreference`.
  - The window runs on the power-saving GPU.
  - Opening DasVis builds the full-screen device on the fast one (full screen already rebuilds the device), and closing
    it goes back.
  - A setting overrides this: *Always / Only for visuals / Never*.
- `gpu.*`: R11G11B10 targets, ping-pong pairs, timestamp queries per pass, `.cso` loading, hot reload of user scenes.
  - User scenes live in `%APPDATA%\local.wreckbox\wreckbox\dasvis\scenes`.
  - **F5** reloads them; compile errors show on screen, not as a crash.
- **Tiers** (§4): measured, remembered per GPU, dynamic render scale, the `quality` knob, the 30 fps cap on battery.
- Post chain:
  - bloom at quarter resolution, 3–6 levels by tier
  - ACES tone mapping with exposure
  - finish: grain, chromatic aberration, vignette
  - the **flash limiter**
  - upscale + RCAS sharpen
- **Live features:** from the shared FFT; kick / snare / hats onsets from live band flux; envelopes and springs; a
  phase-locked loop for the beat.
- Scenes (all Light): **Feedback Classic, Neon Tunnel, Kaleido Cover**, plus the Spectrum Ring overlay.
- UI:
  - a **DasVis** chip next to MilkDrop / Bars
  - **D** switches to it
  - the panel shows the scene's name
  - a right-click menu with Intensity, Quality (Auto / Low / Medium / High), Save battery, Reduce flashing and Open
    scenes folder
- DasVis is drawn straight from the shared texture: no read-back, and one frame less latency than MilkDrop.

### Phase 2: the song map ✅ 2026-10-11
**Built** (`src/dasvis/songmap.*`, `decode_mono_stream` in `src/engine/decode.*`):
- **One streaming pass:** the decoder now hands samples over in 4,096-sample chunks (`decode_mono_stream`;
  `decode_mono` is built on it and unchanged), so the song is never held whole.
  - STFT 2,048 / 512 at 22.05 kHz
  - HPSS from medians over 17 frames × 17 bins
  - five pseudo-stem levels, plus per frame: whole-spectrum flux, RMS, 32 log bands and 12 chroma
- **From those summaries:**
  - stem onsets, envelopes (bytes) and hits
  - the tempo: the library's BPM; otherwise the onsets' autocorrelation, scored on 4 multiples of each lag
  - the beat grid: dynamic programming over an onset curve of kick + snare + bass (hats are often off the beat),
    refined to a fraction of a frame; the final BPM is the slope of a line through the beats
  - downbeats: bass onsets + chord change + kick, per beat offset
  - sections: Foote novelty from a 32-beat kernel worked out band by band (no full self-similarity matrix), plus
    energy change, snapped to 4-bar boundaries; same-kind neighbours merged
  - labels from energy, kick density, rise and snare / hat density; drops and the quiet bars before them
- **Cache:** `_cache\dasvis\maps\<hash of id + file size + date>.map`, version-checked, written atomically. A song is
  analysed once.
- **The app:**
  - analysis on a worker thread at background priority, only while DasVis is open: the current song, then the next
    one in the queue
  - radio stays live
  - the renderer takes the beat / bar / phrase / section / build / drop / gap from the map at the heard time; the
    live kicks pull a steady error of more than 25 ms back into step (the dev view shows it: +0 to +3 ms on real
    songs)
  - a new scene on each drop; auto-advance waits for a 4-bar boundary
- **Shaders:** `u_song` / `u_drop` (PHRASE, SECTION, SECTION_T, BUILD, DROP, TO_DROP, GAP, PART_ENERGY).
  - Every scene through the final pass: colour drains and the vignette closes as a build-up rises, the breath before the
    drop dims, the drop punches (within the flash budget) and splits the colours.
  - Per scene: the tunnel narrows and lights every ring on the drop; Feedback Classic pulls harder and bursts; Kaleido
    brings more mirrors into view.
- **Panel:** *Build-up · drop in N bars* (peach), the section's name, *Analysing the song…*, *Live*. **Ctrl+Shift+D**
  draws the map (or `WRECKBOX_DASVIS_DEV=1`). `dasvis_tests --map <file> [bpm]` prints any song's map.

**Measured:**
- **Synthetic 128 BPM song** (intro, build-up with a silent last bar, drop, breakdown, drop):
  - beats: median error 2.5 ms, 99 % within 25 ms, with or without the BPM given
  - downbeats right
  - parts exactly *Intro 0 · Build-up 16 · Drop 24 · Breakdown 40 · Drop 48*
  - both drops within a beat; the breath found at bar 23
- **6-minute song:** **4.4 s and 6 MB** at most while analysing (budget: 30 MB; the whole song as floats would be
  32 MB).
- **Real songs from your library** (read-only):
  - *Fake Acid*: 133.0 BPM, intro → drop → … → breakdown 2:03 → build-up 2:17 → drop 2:32 → … → outro, in 5.8 s
  - *Let It Happen*: 125.0 BPM
  - *Galactic Symphony*: 99.6 BPM, the same as the library's own analysis
  - In the app, the map was ready 7 s into *Fake Acid*. Through the build-up the panel counted *drop in 4 bars* and the
    picture drained and tightened; the drop's first beat changed the scene.
- **Low-tier check:** 5.5–5.9 % of one core for the whole app (phase 1: 5.1 % in one run), +18.7 MB; the analysis
  itself (1 s for the test file) runs before and outside that.
- All 12 suites pass.

**Changed from the plan:**
- The kernel is 32 beats, not 16: 16 cut on 4-bar chord cycles in intros.
- Boundaries snap to 4 bars, not 8–16: songs aren't always built in 16s.
- The pseudo-stem envelopes are in the map but not yet used: that's phase 3.
- **Still to do by you:** watch a few songs you know through their drops. If the panel names a part wrong, tell me the
  song and the time.

**As planned:**
- §6 in full: streaming STFT and HPSS, pseudo-stems, beat grid, downbeats, phrases, sections, drops; the binary cache;
  analysis only while DasVis is open, at background priority.
- The uniforms gain `bar`, `phrase`, `section`, `build`, `drop` and `to_drop`.
- **HUD** on the panel's preset line: *Build-up · drop in 4 bars*, *Drop*, *Breakdown*, *Live*.
- A dev view (**Ctrl+Shift+D**) draws the map over the picture: beats, bars, sections, drop markers, and the playhead.
- Tests use synthetic songs generated in the test:
  - a 128 BPM kick, 16 quiet bars, an 8-bar riser, then a loud section
  - Assert: beats within 20 ms, downbeats right, the build found, the drop within one beat of the truth.
  - Also: the cache round-trips, a changed version rebuilds the map, and peak memory during analysis stays under the
    budget.

### Phase 3: stems drive the layers ✅ 2026-10-11
**Built:**
- **`dasvis::drive()`** (`dasvis.*`), what moves the pictures each frame:
  - **With a map:** kick / snare / hats are the map's envelopes, with 60 % of the live ones on top for anything the map
    missed. Bass and lead are the map's harmonic envelopes. The hits are the map's in the frame's (t0, t1], none
    across a seek. *Time since the last kick* comes from the map's hits.
  - **Without a map:** the live features, and its own count of time since the last kick.
- **Shaders:** `u_hits` (KICK_HIT, SNARE_HIT, HATS_HIT, SINCE_KICK); BASS and LEAD are documented as the harmonic
  stems.
  - **Neon Tunnel:** each kick's pulse races down the tunnel at a constant speed (from SINCE_KICK, not the fading
    envelope); the snare flashes the seams; the lead shifts the ring colours.
  - **Feedback Classic:** the snare flashes a wider ring that the zoom carries outwards; the lead turns the ink's
    colours.
  - **Kaleido Cover:** the hats scatter glints over the pattern (mirrored with it).
  - **Spectrum ring:** the rim flashes on the snare.
- **Palette from the cover** (`palette.*`):
  - k-means (k = 5, started from lightness quantiles, so the same cover always gives the same colours) in Oklab on a
    ≤ 64² thumbnail in linear light, with hue counted double
  - the clusters sorted dark → light, set on a fixed lightness ladder, chroma lifted 30 %, kept inside sRGB
  - grey covers (black-and-white photos) give none: the key's palette then
  - the palette on screen fades to a new one over about a second
- **Key colour:** the Camelot hue; minor keys are nudged a sixth of the way towards blue and softened.

**Measured:**
- **In the app** with *Afterhours* (a red / cream neon cover): the kaleidoscope is the cover itself, Feedback Classic
  and Neon Tunnel take its colours, and the spectrum ring spins it in the tunnel. The map labelled the song's parts
  and analysed the next song in the queue in the background.
- **Tests:** a red / blue / white cover gives a red and a blue, dark → light, inside sRGB, the same every time; a grey
  cover and no cover give none; 1A is cooler than 1B. A kick from the map fires on the frame after it, the snare
  likewise, a seek fires nothing, time since the kick counts right, live and map alike. All 12 suites pass.
- **Low-tier check:** 8–9 % of one core this time, against 5.5–5.9 % in phase 2. An A/B with the phase 2 shaders on
  the same build measured the same (8.9–11.5 %). The difference is the PC's state, not phase 3: the CPU was at 54 % load
  from other apps, and warm after an hour of builds. The UI thread's own work per frame is unchanged at ~0.5 ms.
  Memory: +18.7 MB, unchanged.
- **Still to do by you:** look at a few songs with colourful covers, and a few with black-and-white ones.

**As planned:**
- The five pseudo-stem envelopes and the `kick_hit` events come from the map in step with the heard position, with
  live amplitude on top.
- Every phase 1 scene is rewired to use stems (tunnel pulses on kick, rings flash on snare, and so on).
- **Palette from the cover:** k-means (k = 5) in Oklab on a 64² thumbnail, once per album.
- **Key colour:** Camelot number → hue, minor shifts cooler.

### Phase 4: simulations (pixel shaders, scaled by grid size) ✅ 2026-10-11
**Built:**
- **How simulations run** (`dasvis.cpp`): each built-in scene names its simulation. It's made when the scene comes on
  screen, at the tier's size, and dropped when it goes, so a PC only holds the one on screen. Everything is pixel
  shaders on ping-pong textures (feature level 10_0 has no compute shaders); points, stars and ridges are drawn with no
  vertex buffers (the vertex shader works out each one from `SV_VertexID`). A shader file can now hold a scene and its
  simulation's passes (`dasvis_shader(name file profile entry)`). New in `dasvis.hlsli`: `u_sim`, `t_sim` (t5) and
  `hashu()`, an integer hash that stays random for big inputs (particle numbers, beat counts).
- **Grids by tier:** 128 / 192 / 256 cells tall (Low / Medium / High).
- **Particle layer** over Feedback Classic (`sims/particles.hlsl`): 16k / 37k / 65k particles; position and velocity in
  an RGBA32F texture, moved by curl noise (faster with the lead), born on the ring, thrown outwards on kicks, sparkling
  on the hats; drawn as points added to the picture, so the feedback turns them into trails. Each is fainter where
  they're crowded.
- **Ink Ocean** (`scenes/ink.hlsl`): stable fluids (advection, vorticity confinement, divergence, 8 / 12 / 16 Jacobi
  steps, gradient), flow and pressure in RG16F. The kick fires three jets that move on every beat; the lead keeps a
  current going round; the bass turns the whole ocean; the drop fires five jets, one per palette colour. The dye is the
  picture itself (the last frame carried by the flow), so it has no texture of its own.
- **Coral** (`scenes/coral.hlsl`): Gray-Scott, 2 / 3 / 4 steps a frame (the same growth speed on screen at every
  tier), in RG32F (RG16F where the card can't filter 32-bit floats), lit as relief. Energy moves it from spots to
  mazes; kicks seed growth; a patch dies back on each bar and the drop clears a ring, so it never sets solid.
- **Galaxy** (`scenes/galaxy.hlsl`): 64k / 256k / 1M stars, each on an elliptical orbit whose tilt grows with its size
  (the density-wave way spiral arms form); nothing is stored. Arms turn on the bar, kicks send a density wave out,
  the bass lights the core, the drop is a supernova (a shell of stars flies out).
- **Ridge Lines** (`scenes/ridges.hlsl`): 128 rows of spectrum history (R32F, 128 KB, a row every 1/30 s); 64 / 96 / 128
  ridges of 96 / 160 / 224 points, each a line with a dark curtain under it, drawn back to front (no depth buffer, no
  ray marching). Dusk sky and sun: the bass lights the sun, the hats sparkle on the ridges, a build-up lifts the camera.
- **Fewer uploads:** the constants are only uploaded when they changed, so the fluid's many small passes cost no
  more CPU than one.

**Measured:**
- **Low-tier check** (feature level 10_0, Low, 2 cores, same session), GPU time a frame against Neon Tunnel's 4.5 ms:
  Coral 2.4, Ridge Lines 2.4, Ink Ocean 2.7, Galaxy 3.3, Feedback Classic with particles 4.1 ms. CPU 8–9 % of one
  core, the same as Neon Tunnel's in an A/B (before the upload fix Ink Ocean was 13.8 %). Memory **+19 to +20 MB** while
  open, back to where it was after closing, for every scene.
- **High tier on the Intel GPU:** each simulation adds about 2 MB. Ink Ocean 6.2 ms (13.7 ms with 512-cell grids,
  which is why they're capped at 256). The whole of DasVis at High and full size is +31 to +34 MB, over the +30 MB
  budget whatever the scene (Ridge Lines measures +31.6): the full-size HDR targets from phase 1, not the
  simulations. To fix in phase 5 or with you: cap High's render scale at 90 %, or share one HDR target.
- **Tests:** every scene renders on WARP at Medium (Direct3D 11) and at Low (feature level 10_0): not black, not
  white, moving; Feedback Classic with its particles stays bounded over 10 s. All 12 suites pass.
- **In the app** (muted test library): screenshots of all seven scenes at High and Low.

**Changed from the plan:**
- **Grids up to 256, not 512, and 65k particles, not 1M:** an integrated GPU's textures come out of the PC's RAM, and
  512 cost ~10 MB more for little visible gain (the detail on screen is the full-size picture drawn over the grid).
- **The fluid's dye is the picture itself,** not a separate texture twice the grid's size (no memory at all).
- **Galaxy's stars aren't simulated:** each one's place is worked out from the time, so a million stars cost no
  memory.
- **Points, not point sprites:** one pixel each, at the internal size, softened by the bloom and the trails.

**As planned:**
- **Particles:**
  - Positions and velocities live in two state textures (128² to 1024² by tier), updated by a pixel shader.
  - They're drawn as point sprites: a vertex shader reads the state with `Load` (SV_VertexID).
  - Emit, curl, attract, die; additive in HDR.
- **Fluid:** stable fluids at 128²–512²: advect, divergence, 8–20 Jacobi pressure iterations by tier, subtract
  gradient. The dye is twice the grid size.
- **Reaction-diffusion:** Gray-Scott at 128²–512², 2–8 steps a frame by tier.
- Scenes: **Ink Ocean, Coral, Galaxy, Ridge Lines**, and the **Particles** mid layer.

### Phase 5: the director ✅ 2026-10-11
**Built:**
- **`dasvis::Director`** (`director.*`), pure C++ with no GPU, so it's tested on its own:
  - **Tags** for each built-in scene: the energy it suits (calm / mid / peak), its cost class, *hero* (Neon Tunnel,
    Galaxy), whether the particle layer may go over it, whether the spectrum ring suits it. Your own scenes count as
    light ones that suit anything.
  - **Picking:** among the scenes that fit (measured cost at the tier's smallest render size within the frame budget;
    not measured yet: anything but a heavy scene on Low), never the one on screen nor one of the last 4 if there's
    anything else; weighted ×3 for the right energy (×0.4 otherwise) and by your taste. A drop gets a hero. The random
    numbers come from the song's seed and the pick's number, so a song plays out the same way every time.
  - **Layers:** the particle layer (now its own simulation, over Feedback Classic, Neon Tunnel or Kaleido Cover) never
    on chill, half the time on normal outside calm parts, always on insane when there's room (the scene costs at most
    70 % of the budget). The spectrum ring where the scene suits it, and on every peak on insane.
  - **Transitions:** crossfade (calm to calm, 4 beats), noise wipe (the default, 2), zoom through (build-up to drop,
    1), shatter (into a drop, 2: Voronoi shards with lit cracks), glitch (dubstep / drum & bass, 1: bands shifted and
    split into red, green and blue).
  - **Events on drops:** shockwave (a refraction ring with a colour split), white-out (to white over the last beat,
    back over the bar, within the flash budget), or both; chill gets only the shockwave.
- **When it picks** (`dasvis.cpp`): as each part of the song map starts (they start on a 4-bar downbeat); on a 16-bar
  phrase when a scene has been on for 64 / 32 / 16 bars (chill / normal / insane), not in a drop; without a map, when
  the scene's time is up (×2 on chill, ×½ on insane), on a bar's downbeat. N / P step to the next scene that fits.
- **Drift:** four slow waves (tens of seconds, from the song's seed) as `DRIFT`; every built-in scene lets one or two
  things wander with them (the swirl, the tunnel's lean, the kaleidoscope's path, the ink's current, the coral's
  colour, the galaxy's distance, the ridges' height).
- **You:** **F** likes the scene (×1.5, up to ×4) and **X** skips it (×0.6, down to ×0.15, and another one now), for
  the song's genre; a heart button in the panel and both in the right-click menu. Saved in the settings
  (`visualizer.dasvis.taste`), with what each scene measured per graphics card (`visualizer.dasvis.costs`), so the
  director knows what fits from the first frame next time.

**Measured:**
- **Tests:** the same seed gives the same 200 picks, another seed others; no repeats within 4; every drop gets a
  hero; on Low a scene measured over budget and a heavy one not measured yet are never picked; a liked scene came up
  73 times against 17 for a skipped one; chill has no white-out. In the renderer (WARP, a hand-made 88-bar map): the
  scene changes exactly at the parts' starts (16, 80, 96, 128, 160 s) and on the 16-bar phrase at 64 s, nowhere else;
  the drop gets a hero; the same song plays out the same twice. All 12 suites pass.
- **In the app** (*Fake Acid*, muted): Kaleido Cover through the breakdown; at the build-up a wipe; the colour drains
  and the vignette closes; the last beat whites out; Galaxy arrives with the drop. F filled the heart; X showed "Less
  Neon Tunnel" and moved to Coral; both were saved.
- **Low-tier check:** CPU 9–11 % of one core with the PC at 41 % load (the same as phase 4), memory +19.5 to +20 MB,
  back after closing.

**Changed from the plan:**
- **Two worlds never render at once.** A transition shows the new scene over the last picture (the feedback buffer
  keeps whatever the new one hasn't replaced yet), so a change costs nothing and the render scale doesn't dip; the Low
  tier's "cut through the white-out" for heavy scenes isn't needed.
- **Likes, skips and costs live in the settings** with the tiers, not in a separate `dasvis.json`.
- **Intensity scales the layers, cuts and drops,** not the particle count (the count follows the tier).

**As planned:**
- §9 in full:
  - budget-aware picking
  - layering (world + mid + overlay)
  - transitions on phrase downbeats
  - drop events
  - intensity
  - likes and skips
  - parameter drift from the track seed
- Deterministic for a given seed, map and tier, so it's testable.

### Phase 6: hero scenes and type ✅ 2026-10-11
**Built:**
- **Six scenes** (13 in all), every one Shader Model 4:
  - **Synthwave** (L): an outrun sun with bass-thickened stripes, a neon grid stepping on the beat, mountains from the
    spectrum's peaks; the drop flares the sun.
  - **Hyperspace** (L, hero): a starfield in layers whose stars stretch through a build-up and jump to light speed on
    the drop; faster with the energy and kicks (`TRAVEL`).
  - **Aurora** (M, calm only): curtains in the key's hue over a lake that reflects them; the lead brightens and folds
    them.
  - **Mandelbox Flight** (H, hero): a ray-marched Mandelbox (the standard distance estimate), orbit-trap colours, glossy
    with rim light and fog in the key's tint; the bass changes the fold; a new camera path each phrase.
  - **Liquid Chrome** (H): five ray-marched metaballs, one per stem, sized by it, reflecting the album cover; they flow
    together on the bar's last beat.
  - **Storm** (H): cloud layers lit from inside; the snare throws a lightning bolt (within the flash budget), kicks
    flicker the clouds, the bass shakes the camera.
- **Mid layer: Ribbon** (`sims/ribbon.hlsl`): the waveform as a twisting ribbon in 3D, one 1,024-vertex strip; the
  director now picks particles *or* the ribbon.
- **Events:** **Cover Shatter** joins the shockwave and white-out (the final pass): the cover breaks into 16 Voronoi
  shards that fly at you on the drop, come back over 8 bars and fade. Normal intensity picks one event per drop
  (shatter comes with a shockwave); insane plays all three.
- **Overlays:** **Beat Sigils** (a ring, triangle or hexagon stamped where each kick says, growing and fading; not on
  chill or in calm parts) and the **Title Card**: the title over the artist, drawn once a song by Direct2D straight
  into a 768 × 192 GPU texture (as outlines, with the window's own device context: no CPU pixels, no glyph atlas),
  letters arriving one each half beat with a flash, then dissolving into sparks at 7–9.5 s; the texture is freed at
  10.5 s.
- **N / P reach every scene** (your choice, the heavy ones too); only the director's own picks keep to what fits.
- **DasVis is the default visualizer.** Settings from before it existed (MilkDrop with no DasVis options) move to it
  once; choosing MilkDrop afterwards sticks. **M** is still one key away.
- New in `dasvis.hlsli`: `TRAVEL`, `SINCE_DROP`, `smin()`, `u_title`.

**Measured:**
- **Tests:** every scene (13 + the example) renders on WARP at Medium (Direct3D 11) and at Low (feature level 10_0),
  N reaches them all; the cover shatter shows outside the middle after a drop and nowhere before it; old MilkDrop
  settings move to DasVis once. All 12 suites pass.
- **GPU time on the Intel UHD:** Low tier (60 % scale) Synthwave 3.9, Hyperspace 4.0, Aurora 4.1 ms; Mandelbox 18.5 ms
  at 33 % scale, Liquid Chrome 7 ms at 40 %, Storm ~15 ms at High 70 %. The heavy ones measure over this PC's Low
  budget, so the director doesn't pick them there; on a faster GPU it will.
- **Memory (Low tier, feature level 10_0, 2 cores):** +20.7 to +21.4 MB while open (phase 5: +19.5 to +20), back to
  where it was after closing. The title card first cost +4.5 MB and kept 3 MB after closing (drawn on the CPU with a
  software Direct2D target); drawing it on the GPU as outlines and freeing it after 10 s brought that to nothing.
  About 1 MB over the +20 MB budget remains: the six extra scenes' compiled shaders.
- **In the app** (muted test libraries): every scene at High; *Afterhours* with its cover in Liquid Chrome, the title
  card, Aurora in the intro and Hyperspace with the spectrum ring after it.

**Found along the way (song map, phase 2):** with the library's analysed tempo (120.1), *Afterhours*' map has no
drop (Intro → Verse → Chorus); without a known tempo it finds one. Worth a look when the analysis is next tuned.

**As planned:**
- **Synthwave, Hyperspace** (Light); **Aurora** (Medium); **Mandelbox Flight, Liquid Chrome, Storm** (Heavy).
- Events: **Cover Shatter, Shockwave, White-out**.
- Overlays: **Title Card**, **Ribbon**, **Beat Sigils**.
- **DasVis becomes the default visualizer** (decision 6 in §3). MilkDrop stays one key away.

### Fixes after phase 6 (2026-10-12)
- **The song map's parts start on the beat where the loudness steps up** (×2 or more, within a bar of the 4-bar
  grid). A downbeat guessed two or three beats off had started *Afterhours*' drop a bar early with the quiet intro in
  it, so it read as a verse: no drop at the library's tempo. *Fake Acid*'s second drop also moved to where its kicks
  come back (149.8 s, was 151.6). A new test plays a drop two beats off an intro's clear bars: the old analysis put it
  two beats early, the new one on the beat. Map cache version 2 (old maps are made again).
- **Nothing is drawn while the window can't be seen** (the display asleep, a maximised window over it): a present then
  doesn't wait for a refresh, so DasVis had run at ~150 fps for nobody (19 % CPU). It now waits, asking a few times a
  second; CPU 1.6 %. `WRECKBOX_RENDER_HIDDEN=1` keeps drawing (paced) for measuring a test window behind others.
- **Low tier compiles the heavy scenes only when they're first shown** (the director doesn't pick them there): about
  1 MB. Settled memory at Low: +20.4 MB, sampled each second (the driver sometimes adds ~2 MB later).
- **High tier stops at 90 % render scale** (the final pass sharpens the upscale): ~3 MB less than full size, which was
  over its +30 MB budget.

### Phase 7: over the top ✅ 2026-10-12
**Built:**
- **Phone VJ deck** ✅ 2026-10-12:
  - A page on the phone-sync server (`src/net/vj_page.h`, plain HTML): `GET /vj?t=<token>`, commands by
    `POST /vj/cmd`, both behind the pairing token like every other route. *Phone VJ deck…* in DasVis's right-click
    menu starts the server if needed and shows a QR code for the page; it goes when a phone first uses it.
  - Controls: the XY pad (the camera: turn, zoom in, or a fisheye pinch out; applied in the final pass, so it moves
    every scene), *Drop now* (a hero scene, the punch, and that drop's events), *Shockwave*, ❤ / *Skip*, intensity,
    *Prev* / *Lock* / *Next*, a button per scene. The page shows the scene and the section (with the countdown to the
    drop) every 1.5 s.
  - Shaking the phone sends a shockwave where the browser allows motion events; phone browsers mostly allow them only
    on secure (https) pages, so the *Shockwave* button is there for the rest.
  - Commands wait in a queue for the next frame (the UI thread applies them); the state is written back each frame.
  - `WRECKBOX_SYNC_HOST=127.0.0.1` keeps the server on this PC (tests: no firewall prompt).
  - **Tested:** the routes refuse a missing or wrong token, serve the page, and pass commands to the handler
    (`sync_tests`); in the GPU test the deck picks a scene, *Drop now* brings a hero, the camera moves the picture. In
    the app (loopback, muted): 13 scenes listed, scene 6 → Galaxy, *Drop now* → Hyperspace at light speed, intensity
    set, the camera turned and zoomed.
- **Room lights (WLED)** ✅ 2026-10-12:
  - `net/wled.*`: WLED's realtime UDP (DRGB, port 21324, up to 490 LEDs a packet; each packet tells WLED to go back to
    its own effect 2 s after the last one, so closing DasVis hands the lights back). At most 40 frames a second, easy on
    a small controller's Wi-Fi. The address is looked up off the UI thread (a name like `wled.local` can take seconds).
  - `dasvis::led_frame()`: the palette's lighter colours along the strip, a step further each beat; brighter on the kick
    and the bass, lit on drops, a few LEDs glinting on the snare. The swings above a resting level are scaled by the
    flash limiter's budget (and halved with *Reduce flashing*), so the room never strobes faster than the screen may.
  - Settings → *Room lights (WLED)*: the address, the LED count, on / off (off by default). The Settings page now works
    on the saved visualizer options (they're only loaded while full screen is open; writing the in-memory ones there
    would have put defaults over the rest).
  - **Tested:** the kick brightens the strip, no swing without flash budget, a smaller one with *Reduce flashing*, the
    colours move with the beat (`dasvis_tests`); a frame reaches a UDP socket standing in for the strip as DRGB, cut to
    490 LEDs (`sync_tests`); the options round-trip (`player_tests`). In the app: 92-byte packets (30 LEDs) at 44 a
    second while DasVis played, brightness moving 99–147; toggling the lights changed only that setting.
  - Not done: Philips Hue (needs a Hue bridge: say if you have one).
- **Video export** ✅ 2026-10-12:
  - `dasvis/video.*`: one pass through the song. Media Foundation decodes it to 48 kHz stereo; for each frame the
    analysis gets the same 2,048-sample window the live player would, DasVis renders on its own Direct3D device at High,
    full size (`Config::fixed_scale`), every scene allowed; the frame is read back and written with the sound up to
    its time by Media Foundation's sink writer: H.264 (on the graphics card when it can; ~15 Mbps at 1080p60, up to
    100) and AAC 192 kbps, in an MP4. Memory stays small however long the song is. Cancel deletes the part-written file.
  - Right-click a downloaded song → *Export DasVis video* → 720p30, 1080p60, 1440p60 or 4K60 → a save dialog (Videos).
    It runs on its own thread at below-normal priority; the sidebar shows how far it's got, with *Cancel*; the folder
    opens when it's done. The cover, the song's key, its map, your intensity and likes, and the title card (drawn once
    by the app, handed over as pixels) make it look as it does in the app.
  - **Measured:** *Afterhours* (2:20) at 1080p60 on the Intel UHD in 160 s (1.1× the song's length), 266 MB; frames
    upright, colours right, the scenes changing as in the app.
  - **Tested:** a 16 s song on WARP at 320 × 180 becomes an MP4 with H.264 of that size, AAC and the song's length
    (read back with Windows' reader); cancelling leaves no file. `dasvis_tests --video` / `--frame` export a real song
    and pull a frame out as a picture.
  - Not done: HEVC (H.264 plays everywhere; say if you want HEVC for smaller 4K files).
- **Spout output** ✅ 2026-10-12 (`dasvis/spout.*`): the sender side of Spout 2 written here (no SDK): a shared
  B8G8R8A8 texture (legacy shared handle), the name in "SpoutSenderNames" (under its mutex), the 280-byte details under
  the sender's name, "ActiveSenderName" when none is active, copies under "<name>_SpoutAccessMutex". Right-click →
  DasVis → *Spout output*, for the full-screen visit; the picture is shared before the player panel is drawn.
  **Tested** with a stand-in receiver (finds the sender, opens the texture on another device, sees the frame; the name
  leaves the list after). Not yet tried with OBS or Resolume.
- **Live wallpaper** ✅ 2026-10-12 (`ui/das_window.*`, `View::wall_tick`):
  - A child window of the desktop's WorkerW (Progman is asked for it with message 0x052C, as live-wallpaper programs
    do), with its own swap chain and its own DasVis on the app's device, at Low (fixed), drawn on a timer: 30 frames a
    second at most, a 4 ms GPU budget. The buffer is 3/4 of the desktop's size, stretched by the compositor: half the
    GPU time, no softer to look at.
  - It holds its picture (no frames) on battery, while a game, a full-screen app, a presentation (Windows' own
    notification state) or, on one screen, a maximised window covers the desktop, and 4 s after the music stops. Full
    screen takes over (the wallpaper goes, the device is rebuilt) and it starts again after.
  - The desktop doesn't repaint over the last frame by itself: when the wallpaper stops, the desktop's wallpaper is set
    again, the same file, for the session only (nothing saved, nothing announced). After a crash the last frame stays
    until the wallpaper next changes.
  - Settings → *Visualizer*, or right-click → DasVis → *Live wallpaper*. Off by default.
  - **Measured** (Intel UHD, 1920 × 1080): 3.4 ms of GPU a frame at ~31 fps, 0.4 % of the CPU (8 threads), +15 MB.
  - **Checked** in the app (muted): it draws behind the icons; F11 hands over to full screen and Esc brings it back;
    closing the app gives the desktop its own wallpaper back. `WRECKBOX_RENDER_HIDDEN=1` lets it draw behind a
    maximised window (for testing).
  - Not checked: Windows 11 24H2 (the WorkerW moved inside Progman; the code looks there too).
- **Multi-monitor** ✅ 2026-10-12: right-click → DasVis → *Screens* (shown with more than one screen):
  - *All screens, as one picture*: full screen covers the whole virtual screen, one DasVis.
  - *Each screen its own scene*: a borderless window on each other monitor (never takes the focus) with its own DasVis
    and swap chain, drawn right after the main one in the same frame: the same song, cover and drops, its own seed (so
    its own look and scenes); the frame's GPU budget is split between them.
  - Not checked: this PC has one screen.
- **Synced lyrics** ✅ 2026-10-12 (`dasvis/lyrics.*`): off by default (Settings, or right-click → DasVis → *Lyrics*).
  - Found in order: a `.lrc` file next to the song, the cache (`_cache\dasvis\lyrics`), LRCLIB (`/api/get` with
    the title, artist, album and length; `/api/search` without an album or length). A "none" answer is cached too, so a
    song is asked about once; a network failure isn't.
  - Kinetic type: each line is drawn like the title card (into the same texture, centred) and the final pass brings
    its letters in one after another, lights them in the palette as they're sung, punches the line on the kick and
    dissolves it into sparks as it ends.
  - **Tested** offline (`dasvis_tests`): LRC lines, repeated stamps, the offset tag, word stamps, a gap ending a line,
    LRCLIB's answers. The tests never ask LRCLIB. In the app: a `.lrc` next to a test song, on the wallpaper and in
    full screen.

- **Phone VJ deck:**
  - a page served by the existing `sync_server`, on the local network, protected by the pairing that already exists
  - XY pad (camera), scene buttons, intensity slider, *Drop now* button, ❤ / skip
  - shake the phone for a shockwave (the browser's motion events)
- **Room lights:** **WLED** first (UDP realtime over the local network, no account, a few bytes a frame); then
  Philips Hue (local bridge, Entertainment API) if you own one. Colours come from the palette, brightness from the
  stems.
- **Video export:**
  - Renders a song **offline at up to 4K60**, at High quality even on a slow PC: it just takes longer. The song map
    makes every frame exact.
  - Encoding uses Windows' **Media Foundation** H.264 / HEVC encoder, with the song's audio muxed in. No new
    dependency.
  - It's in the right-click menu on any song: *Export DasVis video…*
- **Spout output** (shares the texture with OBS / Resolume): the Spout SDK is BSD, a small drop-in.
- **Live wallpaper:**
  - renders behind the desktop icons (the WorkerW window)
  - Low tier, 30 fps, at the most
  - pauses when a full-screen app or game runs, or on battery
- **Multi-monitor:** span, or a different scene on each screen.
- **Synced lyrics** from LRCLIB in kinetic type. This sends song titles to an outside service, so it's **off until you
  turn it on**.

### Phase 8: optional, later (MilkDrop layer ✅ 2026-10-12; real stems waiting on your decision)
**Built:**
- **MilkDrop as a DasVis scene** ✅ 2026-10-12: right-click → DasVis → *MilkDrop presets as a scene* (off by default).
  - A 14th scene, *MilkDrop*, the director can pick like the others (and N / P, the deck). While it's on screen the
    app renders a projectM frame for each DasVis frame at 540p (`View::milk_frame`, shared with MilkDrop mode) and hands
    it over (`DasVis::set_milkdrop`: a BGRA texture, rows bottom-up); the scene shader turns it into linear light,
    punches it on the kick and pulls its colours a little towards the song's palette; DasVis's bloom, transitions,
    drop events, ring, sigils, title card and lyrics go over it. A different preset each time it comes on.
  - projectM starts when the scene first comes on (1.3–1.7 s on a worker thread; the last frame holds meanwhile) and is
    let go 20 s after it leaves; the texture goes as soon as it leaves.
  - **Measured** (Intel UHD, Low): +58 MB while it shows, back to +9 MB after it's let go. MilkDrop alone already
    runs at 43–58 fps on this GPU, so the two together can't hold 60: after ~1.5 s of late frames DasVis drops to a
    steady 30 fps (as *Save battery* does) until the scene changes, then back to 60. Every other frame of MilkDrop
    at 60 averaged 47 fps but juddered.
  - **Tested** (`dasvis_tests`): no MilkDrop scene unless asked; on screen it asks for frames, shows them the right
    way up, holds before the first, and stops asking when it's left. In the app: the scene with a real preset, the
    release after 20 s.
- **Real stems:**
  - Demucs (htdemucs) via ONNX Runtime + DirectML, about 1 minute a song on the GTX, cached.
  - About 80 MB of model plus the runtime, downloaded on first use and checked by SHA-256. It uses a lot of memory
    while it runs, so it's **High tier only, and opt-in**.
  - Only if the pseudo-stems turn out to be too rough on real music.
- **MilkDrop as a DasVis layer:** projectM's frame as a texture, through DasVis's post chain and transitions.
- **Not planned:** real-time AI image generation. It's far too heavy for the PCs this is meant to run on.

## 11. Frame budgets

### High tier (GTX 1650 Ti, 1080p, 16.6 ms)

| Pass | Budget |
|---|---|
| Simulations (only what's on screen) | 3 ms |
| World scene (at the render scale) | 6 ms |
| Feedback + mid + overlay | 1.5 ms |
| Bloom + tone map + finish + limiter + sharpen | 1.5 ms |
| Direct2D panel + present | 1 ms |

### Low tier (Intel HD 4000-class, 1080p output at 50 %, 33.3 ms at 30 fps / 16.6 ms at 60 fps)

| Pass | Budget at 60 fps |
|---|---|
| Simulations (128² grids, 16k particles) | 2 ms |
| World scene (Light / Medium at `quality` 0.3) | 7 ms |
| Feedback + mid + overlay | 2 ms |
| Bloom (3 levels, ¼ res) + finish + sharpen | 3 ms |
| Direct2D panel + present | 2 ms |

If 60 fps doesn't fit at 33 % scale, Low runs at a steady 30 fps rather than an uneven 40–50.

`WRECKBOX_PERF=1` logs each pass's GPU time, the tier, the render scale, CPU time per frame and memory to
`wreckbox.log`, as it does for MilkDrop today.

## 12. Tests

All in `tests/dasvis_tests.cpp`, run by `scripts/build.ps1` and CI:

| What | How |
|---|---|
| Song map | Synthetic songs (see phase 2): beat grid, downbeats, builds, drops, the breath before the drop, cache version |
| Pseudo-stems | A kick-only, hats-only and bass-only signal each light up only their own stem |
| Analysis memory | Peak extra memory while analysing a 6-minute song stays under 30 MB, and drops back afterwards |
| Features | Envelopes rise within 1 frame and fall at their release rate; springs settle with no overshoot |
| Director | Same seed + map + tier → same scene list; no repeats within 4; drops get hero scenes; transitions only on phrase downbeats; **Low tier never picks a scene that doesn't fit** |
| Flash limiter | A strobing input is held to ≤ 3 big brightness changes a second |
| Shaders | Every `.hlsl` compiles at build time for `ps_4_0` / `vs_4_0` (`fxc`); the build fails otherwise. Shader Model 4 is enforced, so nothing needs a new GPU. |
| GPU render | On a device **forced to feature level 10_0**, and on **WARP** (so CI without a GPU works too): each scene renders 60 frames at 320×180 from fake features, at `quality` 0.3 and 1. Checks: no NaN, not all black, not all white, the picture changes between frames, and the feedback stays bounded. |
| Resources released | Open and close DasVis 20 times: GPU memory and private RAM return to where they started (±2 MB) |

**Low-tier check, every phase:**
1. Run on the **Intel UHD** with tier forced to Low (`WRECKBOX_DASVIS_TIER=low`), on a device forced to feature level
   10_0, with the CPU limited to 2 cores (process affinity).
2. Log CPU %, private RAM, GPU memory and frame times over a fixed 3-minute walk.
3. Compare against §4's budgets. A phase doesn't ship if it's over budget.
4. Screenshots on the muted `build\dev\playroot` with `--background` (shot.ps1 / input.ps1), at Low and High side by
   side.

A real old PC is the final check (question 6 below).

## 13. Open questions for you (I've assumed the defaults above)

1. Make DasVis the default once phase 6 is done? (Assumed: yes.)
2. Lyrics need LRCLIB (an outside service). OK to add as an opt-in? (Assumed: opt-in, phase 7.)
3. Which smart lights do you have, if any: WLED, Hue, Nanoleaf, Razer / Corsair keyboard?
4. Real Demucs stems (an ~80 MB download, High tier only) if the pseudo-stems aren't good enough? (Assumed: decide
   after phase 3.)
5. *Reduce flashing* on by default? (Assumed: off, but the 3-a-second cap always applies.)
6. Do you have an older PC or laptop to try it on? Until then, the Low-tier check on this PC's Intel UHD with 2 cores
   is the stand-in.
