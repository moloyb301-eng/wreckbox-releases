// Reading and writing track tags (port of core/src/tags.rs, TagLib instead of Lofty).
// ID3v2.3 for MP3/AIFF/WAV (best Rekordbox compatibility), Vorbis comments for FLAC/OGG, MP4 atoms for M4A.
// Unmanaged tags are kept; files are tagged as a copy and swapped in, so a crash can't leave a half-written track.
#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace wb {

struct TrackTags {
    std::optional<std::string> title;
    std::vector<std::string> artists;
    std::optional<std::string> album, year, genre;
    std::optional<double> bpm;
    std::optional<std::string> key;    // "A minor" or short form "Am"
    std::optional<std::string> isrc;
    std::optional<std::string> cover;  // write only: local path to a JPEG / PNG
    bool has_cover = false;            // read only
};

// "A minor" → "Am", "F# major" → "F#" (the form ID3 TKEY and Rekordbox use).
std::string short_key(const std::string& k);

// Both throw std::runtime_error with a readable message.
TrackTags read_tags(const std::filesystem::path& path);
void write_tags(const std::filesystem::path& path, const TrackTags& t);

}  // namespace wb
