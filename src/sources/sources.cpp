#include "sources/sources.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>

#include "model/paths.h"

namespace fs = std::filesystem;

namespace wb {
namespace {

std::optional<std::string> opt_str(const json& j, const char* k) {
    const auto it = j.find(k);
    return it != j.end() && it->is_string() ? std::optional(it->get<std::string>()) : std::nullopt;
}

template <class T>
void put(json& j, const char* k, const std::optional<T>& v) {
    if (v) j[k] = *v;
}

std::string upper(std::string s) {
    for (auto& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

fs::path dir() { return paths::root() / L"_sources"; }

json read_json(const fs::path& p) {
    const auto text = paths::read_file(p);
    return text ? json::parse(*text, nullptr, false) : json();
}

std::vector<SourcePlaylist> playlists_of(const json& j) {
    std::vector<SourcePlaylist> out;
    if (j.is_object() && j.contains("playlists") && j["playlists"].is_array())
        for (const auto& pl : j["playlists"]) out.push_back(SourcePlaylist::from_json(pl));
    return out;
}

// A library.json made before sources existed (the Mac app, or one copied from the computer) has no
// _sources/spotify.json. Keep it as a source so importing YouTube or CSV doesn't drop those playlists.
void preserve_legacy(const std::string& kind) {
    std::error_code ec;
    if (kind == "spotify" || fs::exists(dir() / L"spotify.json", ec)) return;
    const fs::path legacy = dir() / L"library-legacy.json";
    if (fs::exists(legacy, ec) || !fs::exists(paths::library_file(), ec)) return;
    const json lj = read_json(paths::library_file());
    if (!lj.is_object()) return;
    const Library lib = Library::from_json(lj);
    std::map<std::string, const LibraryTrack*> by_id;
    for (const auto& t : lib.tracks) by_id[t.id] = &t;
    json pls = json::array();
    for (const auto& pl : lib.playlists) {
        if (pl.name.rfind("YT: ", 0) == 0) continue;
        SourcePlaylist sp{pl.name, pl.spotify_id, pl.collaborative, {}};
        for (const auto& id : pl.track_ids) {
            const auto it = by_id.find(id);
            if (it == by_id.end()) continue;
            const LibraryTrack& t = *it->second;
            SourceTrack st;
            if (!t.spotify_ids.empty()) st.spotify_id = t.spotify_ids.front();
            st.name = t.title;
            st.artists = t.artists;
            st.album = t.album;
            st.release_date = t.year;
            st.isrc = t.isrc;
            st.duration_ms = t.duration_ms;
            st.added_at = t.first_added;
            st.artwork_url = t.artwork_url;
            sp.tracks.push_back(std::move(st));
        }
        pls.push_back(sp.to_json());
    }
    paths::write_atomic(legacy, json{{"kind", "spotify-legacy"}, {"user", lib.spotify_user}, {"playlists", pls}}.dump());
}

// Spotify first (its titles and ids win when a song is in several sources), then the legacy library, then the rest.
std::string order(const fs::path& f) {
    const auto n = f.filename().wstring();
    if (n == L"spotify.json") return "0";
    if (n == L"library-legacy.json") return "1";
    return "2" + narrow(n);
}

}  // namespace

SourceTrack SourceTrack::from_json(const json& j) {
    SourceTrack t;
    if (!j.is_object()) return t;
    t.spotify_id = opt_str(j, "spotifyID");
    t.youtube_id = opt_str(j, "youtubeID");
    t.name = opt_str(j, "name").value_or("");
    if (j.contains("artists") && j["artists"].is_array())
        for (const auto& a : j["artists"])
            if (a.is_string()) t.artists.push_back(a.get<std::string>());
    t.album = opt_str(j, "album");
    t.release_date = opt_str(j, "releaseDate");
    t.isrc = opt_str(j, "isrc");
    if (j.contains("durationMs") && j["durationMs"].is_number()) t.duration_ms = int64_t(j["durationMs"].get<double>());
    t.is_local = j.value("isLocal", false);
    t.added_at = opt_str(j, "addedAt");
    t.artwork_url = opt_str(j, "artworkURL");
    return t;
}

json SourceTrack::to_json() const {
    json j = json::object();
    put(j, "spotifyID", spotify_id);
    put(j, "youtubeID", youtube_id);
    j["name"] = name;
    j["artists"] = artists;
    put(j, "album", album);
    put(j, "releaseDate", release_date);
    put(j, "isrc", isrc);
    put(j, "durationMs", duration_ms);
    if (is_local) j["isLocal"] = true;
    put(j, "addedAt", added_at);
    put(j, "artworkURL", artwork_url);
    return j;
}

SourcePlaylist SourcePlaylist::from_json(const json& j) {
    SourcePlaylist p;
    if (!j.is_object()) return p;
    p.name = opt_str(j, "name").value_or("");
    p.id = opt_str(j, "id");
    p.collaborative = j.value("collaborative", false);
    if (j.contains("tracks") && j["tracks"].is_array())
        for (const auto& t : j["tracks"]) p.tracks.push_back(SourceTrack::from_json(t));
    p.file = opt_str(j, "file");
    return p;
}

json SourcePlaylist::to_json() const {
    json j = {{"name", name}, {"collaborative", collaborative}};
    put(j, "id", id);
    put(j, "file", file);
    json& ts = j["tracks"] = json::array();
    for (const auto& t : tracks) ts.push_back(t.to_json());
    return j;
}

namespace sources {

std::vector<SourcePlaylist> load(const std::string& kind) { return playlists_of(read_json(dir() / widen(kind + ".json"))); }

Library save(const std::string& kind, const std::string& user, const std::vector<SourcePlaylist>& playlists) {
    preserve_legacy(kind);
    json pls = json::array();
    for (const auto& pl : playlists) pls.push_back(pl.to_json());
    paths::write_atomic(dir() / widen(kind + ".json"),
                        json{{"kind", kind}, {"user", user}, {"savedAt", iso_seconds_now()}, {"playlists", pls}}.dump(1));
    return rebuild();
}

std::optional<Located> locate(const LibraryPlaylist& pl) {
    std::error_code ec;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir(), ec))
        if (e.is_regular_file() && e.path().extension() == L".json") files.push_back(e.path());
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return order(a) < order(b); });
    std::optional<Located> by_name;
    for (const auto& f : files) {
        const json j = read_json(f);
        if (!j.is_object()) continue;
        const std::string kind = j.value("kind", narrow(f.stem().wstring()));
        const auto pls = playlists_of(j);
        for (size_t i = 0; i < pls.size(); ++i) {
            // An id is unique across sources; a name might repeat ("Mix" and "Mix (2)" after a rebuild), so the first
            // name match only counts if no id match turns up.
            if (pl.spotify_id && pls[i].id == pl.spotify_id) return Located{kind, i, pls[i]};
            std::string n = pls[i].name;
            n.erase(0, n.find_first_not_of(" \t\r\n"));
            n.erase(n.find_last_not_of(" \t\r\n") + 1);
            if (!by_name && n == pl.name) by_name = Located{kind, i, pls[i]};
        }
    }
    return by_name;
}

