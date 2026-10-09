# WreckBox for Windows — design

Date: 2026-10-06 · Status: approved direction. Phases 0–4 are done and verified on the dev PC; see PLAN for what is
still to check.

## 1. Goal

A native C++ Windows version of WreckBox that **feels fast and runs well on weak PCs**, with **the same features as
today's desktop app**, as a **drop-in replacement** for the original Windows build.

**Success means:**

- Cold start to first paint under 0.3 s with a 5,000-track library on an old dual-core with a hard disk.
- Scrolling the track list stays smooth on integrated graphics; idle CPU is 0 %.
- RAM while browsing stays under ~60 MB (analysis jobs add their buffers while running).
- A library folder, settings file and paired phone used by the original build work unchanged with this build, and the
  other way round.
- Analysis results match the Rust engine: same key, BPM within 0.1 on WAV/AIFF (bit-identical input), and within
  normal decoder tolerance on MP3/AAC (see §8).

**Out of scope:** macOS, Android (they keep the original app), Windows 7/8, 32-bit Windows, Dropbox import (phone
only today).

## 2. Decisions

| Question | Decision | Why |
|---|---|---|
| UI toolkit | **Win32 + Direct2D/DirectWrite**, custom-drawn; native `EDIT` controls for text input | Fastest and lightest option; nothing to ship (all in Windows 10); keeps today's dark look and fonts. Cost: we write our own list, buttons, scrollbars. Compared with Dear ImGui and Qt 6 before choosing. |
| Engine | **Port the Rust engine to C++** (`analysis.rs` line by line), checked against the Rust build | One language and toolchain; analysis stays tuned (it was tuned against Essentia). |
| Audio decoding | **dr_libs** for WAV/AIFF (`dr_wav`), MP3 (`dr_mp3`) and FLAC (`dr_flac`); **Media Foundation** for AAC/M4A/ALAC | Measured: Media Foundation's per-packet cost made a 12.7-minute MP3 take 2.8 s to decode, against 0.97 s with dr_mp3. dr_libs output matches Symphonia (the Rust decoder) exactly in the parity runs, and dr_wav reads AIFF, which Media Foundation can't. Media Foundation stays for AAC, where there's no small alternative. |
| Tags | **TagLib 2** (static) | The standard C++ tag library; unified property + picture API across ID3v2, Vorbis, MP4. |
| FFT | Own real-input radix-2 FFT (`engine/fft.h`, ~70 lines, SSE2 only) | Only sizes 1024 and 8192 are used. Packing real input into a half-size complex FFT puts it level with the Rust engine (rustfft) without AVX, which old CPUs lack. |
| JSON | **nlohmann/json** | Header-only, readable, fast enough for multi-MB library files. |
| HTTPS client | **WinHTTP** | Built into Windows, uses the system certificate store and proxy settings. |
| Phone-sync server | **cpp-httplib** (plain HTTP on the LAN, same as today) | One header; handles Range requests. |
| Crypto (PKCE, PBKDF2, tokens) | **Windows CNG** (`BCrypt*`) | Built in, including PBKDF2. |
| Images (covers, screenshots) | **WIC** | Built in; decodes JPEG/PNG, encodes PNG. |
| QR code | **Nayuki qrcodegen** | Tiny, the usual choice. |
| Playback | **libVLC 3** (VLC's engine), an audio-only plugin subset, output through **miniaudio** (WASAPI) | *Changed in phase 5* from Media Foundation: plays anything VLC plays (18 formats tested, trackers, radio), with VLC's equalizer and normalizer. Our own output stage gives the visualizer the exact audible samples. LGPL core, loaded from separate DLLs. |
| Soulseek | Phase 8: run the **existing `slsk_sync.py` sidecar** (bundled Python) as a child process. Phase 10: native C++ client. | No mature C++ Soulseek library exists; the protocol is the riskiest port. The sidecar only runs during a sync, so it doesn't affect how fast the app feels. |
| Packaging | Portable zip, single `wreckbox.exe`, static CRT | Same install story as today (unzip and run). |

## 3. Architecture

One process, one executable. Modules have one job each and only `ui/` touches the window.

```
src/
  engine/     decode (MF / dr_wav → mono float @ 22,050 Hz), analysis (BPM/key/energy), tags (TagLib)
  model/      Library, AppState, FileAnalysis, Settings — JSON in the exact shapes of the original source / the original source
  library/    Store (rows, filters, search, "mixes with"), Matcher, Rescan, Organise, DownloadsWatcher
  sources/    CSV import (Exportify, Google Takeout, TuneMyMusic), catalogue lookups (ISRC, cover, Deezer BPM),
              YouTube, Spotify direct (PKCE + loopback redirect on 127.0.0.1:8888)
  net/        HttpClient (WinHTTP), SyncServer (port 47390), Account, Tunnel (cloudflared), Updates, BugReport
  soulseek/   sidecar bridge (phase 8) → native client (phase 10)
  player/     libVLC engine → our output (miniaudio), queue = the list you started from, playlist files,
              visualizer maths
  ui/         Win32 window, Direct2D renderer, theme, widgets (TrackList, PlayerBar, Sidebar, Inspector,
              Settings, dialogs)
  tools/      wbcore.cpp — command-line engine front end, used by tests and CI
```

**Dependency direction:** `ui → library/sources/net/player/soulseek → model → engine`. Nothing points upward. The
engine has no globals and no knowledge of the library, so `wbcore.exe` and the tests use it directly.

### Store and threading

*Changed during phase 2.* The first plan was "the UI thread owns the Store and workers post results back". But
`rescan`, `organise` and `write_tags` are long sequences of `await`s in the original app, and splitting them into posted callbacks
would have made the port hard to check against the original. So instead:

- **One mutex guards the Store's in-memory state.** It is held only for in-memory reads and writes, **never during
  disk, network or analysis work**, so the UI thread never waits more than a few microseconds.
- File saves have their own small lock, so two saves never race on the same `.tmp` file.
- **Long operations run on worker threads** at below-normal priority, so the UI wins on a dual-core. These are
  `rescan`, `organise`, `write_tags` and the Downloads watcher's 30-second timer.
- **Rescan analyses in parallel**, with `max(1, cores − 1)` workers; the original app does one file at a time. Results
  are applied in file order afterwards, so the outcome doesn't depend on timing.
- `Store::on_changed` fires on any thread. The window turns it into a single
  `PostMessage(hwnd, WM_APP_CHANGED)` → repaint, like `notifyListeners()`.
- One-shot UI jobs (for example "import these CSVs") use a small helper that runs a function on a worker and posts the
  result back to the window (phase 3).
- **The list reads ids, not rows.** `row_ids(filter, playlist, search)` is cheap enough for every keystroke (0.8 ms
  for 5,000 tracks), because each track's normalised search text is built once at load. The list then asks
  `row(id)` only for the rows on screen.

### Rendering

- **Immediate-mode UI** (`ui/ui.*`). Each paint draws the screen from current state and records clickable and
  scrollable regions; input is matched against the last paint's regions.
- **Painting happens only after an invalidation**: input that changes what's under the pointer, a store change, or a
  finished job. There is no frame loop, so **idle CPU is 0** (measured).
- `ID2D1HwndRenderTarget` with `D2D1_PRESENT_OPTIONS_IMMEDIATELY`, so the UI thread never waits for the monitor's
  refresh. The desktop compositor prevents tearing for windowed apps anyway.
- **`WRECKBOX_SOFTWARE=1`** renders on the CPU instead, an escape hatch for broken or flaky GPU drivers on old PCs.
  It's about 1.5× slower to paint and uses ~14 MB less memory.
- **Caches are what make it fast.** The first scroll test averaged 49 ms per frame, because every frame recreated
  ~125 gradient brushes and ~200 text layouts and drew the pixel logo as 800 rectangles. Now:
  - gradient brushes are kept per colour set and only moved
  - text layouts are kept per (string, style, box size), up to 800
  - **one** bitmap brush, re-pointed at each cover with `SetBitmap` (a brush per cover kept 300 covers alive on the
    GPU after the cover cache let them go: +45 MB after a scroll); stroke styles per dash length
  - gradient brushes: at most 48 (each is a small GPU texture, and album placeholders bring a colour per album)
  - the logo is rendered once into a bitmap (`Gfx::cached`)

  Result: a 4.9 ms average frame.
- **The track list is virtualised**: only visible rows are fetched (`row(id)`) and drawn.
- **Covers** (`ui/artwork.*`):
  - loaded on workers in a newest-first lane capped at 48 jobs, so fast scrolling doesn't queue thousands of loads
  - downloaded via the store if needed, then decoded by WIC straight to the size class they're drawn at
    (64 / 128 / 256 / 512 / 1024 px)
  - kept in an LRU of 250–300 Direct2D bitmaps; rows draw a tinted placeholder until a cover arrives
- **Fonts**: Urbanist (UI) and Doto (numbers) are embedded as resources and loaded as a DirectWrite in-memory font
  set. Both are variable fonts, and the named weights (SemiBold, Bold…) work. GDI gets its own copy through
  `AddFontMemResourceEx`, for the native search box.
- **No backdrop blur.** the original app's glass panels blur what's behind them; that is expensive and pointless over a
  near-black background. The plain translucent fills look the same. The one visible place, the floating inspector, is
  drawn opaque instead.
- Icons are Segoe MDL2 Assets glyphs, built into Windows 10 and 11, standing in for the Material icons.
- Per-monitor DPI v2 aware. All layout is in DIPs.

### Player

```
libVLC (decode, resample to 48 kHz, equalizer, normvol)
  → amem callbacks, 16-bit stereo (all VLC 3's amem can give) → float
  → AudioOutput: 0.25 s ring → miniaudio (WASAPI, shared mode, 20 ms periods × 3) → speakers
                 └─ tap: every frame sent to the device (before volume), 1 s, indexed by the device's clock
                       heard_index(t) = sent − queued in the device + time since the last callback
                       ├─ tap_until(heard at the next refresh) → Winamp bars (player::Visualizer)
                       ├─ tap_until(heard two frames from now), each sample once → MilkDrop (projectM)
                       └─ the same samples → player::BeatPulse (kick detector) → zoom + flash
```

- **libVLC owns decoding and timing.** It delivers blocks against its own clock; when our ring is full its write
  blocks. `libvlc_media_player_get_time` already *is* the heard position (measured against frames the device
  played), so the seek bar shows it unchanged.
- **The ring is small on purpose (0.25 s).** Given room, VLC runs ~1.3 s ahead of its clock, and anything VLC applies
  (equalizer, normalizer) would be heard that late. Volume, mute and pause act on our side and are instant.
- **The device runs only while playing**, so a paused WreckBox costs 0 % CPU like an idle one. miniaudio follows the
  default device (headphones unplugged → speakers).
- **Beat sync** (phase 5c). The visualizers draw what is *heard* when the picture appears, not what was last sent:
  - 20 ms device periods. miniaudio's "conservative" profile (100 ms × 3) gave the tap audio in 100 ms lumps up to
    300 ms early, and projectM analyses only the newest 576 samples (12 ms) of each feed, so most kicks were never seen
    (measured with a probe preset: 0% of kicks seen fed in lumps, 100% fed per frame).
  - The tap records every device frame (silence on an underrun too), so its index is the device's clock;
    `heard_index(t)` subtracts what the device still holds (`internalPeriodSizeInFrames × internalPeriods`).
  - MilkDrop's frame is on screen two refreshes later (read back one frame behind, then presented), the bars' one.
  - A Bluetooth default speaker (`PKEY_Device_EnumeratorName` = `BTH…`) adds 180 ms that WASAPI doesn't report; the
    rest is the user's calibration (`[` / `]`, ±300 ms, saved as `syncMs`).
  - In full screen the render target presents on vsync (`Gfx::set_vsync`) and repaints right after each frame, so
    frames land on the display's refresh instead of on 15.6 ms WM_TIMER ticks.
- **libVLC options are core options only.** libVLC refuses to start on options of plugins we don't ship (e.g.
  `--no-lua`). Plugins are found in `plugins\` next to `libvlccore.dll`; the `VLC_PLUGIN_PATH` variable is not seen.
  Local paths must use backslashes.
- **VLC quirks handled** (each has a test): `amem` is 16-bit only; the equalizer's preamp is unity at 12 (so a
  default of 0 was −12 dB); VLC ranks the nearest-sample "ugly" resampler first, so `--audio-resampler=
  speex_resampler`; a track ends with drain → EndReached → flush → Stopped; libVLC's own playlist parsing gives no
  sub-items without its Lua scripts, so `player/playlist.*` parses M3U / PLS / XSPF / ASX itself.
- **libVLC is delay-loaded.** The SDK's import library is MinGW-made and MSVC can't delay-load through it, so CMake
  generates a native one from the DLL's exports. Startup doesn't pay for ~3 MB of DLLs until something plays.
- **The visualizer** is maths in `player/visualizer.*` (unit-tested) and drawing in `ui/view_visualizer.cpp`:
  2,048-point Hann FFT (`RealFft`), log bands 20 Hz–16 kHz (interpolated below ~300 Hz, where a band is narrower than
  a bin), dB scale with a +3 dB/octave tilt, instant attack and Winamp-style falloff with hanging peak caps. It draws
  only while full screen and playing (or while the picture settles after a pause), one frame per display refresh.
- **MilkDrop** (`player/milkdrop.*`) is projectM 4.1, which plays Winamp's `.milk` presets. projectM 4.1 always draws
  its final image to *framebuffer 0* (`projectm_opengl_render_frame_fbo` only exists in the unreleased 4.2), so the
  engine renders into a **WGL pbuffer** (a 3.3 core context on an off-screen surface): framebuffer 0 is then
  off-screen, projectM runs unmodified, and `glReadPixels` into **two PBOs** returns the pixels one frame behind
  without stalling the GPU. The UI uploads them to one streamed Direct2D bitmap (`Gfx::upload_stream`) drawn flipped
  (GL rows are bottom-up). The context is made on a worker thread (the preset scan takes most of a second), released,
  and taken over by the UI thread in `render()`; anything that loads a preset needs it current, so `next()` etc. do
  that. It exists only while full screen is open and in MilkDrop mode. Failure at any step leaves `ok() == false`,
  and the screen shows the Winamp bars. Presets: `milkdrop\presets` next to the exe (projectM's "cream of the crop",
  pinned commit) plus the user's own folder under `%APPDATA%`.
- **Threads:** libVLC's events and Windows' media-control button presses arrive on other threads and are posted to
  the window (`WM_APP_DONE`, `WM_APP_MEDIA`); everything in `Player` runs on the UI thread.

### Soulseek

Two ways to run the sync, with the same files (`_soulseek\sync.json`, `sync.log`, `queue.json`, `overrides.json`,
`sync.pid`) and the same loop. **The sidecar:** `soulseek\python\python.exe slsk_sync.py run` in a job object (it dies with
WreckBox), from `soulseek::Sync`. **The built-in client** (`net/slsk/`, a thread in WreckBox): `match` (pure; tested against
recorded runs of the Python), `protocol` (codec; tested against bytes made by aioslsk), `client` (sockets: one connection
to the server, a listener for peers, the relay for peers that can't be reached, file connections) and `sync` (the loop,
behind a `Backend` interface so it is tested without a network). `soulseek::Sync` picks one: the built-in client when asked
for or when there is no sidecar. Downloads of either end in `_inbox`, and `Sync::import_inbox` files them through
`LibraryStore::organise`.

**What downloads (0.7.1).** The app writes `queue.json` = `{onlyPriority, priorities, ids}`. Both runners take only
`ids` when `onlyPriority` is set (an empty list means nothing), plus explicit retries (`overrides.json` `retryAt`).
- **Picks** live in state.json's `downloadPriority`: `playlist:<name>`, `genre:<name>` and `track:<id>`.
  `downloadMode` (`picked` by default, or `all`) sets `onlyPriority`; the older `priorityOnly` flag is still written.
- `Sync::write_queue` runs on every 15 s refresh, so a picked playlist's new songs join on their own. It rewrites the
  file only when something changed, because a write wakes the runner.
- **Per-song Download** = un-ignore + a `track:` pick + a retry (first in line, and past an old `done` record) + start
  the sync.
- **Don't download** = Ignore.
- **Deleting** = `LibraryStore::delete_file(path, recycle, skip)`. It is allowed in the user's folders and in
  `Tracks`, and refused for the rest of WreckBox's folder. With `skip` the song becomes `ignored`, so the sync leaves it.
- **Quality:** `prefer_smaller` in soulseek.toml makes `quality_of` (C++ and Python) score lossless 0+rank instead of
  10+rank, below any usable lossy file (6 and up).
- **Cancel (0.7.2):**
  - Both runners list the tracks they're on in `_soulseek\active.json` (`{id: {started, name}}`) from start to finish.
    The app shows *Downloading now* from it, and only while the sync runs.
  - **Cancel download** = *Don't download* (Ignore) + `overrides.json[id].cancelAt`.
  - A runner stops a track whose `cancelAt` is at or after that attempt's start: after the search, between sources,
    and during a transfer. The native client polls every 250 ms, reading the file at most once a second; the sidecar
    polls every 2 s.
  - The partial file is dropped and nothing is marked, so it isn't a failed try. A later retry removes `cancelAt`.
- **Progress (0.7.3):** each `active.json` entry also has
  - `state`: `searching`, then `waiting` once a peer is asked, then `downloading`
  - `user` / `ext` (the source)
  - `received` / `size` (bytes) and `speed` (bytes a second, averaged since the first byte)

  The native runner writes from the client's progress callback, at most every 500 ms; the sidecar writes on its 2 s
  poll. `soulseek::Activity::parse` reads it leniently, and the app re-reads it at most twice a second. The Download
  queue page repaints every second while the sync runs.
- Rows draw their own click before their buttons: `Ui` hit-tests the last registered region first.

### Memory

**The app (2026-10-08 pass).** Measured with `build/dev/memtest.ps1` on copies of a real library (287 tracks with
covers, and the same repeated to 5,166): ~43 / 49 MB private at rest while playing, ~50 / 56 MB after scrolling,
~115 MB during full-screen MilkDrop. Where it goes:
- without anything playing, ~26 MB; libVLC's playback pipeline adds ~15 MB of heap while a song plays (a fixed cost,
  not the song's size; its read-ahead buffer isn't it)
- the render target (~12 MB at 1440×900) and the Intel driver's own allocations
- library data: ~1 KB a track
- **`heapType` = SegmentHeap** in the manifest (Windows 10 2004+): the old NT heap kept ~24 MB it had freed after
  loading 5,000 tracks; the segment heap keeps ~3 MB
- **working-set trim** (`trim_memory`, `SetProcessWorkingSetSize(-1, -1)`) when minimized and 8 s after leaving full
  screen, when the OpenGL driver's pages are no longer needed (~89 → ~23 MB in RAM once back in use); pages that are
  needed come back on their own
- `WRECKBOX_PERF=1` logs a `mem:` line every 2 s: private bytes, working set, and each heap's in-use / committed MB

**The engine.**

The Rust engine keeps every FFT frame in memory: about 55 MB for the tempo pass on a 5-minute track. The C++ port
computes the onset envelope and pitch profile **frame by frame** with the same math, so analysis needs only the
decoded mono signal (~26 MB for 5 minutes) plus one frame.

## 4. Compatibility contract

These must stay byte-compatible in meaning (field names, value formats) with the original app. The source of truth is
the reference code. When in doubt, read the original module.

### Folders

| What | Where |
|---|---|
| Library root | `%USERPROFILE%\Music\WreckBox\` |
| Library | `library.json` |
| Per-track state, log, scan folders | `state.json` (log trimmed to the last 5,000 entries) |
| Tracks | `Tracks\` — files named `Artist - Title.ext` (`LibraryTrack.fileName`) |
| Analysis cache | `_cache\analysis.json`, keyed by file path |
| Other caches | `_cache\catalogue_bpm.json`, `_cache\catalogue_lookup.json` (CSV import: MusicBrainz / Deezer results), `_cache\remote_crate.json`, `_cache\organiser_seen.json`, `_cache\artwork\<id>.jpg` |
| Import sources | `_sources\spotify.json`, `youtube.json`, `csv.json`, `library-legacy.json`. `library.json` is rebuilt from all of them, Spotify first |
| Inbox | `_inbox\` |
| Soulseek | `_soulseek\` |
| Settings | `%APPDATA%\local.wreckbox\wreckbox\settings.json` (outside the library folder; original derives this path from Runner.rc) |
| Downloads watched | `%USERPROFILE%\Downloads\` |
| Audio extensions | `mp3 wav aif aiff flac m4a alac aac ogg opus` |

### Rules

- Every JSON write is atomic: write `<file>.tmp`, then rename over the file.
- If `state.json` can't be parsed, **never overwrite it**. Show an error and stop saving state until it is fixed.
- Timestamps are ISO-8601 UTC with whole seconds and a `Z` suffix (`2026-10-06T12:34:56Z`).
- Unknown JSON fields are ignored on read. Fields we don't model must survive a load/save round trip, so the C++ app
  keeps the original object and only changes the fields it knows.
- `FileAnalysis` writes `keyConfidence` (from `keyStrength`) and `loudnessLUFS`, and reads both spellings, exactly
  as the original module does.

### Engine JSON (`wbcore` and the analysis cache)

`analyze` returns camelCase fields, with `null` when a value is absent:
`bpm, bpmConfidence, bpmAlternate, bpmCandidates[], key ("A minor"), camelot ("8A"), keyStrength, keyAgreement (1–3),
energy (0–1), loudnessLufs, durationSec`.

Rounding is the same as the Rust engine: BPM and candidates to 0.1, confidence, key strength and energy to 0.01,
loudness and duration to 0.1.

### Tags written

| Field | MP3 / WAV / AIFF (ID3v2.3) | FLAC / OGG (Vorbis comments) | M4A (MP4 atoms) |
|---|---|---|---|
| Artists | `TPE1` joined with ` / ` | one `ARTIST` per artist | `©ART` joined with `, ` |
| Album artist | `TPE2` = first artist | `ALBUMARTIST` | `aART` |
| BPM | `TBPM` (integer) | `BPM` (integer) | `tmpo` (standard 2-byte integer) **and** `----:com.apple.iTunes:BPM` (text) — see note |
| Key | `TKEY` short form (`Am`, `F#`) | `INITIALKEY` | `----:com.apple.iTunes:initialkey` |
| ISRC | `TSRC` | `ISRC` | `----:com.apple.iTunes:ISRC` |
| Cover | `APIC` front cover | FLAC picture block | `covr` |

