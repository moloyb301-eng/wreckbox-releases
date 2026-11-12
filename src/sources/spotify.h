// Spotify import: PKCE sign-in with the user's own client id (Spotify limits each
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
// One playlist as it is now: `id` = a Spotify playlist id, or nullopt for Liked Songs. `known` is the playlist's entry
// from the list of playlists when the caller has it (saves asking for its name). Throws with a readable message.
SourcePlaylist fetch_playlist(const std::string& token, const std::optional<std::string>& id, const json& known = json());
// Liked Songs, then each of `user`'s own and collaborative playlists in Spotify's order. Pages and playlists are
// fetched side by side (up to six requests at once); `log` gets a line as each finishes.
std::vector<SourcePlaylist> fetch_all(const std::string& token, const std::string& user, const std::function<void(const std::string&)>& log);
Library run(const std::function<void(const std::string&)>& log);

// Tests: talk to a stand-in server instead of https://api.spotify.com/v1/ (the address ends in a slash).
void set_api_base(const std::string& base);

}  // namespace wb::spotify
