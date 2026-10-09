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
