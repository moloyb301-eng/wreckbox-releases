# WreckBox for Windows (native C++)

A native Windows rewrite of [WreckBox](https://github.com/moloyb301-eng/wreckbox) — the DJ library manager that
turns your Spotify and YouTube playlists into one library, finds and analyses the tracks (BPM, key, energy), tags
them and files them so Rekordbox and other DJ software see the right data.

The Flutter + Rust + embedded-Python desktop build works, but it is heavy on old machines. This version is one
small C++ executable built for **weak Windows 10/11 PCs** (old dual-core, ~4 GB RAM, integrated or no GPU):

| | Flutter build (v0.2.0) | This build (target) |
|---|---|---|
| Download | 38 MB zip, measured (Flutter, Rust DLL, embedded Python) | 53 MB zip, measured, with VLC's engine and 9,795 MilkDrop presets (19 MB without the presets; plus the Soulseek Python bundle until phase 10) |
| RAM while browsing 5,000 tracks | not measured yet | target < 60 MB; measured 38 MB at rest, up to 69 MB after heavy scrolling |
| Cold start | not measured yet | target < 0.3 s to first paint; measured ~0.12 s |
| Idle CPU | Flutter frame scheduler | 0 % (draws only on change), measured; 0 % paused too |
| Playing | media_kit (libmpv) | VLC's engine; 1.4 % of one core with the player bar, ~26–30 % with full-screen MilkDrop at 720p (the Winamp bars: 3.3 %) |

It is a **drop-in replacement**: it reads and writes the same `Music\WreckBox` folder and settings file, and speaks
the same phone-sync protocol, so the existing Android app and existing libraries keep working. You can switch
between the Flutter build and this one.

## Status

Work happens in phases — see [docs/PLAN.md](docs/PLAN.md) for the full list with checkboxes.

| Phase | What | State |
|---|---|---|
| 0 | Project skeleton, build, tests | ✅ done |
| 1 | Engine: decode, BPM/key/energy analysis, tags, `wbcore.exe` | ✅ done. Identical results to the Rust engine on every test file, same speed or faster except M4A. See PLAN. |
| 2 | Data model + library store (same JSON files) | ✅ code done and tested (load 5,000 tracks in 189 ms, search in 0.8 ms). Waiting on a round trip of your real library. |
| 3 | Win32 + Direct2D UI: sidebar, Home, track lists with filters and sort, inspector | ✅ on the dev PC: first paint ~130 ms, 5 ms frames, 0 % idle CPU, 2.7 MB exe. A weak-PC check is still to do. |
| 4 | Imports: CSV (Exportify / TuneMyMusic / Takeout) with MusicBrainz + Deezer lookups, Spotify and YouTube direct, Settings page | ✅ CSV import tested live. Spotify / YouTube sign-in needs your client IDs to try. |
| 4+ | Playlist Sync: a Sync button on every playlist mirrors it from its source (Spotify, YouTube or the CSV) | ✅ tested offline; a live Sync needs your keys |
| 5 | Player on VLC's engine: 18+ formats, radio / streams, files outside the library, equalizer, normalizer, Winamp-style full-screen visualizer, media keys | ✅ tested (formats, EQ, pacing, visualizer maths). Needs your ears. |
| 5b | MilkDrop visualizer + player panel (projectM) | ✅ tested and measured; needs your look with real music |
| 6 | Phone sync (QR pairing, LAN server), WreckBox account, "Use from anywhere" tunnel | ✅ tested against fakes; needs the Android app on a real phone |
| 7–10 | Extras, Soulseek, packaging | planned |

## Build

Needs **Visual Studio 2022 Build Tools** (or VS 2022) with the *Desktop development with C++* workload — that
includes MSVC, CMake, Ninja and vcpkg. Nothing else to install; dependencies come from vcpkg on first build.

```powershell
.\scripts\build.ps1            # configure + build Release + run tests
.\scripts\build.ps1 -Debug     # Debug build
```

Output lands in `build\Release\` (or `build\Debug\`):

- `wreckbox.exe` — the app. It opens your `Music\WreckBox` library, the same folder the Flutter build uses.
  - `--root <folder>` opens a different library folder; settings then go in `<folder>\_settings`. Use it with a
    **copy** of your library, or a demo.
  - `--background` opens behind other windows without taking focus (for automated checks).
  - any other arguments are files, folders or URLs to play, as if dropped on the window (Explorer's "Open with").
  - `WRECKBOX_PERF=1` logs paint times to `wreckbox.log` (next to `settings.json`). Run this on a weak PC and send
    the log.
  - `WRECKBOX_SOFTWARE=1` draws on the CPU, for PCs whose graphics driver misbehaves.

  Keys: ↑/↓ move through the list, Esc closes the details panel, Ctrl+F searches, F5 rescans. Right-click a track
  for more.

  **Playing music:** click a track's cover (or Enter, or Play in the details panel); the queue is the list on screen.
  Drop files or folders on the window, or use **+** in the player bar: Open files (Ctrl+O), Open folder, Open URL
  (Ctrl+U, for internet radio and `.m3u` / `.pls` links). Space plays / pauses; media keys work even when WreckBox is
  in the background. The sliders button opens VLC's **equalizer** (presets, 10 bands, preamp) and the **volume
  normalizer**.

  **Sync to phone:** the sidebar's *Sync to phone* page. *Start sharing* shows a QR code (and a link) for the WreckBox Android
  app; only phones that scanned it can connect, *Unpair all phones* revokes them. Sign in to your WreckBox account there to
  save the library to it, and turn on *Use from anywhere* so the phone reaches this computer away from home (WreckBox
  downloads Cloudflare's `cloudflared` the first time, ~55 MB). `WRECKBOX_SYNC_PORT` changes the port (default 47390),
  `WRECKBOX_API` the account service (for testing).

  **Visualizer:** the visualizer button (or F11) goes full screen: **MilkDrop** presets (the swirling ones from Winamp
  and webamp.org, played by projectM) under a player panel with the song, transport, seek, volume, the equalizer
  and an "Up next" list; the panel fades when the mouse is still. **N / P** next / previous preset, **R** random,
  **L** lock the preset, click = next preset, **M** switches between MilkDrop and the Winamp bars, Space pauses,
  Esc / F11 / double-click leave. Right-click: quality (540p / 720p / 1080p), how often presets change, lock, and
  *Open presets folder* — drop your own `.milk` files in `%APPDATA%\local.wreckbox\wreckbox\milkdrop\presets`.
  In the bars mode, click steps through spectrum → oscilloscope → both → off, right-click has every Winamp option, **V**
  switches between the Classic Winamp and WreckBox colours. A PC without OpenGL 3.3 gets the bars automatically
  (`WRECKBOX_NO_MILKDROP=1` forces that).
  - `WRECKBOX_VLC_LOG=1` prints VLC's own log to the console (why a stream won't play).
- Next to `wreckbox.exe`: `libvlc.dll`, `libvlccore.dll` and `plugins\` (VLC's engine — keep them together),
  `milkdrop\` (the preset and texture packs, downloaded by CMake at pinned commits), and `licenses\` (VLC's LGPL / GPL
  texts and source links, shown from Settings → About).
- `wbcore.exe` — engine command line (same commands as the Rust `wbcore`):
  ```powershell
  build\Release\wbcore.exe analyze "C:\Music\track.mp3"          # one JSON line per file
  build\Release\wbcore.exe tags "C:\Music\track.mp3"             # read tags as JSON
  '[{"path":"t.wav","title":"T","artists":["A"],"bpm":120,"key":"A minor"}]' | build\Release\wbcore.exe write-tags
  ```
  Also `wbcore bench FILE…`, which shows where the time goes (decode / tempo / key, in ms).
- `engine_tests.exe`, `model_tests.exe`, `library_tests.exe`, `sources_tests.exe`, `player_tests.exe` — unit tests
  (also run by `ctest`; the player tests play muted, nothing is audible). Optional extras:
  - `WRECKBOX_TEST_LIBRARY=<a COPY of Music\WreckBox>`: round-trips a real library
  - `WRECKBOX_NET_TESTS=1`: calls the real Deezer / MusicBrainz / YouTube services (a live CSV import)
  - `WRECKBOX_BENCH=1`: times load and search on 5,000 tracks

### Checking the engine against the original

`scripts/parity.py` runs both engines on the same files and reports any differences. It needs the original Rust
engine built once from the reference checkout:

```powershell
git clone --depth 1 https://github.com/moloyb301-eng/wreckbox reference/wreckbox   # if not there yet
cargo build --release --manifest-path reference/wreckbox/core/Cargo.toml
python scripts/parity.py "C:\Users\you\Music\WreckBox\Tracks"
```

## Layout

```
wreckbox-win/
  CMakeLists.txt        build definition (one exe + wbcore + tests)
  vcpkg.json            third-party libraries (TagLib, nlohmann-json, dr_libs, miniaudio), pinned with builtin-baseline
  cmake/                vlc.cmake (downloads + checks libVLC, deploys it), vlc_plugins.txt (the audio plugin subset)
  scripts/
    build.ps1           finds VS Build Tools, configures, builds, tests
    parity.py           C++ engine vs Rust engine on real files
  src/
    engine/             decoding, analysis, tags — no UI, no globals
    model/              library / state / analysis / settings JSON, paths (same formats as the Flutter build)
    library/            store (scan, analyse, organise, tag), matcher, Downloads watcher
    sources/            playlist importers: CSV + catalogue lookups, Spotify, YouTube; the merge into library.json;
                        Playlist Sync
    player/             libVLC engine, sound output (miniaudio), player + queue, playlist files, visualizer maths, MilkDrop (projectM)
    net/                WinHTTP client, OAuth (PKCE + loopback sign-in)
    ui/                 the app: window (main.cpp), Direct2D drawing (gfx), immediate-mode widgets (ui),
                        screens (view, view_tracks, view_settings, view_player, view_visualizer), cover cache
                        (artwork), Windows media controls, worker pool (jobs), theme
    tools/wbcore.cpp    the command-line front end to the engine
  res/                  app icon, embedded fonts (Urbanist, Doto — OFL licences included), manifest, version info;
                        licenses/ (VLC's LGPL / GPL, NOTICE)
  tests/                engine_tests, model_tests, library_tests, sources_tests, player_tests — plain asserts, no
                        framework; fixtures/ holds the CSV samples from the original repo and short audio clips in
                        every format the player is tested with
  docs/
    DESIGN.md           architecture, compatibility contract, decisions
    PLAN.md             phased implementation plan with progress
  reference/wreckbox/   the original repo, for porting (not part of the build; git-ignored)
```

## Documentation

- [docs/DESIGN.md](docs/DESIGN.md) — goals, architecture, threading, the file and protocol formats we must stay
  compatible with, library choices, performance budget, testing.
- [docs/PLAN.md](docs/PLAN.md) — what gets built in which order, and how each phase is verified.
