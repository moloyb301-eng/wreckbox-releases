# WreckBox for Windows — implementation plan

Read [DESIGN.md](DESIGN.md) first. Each phase ends with something you can run and check. A phase is done only when
its **Done when** list is checked off with real output, not just written code.

Porting rule: for every module, the matching file in `reference/wreckbox/` is the spec. Port its behaviour (including
messages and log event names), not just its idea.

---

## Phase 0 — Skeleton ✅

- [x] `CMakeLists.txt`: C++20, MSVC static runtime, `/W4`, targets `wbengine` (static lib), `wbcore`, `engine_tests`
- [x] `vcpkg.json` manifest (`x64-windows-static` triplet)
- [x] `scripts/build.ps1`: finds VS Build Tools via `vswhere`, sets `VCPKG_ROOT`, configures, builds, runs `ctest`
- [x] `.gitignore` (`build/`, `reference/`)

**Done when:** `.\scripts\build.ps1` builds from a clean checkout and `ctest` passes. ✅

## Phase 1 — Engine ✅

Port of `core/src/*.rs`.

- [x] `engine/fft.h`: real-input FFT (half-size complex radix-2 + unpack), magnitude spectrum
- [x] `engine/decode.*`: dr_wav (WAV/AIFF), dr_mp3, dr_flac, and Media Foundation (AAC/M4A/ALAC) → mono → the same
      averaging resampler to 22,050 Hz
- [x] `engine/analysis.*`: tempo (spectral flux, autocorrelation, candidates, beat-grid contrast, prior), `fold_bpm`,
      key (HPCP-style profile, EDMA/bgate/Temperley, agreement), camelot, energy, loudness — frame by frame (no
      spectrogram kept in memory). The Rust `WB_*` tuning environment variables are ported too.
- [x] `engine/tags.*`: TagLib read/write with the field mapping from DESIGN §4, atomic copy-and-swap, ID3v2.3,
      `short_key`
- [x] `tools/wbcore.cpp`: `analyze`, `tags`, `write-tags` with the same JSON output
- [x] `tests/engine_tests.cpp`: Camelot, folding, click-track tempo, A-minor chord, short key, tag round trip on a
      generated WAV
- [x] `scripts/parity.py`
- [x] `wbcore bench FILE`: decode / tempo / key timings per file

**Done when:** tests pass, and `parity.py` on a folder of real tracks shows WAV/AIFF identical and compressed formats
with the same key and BPM (within ±0.1) on almost every track, with the differences explained.

**Result (2026-10-06):** done, on this PC's files.

- Test set: 6 synthetic tracks (90–174 BPM, 6 keys) as WAV, M4A (AAC) and FLAC; 2 real MP3s; and 18 odd-format
  scipy test WAVs.
- **Every file both engines could read was identical** (BPM within 0.1, same key, energy within 0.01).
- The C++ engine also reads several WAV variants Symphonia rejects (big-endian, µ-law, RF64).
- Speed:

  | Files | Rust | C++ |
  |---|---|---|
  | 6 WAVs | 0.77 s | 0.82 s |
  | 12.7-minute MP3 | 2.41 s | 2.17 s |
  | 6 M4As | 0.9 s | 1.7 s |

  M4A is slower because Media Foundation is slower (see DESIGN §9).
- Tags written by C++ read back correctly in **both** engines on MP3, FLAC, M4A and WAV, after the M4A BPM fix
  (DESIGN §4).
- **Still to do by the user:** `python scripts/parity.py <your real Tracks folder>`. The synthetic set can't stand in
  for a real library.

## Phase 2 — Model and Store

Based on the original module, the original module, the original module, the original module, the original module, and the organiser part of
the original module.

- [x] `model/`: `LibraryTrack`, `LibraryPlaylist`, `Library`, `TrackState`, `LogEntry`, `AppState`, `FileAnalysis`,
      `Settings`. JSON read/write keeps unknown fields (DESIGN §4). Add `isoSeconds`, `normalized`, `safeFileName`.
      Tested in `tests/model_tests.cpp`.
- [x] `model/paths.*`: every path in DESIGN §4; `writeAtomic`
- [x] `net/http_client.*`: WinHTTP with timeouts, headers, gzip, the system proxy and readable errors. Checked live
      against the Deezer API (`WRECKBOX_NET_TESTS=1`).
- [x] `library/matcher.*`: port of `TrackMatcher`, `compatibleKeys` and `camelotOrder`
- [x] `library/store.*`:
  - load/save (never overwrite an unreadable state file)
  - `row_ids` and rows with filters and search
  - `mixes_with`
  - `set_status`
  - artwork cache
  - `analyze_file` with cache check (size + mtime)
  - Deezer BPM reconcile
  - `rescan` (parallel), `organise`, `write_tags`
- [x] `library/downloads_watcher.*`: port of `DownloadsOrganiser`, including `organiser_seen.json` (keys stay
      compatible with the original build) and the "still being written" check
- [x] Tests:
  - `normalized` / `safe_file_name` cases
  - JSON round trips keeping unknown fields
  - matcher cases
  - a full organise → tag → file → rescan → watcher flow on a throwaway library (`tests/library_tests.cpp`)
- [ ] **Needs your library:** load + save round trip on a copy of a real `Music\WreckBox`
      (`WRECKBOX_TEST_LIBRARY=<copy> build\Release\model_tests.exe`), then open the re-saved copy in the original app.
- Moved to phase 3: the "run on a worker, post the result to the window" helper. It belongs with the window.

**Done when:** a real `Music\WreckBox` folder (copied) loads, re-saves with the same JSON meaning, and the original app
opens the re-saved copy without complaint. Loading 5,000 tracks takes under 400 ms.

**Result so far (2026-10-06):** the code is done and tested. Measured on a synthetic 5,000-track library
(`WRECKBOX_BENCH=1 build\Release\library_tests.exe`):

