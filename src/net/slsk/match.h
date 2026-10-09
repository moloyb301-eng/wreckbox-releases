// Choosing what to download (a port of the matching and ranking half of sidecar/slsk_sync.py, kept identical on purpose:
// tests compare it with the Python on recorded cases). Pure functions, no network.
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "model/model.h"
#include "net/slsk/types.h"

namespace wb::slsk {

struct MatchConfig {
    int min_lossy_kbps = 256;
    int duration_tolerance_seconds = 5;
    bool prefer_smaller = false;  // MP3 / AAC at or above the limit before lossless (about 8 MB a song instead of 30)
};

// ASCII-folded (NFKD, non-ASCII dropped), lower case, "&" → "and", words of a–z / 0–9 joined by single spaces.
std::string norm(const std::string& s);
// Drops "(feat. …)", "- Remastered" and "(Original Mix)" noise from a title; keeps remix / edit names.
std::string clean_title(const std::string& title);
// The searches for a track, best first: the custom words if any, "artist title", then without bracketed / dashed parts.
std::vector<std::string> search_queries(const LibraryTrack& track, const std::string& custom_query = "");

struct Candidate {
    std::string username, path, ext;
    uint64_t size = 0;
    std::optional<int> bitrate, duration;
    bool free_slot = false;
    int speed = 0, queue = 0;
    double quality = 0;
    std::string label() const;  // "FLAC  4.2MB  user  free  120KB/s"
};

// nullopt: not usable (lossy below the limit, or lossy of unknown quality). `smaller`: lossless ranks below any usable
// lossy file (its 1–5 against lossy's 6+), so a FLAC is taken only when there's no good MP3 / AAC.
std::optional<double> quality_of(const std::string& ext, std::optional<int> bitrate, int min_kbps, bool smaller = false);
bool file_matches(const LibraryTrack& track, const std::string& path, std::optional<int> duration, int tolerance_seconds);
// Usable candidates, best first, one per user. `loose`: a custom query, whose words must be in the path instead of the usual
// title / artist check.
std::vector<Candidate> rank(const LibraryTrack& track, const std::vector<UserResult>& results, const MatchConfig& cfg,
                            const std::optional<std::string>& loose = std::nullopt);

}  // namespace wb::slsk
