# WreckBox for Windows — getting started

WreckBox is a DJ library manager: your Spotify and YouTube playlists, the tracks you have, their BPM, key and energy,
a player, and sync to your phone. This is the **native Windows build**: one small program, made for older PCs too.

## Install

1. Download `WreckBox-<version>-win-native-x64.zip` from the releases page.
2. Unzip it anywhere (for example `Documents\WreckBox`) and run **`wreckbox.exe`**. Nothing is installed; delete the
   folder to remove it. Keep the files in the folder together (`libvlc.dll`, `plugins\`, `milkdrop\`, `soulseek\`).
3. If SmartScreen says "Windows protected your PC": **More info → Run anyway**.
4. When Windows asks, allow **private networks** (only needed to send tracks to your phone).

Works on Windows 10 and 11 (64-bit). Your music and library live in `Music\WreckBox`; your settings in
`%APPDATA%\local.wreckbox\wreckbox`. It uses the same folders as the older Flutter build, so you can switch between them.

## Import your playlists (no accounts or keys needed)

1. **Spotify:** go to **exportify.net**, log in with Spotify, click **Export All**. You get one CSV file per playlist.
2. **YouTube / YouTube Music:** **takeout.google.com** → *Deselect all* → tick **YouTube and YouTube Music** → keep only
   **playlists** → export. Or use **tunemymusic.com** → pick YouTube → **Export to file**.
3. In WreckBox: **Settings → Import playlists → Choose CSV files**, and select them all.

Each song is looked up in free music catalogues for its ISRC and cover. A big first import takes a while (about a song
per second); importing again later is quick, and a playlist with the same name is replaced.

## Everyday use

- **Play:** click a track's cover. Space plays and pauses; media keys work. **F11** (or the visualizer button) opens the
  full-screen **MilkDrop** visualizer with the player panel; **N / P** change the preset, **M** switches to the classic
  bars. PCs without OpenGL 3.3 get the bars automatically.
- **Organise:** audio you save to your Downloads folder that is in your playlists is analysed, tagged, renamed
  "Artist - Title" and moved into `Music\WreckBox\Tracks` (Settings → Library folders turns this off).
- **Computer → phone:** *Sync to phone → Start sharing*, then in the phone app *Computer → Scan pairing code*. Sign in to
  a WreckBox account there and turn on *Use from anywhere* to reach this computer away from home.
- **Soulseek:** Settings → Soulseek: choose a username and password (the first login creates the account). *Download
  queue* sets which playlists are fetched first; *Soulseek sync* shows what was found and what wasn't, with retry.
- **Rekordbox:** WreckBox writes BPM, key, ISRC and cover into the files. In Rekordbox select the tracks → right-click →
  **Reload Tag** to see updates.

## Updates

When a newer version is out, WreckBox shows **"WreckBox x.y.z is available" → Download** at the top. Unzip the new
version over the old folder; your library and settings stay. *Settings → About → Check for updates* checks on demand.

## Found a bug?

**Report a bug** (bottom of the sidebar) sends a description, a screenshot of the app and recent activity. No music files
or passwords.

## Licences

The licences of everything inside are in the `licenses` folder (Settings → About → Open licences).