| Operation | Time | Budget |
|---|---|---|
| Load (three files parsed in parallel) | 189 ms | 400 ms |
| Search keystroke | 0.8 ms | 16 ms |
| Save state | 43 ms | |

Only the real-library round trip is left.

## Phase 3 — UI shell ✅ (pending a weak-PC check)

Port of the layout in the original module, the original module and the original module.

- [x] `ui/main.cpp`:
  - Win32 window, per-monitor DPI v2 (manifest + `WM_DPICHANGED`), dark title bar
  - message loop; `WM_APP_DONE` / `WM_APP_CHANGED` dispatch
  - window position saved in settings
  - `--root`, `--background`
- [x] `ui/gfx.*`:
  - Direct2D/DirectWrite with device-lost handling
  - Urbanist and Doto embedded as resources (variable fonts; named weights work)
  - caches for text layouts, gradients, bitmap brushes, stroke styles and pre-rendered bitmaps
  - software fallback (`WRECKBOX_SOFTWARE=1`)
- [x] `ui/theme.h`: tokens, Camelot colours and tints from the original module; Segoe MDL2 glyphs for the icons
- [x] `ui/ui.*` (immediate-mode layer):
  - click and scroll regions, hover
  - draggable scrollbars, wheel
  - widgets: glass, pill, chip, dot label, key badge, BPM readout, energy meter, status dot, artwork, logo, progress
- [x] Search: a native `EDIT` over a drawn pill (IME, clipboard, undo for free), styled through `WM_CTLCOLOREDIT`
- [x] `ui/jobs.*`: worker pool with a FIFO lane and a newest-first lane, results posted back to the window
- [x] `ui/artwork.*`: covers loaded on workers via WIC at draw size, with an LRU of Direct2D bitmaps
- [x] Pages (`ui/view.cpp`, `ui/view_tracks.cpp`):
  - sidebar with counts and playlists
  - Home: stats, recently added, playlist cards
  - track lists: search, BPM presets, key filter + "compatible", sort by title / BPM / key / energy
  - virtualised rows
  - inspector: readouts, playlists, "Mixes with", actions; overlay below 1320 DIPs wide
  - welcome screen and placeholders for the Tools pages
- [x] Keyboard: ↑/↓ move the focus, Esc closes the inspector, Ctrl+F searches, F5 rescans
- [x] Right-click menu on rows: write tags, show in folder, ignore / un-ignore
- [x] Busy text and a disabled "Rescan" while the store works; the list refreshes at most every 750 ms during long
      jobs
- [x] Startup timing in `wreckbox.log`; `WRECKBOX_PERF=1` logs paint times and repaint causes
- Moved to the phase that needs them: toggle and slider (phases 5 and 7), tabs (phase 8), dialog and toast (phase 7)
- Dropped:
  - multi-select: the original app has none either
  - activity-log view: the original desktop app doesn't show one; the log is in `state.json` and in bug reports

**Done when:** browsing a 5,000-track library meets the DESIGN §5 budget on a weak machine (or a VM limited to 2
cores and 4 GB), and the app has 0 % idle CPU.

**Result on this PC (2026-10-06):** 5,000-track demo library, 1440×900 window.

| | Measured | Budget |
|---|---|---|
| First paint after process start | 105–147 ms | 300 ms |
| Library load (background, window already up) | 209–254 ms | 400 ms |
| Paint while scrolling, GPU | avg 4.9 ms, worst 6.7 ms | 16 ms |
| Paint while scrolling, software | avg 7.2 ms, worst 12.5 ms | 16 ms |
| Idle CPU | 0.00 ms over 10 s, every thread | 0 % |
| Memory at rest | 38 MB private / 53 MB working set | 60 MB |
| Memory after heavy scrolling (plateau, no leak) | 69 MB private (GPU) / 55 MB (software) | 60 MB |
| `wreckbox.exe` | 2.72 MB | |

The first scroll test averaged 49 ms. The fixes that brought it down are in DESIGN §3 (Rendering).

**Still to check:**
- a real weak PC (run with `WRECKBOX_PERF=1`, then read `wreckbox.log`)
- display scaling other than 100 %
- the right-click menu by hand (it's a native menu, which the automated checks can't drive)

## Phase 4 — Imports and sources ✅ (pending a live Spotify / YouTube sign-in with your keys)

Based on the original module, the original module, the original module and the original module, plus the import parts of
the original module.

- [x] `sources/sources.*`:
  - each importer saves `_sources/<kind>.json`, and `library.json` is rebuilt from all of them
  - merge keys: ISRC → Spotify id → YouTube id → artist + title (+ duration)
  - different ISRCs never merge
  - a pre-sources `library.json` is kept as `library-legacy.json`
  - Spotify's data wins when a song is in several sources
- [x] `sources/csv_import.*`:
  - RFC 4180 parser; Exportify, TuneMyMusic, Takeout and generic columns, matched loosely
  - YouTube oEmbed for Takeout video ids
  - a re-imported playlist replaces its old version
- [x] Catalogue lookups:
  - MusicBrainz (ISRC, one request per second, with the same match and length rules), then Deezer by ISRC (cover,
    album, length), then the Cover Art Archive as fallback
  - cached in `_cache/catalogue_lookup.json`
  - offline or rate-limited lookups aren't cached, so they're retried next import
- [x] `sources/youtube.*`: title clean-up into artist + title, ISO durations, Google sign-in (loopback on any port +
      PKCE + client secret), playlists + liked music (category Music only), Spotify matching when connected
- [x] `sources/spotify.*`:
  - PKCE sign-in via `127.0.0.1:8888/callback`, the same redirect the original build uses
  - refresh token in settings
  - `Retry-After`-aware rate-limit retries
  - Liked Songs + own / collaborative playlists
  - search for YouTube matching
- [x] `net/oauth.*`:
  - PKCE with CNG SHA-256 and `BCryptGenRandom`
  - loopback HTTP listener on Winsock
  - opens the browser with `ShellExecuteW`
  - form encoding
