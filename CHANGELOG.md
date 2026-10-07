# Changelog

Versions follow [semantic versioning](https://semver.org): MAJOR.MINOR.PATCH.

**Making a release**
1. Set the version in `CMakeLists.txt` (`project(wreckbox VERSION x.y.z)`); it's the only place it lives.
2. Add a section for it below.
3. Commit, then tag the commit: `git tag -a vx.y.z -m "WreckBox x.y.z"`.
4. Pushing the tag runs CI, which builds and tests the zip and refuses a tag that doesn't match `CMakeLists.txt`.

Day-to-day work goes on `main`; bigger features on a branch (`feature/<name>`), merged when the tests pass.

## Unreleased

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