**M4A BPM note (found in parity testing):**

- The Rust engine (lofty) writes a 4-byte `tmpo`, which TagLib reads as 0, and it reads BPM only from the
  `----:com.apple.iTunes:BPM` text atom.
- So the C++ engine writes both atoms.
- When reading, it falls back to the text atom if `tmpo` is missing or 0.
- Checked in all directions: C++→Rust, Rust→C++, and C++ re-tagging a Rust-tagged file.

Also written: title, album and year (`TDRC`, saved as `TYER` in v2.3), plus genre when an override is set. Tags the
app doesn't manage are left alone. Files are tagged **as a copy in the same folder** (`.wreckbox-<pid>-<name>`), the
copy is re-opened to check it is still valid audio, and only then is it swapped in.

### Phone sync (desktop side)

HTTP on port **47390** (the `WRECKBOX_SYNC_PORT` environment variable overrides it), on all IPv4 interfaces. Every
request must carry the pairing token, either in the `x-wreckbox-token` header or as a `?t=` query parameter, or it
gets a 403.

| Endpoint | Returns |
|---|---|
| `GET /info` | `{"name": "WreckBox on <hostname>", "tracks": <downloaded count>}` |
| `GET /library.json` | the raw library file |
| `GET /crate` | `[{"id", "ext", "size", "analysis"}]` for each downloaded track whose file exists |
| `GET /file/<id>` | the audio file. Honours `Range: bytes=a-b` (206 / 416) so the phone can seek while streaming |
| `GET /art/<id>` | cover JPEG (downloads the 640 px Spotify cover into the cache if needed) |

