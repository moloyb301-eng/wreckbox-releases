#include "model/model.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <format>

namespace wb {
namespace {

std::optional<std::string> opt_str(const json& j, const char* k) {
    const auto it = j.find(k);
    return it != j.end() && it->is_string() ? std::optional(it->get<std::string>()) : std::nullopt;
}
std::string str(const json& j, const char* k, std::string fallback = "") { return opt_str(j, k).value_or(std::move(fallback)); }
std::optional<double> opt_num(const json& j, const char* k) {
    const auto it = j.find(k);
    return it != j.end() && it->is_number() ? std::optional(it->get<double>()) : std::nullopt;
}
std::optional<int64_t> opt_int(const json& j, const char* k) {
    const auto it = j.find(k);
    if (it == j.end() || !it->is_number()) return std::nullopt;
    return it->is_number_float() ? int64_t(it->get<double>()) : it->get<int64_t>();  // original num.toInt() truncates
}
bool boolean(const json& j, const char* k, bool fallback) {
    const auto it = j.find(k);
    return it != j.end() && it->is_boolean() ? it->get<bool>() : fallback;
}
std::vector<std::string> strings(const json& j, const char* k) {
    std::vector<std::string> out;
    if (const auto it = j.find(k); it != j.end() && it->is_array())
        for (const auto& v : *it)
            if (v.is_string()) out.push_back(v.get<std::string>());
    return out;
}
json object_or_empty(const json& j) { return j.is_object() ? j : json::object(); }

// What `extra` keeps: only the fields this build doesn't model (usually none), so a 5,000-track library isn't held
// in memory twice. to_json() writes the known fields back over it, so the saved file is the same either way.
json unknown_fields(const json& j, std::initializer_list<const char*> known) {
    json out = object_or_empty(j);
    for (const char* k : known) out.erase(k);
    return out;
}

// Writes `v` under `k`, or removes `k` when absent — mirroring the original app's `if (x != null) 'k': x`.
template <class T>
void put(json& j, const char* k, const std::optional<T>& v) {
    if (v) j[k] = *v;
    else j.erase(k);
}

}  // namespace

// MARK: LibraryTrack

std::string LibraryTrack::artist() const {
    std::string out;
    for (size_t i = 0; i < artists.size(); ++i) out += (i ? ", " : "") + artists[i];
    return out;
}

LibraryTrack LibraryTrack::from_json(const json& j) {
    LibraryTrack t;
    t.extra = unknown_fields(j, {"id", "artists", "title", "album", "year", "isrc", "spotifyIDs", "durationMs", "playlists", "firstAdded",
                                 "fileName", "status", "artworkURL"});
    t.id = str(j, "id");
    t.artists = strings(j, "artists");
    t.title = str(j, "title");
    t.album = opt_str(j, "album");
    t.year = opt_str(j, "year");
    t.isrc = opt_str(j, "isrc");
    t.spotify_ids = strings(j, "spotifyIDs");
    t.duration_ms = opt_int(j, "durationMs");
    t.playlists = strings(j, "playlists");
    t.first_added = opt_str(j, "firstAdded");
    t.file_name = str(j, "fileName");
    t.artwork_url = opt_str(j, "artworkURL");
    return t;
}

json LibraryTrack::to_json() const {
    json j = extra;
    j["id"] = id;
    j["artists"] = artists;
    j["title"] = title;
    put(j, "album", album);
    put(j, "year", year);
    put(j, "isrc", isrc);
    j["spotifyIDs"] = spotify_ids;
    put(j, "durationMs", duration_ms);
    j["playlists"] = playlists;
    put(j, "firstAdded", first_added);
    j["fileName"] = file_name;
    j["status"] = "missing";  // kept for the Mac format
    put(j, "artworkURL", artwork_url);
    return j;
}

// MARK: LibraryPlaylist

LibraryPlaylist LibraryPlaylist::from_json(const json& j) {
    LibraryPlaylist p;
    p.extra = unknown_fields(j, {"name", "spotifyID", "collaborative", "trackIDs"});
    p.name = str(j, "name");
    p.spotify_id = opt_str(j, "spotifyID");
    p.collaborative = boolean(j, "collaborative", false);
    p.track_ids = strings(j, "trackIDs");
    return p;
}

json LibraryPlaylist::to_json() const {
    json j = extra;
    j["name"] = name;
    put(j, "spotifyID", spotify_id);
    j["collaborative"] = collaborative;
    j["trackIDs"] = track_ids;
    return j;
}

// MARK: Library

Library Library::from_json(const json& j) {
    Library l;
    l.extra = unknown_fields(j, {"builtAt", "spotifyUser", "tracks", "playlists"});
    l.built_at = str(j, "builtAt", iso_seconds_now());
    l.spotify_user = str(j, "spotifyUser");
    if (const auto it = j.find("tracks"); it != j.end() && it->is_array())
        for (const auto& t : *it) l.tracks.push_back(LibraryTrack::from_json(t));
    if (const auto it = j.find("playlists"); it != j.end() && it->is_array())
        for (const auto& p : *it) l.playlists.push_back(LibraryPlaylist::from_json(p));
    return l;
}

json Library::to_json() const {
    json j = extra;
    j["builtAt"] = built_at;
    j["spotifyUser"] = spotify_user;
    json& ts = j["tracks"] = json::array();
    for (const auto& t : tracks) ts.push_back(t.to_json());
    json& ps = j["playlists"] = json::array();
    for (const auto& p : playlists) ps.push_back(p.to_json());
    return j;
}

// MARK: TrackState

const char* to_string(TrackStatus s) {
    switch (s) {
        case TrackStatus::downloaded: return "downloaded";
        case TrackStatus::ignored: return "ignored";
        default: return "missing";
    }
}

TrackStatus status_from(const std::string& s) {
    if (s == "downloaded") return TrackStatus::downloaded;
    if (s == "ignored") return TrackStatus::ignored;
    return TrackStatus::missing;
}

TrackState TrackState::from_json(const json& j) {
    TrackState s;
    s.extra = unknown_fields(j, {"status", "localPath", "source", "updatedAt"});
    s.status = status_from(str(j, "status"));
    s.local_path = opt_str(j, "localPath");
    s.source = opt_str(j, "source");
    s.updated_at = str(j, "updatedAt", iso_seconds_now());
    return s;
}

json TrackState::to_json() const {
    json j = extra;
    j["status"] = to_string(status);
    put(j, "localPath", local_path);
    put(j, "source", source);
    j["updatedAt"] = updated_at.empty() ? iso_seconds_now() : updated_at;
    return j;
}

// MARK: LogEntry

LogEntry LogEntry::make(std::string event, std::string detail, std::optional<std::string> track_id) {
    // Same id shape as the original source: microseconds since the epoch in hex, then a counter.
    static std::atomic<unsigned> counter{0};
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return LogEntry{std::format("{:012x}-{:04x}", us, counter++ & 0xffff), iso_seconds_now(), std::move(event), std::move(track_id),
                    std::move(detail)};
}

LogEntry LogEntry::from_json(const json& j) {
    LogEntry e = make(str(j, "event"), str(j, "detail"), opt_str(j, "trackID"));
    if (auto id = opt_str(j, "id")) e.id = *id;
    if (auto d = opt_str(j, "date")) e.date = *d;
    return e;
}

json LogEntry::to_json() const {
    json j = {{"id", id}, {"date", date}, {"event", event}, {"detail", detail}};
    put(j, "trackID", track_id);
    return j;
}

// MARK: AppState

AppState AppState::from_json(const json& j) {
    AppState s;
    s.extra = unknown_fields(j, {"tracks", "log", "genreOverrides", "scanFolders", "downloadPriority", "priorityOnly", "downloadMode"});
    if (const auto it = j.find("tracks"); it != j.end() && it->is_object())
        for (const auto& [k, v] : it->items()) s.tracks[k] = TrackState::from_json(v);
    if (const auto it = j.find("log"); it != j.end() && it->is_array())
        for (const auto& e : *it) s.log.push_back(LogEntry::from_json(e));
    if (const auto it = j.find("genreOverrides"); it != j.end() && it->is_object())
        for (const auto& [k, v] : it->items())
            if (v.is_string()) s.genre_overrides[k] = v.get<std::string>();
    s.scan_folders = strings(j, "scanFolders");
    s.download_priority = strings(j, "downloadPriority");
    // "Only what I pick" is the default since 0.7.1; the older priorityOnly flag is still written for older readers.
    const auto mode = j.find("downloadMode");
    s.priority_only = !(mode != j.end() && mode->is_string() && mode->get<std::string>() == "all");
    return s;
}

json AppState::to_json() const {
    json j = extra;
    json& ts = j["tracks"] = json::object();
    for (const auto& [k, v] : tracks) ts[k] = v.to_json();
    json& lg = j["log"] = json::array();
    for (size_t i = log.size() > 5000 ? log.size() - 5000 : 0; i < log.size(); ++i) lg.push_back(log[i].to_json());
    j["genreOverrides"] = genre_overrides;
    j["scanFolders"] = scan_folders;
    j["downloadPriority"] = download_priority;
    j["priorityOnly"] = priority_only;
    j["downloadMode"] = priority_only ? "picked" : "all";
    return j;
}

// MARK: FileAnalysis

FileAnalysis FileAnalysis::from_json(const json& j) {
    FileAnalysis a;
    // analyzedAt and bpmAmbiguous are recomputed on save, as in the original source.
    a.extra = unknown_fields(j, {"path", "sizeBytes", "modified", "artist", "title", "durationSec", "bpm", "bpmAmbiguous", "bpmConfidence",
                                 "bpmAlternate", "bpmCandidates", "key", "camelot", "keyStrength", "keyConfidence", "keyAgreement", "energy",
                                 "loudnessLUFS", "loudnessLufs", "libraryTrackID", "analyzedAt", "engine"});
    a.path = str(j, "path");
    a.size_bytes = opt_int(j, "sizeBytes").value_or(0);
    a.modified = opt_str(j, "modified");
    a.artist = opt_str(j, "artist");
    a.title = opt_str(j, "title");
    a.duration_sec = opt_num(j, "durationSec");
    a.bpm = opt_num(j, "bpm");
    a.bpm_confidence = opt_num(j, "bpmConfidence");
    a.bpm_alternate = opt_num(j, "bpmAlternate");
    if (const auto it = j.find("bpmCandidates"); it != j.end() && it->is_array())
        for (const auto& c : *it)
            if (c.is_number()) a.bpm_candidates.push_back(c.get<double>());
    a.key = opt_str(j, "key");
    a.camelot = opt_str(j, "camelot");
    a.key_strength = opt_num(j, "keyStrength");
    if (!a.key_strength) a.key_strength = opt_num(j, "keyConfidence");
    if (const auto k = opt_int(j, "keyAgreement")) a.key_agreement = int(*k);
    a.energy = opt_num(j, "energy");
    a.loudness_lufs = opt_num(j, "loudnessLUFS");
    if (!a.loudness_lufs) a.loudness_lufs = opt_num(j, "loudnessLufs");
    a.library_track_id = opt_str(j, "libraryTrackID");
    a.engine = str(j, "engine", "wreckbox");
    return a;
}

FileAnalysis FileAnalysis::from_engine(const json& engine, std::string path, int64_t size, std::string modified,
                                       std::optional<std::string> track_id) {
    json j = engine;
    j["path"] = std::move(path);
    j["sizeBytes"] = size;
    j["modified"] = std::move(modified);
    if (track_id) j["libraryTrackID"] = *track_id;
    return from_json(j);
}

json FileAnalysis::to_json() const {
    json j = extra;
    j["path"] = path;
    j["sizeBytes"] = size_bytes;
    put(j, "modified", modified);
    put(j, "artist", artist);
    put(j, "title", title);
    put(j, "durationSec", duration_sec);
    put(j, "bpm", bpm);
    j["bpmAmbiguous"] = bpm_unsure();
    put(j, "bpmConfidence", bpm_confidence);
    put(j, "bpmAlternate", bpm_alternate);
    j["bpmCandidates"] = bpm_candidates;
    put(j, "key", key);
    put(j, "camelot", camelot);
    put(j, "keyConfidence", key_strength);
    put(j, "keyAgreement", key_agreement);
    put(j, "energy", energy);
    put(j, "loudnessLUFS", loudness_lufs);
    put(j, "libraryTrackID", library_track_id);
    j["analyzedAt"] = iso_seconds_now();  // as the original source does on every save
    j["engine"] = engine;
    return j;
}

// MARK: Helpers

std::string iso_seconds_now() {
    return std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
}

std::string iso_seconds(int64_t unix_seconds) {
    return std::format("{:%FT%TZ}", std::chrono::sys_seconds(std::chrono::seconds(unix_seconds)));
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    std::string s(WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), int(s.size()), nullptr, nullptr);
    return s;
}

