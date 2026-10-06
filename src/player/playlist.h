// Playlist files (.m3u / .m3u8 / .pls / .xspf / .asx) → their entries, from disk or from a radio station's URL.
// libVLC 3 only reveals a playlist's entries once it starts playing it, so WreckBox reads these simple formats itself.
// HLS streams (.m3u8 with #EXT-X- tags) aren't lists of tracks — they're left for VLC to play as one stream.
#pragma once
#include <string>
#include <vector>

namespace wb::player {

bool is_playlist(const std::string& location);
// Parses playlist text. Relative entries are resolved against `base` (the playlist's folder or URL).
std::vector<std::string> parse_playlist(const std::string& text, const std::string& base);
// Reads (or downloads) and parses; anything that isn't a playlist — or an HLS stream — comes back as itself.
// Blocking (disk / network): call from a worker.
std::vector<std::string> expand_playlist(const std::string& location);

}  // namespace wb::player