- Pairing URI shown as a QR code: `wreckbox://pair?hosts=<private IPv4s, comma-separated>&port=47390&t=<token>`.
- The token is 18 random bytes, base64url without padding, stored as `desktopPairToken` in settings.
- Implementation notes: cpp-httplib serves it (`net/sync_server.*`); a file is a content provider, and httplib answers
  `Range` itself (206 / 416, `Content-Range`). The server exists only between "Start sharing" and "Stop sharing" (or
  while the tunnel is on); the pairing token is checked by a pre-routing handler, before any route, and compared in
  constant time. The server thread sleeps on `accept`, so a shared WreckBox still costs 0 % CPU.

### Account service and tunnel

- API base: `https://wreckbox-api.moloyb301.workers.dev`.
- Requests send the headers `x-wreckbox-device: <deviceId>` and `authorization: Bearer <token>`.
- The password never leaves the device. It is turned into a key with PBKDF2-HMAC-SHA256, 200,000 rounds, salt
  `wreckbox:<lower-cased email>`, 32 bytes, sent as hex.
- Endpoints: `/v1/signup`, `/v1/login`, `/v1/logout`, `/v1/password`, `/v1/blob/library` (PUT/GET),
  `/v1/blob/state`, `/v1/devices`.
- "Use from anywhere":
  - downloads `cloudflared-windows-amd64.exe` into the settings folder
  - runs `tunnel --no-autoupdate --url http://127.0.0.1:47390`
