// Sync one playlist: fetch it again from where it came from (Spotify, YouTube, or the CSV file it was imported from)
// and make the library's copy match it — new songs added, removed songs dropped from the playlist. Only that playlist
// changes; downloaded files, analysis and track status are never touched.
#pragma once
#include <functional>
#include <optional>
#include <string>

#include "model/model.h"

namespace wb::sync {

struct Result {
    std::string name;  // the playlist's name now (it may have been renamed at the source)
    size_t added = 0, removed = 0, total = 0;
};

// Why this playlist can't be synced (a sentence for the UI), or nullopt if it can.
std::optional<std::string> unavailable(const LibraryPlaylist& pl);

// Runs on a worker thread (network, sign-in in the browser if needed). Throws with a readable message.
Result playlist(const LibraryPlaylist& pl, const std::function<void(const std::string&)>& log);

}  // namespace wb::sync
