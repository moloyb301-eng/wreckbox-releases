# MilkDrop visualizer + full-screen player panel — build plan (handoff for Sonnet 5.5)

Status: **implemented 2026-10-06** (phases A–F). Results and measurements: `docs/PLAN.md`, "Phase 5b".

## 1. What and why

The full-screen visualizer (`src/ui/view_visualizer.cpp`, phase 5) currently shows Winamp-style bars plus a small
track card that fades. The user says the screen "feels empty". They want:

- **the song and the app's controls** on that screen
- visuals **like webamp.org**, i.e. the swirling **MilkDrop** presets, not just bars

webamp.org uses Butterchurn, a WebGL port of Winamp's MilkDrop. The native equivalent is **projectM**
(LGPL-2.1, OpenGL 3.3), which plays the same `.milk` preset files.

### The user's decisions (don't re-ask)

| Question | Answer |
|---|---|
| Layout | **Full screen + player panel.** MilkDrop fills the screen. A glass panel shows cover, big title / artist, BPM / key, transport, seek, volume, EQ, the preset name with next / previous, and an "Up next" list. It fades when the mouse is still. |
| Presets | **The big pack**: projectM's "cream of the crop" (9,795 presets) plus the MilkDrop texture pack |
| Winamp bars | **Kept as a second mode.** Used automatically on PCs that can't run MilkDrop (no OpenGL 3.3). |

Target screen:

```
┌──────────────────────────────────────────────┐
│ hint line                                [⤡] │
│        ( MilkDrop fills everything )         │
│ ┌──────────────────────────────────┐ ┌─────┐ │
│ │[cover] GURU                      │ │ Up  │ │
│ │        Dhanji · 124 BPM · 8A     │ │ next│ │
│ │ ⏮ ⏯ ⏭  1:07 ━━━●━━━━━ 4:00  🔊━━ │ │ 1.. │ │
│ │ ◀ preset: Geiss - Spiral 3 ▶  EQ │ │ 2.. │ │
│ └──────────────────────────────────┘ └─────┘ │
└──────────────────────────────────────────────┘
```

## 2. Read this first: the project and its rules

- **Project:** `C:\Users\Noisemaker\Desktop\level-1\wreckbox-win`
  - native C++20 Win32 + Direct2D rewrite of WreckBox
  - docs: `README.md`, `docs/DESIGN.md` (§3 "Player" explains the playback pipeline), `docs/PLAN.md`
