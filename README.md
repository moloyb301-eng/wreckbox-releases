<div align="center">

<img src="res/logo.svg" alt="WreckBox logo" width="96" height="96">

# WreckBox for Windows

### Your music, sorted.

*Bring in your Spotify and YouTube playlists, your music folder and your CSV lists. WreckBox keeps them in one library, works out the BPM, key and energy of each track, tags the files and tidies them up so your DJ software can read them.*

<br>

[![Platform](https://img.shields.io/badge/Windows%2010%20%2F%2011-x64-A9C8F0?style=for-the-badge&logo=windows&logoColor=08080A&labelColor=08080A)](#-download--install)
[![Language](https://img.shields.io/badge/C%2B%2B20-native-BB96DA?style=for-the-badge&logo=cplusplus&logoColor=08080A&labelColor=08080A)](#-tech-stack)
[![Audio](https://img.shields.io/badge/libVLC-18%2B_formats-EFAF86?style=for-the-badge&logo=vlcmediaplayer&logoColor=08080A&labelColor=08080A)](#-features)
[![Visualizer](https://img.shields.io/badge/MilkDrop-9%2C795_presets-EFAF86?style=for-the-badge&labelColor=08080A)](#-using-it)

[**⬇️ Download**](#-download--install) · [**✨ Features**](#-features) · [**🎹 Using it**](#-using-it) · [**🔨 Build**](#-build-it-yourself) · [**📝 Changelog**](CHANGELOG.md)

</div>

---

<br>

## ⬇️ Download & install

<div align="center">

[![Download zip](https://img.shields.io/badge/WreckBox%200.7.2-win--native--x64.zip%20%C2%B7%2071%20MB-EFAF86?style=for-the-badge&logo=windows&logoColor=08080A&labelColor=08080A)](https://raw.githubusercontent.com/moloyb301-eng/wreckbox-releases/windows-native/downloads/WreckBox-0.7.2-win-native-x64.zip)

</div>

The latest build (0.7.2). You choose exactly what Soulseek downloads (single songs or whole playlists), can cancel the song that's downloading right now, and can delete songs to free up space.

### ⚡ One command

Press **Win + R**, type `cmd`, press Enter, then paste this (the copy button is on the right of the box) and press
Enter. It downloads WreckBox, unzips it into `%LOCALAPPDATA%\Programs\WreckBox` and starts it. Run it again to update.

```cmd
curl -fL -o "%TEMP%\WreckBox.zip" https://raw.githubusercontent.com/moloyb301-eng/wreckbox-releases/windows-native/downloads/WreckBox-0.7.2-win-native-x64.zip && (if not exist "%LOCALAPPDATA%\Programs\WreckBox" mkdir "%LOCALAPPDATA%\Programs\WreckBox") && tar -xf "%TEMP%\WreckBox.zip" -C "%LOCALAPPDATA%\Programs\WreckBox" && del "%TEMP%\WreckBox.zip" && start "" "%LOCALAPPDATA%\Programs\WreckBox\wreckbox.exe"
```

Close WreckBox before updating. The command uses `curl` and `tar`, which come with Windows 10 and 11.

### 📦 Or by hand

1. **Unzip** the download into a folder of its own, e.g. `C:\WreckBox`. Keep everything together: `wreckbox.exe` needs
   the files next to it.
2. **Run `wreckbox.exe`.** Windows SmartScreen may say the app isn't recognised, because it isn't signed. Choose
   *More info → Run anyway*.

> Windows 10 or 11, 64-bit. It uses your existing `Music\WreckBox` library, so nothing needs moving.

> **Good to know:** the account and bug reports still go to the original WreckBox services. The update check only
> looks at releases of this build. See the [changelog](CHANGELOG.md).

<br>

## ✨ Features

| | Feature | What it does |
|---|---|---|
| 🗂️ | **Library** | Every track in one list, with search (Ctrl+F), filters, sorting, a details panel and a file-type column |
| 🎚️ | **Analysis** | BPM, musical key (shown in Camelot colours) and energy for every track, written back into the tags |
| 🎧 | **FLAC filter** | FLAC shows up in purple. *FLAC* and *Not FLAC* filters help you find the lower-quality files |
| 📥 | **Imports** | CSV files from Exportify, TuneMyMusic and Google Takeout, plus Spotify and YouTube sign-in with your own client ID |
| 🔄 | **Playlist Sync** | A *Sync* button on every playlist pulls it up to date from where it came from |
| 📁 | **My folders** | The songs already on your PC, in the folders you pick. WreckBox's own downloads are left out |
| ⬇️ | **Downloads organiser** | Watches your Downloads folder and files new songs into the library |
| ▶️ | **Player** | 18+ formats, internet radio and `.m3u` / `.pls` links, files from outside the library, media keys |
| 🎛️ | **Equalizer & normalizer** | 10-band EQ with presets and preamp, plus a volume normalizer |
| 🌈 | **MilkDrop visualizer** | Full-screen MilkDrop presets (9,795 of them) with a player bar: song, transport, seek, volume, EQ and "Up next" |
| 🥁 | **Beat sync** | Kicks pulse the picture, presets change on drops, and you can nudge the timing by 25 ms |
| 📊 | **Winamp bars** | Classic spectrum and oscilloscope modes. Used automatically on a PC without OpenGL 3.3 |
| 📱 | **Phone sync** | Pair the WreckBox Android app with a QR code, then sync over your Wi-Fi |
| ☁️ | **Use from anywhere** | Reach your library from your phone away from home, through a Cloudflare tunnel |
| 🔍 | **Soulseek** | Downloads only the songs and playlists you pick, in best or smaller quality, with a queue you can reorder and cancel (even mid-download), through a bundled helper or a built-in client (beta) |
| 🧹 | **Free up space** | Delete songs one at a time, a selection, or a whole playlist's downloads (to the Recycle Bin), and see how much space they use |
| 🧾 | **Account** | Sign in to save your library to your WreckBox account |
| 🩹 | **Housekeeping** | Update check, bug reports, onboarding and a Settings page |

<br>

<details>
<summary><b>📁 Library details</b></summary>
<br>

**Types & FLAC.** Every track list has a **Type** column. FLAC stands out in purple. The **FLAC** and **Not FLAC**
filters work together with search and sorting. *Not FLAC* shows the songs in another format, which are the ones worth
replacing.

**My folders** (sidebar, under Library) lists the songs already on your PC: your `Downloads` and `Music` folders, plus
any folder you add with **Add folder…**. The header names the folders. A song that matches your library shows the
library's names and cover. The details panel shows the type and size, and says "Not in your library" when it isn't.
*Write tags* is hidden there, so your own files keep their tags. A file WreckBox can't analyse doesn't appear.

**Selecting many songs.** Ctrl + click adds or removes a song, Shift + click selects a range, Ctrl + A selects the whole
list, Esc clears it. A bar at the bottom then offers *Download*, *Don't download* and *Delete files* for all of them. The
**Size** column (sortable) helps find the biggest files.

**Deleting a song from the PC.** Right-click it and choose *Move to Recycle Bin*, or use the small *Delete from PC* link
at the bottom of its details panel (click twice). It goes to the Recycle Bin, so you can get it back, but the space only
comes back once you empty the Recycle Bin. A deleted library song is marked *Ignored*, so Soulseek won't fetch it again;
*Download* brings it back.

**Imports.** CSV files from Exportify, TuneMyMusic or Google Takeout are matched with MusicBrainz and Deezer for the
details. Spotify and YouTube need your own client ID, and then import your playlists and liked music.

</details>

<details>
<summary><b>📊 Resource use</b></summary>
<br>

Measured on the developer's PC:

| | |
|---|---|
| Memory (private), a real 287-track library with covers | ~43 MB playing at rest, ~50 MB after scrolling through it (was ~92 MB before 2026-10-08) |
| Memory, the same library repeated to 5,166 tracks | ~49 MB playing at rest, ~56 MB after scrolling |
| Memory, full-screen MilkDrop | ~115 MB while it runs; given back when you leave |
| Startup | ~0.12 s to the first paint |
| CPU when idle | 0 %: the window only redraws when something changes |
| Playing | ~1.4 % of one core with the player bar; ~42 % with the full-screen MilkDrop visualizer at 720p, 60 fps |
| Download | 71 MB zip, including VLC's engine, the MilkDrop presets and the Soulseek helper |

</details>

<br>

## 🧰 Tech stack

| Area | Choice |
|---|---|
| **Language** | C++20, built with MSVC, CMake and vcpkg |
| **Interface** | Win32 with Direct2D. Urbanist for text, Doto for labels and readouts |
| **Audio** | libVLC engine, with miniaudio for output |
| **Visualizer** | projectM (MilkDrop presets) |
| **Tags & decoding** | TagLib, dr_libs |
| **Network** | WinHTTP, nlohmann-json, cpp-httplib, nayuki QR code generator |

<br>

## 🎹 Using it

<details>
<summary><b>⌨️ Keys & playing music</b></summary>
<br>

| Key | What it does |
|---|---|
| ↑ / ↓ | Move through the list |
| Enter | Play the selected track |
| Space | Play / pause |
| Esc | Close the details panel |
| Ctrl+F | Search |
| Ctrl+O | Open files |
| Ctrl+U | Open a URL (radio, `.m3u` or `.pls`) |
| F5 | Rescan the library |
| F11 | Visualizer, full screen |
| H | Hide or show the player bar (in full screen) |

Click a track's cover to play it. The queue is the list on screen. You can also drop files or folders onto the window,
or use **+** in the player bar. Media keys work even when WreckBox is in the background.

The sliders button opens the **equalizer** (presets, 10 bands, preamp) and the **volume normalizer**.

</details>

<details>
<summary><b>🌈 Visualizer</b></summary>
<br>

The visualizer button, or **F11**, goes full screen. The MilkDrop presets run under a player bar across the bottom of
the screen. The bar fades when the mouse is still. Its ⌄ button, or **H**, hides it, and *Show player* brings it back.

- **N / P** next / previous preset · **R** random · **L** lock the preset · click = next preset · **M** switch between
  MilkDrop and the Winamp bars · **Space** pauses · **Esc**, **F11** or double-click leaves.
- The picture follows the beat. Each kick pulses it and presets change on drops. **[** and **]** move the picture
  earlier or later by 25 ms if it's out of step. Bluetooth speakers are allowed for automatically.
- Right-click for the options: beat pulse (off / subtle / strong), preset change on drops, beat sensitivity, preset set
  (beat-heavy or all), beat sync, quality (540p / 720p / 1080p), how often presets change, lock, and *Open presets
  folder*.
- Your own presets: drop `.milk` files into `%APPDATA%\local.wreckbox\wreckbox\milkdrop\presets`.
- In the bars mode, a click steps through spectrum → oscilloscope → both → off. Right-click has every Winamp option.
  **V** switches between the Classic Winamp and WreckBox colours.
- A PC without OpenGL 3.3 gets the bars automatically. `WRECKBOX_NO_MILKDROP=1` forces them.

</details>

<details>
<summary><b>📱 Sync to phone</b></summary>
<br>

Open *Sync to phone* in the sidebar. *Start sharing* shows a QR code and a link for the WreckBox Android app. Only phones
that scanned the code can connect, and *Unpair all phones* revokes them all.

Sign in to your WreckBox account to save your library to it. Turn on *Use from anywhere* to reach this computer away
from home. The first time, WreckBox downloads Cloudflare's `cloudflared` (about 55 MB).

Environment settings: `WRECKBOX_SYNC_PORT` changes the port (default 47390). `WRECKBOX_API` points the account at another
service, for testing.

</details>

<details>
<summary><b>🔍 Soulseek</b></summary>
<br>

Soulseek sync runs through a bundled helper, which you sign in to with your own Soulseek account. Songs land in your
library through the Downloads organiser. There's also a built-in client in beta. It's opt-in until it has been tried on
the live network.

**You decide what downloads.** By default (*Download: Only what I pick* on the Soulseek page) nothing downloads until you
pick it:
- **One song:** *Download* in its details panel or right-click menu. It goes to the front of the queue and the sync
  starts. *Don't download* skips it.
- **A playlist:** *Download playlist* at the top of the playlist. Its missing songs download, including songs added to it
  later. Click again to stop.
- **Many songs:** select them (Ctrl / Shift + click) and use the bar at the bottom.
- *Everything missing* on the Soulseek page brings back the old behaviour.

**Seeing the queue.** The Soulseek page's **Wanted** tab lists what will download, in order, with *Don't download* and
*Download first* on each song. *Downloaded*, *Not found* and *Failed* are as before.

**Saving space.**
- **Quality:** *Best (FLAC first)* or *Smaller (MP3 320 first)*. Smaller takes a 256 kbps+ MP3 / AAC when there is one
  (about 8 MB a song instead of about 30 MB) and FLAC only when there isn't.
- **Free up space** on a playlist sends its downloaded songs to the Recycle Bin (it asks first, with the size). Songs
  another pick still wants are kept, and your own files outside WreckBox's folder are never touched by it.
- The Soulseek page shows how much WreckBox's songs use and how much space is free; each playlist shows its size.

**Cancelling the song that's downloading.** The song the sync is on right now shows *Downloading now*, at the top of the
**Wanted** tab and in its details panel. **Cancel download** (or the ✕ on its row, or its right-click menu) stops it
within a second or two, deletes the partial file and marks the song *Ignored*. It doesn't count as a failed try, and
*Download* brings it back. The rest of the sync carries on; *Stop* still stops everything.

</details>

<br>

## 🔨 Build it yourself

<details>
<summary><b>Requirements & commands</b></summary>
<br>

You need **Visual Studio 2022 Build Tools** (or Visual Studio 2022) with the *Desktop development with C++* workload.
That includes MSVC, CMake, Ninja and vcpkg. Dependencies come from vcpkg on the first build.

```powershell
.\scripts\build.ps1            # configure, build Release and run the tests
.\scripts\build.ps1 -Debug     # Debug build
```

The output goes to `build\Release\` (or `build\Debug\`):

- **`wreckbox.exe`**: the app. It opens your `Music\WreckBox` library.
  - `--root <folder>` opens a different library folder. Settings go in `<folder>\_settings`. Use a **copy** of your
    library, or a demo.
  - `--background` opens the window behind the others without taking focus.
  - Any other arguments are files, folders or URLs to play, as if you'd dropped them on the window.
  - `WRECKBOX_PERF=1` logs paint times to `wreckbox.log`. `WRECKBOX_SOFTWARE=1` draws on the CPU, for drivers that misbehave.
- Next to it: `libvlc.dll`, `libvlccore.dll`, `plugins\` (keep these together), `milkdrop\` (presets and textures) and
  `licenses\`.
- **`wbcore.exe`**: the engine's command line.
  ```powershell
  build\Release\wbcore.exe analyze "C:\Music\track.mp3"      # one JSON line per file
  build\Release\wbcore.exe tags    "C:\Music\track.mp3"      # read tags as JSON
  '[{"path":"t.wav","title":"T","bpm":120,"key":"A minor"}]' | build\Release\wbcore.exe write-tags
  ```
  `wbcore bench FILE…` shows where the time goes (decode, tempo, key) in milliseconds.
- **Tests:** 11 suites, run by `build.ps1` through `ctest`. The player tests play muted. Optional extras:
  - `WRECKBOX_TEST_LIBRARY=<a COPY of Music\WreckBox>`: round-trip a real library
  - `WRECKBOX_NET_TESTS=1`: call the real Deezer, MusicBrainz and YouTube services
  - `WRECKBOX_BENCH=1`: time loading and searching 5,000 tracks

</details>

<br>

## 📝 Changelog

See [CHANGELOG.md](CHANGELOG.md) for what changed in each version and how releases are made.

<br>

---

<div align="center">

**WreckBox for Windows** · [releases](https://github.com/moloyb301-eng/wreckbox-releases/tree/windows-native)

🎛️

</div>
