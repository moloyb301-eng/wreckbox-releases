#include "sources/sync.h"

#include <algorithm>
#include <set>
#include <stdexcept>

#include "model/settings.h"
#include "sources/csv_import.h"
#include "sources/sources.h"
#include "sources/spotify.h"
#include "sources/youtube.h"

namespace wb::sync {

std::optional<std::string> unavailable(const LibraryPlaylist& pl) {
    const auto loc = sources::locate(pl);
    if (!loc) return "This playlist isn't in any import source — import it again from Settings to enable Sync.";
    const auto& s = Settings::current();
    if (loc->kind == "spotify") {
        if (s.spotify_client_id.empty()) return "Add your Spotify client ID in Settings to sync this playlist.";
        return std::nullopt;
    }
    if (loc->kind == "youtube") {
        if (s.google_client_id.empty()) return "Add your Google client ID in Settings to sync this playlist.";
        if (!loc->playlist.id) return "This YouTube playlist has no id — import YouTube again from Settings.";
        return std::nullopt;
    }
    if (loc->kind == "csv") {
        if (!loc->playlist.file) return "Imported before Sync existed — import this CSV once more from Settings to enable Sync.";
        return std::nullopt;
    }
    if (loc->kind == "spotify-legacy") return "Imported before sources existed — re-import from Settings to enable Sync.";
    return "This kind of playlist can't be synced.";
}

Result playlist(const LibraryPlaylist& pl, const std::function<void(const std::string&)>& log) {
    if (const auto why = unavailable(pl)) throw std::runtime_error(*why);
    const auto loc = *sources::locate(pl);

    SourcePlaylist fresh;
    if (loc.kind == "spotify") {
        log("Signing in to Spotify…");
        const std::string tok = spotify::access_token();
        log("Fetching " + pl.name + " from Spotify…");
        fresh = spotify::fetch_playlist(tok, loc.playlist.id);
    } else if (loc.kind == "youtube") {
        log("Fetching " + pl.name + " from YouTube…");
        fresh = youtube::fetch_playlist(*loc.playlist.id);
    } else {
        fresh = csv::reread(*loc.playlist.file, loc.playlist.name, log);
    }

    const Library lib = sources::replace(loc.kind, loc.index, fresh);

    // The playlist as rebuilt: by id when it has one, else by its (trimmed) name.
    std::string want = fresh.name;
    want.erase(0, want.find_first_not_of(" \t\r\n"));
    want.erase(want.find_last_not_of(" \t\r\n") + 1);
    const auto now = std::find_if(lib.playlists.begin(), lib.playlists.end(), [&](const LibraryPlaylist& p) {
        return fresh.id ? p.spotify_id == fresh.id : p.name == want;
    });
    Result r;
    if (now == lib.playlists.end()) return r;
    r.name = now->name;
    const std::set<std::string> before(pl.track_ids.begin(), pl.track_ids.end()), after(now->track_ids.begin(), now->track_ids.end());
    for (const auto& id : after) r.added += !before.contains(id);
    for (const auto& id : before) r.removed += !after.contains(id);
    r.total = after.size();
    return r;
}

}  // namespace wb::sync