- Implementation notes: `net/tunnel.*` keeps cloudflared in a job object (`KILL_ON_JOB_CLOSE`), so it can't outlive
  WreckBox, reads its stderr on a thread for the `https://….trycloudflare.com` address (ignoring `api.`), registers it
  with the account and again every 5 minutes (which also uploads the library), and starts a new run 10 seconds after the
  process drops. `stop()` returns at once; the worker kills the process and tells the account afterwards. The account
  call set is `net/account.*`; its PBKDF2 is CNG's `BCryptDeriveKeyPBKDF2`.
  - finds the `https://*.trycloudflare.com` address in the tool's output
  - registers it via `/v1/devices` every 5 minutes
  - restarts the tunnel 10 s after it drops

### Updates and bug reports

- **Updates**: `GET https://api.github.com/repos/moloyb301-eng/wreckbox-releases/releases?per_page=30` (newest first),
  take the first release, not a draft or pre-release, with a `win-native` asset, and compare its tag with our version.
  (The original build reads `/releases/latest`; the native build can't, because that is usually the original app's.)
- **Bug reports**: `POST` to the relay URL with header `X-WreckBox-Key`, sending JSON with `title, description, app,
  version, platform, reporter, contact, logs` and up to 3 screenshots (base64).
- Both constants live in one `config.h`, mirroring the original module.

