// Playlist import from CSV files — no Spotify / Google developer keys needed (port of app/lib/csv_import.dart).
//
// Understands the common export formats (column names are matched loosely):
//   • Exportify (Spotify):        Track URI, Track Name, Artist Name(s), Album Name, Release Date, Duration (ms), Added At
//   • TuneMyMusic (Spotify, YouTube Music, …): Track name, Artist name, Album, Playlist name, ISRC?
//   • Google Takeout YouTube playlists: Video ID, Playlist Video Creation Timestamp — titles come from YouTube's public
//     oEmbed endpoint (no key)
//   • Any CSV with title + artist columns
//
// Every track is then enriched from free catalogues (MusicBrainz → ISRC; Deezer by ISRC → cover, album, length; Cover
// Art Archive as the fallback cover), cached in _cache/catalogue_lookup.json so re-imports are instant.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "sources/sources.h"

namespace wb::csv {

using Log = std::function<void(const std::string&)>;

// RFC 4180: quoted fields, doubled quotes, newlines inside quotes; empty rows dropped; UTF-8 BOM skipped.
std::vector<std::vector<std::string>> parse_csv(const std::string& text);

// One CSV file → playlists (one per "Playlist name" value, or one named after the file). `lookup_youtube` resolves
// Takeout video ids to titles over the network.
std::vector<SourcePlaylist> parse_file(const std::string& utf8_path, bool lookup_youtube = true, const Log& log = {});

// Fills in ISRC, cover, album and length from the catalogues. `cache` is the catalogue_lookup.json object.
SourceTrack enrich(const SourceTrack& t, json& cache);

struct Result {
    std::vector<SourcePlaylist> playlists;
    size_t tracks = 0, enriched = 0;
};
// Parse, enrich, and add / replace those playlists in the "csv" source, then rebuild library.json. Throws on failure.
// Each playlist remembers its file, so Sync can re-read it later.
Result run(const std::vector<std::string>& utf8_paths, const Log& log);

// Re-reads playlist `name` from the CSV it was imported from (after the user re-exported it), enriched like an import.
// Throws with a readable message if the file is gone or no longer has that playlist.
SourcePlaylist reread(const std::string& utf8_path, const std::string& name, const Log& log);

}  // namespace wb::csv