- [x] `net/http_client`: response headers (for `Retry-After`)
- [x] Settings page (`ui/view_settings.cpp`):
  - Import playlists: steps, links, `IFileOpenDialog` multi-select, live progress
  - Spotify direct and YouTube direct: guides, Copy buttons for the redirect URIs, client ID / secret fields
  - bug-report name and contact
  - About
  - Save settings with a confirmation
- [x] Text boxes generalised: any number of native `EDIT`s over drawn fields. Each box has its own background so it
      blends with its field; Tab / Shift+Tab move between fields, Esc leaves.
- [x] Tests (`tests/sources_tests.cpp`, using the original fixtures copied to `tests/fixtures`):
  - the original `csv_test` and merge / legacy tests
  - PKCE against FIPS SHA-256 vectors
  - the loopback redirect (a simulated browser hits it; 404 for other paths; a busy port gives a readable error)
  - Spotify track parsing
  - `WRECKBOX_NET_TESTS=1`: a live import against MusicBrainz, Deezer and YouTube. It passed: ISRCs from the right
    labels, covers found, no length trusted without an ISRC, about 3 s for 3 tracks.
- Not on Windows: Dropbox. It's phone-only in the original app too. The Soulseek login comes with phase 8, the update
  check with phase 7.

**Done when:** the same CSV files give the same `library.json` content as the original app (compare playlists, track
ids and order).

**Status (2026-10-06):**
- **Done:** the parsing and merge rules are ported line by line, and the original tests pass with the same expectations.
  A CSV import, opened in the app, shows its playlists and covers; settings save correctly.
- **Still to do by you:**
  - import the same CSVs in both builds and compare `library.json`
  - a real Spotify and YouTube sign-in. It needs your client IDs, which this PC doesn't have; the parts that don't
    need keys are tested.

### Added after phase 4: Playlist Sync

Every playlist page has a **Sync** button that fetches the playlist again from where it came from and **mirrors**
it: songs added at the source are added, songs removed at the source drop out of the playlist. Files, analysis and
track states are never touched.

- [x] `sources/sync.*`:
  - `sync::playlist` refetches by the playlist's source: Spotify (by id, or Liked Songs), YouTube (by id, `LL` =
    liked videos) or the CSV file it was imported from (`csv::reread`)
  - replaces that playlist in its `_sources/<kind>.json` and rebuilds `library.json`
  - `sync::unavailable` says why a playlist can't sync (signed out, CSV moved, imported before sources existed)
- [x] UI: a Sync pill in the playlist header. It's disabled with the reason while unavailable; the result line reads
      "+3 added · 1 removed" (lilac) or the error (peach). A playlist renamed at the source follows its new name.
- [x] Tests (`tests/sources_tests.cpp`, offline): a CSV-sourced playlist mirrored through `sources::replace` gains
      the added song and loses the removed one; another playlist sharing a song keeps it; `state.json` is byte-for-byte
      unchanged; the playlist keeps its file for the next Sync; `unavailable` gives the right reason for a playlist in
      no source, a CSV imported before Sync, and Spotify without a client ID.
- [ ] Live: Sync a real Spotify / YouTube playlist (needs your keys) and a re-exported CSV.

## Phase 5 — Player ✅ (pending your listening check)

Based on the original module and the original module, **on VLC's engine** (libVLC) rather than the planned Media
Foundation, so that it plays anything VLC plays. It adds VLC's equalizer, the volume normalizer, internet radio,
files outside the library, and a Winamp-style visualizer. How it fits together: DESIGN.md §3 "Player".

- [x] **libVLC 3.0.24** (`cmake/vlc.cmake`):
  - downloaded once from VideoLAN and checked against its SHA-256
  - an **audio-only subset of 62 plugins** (`cmake/vlc_plugins.txt`, 34 MB of VLC's ~150 MB) deployed next to the
    exe, and VLC's plugin cache regenerated so startup stays fast
  - **delay-loaded**: a native import library is generated from `libvlc.dll`'s exports (the SDK's is MinGW-made, and
    MSVC can't delay-load through it), so libVLC loads on first play, not at startup
- [x] `player/vlc_engine.*`: one libVLC instance and player; `amem` callbacks into our output; VLC's equalizer (10
      bands, preamp, 18 presets); `normvol` per media; events posted to the UI thread; `WRECKBOX_VLC_LOG=1` prints
      VLC's log
- [x] `player/audio_output.*`: a 0.25 s ring buffer → **miniaudio** on WASAPI (follows the default device), volume
      (cubed) and mute, and a 4,096-sample tap of what's audible for the visualizer. The device runs only while
      playing.
- [x] `player/player.*`: the port of the original module:
  - queue = the playable tracks of the list on screen; previous restarts after 3 s; ended → next; unplayable → skip
  - files, folders (searched recursively) and URLs; `.m3u` / `.m3u8` / `.pls` / `.xspf` / `.asx` expanded by
    `player/playlist.*`, including radio playlists over HTTP; HLS streams go to VLC as they are
  - volume, mute, equalizer and normalizer saved in `settings.json` under `"player"`