## 5. Performance budget (reference machine: dual-core, 4 GB, HDD, Intel HD graphics)

| Action | Budget | Measured on the dev PC (2026-10-06) |
|---|---|---|
| Cold start → first paint | < 300 ms | 105–147 ms; 112–131 ms with the player (libVLC loads on first play) |
| Load a 5,000-track library + state + analysis cache | < 400 ms, on a worker thread; the window shows before it finishes | 189–254 ms |
| Search keystroke → list updated | < 16 ms for 5,000 tracks | 0.8 ms |
| Scroll | every frame under 16 ms | avg 4.9 ms, worst 6.7 ms (GPU); avg 7.2 ms (software) |
| Analyse a 5-minute MP3 | < 2 s per core | ~0.85 s, scaled from a 12.7-minute MP3 that took 2.17 s |
| Idle CPU, nothing playing | 0 % | 0.00 ms of CPU in 10 s |
| CPU while playing | low | 1.4 % of one core with the player bar; 3.3 % with the full-screen visualizer at 60 fps |
| CPU while paused | 0 % | 0.0 % (the sound device is stopped, no repaint timer); also paused in full-screen MilkDrop, once the picture has settled (4 s) |
| Full-screen MilkDrop, 720p | < 14 ms a frame, or it steps down in quality | 7.5–8.7 ms a frame (60 fps), 26–30 % of one core; 1080p averages 16–20 ms here, so it drops to 720p; 4 ms of that is the pbuffer read-back |
| MilkDrop start | the window keeps drawing | 0.9–1.2 s on a worker thread; first paint of the app is unchanged (GL starts only in full screen) |
| RAM browsing 5,000 tracks | < 60 MB | 38 MB private at rest; plateaus at 69 MB after heavy scrolling (55 MB with software rendering) |