Library replace(const std::string& kind, size_t index, const SourcePlaylist& pl) {
    // The legacy file is named library-legacy.json but its kind is "spotify-legacy"; every other kind is its file name.
    const fs::path file = dir() / widen(kind == "spotify-legacy" ? "library-legacy.json" : kind + ".json");
    json j = read_json(file);
    if (!j.is_object() || !j.contains("playlists") || !j["playlists"].is_array() || index >= j["playlists"].size())
        throw std::runtime_error("The playlist's source file changed — import it again from Settings.");
    j["playlists"][index] = pl.to_json();
    j["savedAt"] = iso_seconds_now();
    paths::write_atomic(file, j.dump(1));
    return rebuild();
}

Library rebuild() {
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir(), ec))
        if (e.is_regular_file() && e.path().extension() == L".json") files.push_back(e.path());
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return order(a) < order(b); });
    std::string user;
    std::vector<SourcePlaylist> all;
    for (const auto& f : files) {
        const json j = read_json(f);
        if (!j.is_object()) continue;
        if (j.value("kind", "").rfind("spotify", 0) == 0) user = j.value("user", user);
        for (auto& pl : playlists_of(j)) all.push_back(std::move(pl));
    }
    Library lib = build(user, all);
    paths::write_atomic(paths::library_file(), lib.to_json().dump(2));
    return lib;
}

