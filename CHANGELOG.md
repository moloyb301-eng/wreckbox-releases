# Changelog

Versions follow [semantic versioning](https://semver.org): MAJOR.MINOR.PATCH.
After 0.7.0, each release goes up by one patch number: 0.7.1, 0.7.2, and so on up to 0.7.99.

**Making a release**
1. Set the version in `CMakeLists.txt` (`project(wreckbox VERSION x.y.z)`); the exe's version resource and the zip's
   name follow it. Also `res/app.manifest`, `vcpkg.json` and the README's download links (badge and one-line command; both point at raw.githubusercontent.com — GitHub's own
   `/raw/` and `?raw=1` links fail with "Not Found" for signed-in visitors on a file this big).
2. Add a section for it below.
3. Commit, then tag the commit: `git tag -a vx.y.z -m "WreckBox x.y.z"`.
4. Pushing the tag runs CI, which builds and tests the zip and refuses a tag that doesn't match `CMakeLists.txt`.

Day-to-day work goes on `main`; bigger features on a branch (`feature/<name>`), merged when the tests pass.

## Unreleased

- **Spout output** (right-click → DasVis): share the picture with OBS, Resolume and other Spout programs on this PC.
- **Export DasVis videos**: right-click a song → *Export DasVis video* → 720p, 1080p, 1440p or 4K. An MP4 (H.264, AAC)
  of the song with DasVis, full quality and frame-exact on any PC; it works in the background, about as long as the
  song at 1080p60 on an Intel laptop GPU.
- **Room lights**: a WLED LED strip on your network can follow DasVis: the song's colours, brighter on the kick and the
  bass, lit up on drops, never strobing faster than the screen may. Settings → *Room lights (WLED)*; off by default.
- **Phone VJ deck for DasVis**: scan a QR code with a phone on the same Wi-Fi and run the show from it: an XY pad for
  the camera, *Drop now*, *Shockwave* (or shake the phone, where the browser allows it), scene buttons, intensity, like
  and skip. Protected by the same pairing code as phone sync.
- **DasVis**: drops land on the right beat (a downbeat guessed wrong could start one a bar early, or miss it); nothing
  is drawn while the window can't be seen (it ran at full speed for nobody when the display slept); a little less
  memory on old and fast PCs.
- **DasVis is now the default visualizer** (MilkDrop is still on **M**), with six more scenes: *Synthwave*,
  *Hyperspace*, *Aurora*, and for faster graphics cards *Mandelbox Flight*, *Liquid Chrome* and *Storm*. Each song opens
  with its title and artist, letter by letter on the beat; drops can shatter the cover at you; kicks stamp sigils; a
  ribbon of the waveform can fly over the scene.
- **The DasVis director**: scenes now follow the song. A calm scene for the breakdown, a hero scene on each drop
  (zooming through from the build-up, with a shockwave or a white-out), changes on the downbeat with a wipe,
  crossfade, shatter or glitch, only scenes this PC runs smoothly, and never the same few in a row. **F** likes a scene
  and **X** skips it, per genre. Long sections keep evolving, as each scene's settings drift slowly.