The dev PC is much faster than the target. The weak-PC numbers come from a run with `WRECKBOX_PERF=1`; see PLAN
phase 3.

Each phase that touches a budget measures it (an ETW trace or a simple timer log in `wreckbox.log`) before it is
marked done.

## 6. Error handling

- Inside the engine, failures are C++ exceptions with readable messages, as with Rust's `anyhow`. The engine's public
  functions (`engine.h`) catch everything and return `{"error": "..."}`, like the Rust `guarded` FFI wrapper. Nothing
  throws past that boundary.
- Every failure the user would care about goes into the activity log (`state.log`), using the same event names as
  the original app (`analysis failed`, `tag failed`, `organise failed`, …).
- Network calls time out like the original source does (4–60 s depending on the call) and report readable messages
  ("No internet connection.").
- A crash in a worker job is caught at the job boundary, logged, and the job reports an error. The app keeps running.

## 7. Security

- The sync server rejects any request without the pairing token before any routing happens, and serves only files
  that the Store maps from a track id. A path never comes from the request.
- Tokens and refresh tokens live only in `settings.json` under `%APPDATA%`, the same as today.
- PKCE for Spotify and Google, with no client secret in the app. Google's "Desktop app" secret is the user's own,
  stored in their settings, and Google itself calls it non-confidential.
