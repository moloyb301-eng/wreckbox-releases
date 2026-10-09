// Data model. JSON shapes match the original and Mac apps exactly
// (library.json, state.json, _cache/analysis.json), so the same library folder works with every build.
//
// Each record keeps the JSON object it was read from (`extra`) and writes its known fields over it, so fields
// another app version added survive a load/save round trip.
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace wb {

using json = nlohmann::json;

struct LibraryTrack {
    std::string id;  // ISRC when available, else spotify:<id>, else normalised artist+title
    std::vector<std::string> artists;
    std::string title;
    std::optional<std::string> album, year, isrc;
    std::vector<std::string> spotify_ids;
    std::optional<int64_t> duration_ms;
    std::vector<std::string> playlists;
    std::optional<std::string> first_added;
    std::string file_name;  // "Artist - Title" stem used for files
    std::optional<std::string> artwork_url;
    json extra = json::object();

    std::string artist() const;  // artists joined with ", "
    static LibraryTrack from_json(const json& j);
    json to_json() const;
};

struct LibraryPlaylist {
    std::string name;
    std::optional<std::string> spotify_id;
    bool collaborative = false;
    std::vector<std::string> track_ids;
    json extra = json::object();

    static LibraryPlaylist from_json(const json& j);
    json to_json() const;
};

struct Library {
    std::string built_at;  // ISO-8601, kept as read; new libraries get iso_seconds(now)
    std::string spotify_user;
    std::vector<LibraryTrack> tracks;
    std::vector<LibraryPlaylist> playlists;
    json extra = json::object();

    static Library from_json(const json& j);
    json to_json() const;
};

enum class TrackStatus { missing, downloaded, ignored };
const char* to_string(TrackStatus s);
TrackStatus status_from(const std::string& s);  // unknown → missing

struct TrackState {
    TrackStatus status = TrackStatus::missing;
    std::optional<std::string> local_path;
    std::optional<std::string> source;  // "scan", "soulseek", "organizer", "phone-sync", "dropbox", "manual", …
    std::string updated_at;             // iso_seconds
    json extra = json::object();

    static TrackState from_json(const json& j);
    json to_json() const;
};

struct LogEntry {
    std::string id, date, event;
    std::optional<std::string> track_id;
    std::string detail;

    static LogEntry make(std::string event, std::string detail, std::optional<std::string> track_id = std::nullopt);
    static LogEntry from_json(const json& j);
    json to_json() const;
};

// state.json — tolerant of missing keys, like the Mac app.
struct AppState {
    std::map<std::string, TrackState> tracks;
    std::vector<LogEntry> log;
    std::map<std::string, std::string> genre_overrides;
    std::vector<std::string> scan_folders;
    std::vector<std::string> download_priority;
    bool priority_only = true;  // "downloadMode": true = only what's picked ("track:<id>" / "playlist:<name>" / "genre:<name>")
    json extra = json::object();

    static AppState from_json(const json& j);
    json to_json() const;  // log trimmed to the last 5,000 entries
};

// One analysed file, keyed by path in _cache/analysis.json.
struct FileAnalysis {
    std::string path;
    int64_t size_bytes = 0;
    std::optional<std::string> modified;  // iso_seconds of the file's mtime
    std::optional<std::string> artist, title;
    std::optional<double> duration_sec, bpm, bpm_confidence, bpm_alternate;
    std::vector<double> bpm_candidates;
    std::optional<std::string> key, camelot;
    std::optional<double> key_strength;
    std::optional<int> key_agreement;
    std::optional<double> energy, loudness_lufs;
    std::optional<std::string> library_track_id;
    std::string engine = "wreckbox";
    json extra = json::object();

    bool bpm_unsure() const { return bpm_confidence.value_or(1) < 0.25; }
    bool key_unsure() const { return key_agreement && (*key_agreement < 2 || key_strength.value_or(1) < 0.5); }
    static FileAnalysis from_json(const json& j);
    // From the engine's analyze_file() result plus the file facts the store adds.
    static FileAnalysis from_engine(const json& engine, std::string path, int64_t size, std::string modified,
                                    std::optional<std::string> track_id);
    json to_json() const;
};

// "2026-10-06T12:34:56Z" for now / for a Windows FILETIME-based time point.
std::string iso_seconds_now();
std::string iso_seconds(int64_t unix_seconds);

// Lower-case, accent-free, words separated by single spaces (same as the original source `normalized`).
std::string normalized(const std::string& s);

// File-name-safe stem: no path separators / reserved characters, no trailing dots or spaces, ≤ 180 UTF-16 units.
std::string safe_file_name(const std::string& s);

// UTF-8 ↔ UTF-16 for Win32 calls.
std::wstring widen(const std::string& s);
std::string narrow(const std::wstring& s);

}  // namespace wb
