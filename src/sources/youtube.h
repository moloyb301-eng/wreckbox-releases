// YouTube import (port of app/lib/youtube.dart): the user's YouTube / YouTube Music playlists and liked music videos
// become library playlists ("YT: …"). Video titles are cleaned into artist + title and, when Spotify is connected,
// matched to the Spotify track for proper metadata so they merge with the same song from Spotify.
//
// Each user creates a Google Cloud OAuth client of type "Desktop app" with the YouTube Data API v3 enabled
// (read-only scope). Sign-in goes through the system browser with a loopback redirect + PKCE.
#pragma once
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "model/model.h"
#include "sources/sources.h"

namespace wb::youtube {

// "Artist - Title (Official Video)" / YouTube Music "Artist - Topic" channels → (artists, title).
std::pair<std::vector<std::string>, std::string> parse_title(const std::string& video_title, const std::string& channel);
std::optional<int64_t> parse_iso_duration(const std::string& s);  // "PT3M20S" → 200000

// Imports playlists + liked music into the "youtube" source and rebuilds the library. Throws with a readable message.
Library run(const std::function<void(const std::string&)>& log);
// One playlist as it is now ("LL" = liked music). Signs in if needed. Throws with a readable message.
SourcePlaylist fetch_playlist(const std::string& id);

}  // namespace wb::youtube