- OAuth redirects are caught by a loopback listener bound to `127.0.0.1` only, and it checks the `state` value.
  Spotify uses the fixed `:8888/callback` its app registration needs; Google uses any free port.
- Child processes (cloudflared, the Python sidecar) are started with full paths and argument arrays. No shell is
  involved.

## 8. Testing

| Level | What | How |
|---|---|---|
| Engine unit | Camelot codes, BPM folding, 128 BPM click track, A-minor chord, short key form | `tests/engine_tests.cpp`, plain asserts, run by `ctest` (ports of the Rust tests) |
| Engine parity | C++ vs Rust `wbcore analyze` on real files | `scripts/parity.py <folder>`: WAV/AIFF must match exactly (±0.1 BPM, same key). Compressed formats may differ slightly because Media Foundation and Symphonia handle encoder delay and padding differently; any key or BPM difference is listed for review. |
| Tag round trip | write → read for MP3, FLAC, M4A, WAV, AIFF | test + `wbcore write-tags` / `wbcore tags`, the same as the CI smoke test in the original repo |
| Model round trip | load + save a copy of a real library folder and compare the JSON meaning | test (phase 2), using `WRECKBOX_TEST_LIBRARY` like the original tests do. **Always on a copy.** |
| Protocol | the current Android app pairs, lists, downloads and streams | manual checklist (phase 6) |
| Player | 18 formats play in full with real audio and the right length; seek; equalizer effect and unity gain; playlists; queue rules; visualizer maths; real-time pacing and clock on the real device (muted) | `tests/player_tests.cpp`, through the shipped plugin subset into a capturing sink (nothing audible) |
| Performance | the §5 budget | timer log + manual check on a weak machine (phase 3 onward) |

