// Library sources (port of app/lib/sources.dart). Each importer (Spotify, YouTube, CSV) saves its playlists to
// _sources/<kind>.json; library.json is rebuilt from all of them, so re-importing one source never drops the others.
// One library entry per recording (ISRC → Spotify id → YouTube id → artist/title/duration), tagged with every
// playlist it appears in.
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "model/model.h"

namespace wb {

struct SourceTrack {
    std::optional<std::string> spotify_id, youtube_id, isrc, album, release_date, added_at, artwork_url;
    std::string name;
    std::vector<std::string> artists;
    std::optional<int64_t> duration_ms;
    bool is_local = false;

    static SourceTrack from_json(const json& j);
    json to_json() const;
};

struct SourcePlaylist {
    std::string name;
    std::optional<std::string> id;
    bool collaborative = false;
    std::vector<SourceTrack> tracks;
    std::optional<std::string> file;  // CSV playlists: the file they were imported from (UTF-8), so Sync can re-read it

    static SourcePlaylist from_json(const json& j);
    json to_json() const;
};

namespace sources {

std::vector<SourcePlaylist> load(const std::string& kind);  // empty if this importer hasn't saved anything
// Saves one importer's playlists and rebuilds library.json from every source. Throws on write failure.
Library save(const std::string& kind, const std::string& user, const std::vector<SourcePlaylist>& playlists);
Library rebuild();
Library build(const std::string& user, const std::vector<SourcePlaylist>& playlists);  // pure: no files

// Where a library playlist came from: which source file ("spotify", "youtube", "csv", "spotify-legacy", …) and its
// position there. Matched by Spotify / YouTube id when it has one, else by name.
struct Located {
    std::string kind;
    size_t index = 0;
    SourcePlaylist playlist;
};
std::optional<Located> locate(const LibraryPlaylist& pl);
// Replaces playlist `index` of source `kind` with `pl` (everything else untouched) and rebuilds library.json.
Library replace(const std::string& kind, size_t index, const SourcePlaylist& pl);

}  // namespace sources
}  // namespace wb