- **Four new DasVis scenes and a particle layer**: *Ink Ocean* (a fluid simulation), *Coral* (reaction-diffusion),
  *Galaxy* (up to a million stars), *Ridge Lines* (the spectrum's history in 3D), and particles over *Feedback
  Classic*. All run on old graphics cards (feature level 10_0) and are sized to the PC: on the old-PC check each new
  scene costs less GPU time than Neon Tunnel, and DasVis stays at about +20 MB.
- **DasVis takes its colours from the album cover**: five colours picked from the cover (k-means in Oklab), from dark
  to light; black-and-white covers fall back to the song's key, and minor keys look cooler. The kick, snare, hats, bass
  and lead found by the song map each move something different, on the song's exact timing.
- **DasVis knows the song** (the song map): each song is analysed once in the background while DasVis is open (about
  5 s and 6 MB for a 6-minute song), then cached.
  - It finds the beats, bars, parts (intro, build-up, drop, breakdown …), each drop and the quiet bars before it.
  - Build-ups drain the colour and close in, the bar before a drop dims, the drop punches, and a new scene starts on
    each drop.
  - The player bar counts down to the drop; **Ctrl+Shift+D** shows the map.
- **DasVis preview**: WreckBox's own visualizer. **D** or the *DasVis* chip in full screen.
  - Three scenes (Feedback Classic, Neon Tunnel, Kaleido Cover) that move with the kick, snare, hats, bass and the
    beat, in HDR with bloom and a filmic finish. Your own scenes from a folder, with F5 to reload.
  - Measures each PC and picks Low, Medium or High quality, remembered per graphics card. On old-PC settings (feature
    level 10_0, 2 cores) it uses about 5 % of one core and 19 MB, against MilkDrop's 26–30 % and ~65 MB.
  - Full-screen flashes held to 3 a second (1 with *Reduce flashing*); 30 fps on battery; 60 fps on high-refresh
    screens.
- **Groundwork for DasVis**, WreckBox's own visualizer (`docs/DASVIS-PLAN.md`). The window now draws through a
  Direct3D 11 device the app owns, so the visualizer will be able to share the GPU with the interface without copying
  pictures back. Nothing looks different, and memory and start-up time are unchanged. Old graphics cards (Direct3D
  feature level 10_0) and PCs with no usable GPU (Windows' software renderer) both work. The log names the graphics card
  in use.

## 0.7.3 — 2026-10-09

- **Download queue page, rebuilt.**
  - **Now:** each song being fetched has a progress bar with %, size and speed; *Searching Soulseek…* / *Waiting
    for <user>…* before the transfer starts; ✕ cancels it.
  - **Up next:** the waiting songs in order, with *Download first* and ✕.
  - **Just finished:** songs from the last 10 minutes, downloaded (format, size) or not (why), with *Try again*.
  - Playlists to download below that.
  - *Start sync* / *Stop* at the top; the sidebar shows how many songs are waiting.
- Both runners report progress in `active.json` (state, source, bytes, size, speed), about twice a second.
- **Fixed:** the buttons on the Soulseek page's rows (Ignore, Retry, Retry with your own words, Don't download) did
  nothing; a click on the row underneath took them.

## 0.7.2 — 2026-10-09

- **Cancel the song that's downloading right now.** It shows *Downloading now* (top of the Wanted tab, details panel).
  **Cancel download** stops it within a second or two, while it's searching or mid-transfer. The partial file is
  deleted, the song is marked Ignored, and it doesn't count as a failed try. The rest of the sync carries on. *Don't
  download* on several songs stops any of them that are in progress too. Both the built-in client and the helper
  support it.

## 0.7.1 — 2026-10-09

- **You choose what Soulseek downloads.** New default *Only what I pick*: nothing downloads until you pick it.
  - **Download** / **Don't download** on any song (details panel, right-click menu).
  - **Download playlist** on every playlist, which keeps up as the playlist grows.
  - *Everything missing* on the Soulseek page brings back the old behaviour.
- **Multi-select**: Ctrl + click, Shift + click, Ctrl + A, Esc. A bar acts on the whole selection: download, don't
  download, delete files.
- **Delete any downloaded song** (Recycle Bin). It is then marked Ignored, so the sync won't fetch it again.
  **Free up space** on a playlist deletes its downloads, except songs another pick still wants.
- **Wanted** tab on the Soulseek page: the download queue in order, with *Don't download* / *Download first*.
- **Storage**: how much WreckBox's songs use and how much is free; each playlist's size; a sortable **Size** column.
- **Quality: Best / Smaller.** Smaller prefers a 256 kbps+ MP3 / AAC over FLAC (about 8 MB a song instead of 30).
- **Fixed:** with "Then everything else" off and nothing picked, the sync downloaded *everything*. The queue also
  missed songs added to a picked playlist later; it now updates itself.

## 0.7.0 — 2026-10-09

- **Version numbers jump from 0.2 to 0.7** so this build never looks older than the original WreckBox apps (0.6.x)
  that share the same GitHub repo. Nothing else changes about numbering.
- **Less memory**: about half after scrolling a library with covers (~92 → ~50 MB private on a 287-track library;
  ~103 → ~56 MB on 5,166 tracks), with the same features and paint times
  - covers and album colours no longer pile up on the graphics card while you scroll (one shared brush, a small
    gradient cache)
  - the Windows segment heap gives freed memory back instead of keeping it
  - idle memory pages go back to Windows when the window is minimized and after the full-screen visualizer

## 0.2.0 — 2026-10-08

- **App icon**: the WreckBox logo (window, taskbar, Explorer)
- **Full-screen player bar**: one bar across the bottom of the screen, with "Up next" inside it, and a button (or **H**)
  to hide it; remembered
- **Delete from PC** in My folders, tucked away (right-click menu, and a small link at the bottom of the details panel
  that asks twice); the file goes to the Recycle Bin
- **Fixed:** "WreckBox 0.6.0 is available" — the update check announced the original app's releases; it now only
  offers releases of this build

- **File type** column on every track list (FLAC stands out) and **FLAC / Not FLAC** filters
- **My folders**: the songs already on your PC in the folders you pick (WreckBox's own downloads are left out), in
  the library or not, with the same search, filters, sorting, playback and inspector as the track list, and an
  *Add folder…* button

## 0.1.0 — 2026-10-07

The first build of the native C++ / Direct2D rewrite of WreckBox for Windows. It reads and writes the same library
files and settings as the original, so it can replace it.

- Library: tracks, BPM / key analysis (identical results to the original engine), tags, Downloads organiser
- Imports: CSV, Spotify, YouTube; Playlist Sync
- Player: libVLC playback, 10-band EQ, internet radio, media keys, tooltips
- Visualizer: full-screen MilkDrop (9,795 presets) with beat sync, beat pulse and preset changes on drops; Winamp bars
- Phone sync over Wi-Fi, account and tunnel ("use from anywhere")
- Update check, bug report, Settings, onboarding
- Soulseek sync through the bundled Python helper, plus a built-in client (beta)
- Release zip, smoke test and GitHub Actions CI

Known: the account service, bug relay and update check still point to the original developer's servers.
