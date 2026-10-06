// Matching audio files to library tracks (port of app/lib/matcher.dart): ISRC first, then artist + title from tags
// or "Artist - Title" file names, with a duration check.
#pragma once
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "model/model.h"

namespace wb {

struct FileFacts {
    std::string path;  // UTF-8
    std::optional<std::string> title, isrc;
    std::vector<std::string> artists;
    std::optional<double> duration_sec;
};

class TrackMatcher {
public:
    explicit TrackMatcher(const std::vector<LibraryTrack>& tracks);

    // Strips noise that differs between Spotify titles and file tags / names, keeping remix / edit names.
    static std::string clean_title(const std::string& s);

    // Index into `tracks`, or nullopt.
    std::optional<size_t> match(const FileFacts& f) const;

private:
    double gap(size_t i, const FileFacts& f) const;

    const std::vector<LibraryTrack>& tracks_;
    std::unordered_map<std::string, size_t> by_isrc_;
    std::unordered_map<std::string, std::vector<size_t>> by_title_;
};

// Keys that mix with Camelot code `c`: same, ±1 on the wheel, relative major / minor.
std::set<std::string> compatible_keys(const std::string& c);

// Sort order for Camelot codes (1A, 1B, 2A, …); unknown last.
int camelot_order(const std::optional<std::string>& c);

}  // namespace wb