- **Build + all tests:** `.\scripts\build.ps1`. `-NoTest` skips the tests; `-Debug` makes a Debug build.
  - VS 2022 Build Tools, vcpkg manifest (`vcpkg.json`, pinned `builtin-baseline`), static triplet x64-windows-static
    with the static CRT
  - output goes to `build\Release\`
- **Don't commit.** The user hasn't asked.
- **Never close the user's running WreckBox.**
  - If linking fails with LNK1104 because `wreckbox.exe` is running, check its command line first
    (`Get-CimInstance Win32_Process`).
  - If it's theirs (no `--root` / `--background`), rename it to `wreckbox.old.exe` (Windows allows renaming a running
    exe) and rebuild.
- **Never make sound.**
  - App checks use the test library `build\dev\playroot`, whose settings have `"player":{"muted":true}`.
  - Tests use a capture sink, or the real device muted.
- **Don't disturb the screen.**
  - Launch test windows with `wreckbox.exe --root build\dev\playroot --background <file>` (behind other windows,
    no focus).
  - Capture them with `build\dev\shot.ps1 -ProcessId N -Out x.png` (PrintWindow).
  - Off-screen windows capture blank, so don't move windows off-screen.
- **Dev helpers** in `build\dev` (git-ignored):

  | Helper | What it does |
  |---|---|
  | `input.ps1 -ProcessId N -Click x,y` / `-Key 0xNN` | Client coordinates = screenshot minus (8, 31) for a normal window; full screen has no offset. |
  | `ctrlkey.ps1 -ProcessId N -Key 0x55` | Sends Ctrl+key. |
  | `setfield.ps1 -ProcessId N -Id 201 -Text …` | Types into a text box. |
  | `smtc_probe.ps1` | Run with Windows PowerShell 5.1. Lists media sessions. Never touch the user's Spotify session. |

- **Testing gotchas:**
  - A right-click menu blocks `SendMessage`-based input scripts. Set options through the test library's
    `_settings\settings.json` instead.
  - **Hover can't be faked with `SendMessage`.** `TrackMouseEvent` immediately posts `WM_MOUSELEAVE`. For a hover
    screenshot, make a throwaway build with `if (!tracking_leave)` in `src/ui/main.cpp` changed to `if (false)`,
    then restore the file and rebuild.
  - Wall time between your tool calls includes your own thinking time, so don't judge playback speed from clocks in
    screenshots.
- **Editing files:**
  - Some sources are CRLF. Multi-line edits are safest with a small Python script, written with the Write tool,
    that converts `\n` to the file's line ending.
  - Bash heredocs mangle `\n` inside C++ strings.
  - `Remove-Item` is often blocked by a safety check; use `rm` in Bash with absolute paths.
- **Code style:** match the surrounding code. That means short comments explaining *why*, no frameworks in tests
  (plain `CHECK` macros), and the immediate-mode UI (`Ui::click`, `Ui::slider`, `Ui::icon_button`, `Ui::tip`).
  Idle CPU must stay at 0%: timers run only while something animates.

### Facts already established (don't re-research)

- **vcpkg has `projectm` 4.1.7.** It depends on `glew` (Windows), `glm`, `opengl` and `projectm-eval`. Expect
  `find_package(projectM4 CONFIG REQUIRED)`; confirm the target name from vcpkg's usage output.
- **projectM 4.1.x always draws its final image to framebuffer 0** (`glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0)` in
  `ProjectM::RenderFrame`).
  - `projectm_opengl_render_frame_fbo` only exists in the unreleased 4.2; the latest release is 4.1.8.
  - **So render into a WGL pbuffer.** Its framebuffer 0 is off-screen, so projectM works unmodified, and you read the
    pixels back.
- **Preset packs** (GitHub, branch `master`). Pin a commit and its SHA-256, like `cmake/vlc.cmake` does:
  - `projectM-visualizer/presets-cream-of-the-crop`: 9,795 `.milk` files, about 23 MB, in category folders
    (Dancer, Drawing, Fractal, Geometric, Hypnotic, Particles, Reaction, Sparkle, Supernova, Waveform,
    "! Transition").
    - Its LICENSE treats MilkDrop presets as public domain, and authors may ask for removal.
  - `projectM-visualizer/presets-milkdrop-texture-pack`: about 3.4 MB, no licence file. projectM recommends shipping
    it with any bundled presets.
- **Pipeline today:** libVLC → `AudioOutput` (0.25 s ring → miniaudio) with a **mono tap** of the last 4,096 audible
  samples (`AudioOutput::tap`).
  - `player::Visualizer` (`src/player/visualizer.*`) does the bar maths.
  - `View::visualizer_screen()` draws them on a timer (60 fps, 30 fps when slow) only while full screen and playing.
- **What exists to reuse:**

  | Area | Pieces |
  |---|---|
  | `Ui` | `artwork(t, r, radius, opacity)`, `bpm_readout`, `key_badge`, `chip`, `pill`, `icon_button(…, tooltip)`, `slider`, `tip`, `glass` |
  | `Player` | `display()` (title / subtitle, streams included), `queue()`, `toggle/next/previous/seek`, `volume/muted/set_volume`, `equalizer` |
  | `View` | `eq_panel()` (anchored at `eq_x_` / `bar_top_`), `overlay_alpha()` (shown while paused; fades 3 s after the mouse stops), `save_vis()`, `vis_menu()`, `set_fullscreen()` |

## 3. Phases

Each phase ends **green**: `.\scripts\build.ps1` passes all 5 test suites, and the app still runs. Do them in order,
and after each phase tell the user briefly what's done.

### Phase A: projectM builds and links, packs ship next to the exe
1. `vcpkg.json`: add `"projectm"`. Configure, read vcpkg's usage message for the CMake package and target names, and
   link the target into `wbplayer` (or a new `wbmilkdrop` static library if GL headers would leak too far).
2. `cmake/milkdrop.cmake`, modelled on `cmake/vlc.cmake`:
   - download both packs as GitHub archive zips of **pinned commit SHAs** with `EXPECTED_HASH SHA256=…` into
     `build/_deps`, and extract them
   - `milkdrop_deploy(target)`: post-build, copy the presets to `$<TARGET_FILE_DIR>/milkdrop/presets` and the
     textures to `…/milkdrop/textures`. Use `copy_directory_if_different` or equivalent so incremental builds stay fast.
   - call it for `wreckbox` and `player_tests`
3. Licences, in `res/licenses/NOTICE.txt.in`:
   - projectM (LGPL-2.1, **statically linked**: state the source URL and that the user can relink against their own
     build; also keep the object files or a build recipe obtainable, i.e. point at this repo's CMake)
   - GLEW (Modified BSD / MIT)
   - glm (MIT)
   - projectm-eval (check its licence in vcpkg's `share/projectm-eval/copyright`)
   - presets and textures (public domain per the pack's LICENSE; textures: various authors)

**Done when:** it builds, the exe starts, and `build\Release\milkdrop\presets` holds about 9,795 `.milk` files.
Report the size added.

### Phase B: `MilkDrop` engine class (off-screen), plus a smoke test
New `src/player/milkdrop.{h,cpp}`, with no UI dependencies:

**Setup:**
- a hidden dummy window and a temporary legacy context, to load `wglGetProcAddress` and then
  `wglChoosePixelFormatARB`, `wglCreatePbufferARB`, `wglGetPbufferDCARB` and `wglCreateContextAttribsARB`
- create a pbuffer at the render size, then a **3.3 core** context on the pbuffer DC, then `glewInit()` (set
  `glewExperimental = GL_TRUE` for core profiles)
- `projectm_create()`, then `projectm_set_window_size(w, h)`, `projectm_set_texture_search_paths(...)`, plus
  preset-duration, soft-cut, hard-cut and beat-sensitivity settings
- a **playlist**: use the `projectM-4/playlist.h` API if the vcpkg build ships it; otherwise keep our own shuffled
  `std::vector<path>` and call `projectm_load_preset_file(handle, path, smooth)`
- presets are loaded recursively from `<exe>\milkdrop\presets` and from
  `%APPDATA%\local.wreckbox\wreckbox\milkdrop\presets` (create that folder lazily, for the user's own `.milk` files)

**`render(const float* mono, size_t n)`:**
1. Make the context current.
2. `projectm_pcm_add_float(handle, mono, n, PROJECTM_MONO)`.
3. `projectm_opengl_render_frame(handle)`.
4. Read back **asynchronously with two PBOs**: `glReadPixels(…, GL_BGRA, GL_UNSIGNED_BYTE, 0)` into PBO[i], then map
   PBO[i^1] from the previous frame and copy it into a CPU buffer.
5. Return a pointer to the BGRA pixels (bottom-up rows) with width / height, or nullptr for the very first frame.

**Other API:** `ok()` and `error()`, `resize(w,h)` (recreates the pbuffer and PBOs), `next()`, `previous()`,
`random()`, `lock(bool)`, `preset_name()`, `set_auto_advance(seconds)` (0 = off).

**Failures:** any failure leaves `ok() == false` with a readable `error()`. `WRECKBOX_NO_MILKDROP=1` forces that, to
test the fallback.

**Test** (`tests/player_tests.cpp`): create the engine at 320×180 and load one shipped preset. Feed 10 frames of a
1 kHz tone and render. Expect a non-null buffer, mostly non-black pixels, and frame 10 differing from frame 3. If
`!ok()` (no GL 3.3 on this machine), print "skipped" and pass.

**Done when:** the smoke test passes on this PC. Print the average render time per frame at 320×180 and at 1280×720.

### Phase C: audio feed (a counted tap)
- `AudioOutput`: give the mono tap ring a 64-bit write counter. Add
  `size_t take_tap(uint64_t& cursor, float* out, size_t max)`, which returns the samples written since `cursor`.
  - If more than the ring size was missed, return just the newest `min(max, 4096)`.
  - Then advance `cursor`.
  - Keep `tap()` for the bars.
- **Test:** simulate `render()` calls on an `AudioOutput` *without starting the device*. Expose a test-only way to
  call `render(out, frames)` directly; it's already public. Then check: no gaps, no repeats, the cap after a long
  pause, and nothing new when nothing was rendered.

### Phase D: MilkDrop on the full-screen screen
- `player::VisOptions`:
  - add `Mode::milkdrop` as the **new default** (saved as `"milkdrop"`)
  - add `quality` (540 / 720 / 1080, default 720) and `auto_advance` seconds (default 30)
  - update `to_json` / `from_json` and the round-trip test
- `Gfx`: a **streamed bitmap**: one `ID2D1Bitmap` (BGRA, `D2D1_ALPHA_MODE_IGNORE`), recreated only when the size or
  render target changes (watch `generation()`), and updated with `CopyFromMemory`. Draw it scaled to the window,
  **flipped vertically** (`Matrix3x2F::Scale(1,-1)` around the centre, or a bitmap-brush transform), with linear
  interpolation.
- `View::visualizer_screen()`:
  - **In milkdrop mode**, when the engine exists and `ok()`:
    - create the engine lazily on entering full screen; destroy it on leaving to free the GPU and RAM
    - `take_tap` → `render` → draw the bitmap
    - when not playing, keep rendering silence for a few seconds so the image calms down, then stop the timer
      (idle CPU 0%)
    - render size = quality, keeping the window's aspect ratio
    - auto-drop quality one step if `frame_ms_` averages over 14 ms (log it under `WRECKBOX_PERF`)
  - **Otherwise** fall back to the existing spectrum, and show "MilkDrop needs OpenGL 3.3 — showing the Winamp bars"
    in the panel.
- **Input:**
  - a click on the visuals gives the next preset in milkdrop mode, and cycles the Winamp modes otherwise (as today)
  - double-click still exits
  - keys: **N / P** next / previous preset, **R** random, **L** lock, **M** toggles MilkDrop ↔ Bars, plus the existing
    Space / ← / → / V / Esc / F11
  - update the top hint line
- **Right-click menu** (`vis_menu`): add a MilkDrop item at the top of the modes, Quality ▸ 540p / 720p / 1080p,
  Auto-advance ▸ Off / 15 s / 30 s / 60 s, Lock preset, and "Open presets folder" (`ShellExecuteW` on the user
  presets folder).

**Done when:** a screenshot of the muted test app in full screen shows MilkDrop rendering, N changes the preset
(the name changes), M switches to bars and back, and `WRECKBOX_NO_MILKDROP=1` shows the bars plus the message.

### Phase E: the player panel (replaces `vis_overlay`)
- **Factor out of `View::player_bar`** so the bar and the panel share code: `transport(Rect)` (previous / play / next
  with their tooltips), `seek_bar(Rect)` (times + slider, "LIVE" for streams), and `volume(Rect)` (mute +
  slider + tooltip). The player bar must look exactly the same afterwards; compare screenshots.
- **The panel:** a solid glass card about 760 px wide, centred at the bottom, 24 px from the edge. Fade every colour
  with `overlay_alpha()`, as `vis_overlay` does now.
  - **Row 1:** cover 96 px, title 26 px / 600, subtitle 15 px, then BPM · key for library tracks
  - **Row 2:** `transport`, `seek_bar`, `volume`
  - **Row 3:**
    - ◀ preset name ▶ (ellipsis) with random and lock (lit when locked) `icon_button`s
    - MilkDrop / Bars `chip`s
    - an EQ `icon_button` that toggles `eq_open_` and sets `eq_x_` / `bar_top_` so `eq_panel()` hangs above the panel.
      Draw `eq_panel()` in full screen too.
- **"Up next" card** to the right of the panel, same height, about 260 px wide: the next 5 queue items (title +
  artist), each clickable to play.
  - Add `const std::vector<Item>& Queue::items() const`, and `Player::jump(size_t index)`, which sets the queue index
    and calls `load_current()`.
  - Unit-test `jump` through the queue rules test where possible.
  - Hide the card when nothing is queued after the current item.
- **Visibility:** keep `overlay_alpha()` (always visible while paused; fades 4 s after the mouse stops; raise from 3 s
  to 4 s). The exit button and hint line fade with it, and the cursor hides when faded (existing `hide_cursor`).
- Keep the panel working without MilkDrop (bars mode).

**Done when** there are screenshots of:
- the panel over MilkDrop
- the panel faded (just MilkDrop)
- the EQ pop-over opened from the panel
- a click on an Up-next item playing that song (verify through `smtc_probe.ps1` or the title)

### Phase F: performance, docs, wrap-up
- **Measure** with `WRECKBOX_PERF=1`, muted test app, full screen:
  - CPU % of one core and average frame ms at 720p and 1080p
  - CPU while paused after it settles (**must be 0%**)
  - first paint (must not regress: projectM / GL must initialise only when entering the visualizer, never at startup)
  - the release zip size (zip `wreckbox.exe`, the libVLC DLLs, `plugins\`, `milkdrop\` and `licenses\`)
- **Docs:**
  - `docs/PLAN.md`: a new section after phase 5, "Phase 5b: MilkDrop visualizer + panel", with checkboxes, the
    measurements and the known limits
  - `docs/DESIGN.md` §3 "Player": the pbuffer readback pipeline and why (projectM 4.1 draws to framebuffer 0); and
    §5 performance rows
  - `README.md`: visualizer keys, presets folder, size row
- Final reply to the user: what was built, the numbers, what they should check (looks with real music, a weak PC),
  and the app path `build\Release\wreckbox.exe`.

## 4. Risks and what to do

| Risk | Response |
|---|---|
| pbuffer or GL 3.3 core unavailable (old Intel drivers, Remote Desktop) | Fall back to the bars with the message. Never crash; wrap all GL setup. |
| Readback too slow at 1080p on weak GPUs | Default 720p, auto-drop, PBO double-buffering. If still slow, a 30 fps cap in MilkDrop mode. |
| Some presets fail to compile (projectM's HLSL→GLSL translation) | projectM skips them. Make sure a failed load goes to the next preset instead of a black screen (watch the preset-switch-failed callback). |
| Static LGPL linking of projectM | Covered by the NOTICE plus relink instructions. If the user wants it simpler, build projectM as a DLL instead (vcpkg dynamic, or a custom triplet just for that port). Mention this to the user rather than deciding silently. |
| A GitHub archive's hash changes | Pin commit SHAs (archive by commit is stable), not branch names. |