- [x] `player/visualizer.*` (maths) and `ui/view_visualizer.cpp` (drawing): see "Visualizer" below
- [x] Player bar (`ui/view_player.cpp`): cover, title / artist (or the radio's now-playing title), error line, BPM
      and key, previous / play / next, seek slider with times ("LIVE" for streams), volume + mute, EQ, visualizer,
      Open. Repaints 4 times a second only while playing.
- [x] Equalizer pop-over: on/off, VLC's presets, Reset, normalizer toggle, preamp + 10 vertical band sliders
      (−20…+20 dB, live while dragging, saved on release)
- [x] New widgets: `Ui::slider` (horizontal / vertical, through the same hit list as clicks, so pop-overs on top
      win), `Ui::icon_button`
- [x] Ways to play:
  - click a row's cover (the playing row shows ‖ / ▶ and a lilac title)
  - Enter on the focused row; the inspector's Play / Pause; "Play" in the row's right-click menu
  - Open files… (Ctrl+O), Open folder…, Open URL… (Ctrl+U), drag & drop onto the window, or
    `wreckbox.exe <files / folders / URLs>` (Explorer's "Open with")
- [x] Keys: Space play / pause, F11 visualizer. Media keys via `WM_APPCOMMAND`, and **Windows' media controls**
      (`ui/media_controls.*`, SMTC): media keys work while WreckBox is in the background, and the volume flyout
      shows title, artist and cover
- [x] Licences: `res/licenses/` (LGPL-2.1, GPL-2.0, NOTICE with VLC's source link) copied next to the exe;
      Settings → About names libVLC and opens the folder
- [x] Tests (`tests/player_tests.cpp`, through the shipped plugin subset, into a capturing sink, so nothing is
      audible):
  - **18 formats** play in full with real audio (finite, within ±1, not silent) and the right length: WAV, 24-bit
    WAV, ADPCM WAV, AIFF, FLAC, ALAC, AAC, MP3, MPEG layer 2, Ogg Vorbis, Opus, WMA, WavPack, TTA, AC-3, E-AC-3,
    DTS, ProTracker MOD
  - seeking; equalizer (bass boost + treble cut moves the balance ~40×); a flat equalizer and VLC's "Flat" preset
    leave the level unchanged (±0.5 dB); normalizer loads
  - playlists: `.m3u` with relative paths, PLS, XSPF, ASX, `file://` URIs, relative URLs
  - queue rules; visualizer maths (band placement at 100 Hz / 1 kHz / 5 kHz, levels, silence, bar fall, peak hang
    and fall, scope trigger, options round trip)
  - **pacing** through the real sound device, muted: 2 s of wall clock ≈ 2 s of track; the shown time matches
    what the device played; a whole track reaches the device with nothing lost

**Found and fixed on the way** (each is now covered by a test):
- VLC 3's `amem` output only delivers 16-bit audio, whatever format is asked for; it was being read as float
  (garbage samples, then a crash). Converted in the callback now.
- VLC's equalizer preamp has 12 as unity gain, so "on, all flat" was 12 dB quieter. Default preamp is now 12; the
  UI shows real gain.
- VLC ranks its "ugly" (nearest-sample) resampler first; `--audio-resampler=speex_resampler` picks a proper one.
- VLC's time already is the heard position; subtracting our buffer made the clock 1.3 s late.
- VLC runs ~1.3 s ahead if it can, which made EQ changes audible 1.3 s late; the 0.25 s buffer brings that down.

**Visualizer** (Winamp 2's, full screen): spectrum (normal / fire / line bars, peak caps that hang then fall,
5 falloff speeds each), oscilloscope (dots / lines / solid), both, or off. Classic Winamp colours or WreckBox pastel.
Click cycles the mode, right-click has every option, V switches the look, Space / ← / → control playback, Esc / F11 /
double-click leave. A track card fades out 3 s after the mouse stops, the cursor hides with it. Options are saved
under `"visualizer"`. 2,048-point FFT, log bands 20 Hz–16 kHz, +3 dB/octave tilt so the highs show. 60 fps while
playing (30 fps if frames run slow), nothing while paused.

**Measured (dev PC, 2026-10-06):** CPU 1.4 % of one core playing with the bar, 3.3 % with the full-screen
visualizer, 0.0 % paused. First paint 112–131 ms (libVLC isn't loaded until the first play). Release zip 18.9 MB
(exe 3.5 MB, libVLC 3.1 MB, plugins 34.2 MB unzipped; FFmpeg's plugin is 18 MB of that).

**Not supported:**
- DSD (`.dsf` / `.dff`): VLC 3 has no DSD decoder (VLC 4 adds one). *Upgrade path: our own DSD → PCM decimation
  fed to libVLC through `libvlc_media_new_callbacks`.*
- APE: should play through VLC's FFmpeg demuxer (it opened DSF fine), but no APE encoder exists on this PC to make a
  test file.
- TTA and ADPCM WAV play in full, but VLC estimates their length wrongly (the seek bar's end is off).
- Out of scope, as planned: audio CDs, gapless / crossfade, video. (MilkDrop came later: phase 5b.)

**Still to do by you:** listen. Check the sound is clean, that unplugging headphones moves playback to the speakers,
and try your own odd files and radio stations.

## Phase 5b — MilkDrop visualizer + player panel ✅ (pending your look with real music)

The full-screen visualizer felt empty, so it now plays **MilkDrop presets** (the swirling ones on webamp.org) with
projectM, under a glass player panel. Design and decisions: `docs/MILKDROP-PLAN.md`.

- [x] **A: projectM builds and links.** vcpkg `projectm` 4.1.7 (+ glew, glm, projectm-eval), statically. `cmake/milkdrop.cmake`
  downloads projectM's "cream of the crop" pack (9,795 presets) and the MilkDrop texture pack from GitHub archives of
  **pinned commits** (SHA-256 checked) and copies them to `milkdrop\presets` and `milkdrop\textures` next to the exe
  (copied only when missing, so incremental builds stay fast). Licences in `NOTICE.txt`.
- [x] **B: `player::MilkDrop`** (`player/milkdrop.*`, no UI): projectM rendered off-screen into a WGL pbuffer
  (3.3 core context), pixels read back through two PBOs one frame behind. Presets come from `milkdrop\presets` and
  from `%APPDATA%\local.wreckbox\wreckbox\milkdrop\presets` (your own `.milk` files). Shuffled playlist (projectM's
  playlist library) with our own history for "previous"; a preset that won't compile is skipped (retried up to 10×).
  Any failure → `ok() == false` + `error()`; `WRECKBOX_NO_MILKDROP=1` forces it.
- [x] **C: counted audio tap.** `AudioOutput::take_tap(cursor, out, max)`: every audible sample once, in order; after a
  long pause only the newest 4,096. Tested without a sound device (`AudioOutput(false)`).
- [x] **D: on the full-screen screen.** MilkDrop is the default mode (`"mode": "milkdrop"`, `"quality"` 540 / 720 / 1080,
  `"autoAdvance"` seconds). The engine is built on a worker thread when full screen opens (about a second: it reads
  ~10,000 files) and freed when it closes; the picture is a streamed Direct2D bitmap. Without OpenGL 3.3 the Winamp bars
  show with "MilkDrop needs OpenGL 3.3". Keys: **N / P** next / previous, **R** random, **L** lock, **M** MilkDrop ↔ bars;
  click = next preset; right-click menu: MilkDrop, quality, auto-advance, lock, open presets folder. Paused: the picture
  calms for 4 s, then holds with no timer.
- [x] **E: the player panel** (replaces the small track card): cover, big title / artist, BPM · key, transport, seek,
  volume (the same `transport` / `seek_bar` / `volume` code as the player bar — the bar is pixel-identical), preset
  ◀ name ▶ with random and lock, MilkDrop / Bars chips, the equalizer pop-over; and an **Up next** card (next 5 queue
  items, click to play: `Player::jump`). It fades as one layer 4 s after the mouse stops (always while paused or while
  the equalizer is open).
- [x] **F: measured** (below) and documented.

**Measured (dev PC, muted, full screen at 1920×1080, 2026-10-06):**

| | |
|---|---|
| Render + read back, MilkDrop alone (unit test) | 1.1 ms at 320×180, 5.1 ms at 1280×720 |
| 720p (default): frame build / with present | 7.5–8.7 ms / 10.5–11.8 ms, 60 fps, **26–30 % of one core** |
| 1080p | 16–20 ms: past the 14 ms limit, so it steps down to 720p by itself after ~1.5 s (logged under `WRECKBOX_PERF`) |
| Paused after the 4 s settle | **0 %** (0.00–0.03 s of CPU in 10 s) |
| First paint (visualizer never opened) | 106–125 ms; the old build 121–147 ms: no change, GL starts only in full screen |
| MilkDrop start (worker thread, UI keeps drawing) | 0.9–1.2 s |
| RAM while it runs | 166–237 MB working set (125–182 MB private); freed on leaving full screen |
| exe | 4.6 MB (was 3.5 MB) |
| release zip (exe, VLC, `milkdrop\`, `licenses\`) | **53.1 MB** (was 18.9 MB): the presets are 34 MB of it |

**Known limits:**
- It needs OpenGL 3.3 and WGL pbuffers (old Intel drivers, Remote Desktop may not have them): the bars show then.
- On a weak GPU, 720p is already heavy: it drops to 540p, and the next step would be a 30 fps cap (not built yet).
  The reference weak PC hasn't run it.
- There's no per-preset rating or favourites list. (Hard cuts on drops: phase 5c.)
- Some presets don't compile in projectM's HLSL → GLSL step; they're skipped (count: `MilkDrop::failed_presets()`).
- projectM is **statically** linked (LGPL-2.1): `NOTICE.txt` says how to relink. Building it as a DLL instead is the
  alternative if that is not enough.
- The presets' LICENSE treats them as public domain, and authors can ask for removal.

**Still to do by you:** look at it with real music, on the weak PC, and say whether 720p / 30 s per preset feel right.

## Phase 5c — MilkDrop beat sync ✅ (pending your listen with real music)

You reported that MilkDrop didn't move with the beat. The causes, found in the code and in miniaudio's and projectM's
source, and proved by a test before anything was changed:

1. The sound device ran miniaudio's "conservative" profile, **100 ms periods × 3**, so the visualizer tap got audio in
   100 ms lumps; projectM analyses only the **newest 576 samples (12 ms)** of each feed, so most kicks were never seen.
   A probe preset (`tests/fixtures/milkdrop/beat_probe.milk`, brightness = `bass`) fed a 120 BPM kick track:
   **100% of kicks light the screen fed per frame, 0% fed in 100 ms lumps.**
2. The tap was filled when audio was *queued*: the pictures ran up to ~300 ms ahead of the sound (more with Bluetooth).
3. Frames were paced by WM_TIMER (15.6 ms steps, flipping to 31 ms right at the measured frame times).
4. Hard cuts were off, and the shuffle included the "! Transition" and the slow categories.

- [x] 20 ms device periods; the tap records every device frame (1 s deep) and `heard_index` / `tap_until` give the
      audio being heard at any moment (`player/audio_output.*`)
- [x] MilkDrop is fed exactly the samples heard while its frame is on screen (two refreshes ahead); the Winamp bars look
      at the next refresh; Bluetooth speakers add 180 ms automatically; `[` / `]` nudge the sync by 25 ms (shown at the
      top, saved as `syncMs`)
- [x] Full screen presents on vsync and repaints after each frame (`Gfx::set_vsync`): frames on the display's refresh
- [x] Presets: **hard cuts on drops** (at most every 15 s), beat sensitivity 2 (Low / Normal / High), **beat-heavy
      categories** by default (no Fractal, Hypnotic; "! Transition" never) — 8,157 presets; "All" in the menu
- [x] **Beat pulse** (strong by default): `player::BeatPulse` finds kicks in the same heard audio (low-pass energy
      against its last-second average); each kick zooms the picture 5% and flashes it, easing back over ~200 ms. Off /
      Subtle / Strong in the menu.
- [x] Tests (`player_tests`): the beat-sync probe above (≥ 90% of kicks, per-frame feed); the heard clock with fake
      timestamps (latency, time since the callback, clamping, underruns, the tap's depth); `tap_until` contents; the
      kick detector (16 / 16 kicks on time on a 120 BPM track, none in hi-hats or silence, gone 300 ms later); the new
      options round trip and clamp.

**Measured (dev PC, muted, 1920×1080):** MilkDrop gets ~800 samples a frame with no frame missing new audio (was 0
then ~4,800 every sixth frame); the device queues 60 ms; ~59 fps on most presets, 30–40 on the heaviest ones (18–26 ms
frames); 42% of one core playing (was 26–30%: the frame rate is now really 60); **0 ms of CPU in 10 s paused**.

**Still to do by you:** play music with clear kicks. If the pictures feel early or late (Bluetooth, a TV), press `[` or
`]` until they sit on the beat; it's remembered.

## Phase 6 — Phone sync, account, tunnel ✅ (pending the Android app on a real phone)

Based on the original module (server side), the original module and the original module.

- [x] `net/sync_server.*`: cpp-httplib on a worker thread; the token check runs first, for every route (header
      `x-wreckbox-token` or `?t=`, compared without early exit); endpoints and Range handling as in DESIGN §4. Port 47390,
      `WRECKBOX_SYNC_PORT` overrides. Started by "Start sharing" (or by the tunnel); nothing listens before that.
- [x] Pairing screen (`ui/view_phone.cpp`): QR code (Nayuki qrcodegen → Direct2D, cached), the pairing link with Copy,
      "Unpair all phones" (a new token, saved)
- [x] Windows firewall: nothing to do; the system "allow private networks" prompt appears the first time it listens
- [x] `net/account.*`: PBKDF2-HMAC-SHA256 through CNG (checked against the published vectors), sign up, sign in, sign out,
      change password, upload library + crate summary (debounced 2 minutes after the last change), device registration
- [x] `net/tunnel.*`: downloads cloudflared once, `CreateProcessW` with a pipe, finds the address in its log, registers it,
      5-minute heartbeat, brings it back 10 seconds after it drops; the process is in a job object, so it dies with WreckBox
- [x] Account UI and "Use from anywhere" on the Sync to phone page; the tunnel starts again at launch if it was on

**Tests** (`tests/sync_tests.cpp`, 23 s, no real service touched):
- the server over real HTTP on 127.0.0.1: no / wrong token on every route → 403; `/info`, `/library.json`, `/crate` (size,
  analysis); a whole file; `Range` (`a-b`, `a-`, `-n`, an end past the file → 206 with the right `Content-Range`; a start past
  the end → 416); track ids with `:` `/` and spaces sent URL-encoded; unknown / not-downloaded → 404; unpairing (old token
  dead at once, new one saved); stop and start again
- the account against a small fake of the service: signs up, signs in (the email is trimmed and lower-cased before the key is
  derived; the password never appears in a request), wrong password, changes the password, signs out (also offline), a 401
  ends the session, the library and a crate summary with no file paths are uploaded, a burst of changes makes one upload
- the tunnel with a stand-in for cloudflared (a script): finds the address (not `api.trycloudflare.com`, which is
  cloudflared's own error text), registers, heartbeats, `stop()` returns at once and the process is really killed, the
  account is told, a quitting process is brought back, no address → "Couldn't connect…", signed out → "Sign in…"

**Checked by hand on the dev PC:** the page in the real window (QR, link, sign-up form); the server answered a `curl` over
the PC's real Wi-Fi address (403 without the token, `/info` and `/crate` with it); sign-up and "Use from anywhere" against a
local fake of the account service: the real cloudflared was downloaded, started, and registered an address with it. I did
**not** call the public tunnel address from outside, and never touched the real account service.

**Differences from the original build (on purpose):**
- A wrong "current password" in `change_password` no longer signs you out (original treats every 401 but login's as an
  ended session).
- `find_tunnel_url` ignores `api.trycloudflare.com`, which the original app's regex would take for the tunnel on a failed start.
- The library is also uploaded 2 minutes after changes settle while signed in (original has the function but never calls it).
- Stopping the tunnel never waits for the network: the account is told from the worker thread.

**Known limits:**
- The cloudflared download (55 MB) has no progress bar; on this PC's connection it took ~15 minutes. The message says
  "Downloading Cloudflare tunnel tool…" meanwhile.
- If a computer has several private addresses (Wi-Fi, a virtual switch), the QR code lists them all, as before.

**Still to do by you:** the Android app, unchanged, on a real phone: pair by QR, list the crate, download with analysis,
stream with seeking, and use it away from home through the tunnel with your real account.

## Phase 7 — Extras ✅ (pending a real bug report and a real newer release)

Port of the update check and bug-report parts of the original module, plus the original module, the original module
and the original module.

- [x] **Update check** (`net/updates.*`): the newest release of the public releases repo **that carries this build's
      `win-native` zip** is compared with this build's version (2026-10-08: the repo's latest release is the original app's
      0.6.0, which this build used to announce; drafts and pre-releases are skipped too) (from `CMakeLists.txt`); a "WreckBox x.y.z is available — Download / Later" banner shows above the page.
      Runs once at startup on a worker (not for a `--root` test library, nor with `WRECKBOX_NO_UPDATE_CHECK=1`), and from
      Settings → About → *Check for updates* (which also says "up to date" or why it couldn't check). "Later" hides that
      version until the next one. It prefers an asset named `…win-native…`, then `…windows…`, then the release page.
- [x] **Bug report** (`net/bug_report.*`, `ui/view_extras.cpp`): *Report a bug* at the bottom of the sidebar. Automatic
      screenshot of the app (`PrintWindow` → WIC PNG, scaled to ≤ 1600 px, taken before the dialog opens), up to 2 more
      images, title and description; sent with the version, Windows version, reporter name / contact from Settings and the
      last 40 log entries (+ Soulseek's) to the relay, which files the GitHub issue. Shows "Sent — thank you! (report #n)".
- [x] **Settings page:** *Library folders* (the scan folders, add / remove / reset, Scan now), *Organise new downloads
      automatically* (on / off), the Spotify client ID, the reporter fields and *Use from anywhere* (the same switch as the
      phone page) were there or are now; plus the Soulseek login (phase 8).
- [x] **The Downloads organiser is running now.** `DownloadsWatcher` existed (phase 2) but nothing started it: it is
      started after the library loads (every 30 s; not for a `--root` test library, which has no Downloads folder).
- [x] **First-run onboarding:** the welcome screen ("Start by importing your playlists" → Settings) shows until there is a
      library; the `onboarded` flag is set after the first import, as in the original app.

**Tests** (`tests/extras_tests.cpp`, against local fakes of GitHub and the relay): version comparison, the update check
(newer / same / no Windows file / 403 / 500 / not JSON / unreachable; our own zip preferred), the report JSON (fields, 40
newest log entries, newest first, at most 3 images, base64), sending and the relay's error messages, the folder list
(add, add again with a slash, remove, reset; saved in settings.json and state.json).

**Checked by hand:** the dialog and the Settings sections in the real window; a report sent to a local fake relay arrived
with a real 218 KB screenshot of the app. **Not done:** a report to the real relay (it would file an issue in the original
developer's bugs repo) and the banner against a real newer release — the releases repo's latest is the original build's
v0.3.1, so the banner will offer that until this build has its own releases.

## Phase 8 — Soulseek (sidecar bridge) ✅ (pending a real sync)

Based on the original module. Runs `slsk_sync.py` with the bundled embedded Python, as the original app does.

- [x] **Sidecar bundle:** `sidecar/slsk_sync.py` (copied from the original repo's sidecar, unmodified) and
      `scripts/bundle_soulseek.ps1`: Python 3.11.9 embeddable (hash pinned in `scripts/python-embed.sha256`), `aioslsk`
      wheels for 3.11 / win_amd64 fetched with any Python that has pip, the `_pth` fix, and a smoke test. 38.9 MB in
      `soulseek\`.
- [x] **Bridge** (`net/soulseek.*`): starts it with the same arguments and environment (`WRECKBOX_ROOT`,
      `WRECKBOX_SLSK_CONFIG`, `PYTHONIOENCODING`) in a job object (it dies with WreckBox), notices a sync left over from an
      earlier run (its pid file + a live python), reads `sync.json` / `overrides.json` / the tail of `sync.log` every 15 s,
      files what lands in `_inbox` through `LibraryStore::organise` (by exact name or `Name (2)`), writes `queue.json`
      (priorities, then everything else unless "priority only") and retry requests (`overrides.json`, with custom queries).
- [x] **UI:** *Soulseek sync* (start / stop, Downloaded / Not found / Failed with counts, Retry all, retry, retry with your
      own search words, ignore, the log with ✓ / ✗ colours), *Download queue* (priority list, up / down / remove, add a
      playlist, "Then everything else"), Settings → Soulseek (login → `soulseek.toml`, the same file and format).
- [x] Downloads finish through `LibraryStore::organise`, as in the original app.

**Tests** (`tests/soulseek_tests.cpp`): the login file (quotes and backslashes), the queue order for every case, retry
requests, reading results and the log, filing from `_inbox`, and a process run with a stand-in script (arguments,
environment, results, stop, restart, a leftover sync noticed and stopped). The bundled sidecar itself was started with its
own Python (`--help` imports aioslsk); it was **not** run against the Soulseek network: that needs a real account and the
first login with a new name creates one.

## Phase 9 — Packaging and CI ✅ (pending a pushed tag and a clean Windows 10 VM)

- [x] `.github/workflows/build.yml`: Windows job (cached vcpkg → `build.ps1` build + every test suite → Soulseek bundle →
      `smoke.ps1` on the build folder → `package.ps1` → `smoke.ps1` again on the unzipped zip → artifact); on `v*` tags a
      release job attaches the zip to the releases repo (secret `RELEASES_TOKEN`, variable `RELEASES_REPO`).
- [x] `scripts/smoke.ps1`: a generated click track is analysed (~120 BPM), tagged and read back; the app starts with a
      throwaway library, draws its window, and **exits cleanly** when closed; the sidecar starts with its own Python.
- [x] `scripts/package.ps1` → `dist\WreckBox-<version>-win-native-x64.zip` (the version is `project(VERSION)` in
      `CMakeLists.txt`, the one place): `wreckbox.exe`, libVLC, `plugins\`, `milkdrop\`, `licenses\`, `soulseek\`,
      `README.md`. 69.9 MB (a third of it the MilkDrop presets, half the Soulseek Python).
- [x] App icon and version resource were already in (`res/app.rc`); the version resource follows `project(VERSION)`.
- [x] Asset naming: `win-native` until cut-over (DESIGN §9); the update check only offers releases that have it.
- [x] `docs/INSTALL.md` (shipped as the zip's `README.md`) and a README update.

**Checked here:** the zip, unzipped to a new folder with only `System32` on `PATH` (no VC runtime, no Python, no Visual
Studio), passes the smoke test; `wreckbox.exe` imports only Windows DLLs and the bundled libVLC. **Not done:** pushing a tag
(the workflow has never run) and a real clean Windows 10 VM.

**Bugs found by the smoke test and fixed:** the app crashed on every exit (an access violation after the window was gone):
the media-controls object touched its window while being destroyed, and COM was shut down before the Direct2D / WIC objects
were released. Both are fixed; this also affected the earlier builds.

## Phase 10 — Native Soulseek client ✅ built and tested against stand-ins; the Python sidecar stays until it has run for real

Replaces the sidecar with C++ that does what `slsk_sync.py` uses from `aioslsk`. It is **opt-in** (Settings → Soulseek →
*Use the built-in client (beta)*, or `WRECKBOX_SLSK_NATIVE=1`) and is used automatically when the sidecar isn't installed.

- [x] **Matching and ranking** (`net/slsk/match.*`): `norm`, `clean_title`, the search queries, `quality_of`, `file_matches`
      and `rank`, a line-for-line port. `tests/fixtures/slsk_golden.json` is recorded from the real Python
      (`make_slsk_golden.py`): all 262 cases (22 norm, 19 clean, 14 query, 108 quality, 91 match, 8 whole rankings with
      labels) agree.
- [x] **Wire protocol** (`net/slsk/protocol.*`): server login (version 175.1, MD5), listen port, status, share counts, ping,
      file search, peer address, ConnectToPeer / CantConnectToPeer; peer init and pierce-firewall; queue upload, transfer
      request / reply, place in queue, upload failed / denied; the zlib search reply. 28 messages are compared **byte for
      byte** with what aioslsk puts on the wire (`make_slsk_wire_golden.py` → `slsk_wire.json`), in both directions; garbage
      input never crashes a parser.
- [x] **Client** (`net/slsk/client.*`): logs in, listens for peers, searches (answers from peers that connect to us *and* from
      peers the server relays to us), downloads — directly, through the server's relay when the peer can't be reached,
      with the file connection either way — with a queue timeout ("still queued (place 4)"), a stall timeout, cancel, and a
      clean `close()`. Tested over real sockets against a stand-in server and peers (`tests/slsk_net_tests.cpp`): 8 download
      scenarios, search, login failures, cancelling, closing mid-download.
- [x] **Sync loop** (`net/slsk/sync.*`): the sidecar's `Syncer`: due tracks (retries first, the app's queue and priorities,
      attempts, wait times, the inbox), searches spaced out, up to 4 sources per track, `_inbox` delivery with the 90 % size
      check, and the same `sync.json` / `sync.log` / `sync.pid` / lock, so the Soulseek page, queue and retries are unchanged;
      one pass or `run` with the sleep that wakes when you ask for a retry (`tests/slsk_sync_tests.cpp`, and one end-to-end
      test through the real client).
- [ ] **Remove the bundled Python** — **not done, on purpose.** The "done when" is a week of real use with equal or better
      results than the sidecar. Nothing here has talked to the real Soulseek network: that needs a real account (the first
      login with a new name creates one) and other people's computers. Until it has, the zip keeps the sidecar (38.9 MB of the
      69.9 MB). To drop it: delete `soulseek\` from the install (or `scripts/package.ps1 -NoSoulseek`), and the built-in client
      runs by itself.

**Known limits** (also listed in Settings): it **does not upload** — it shares nothing, so peers that refuse users with no
shared files will refuse it (the sidecar shares your Tracks folder); no obfuscated connections; it doesn't join the
distributed network (searches still work through the server); no resume of a partial download; no UPnP. The wire details
come from aioslsk's code and the protocol notes, so a real server may still differ in ways the stand-ins can't show.

**To check with a real account:** *Start sync* on a small playlist, then compare `sync.json` / `sync.log` with the sidecar's on
the same queue.

---

## Phase 11 — File type and "My folders" ✅ (Windows first; the other apps later)

You asked to see at a glance whether each song is a FLAC, for songs WreckBox downloaded and for files already on the PC,
and for a separate list of the music on this computer with the same features as the track list.

- [x] **Type column** on every track list (FLAC in lilac, any other type in peach; blank while a song has no file), sortable
- [x] **FLAC / Not FLAC** filter chips beside the BPM and key filters ("Not FLAC" = has a file of another type: the ones
      to upgrade). The row wraps onto a second line when the inspector leaves no room.
- [x] **My folders** (sidebar, under Library; first called "On this PC"): the songs already on the PC in your library
      folders, found by *Rescan & analyse*, whether or not they're in the WreckBox library. **WreckBox's own folder
      (`Music\WreckBox`, where its downloads go) is never listed**, even though `Music` contains it: you asked for only
      the songs you already had. *Add folder…* on the page picks more folders (the same list as Settings → Library
      folders) and rescans. Same list: search (tags, file name and the matched track), BPM / key /
      type filters, sorting, playback, the inspector, *Mixes with*, *Show in folder*. The header counts the songs and
      the FLACs and names the folders.
  - A file that matches a library track shows the track's names and cover; any other file shows its own tags (or its
    file name).
  - The inspector says "In your folders" and shows the type, size and "Not in your library". *Write tags* is hidden there: tags come from the
    library, so a file shown as itself keeps its own tags.
  - Files outside the library folders aren't listed, even if the organiser analysed them.
- [x] **Delete from PC** (2026-10-08), kept out of the way: *Move to Recycle Bin* at the end of a My folders row's
      right-click menu, and a small dim *Delete from PC* link at the very bottom of the inspector that asks again
      ("Move it to the Recycle Bin? Click again"). The file goes to the **Recycle Bin**, so it can be restored; it stops
      playing first, leaves the list, and a library track it was the file of goes back to *Missing*. WreckBox's own
      folder is refused (`LibraryStore::delete_file`).
- [x] Tests (`library_tests`): a picked folder with an unmatched file and a matched one, Tracks left out, search, the
      file type, the library lists unchanged; delete (permanently in the test, to keep out of your Recycle Bin), deleting
      it twice, and the refusal for a file in Tracks.

**Known limit:** a file the engine can't analyse doesn't appear (the list is built from the analysis cache).

**Full-screen player bar** (2026-10-08, phase 5b's panel): one glass bar along the bottom, edge to edge (16 px from the
sides), 236 px tall: song, transport, a seek bar as wide as the screen allows, preset controls, and "Up next" as its
right-hand column behind a divider (from 1,100 px wide). A ⌄ button at its top right (or **H**) hides it, leaving a
small *Show player* button; that choice is remembered (`VisOptions::panel`, `"panel"` in the visualizer settings).

---

## Cross-cutting

- Before calling a phase done, compare its visible behaviour with the original app side by side on the same library
  copy.
- Every deliberate shortcut gets a `ponytail:` comment naming its limit and how to upgrade it.
- Never test against the real `Music\WreckBox`. Use a copy (`WRECKBOX_TEST_LIBRARY`).