std::string normalized(const std::string& s) {
    std::wstring w = widen(s);
    if (!w.empty()) {
        std::wstring lower(w.size() * 2 + 8, L'\0');
        const int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, w.data(), int(w.size()), lower.data(), int(lower.size()),
                                    nullptr, nullptr, 0);
        lower.resize(n > 0 ? size_t(n) : 0);
        w = std::move(lower);
    }
    // Same accent table as the original source; combining marks (U+0300–U+036F) are dropped first.
    static const std::pair<std::wstring_view, wchar_t> accents[] = {
        {L"áàâäãå", L'a'}, {L"éèêë", L'e'}, {L"íìîï", L'i'}, {L"óòôöõø", L'o'}, {L"úùûü", L'u'}, {L"ñ", L'n'}, {L"ç", L'c'}, {L"ýÿ", L'y'}};
    std::string out;
    bool in_word = false;
    for (wchar_t c : w) {
        if (c >= 0x300 && c <= 0x36f) continue;
        for (const auto& [set, plain] : accents)
            if (set.find(c) != std::wstring_view::npos) c = plain;
        const bool word = (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9');
        if (word) {
            if (!in_word && !out.empty()) out += ' ';
            out += char(c);
        }
        in_word = word;
    }
    return out;
}

std::string safe_file_name(const std::string& s) {
    // the original app's String.trim() whitespace set.
    auto ws = [](wchar_t c) {
        return (c >= 0x09 && c <= 0x0d) || c == 0x20 || c == 0x85 || c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) || c == 0x2028 ||
               c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000 || c == 0xfeff;
    };
    std::wstring w = widen(s);
    for (auto& c : w)
        if (c < 0x20 || std::wstring_view(L"/\\:*?\"<>|").find(c) != std::wstring_view::npos) c = L'_';
    size_t a = 0, b = w.size();
    while (a < b && ws(w[a])) ++a;
    while (b > a && ws(w[b - 1])) --b;
    w = w.substr(a, b - a);
    if (w.size() > 180) w.resize(IS_HIGH_SURROGATE(w[179]) ? 179 : 180);  // never split a surrogate pair
    while (!w.empty() && (w.back() == L'.' || w.back() == L' ')) w.pop_back();
    return narrow(w);
}

}  // namespace wb