Library build(const std::string& user, const std::vector<SourcePlaylist>& sources) {
    Library lib;
    lib.built_at = iso_seconds_now();
    lib.spotify_user = user;
    std::map<std::string, size_t> index;
    std::map<std::string, int> seen_names;
    for (const auto& src : sources) {
        // Trimmed name; a repeated name becomes "Name (2)".
        std::string name = src.name;
        name.erase(0, name.find_first_not_of(" \t\r\n"));
        name.erase(name.find_last_not_of(" \t\r\n") + 1);
        if (name.empty()) name = "Untitled";
        if (++seen_names[name] > 1) name += " (" + std::to_string(seen_names[name]) + ")";
        LibraryPlaylist pl;
        pl.name = name;
        pl.spotify_id = src.id;
        pl.collaborative = src.collaborative;
        for (const auto& t : src.tracks) {
            if (t.is_local || t.name.empty()) continue;
            const std::string first = t.artists.empty() ? "" : t.artists.front();
            // Loose key without duration so a YouTube upload (different length) still merges with the Spotify track.
            const std::string loose = "nt:" + normalized(first) + "|" + normalized(t.name);
            const std::string fuzzy = "na:" + normalized(first) + "|" + normalized(t.name) + "|" + std::to_string(t.duration_ms.value_or(0) / 5000);
            std::vector<std::string> strong_keys, weak_keys{fuzzy, loose};
            if (t.isrc) strong_keys.push_back("isrc:" + upper(*t.isrc));
            if (t.spotify_id) strong_keys.push_back("sp:" + *t.spotify_id);
            if (t.youtube_id) strong_keys.push_back("yt:" + *t.youtube_id);
            // Strong keys (ISRC, Spotify / YouTube id) merge anything. Artist + title keys only merge when one side
            // has no ISRC (e.g. a YouTube upload) — two recordings with different ISRCs (radio edit vs original) stay apart.
            std::optional<size_t> hit;
            for (const auto& k : strong_keys)
                if (const auto it = index.find(k); it != index.end()) {
                    hit = it->second;
                    break;
                }
            if (!hit)
                for (const auto& k : weak_keys)
                    if (const auto it = index.find(k); it != index.end() && (!t.isrc || !lib.tracks[it->second].isrc)) {
                        hit = it->second;
                        break;
                    }
            std::vector<std::string> keys = strong_keys;
            keys.insert(keys.end(), weak_keys.begin(), weak_keys.end());
            if (hit) {
                LibraryTrack& old = lib.tracks[*hit];
                if (!old.album) old.album = t.album;
                if (!old.isrc && t.isrc) old.isrc = upper(*t.isrc);
                if (t.spotify_id && std::find(old.spotify_ids.begin(), old.spotify_ids.end(), *t.spotify_id) == old.spotify_ids.end())
                    old.spotify_ids.push_back(*t.spotify_id);
                if (!old.duration_ms) old.duration_ms = t.duration_ms;
                if (std::find(old.playlists.begin(), old.playlists.end(), name) == old.playlists.end()) old.playlists.push_back(name);
                if (t.added_at && (!old.first_added || *t.added_at < *old.first_added)) old.first_added = t.added_at;
                if (!old.artwork_url) old.artwork_url = t.artwork_url;
                for (const auto& k : keys) index.emplace(k, *hit);
                pl.track_ids.push_back(old.id);
                continue;
            }
            LibraryTrack nt;
            nt.id = t.isrc ? upper(*t.isrc) : t.spotify_id ? "spotify:" + *t.spotify_id : t.youtube_id ? "youtube:" + *t.youtube_id : fuzzy;
            nt.artists = t.artists;
            nt.title = t.name;
            nt.album = t.album;
            if (t.release_date && t.release_date->size() >= 4) nt.year = t.release_date->substr(0, 4);
            if (t.isrc) nt.isrc = upper(*t.isrc);
            if (t.spotify_id) nt.spotify_ids = {*t.spotify_id};
            nt.duration_ms = t.duration_ms;
            nt.playlists = {name};
            nt.first_added = t.added_at;
            std::string joined;
            for (size_t i = 0; i < t.artists.size(); ++i) joined += (i ? ", " : "") + t.artists[i];
            nt.file_name = safe_file_name(joined + " - " + t.name);
            nt.artwork_url = t.artwork_url;
            lib.tracks.push_back(std::move(nt));
            for (const auto& k : keys) index.emplace(k, lib.tracks.size() - 1);
            pl.track_ids.push_back(lib.tracks.back().id);
        }
        lib.playlists.push_back(std::move(pl));
    }
    return lib;
}

}  // namespace sources
}  // namespace wb
