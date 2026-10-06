// Spotify import (port of app/lib/spotify.dart): PKCE sign-in with the user's own client id (Spotify limits each
// developer app to a few users, so every WreckBox user registers their own), then Liked Songs + their playlists.
#pragma once
#include <functional>
#include <optional>
#include <string>

#include "model/model.h"
#include "sources/sources.h"

namespace wb::spotify {

inline constexpr const char* kRedirect = "http://127.0.0.1:8888/callback";  // what users register in their Spotify app
inline constexpr const char* kScopes = "playlist-read-private playlist-read-collaborative user-library-read";

std::string access_token();  // from the saved refresh token, else a browser sign-in. Throws with a readable message.
std::optional<SourceTrack> parse_track(const json& t, const std::optional<std::string>& added_at = std::nullopt);
// Best Spotify match for an artist + title (gives YouTube imports proper metadata). Never throws.
std::optional<SourceTrack> search(const std::string& token, const std::string& artist, const std::string& title);
// One playlist as it is now: `id` = a Spotify playlist id, or nullopt for Liked Songs. Throws with a readable message.
SourcePlaylist fetch_playlist(const std::string& token, const std::optional<std::string>& id);
Library run(const std::function<void(const std::string&)>& log);

}  // namespace wb::spotify