## 9. Risks

| Risk | Mitigation |
|---|---|
| Decoders handle start padding differently from Symphonia, which shifts results slightly | MP3 and FLAC use dr_libs, which matched Symphonia exactly in testing. AAC uses Media Foundation, which also matched on the test set. The parity script flags any difference on real libraries. |
| Media Foundation is slow on AAC (about 2× the Rust engine's time on M4A in testing) | Acceptable for now: analysis runs in the background on the worker pool. *Upgrade path: fdk-aac or a minimal MP4 demuxer + Media Foundation AAC transform called directly, if M4A-heavy libraries feel slow.* |
| AIFF isn't supported by Media Foundation | dr_wav decodes it. |
| OGG/Opus needs the optional Windows "Web Media Extensions" | Those files report "unsupported" and are listed in the log. *Upgrade path: add `stb_vorbis` + `libopus` if anyone has them.* |
| Writing our own widgets takes a long time | Build only the widgets WreckBox uses (list, button, toggle, tabs, slider, progress, dialog), and use native `EDIT` for text. |
| Soulseek protocol port | Keep the sidecar until the native client passes the same sync results on a real queue. |
| libVLC: some shipped plugins (and the FFmpeg inside them) are GPL | They're separate, unmodified DLLs, and the GPL / LGPL texts and source link ship with the app. If WreckBox is ever distributed closed-source, review which GPL plugins can be dropped (`cmake/vlc_plugins.txt`). |
| VLC 3 can't decode DSD; it misjudges the length of TTA and ADPCM WAV | DSD isn't offered in Open. *Upgrade path: our own DSD → PCM decimation fed through `libvlc_media_new_callbacks`, or VLC 4 when it's released.* |
| Releases: the original update check picks the first asset whose name contains `windows` | Until cut-over, name the native zip `WreckBox-<v>-win-native-x64.zip` so original users aren't switched by accident. At cut-over, the native zip takes the `windows` name (phase 9). |
