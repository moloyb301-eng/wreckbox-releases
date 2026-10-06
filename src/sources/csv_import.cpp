#include "sources/csv_import.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <regex>
#include <set>
#include <thread>

#include "model/paths.h"
#include "net/http_client.h"
#include "sources/youtube.h"

namespace fs = std::filesystem;

namespace wb::csv {
namespace {

struct Offline {};  // network down / service busy: keep what we have, don't cache, try again next import

std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string upper(std::string s) {
    for (auto& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string header_key(const std::string& h) {
    std::string out;
    for (const char c : lower(h))
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out += c;
    return out;
}

// Index of the first header matching any of the names (compared without spaces / punctuation).
std::optional<size_t> col(const std::vector<std::string>& header, std::initializer_list<const char*> names) {
    std::vector<std::string> keys;
    for (const auto& h : header) keys.push_back(header_key(h));
    for (const char* n : names)
        if (const auto it = std::find(keys.begin(), keys.end(), header_key(n)); it != keys.end()) return size_t(it - keys.begin());
    return std::nullopt;
}

std::optional<int64_t> duration_ms(std::string v) {
    v = trim(v);
    if (v.empty()) return std::nullopt;
    if (v.find(':') != std::string::npos) {
        int64_t s = 0;
        size_t start = 0;
        for (;;) {
            const size_t c = v.find(':', start);
            const std::string part = trim(v.substr(start, c == std::string::npos ? std::string::npos : c - start));
            int x = 0;
            try {
                x = std::stoi(part);
            } catch (...) {
            }
            s = s * 60 + x;
            if (c == std::string::npos) break;
            start = c + 1;
        }
        return s * 1000;
    }
    try {
        size_t used = 0;
        const double n = std::stod(v, &used);
        if (used != v.size()) return std::nullopt;
        return n > 20000 ? std::llround(n) : std::llround(n * 1000);  // ms or seconds
    } catch (...) {
        return std::nullopt;
    }
}

// Exportify / TuneMyMusic separate artists with commas or semicolons. ("Tyler, The Creator" is kept whole.)
std::vector<std::string> split_artists(std::string v) {
    const std::string tyler = "Tyler, The Creator";
    for (size_t at; (at = v.find(tyler)) != std::string::npos;) v.replace(at, tyler.size(), std::string("Tyler\x01 The Creator"));
    static const std::regex sep(R"(\s*[;|,]\s*)");
    std::vector<std::string> out;
    for (std::sregex_token_iterator it(v.begin(), v.end(), sep, -1), end; it != end; ++it) {
        std::string a = it->str();
        std::replace(a.begin(), a.end(), '\x01', ',');
        a = trim(a);
        if (!a.empty()) out.push_back(a);
    }
    return out;
}

std::optional<std::pair<std::string, std::string>> oembed(const std::string& video_id) {
    const auto r = http::get("https://www.youtube.com/oembed?format=json&url=" + http::url_encode("https://www.youtube.com/watch?v=" + video_id), {},
                             std::chrono::seconds(10));
    if (r.status != 200) return std::nullopt;  // private / deleted video, or offline
    const json j = json::parse(r.body, nullptr, false);
    if (!j.is_object()) return std::nullopt;
    return std::pair{j.value("title", ""), j.value("author_name", "")};
}

// MARK: Catalogues

const http::Headers kUserAgent{{"User-Agent", "WreckBox/0.1 (https://github.com/moloyb301-eng/wreckbox-releases)"}};

std::string clean(const std::string& t) {
    static const std::regex feat(R"(\s*[\(\[][^\)\]]*(feat|ft|with)\.?[^\)\]]*[\)\]])", std::regex::icase);
    return normalized(std::regex_replace(t, feat, ""));
}

std::set<std::string> words(const std::string& s) {
    std::set<std::string> out;
    size_t start = 0;
    while (start < s.size()) {
        const size_t sp = std::min(s.find(' ', start), s.size());
        if (sp > start) out.insert(s.substr(start, sp - start));
        start = sp + 1;
    }
    return out;
}

// MusicBrainz allows one request per second per client.
json musicbrainz(const SourceTrack& t) {
    static std::mutex m;
    static auto last = std::chrono::steady_clock::time_point{};
    {
        std::lock_guard lock(m);
        const auto wait = std::chrono::milliseconds(1100) - (std::chrono::steady_clock::now() - last);
        if (wait.count() > 0) std::this_thread::sleep_for(wait);
        last = std::chrono::steady_clock::now();
    }
    auto esc = [](std::string s) {
        for (size_t at = 0; (at = s.find('"', at)) != std::string::npos; at += 2) s.insert(at, "\\");
        return s;
    };
    static const std::regex feat(R"(\s*[\(\[][^\)\]]*(feat|ft|with)\.?[^\)\]]*[\)\]])", std::regex::icase);
    const std::string title = std::regex_replace(t.name, feat, "");
    const std::string q = "recording:\"" + esc(title) + "\" AND artist:\"" + esc(t.artists.front()) + "\"";
    const auto r = http::get("https://musicbrainz.org/ws/2/recording?query=" + http::url_encode(q) + "&fmt=json&limit=8&inc=isrcs", kUserAgent,
                             std::chrono::seconds(15));
    if (r.status == 0 || r.status == 503) throw Offline{};  // offline or rate limited: retry on a later import
    if (r.status != 200) return nullptr;
    const json body = json::parse(r.body, nullptr, false);
    if (!body.is_object() || !body.contains("recordings")) return nullptr;
    static const char* variants[] = {"live", "remix", "edit", "acoustic", "instrumental", "karaoke", "video", "cover", "sped", "slowed", "remaster"};
    const std::string want = clean(t.name);
    const auto want_words = words(want);
    json best = nullptr;
    double best_score = -1;
    for (const auto& rec : body["recordings"]) {
        const std::string got = clean(rec.value("title", ""));
        const double score0 = rec.contains("score") && rec["score"].is_number() ? rec["score"].get<double>() : 0;
        if (score0 < 85 || !(got == want || got.rfind(want, 0) == 0 || want.rfind(got, 0) == 0)) continue;
        const auto got_words = words(got);
        if (std::any_of(std::begin(variants), std::end(variants), [&](const char* v) { return got_words.contains(v) && !want_words.contains(v); }))
            continue;
        std::vector<std::string> isrcs;
        if (rec.contains("isrcs") && rec["isrcs"].is_array())
            for (const auto& i : rec["isrcs"])
                if (i.is_string()) isrcs.push_back(i.get<std::string>());
        const std::optional<int64_t> len = rec.contains("length") && rec["length"].is_number() ? std::optional(rec["length"].get<int64_t>()) : std::nullopt;
        double score = score0 + (isrcs.empty() ? 0 : 50) + (got == want ? 10 : 0);
        if (len && t.duration_ms) score -= std::clamp(double(std::llabs(*len - *t.duration_ms)) / 1000.0, 0.0, 60.0);
        if (score > best_score) {
            best_score = score;
            const json release = rec.contains("releases") && rec["releases"].is_array() && !rec["releases"].empty() ? rec["releases"][0] : json::object();
            // Only trust the ISRC when the length agrees (otherwise it may be another release of the song). A length is
            // only trusted from an ISRC-confirmed match: a wrong length would make Soulseek reject the right file.
            const bool length_ok = !len || !t.duration_ms || std::llabs(*len - *t.duration_ms) <= 5000;
            const json isrc = length_ok && !isrcs.empty() ? json(isrcs.front()) : json(nullptr);
            best = {{"isrc", isrc},
                    {"durationMs", !isrc.is_null() && len ? json(*len) : json(nullptr)},
                    {"album", release.value("title", json(nullptr))},
                    {"releaseId", release.value("id", json(nullptr))},
                    {"date", release.value("date", json(nullptr))}};
        }
    }
    return best;
}

json deezer_by_isrc(const std::string& isrc) {
    const auto r = http::get("https://api.deezer.com/track/isrc:" + http::url_encode(isrc), {}, std::chrono::seconds(10));
    if (r.status == 0) throw Offline{};
    const json d = json::parse(r.body, nullptr, false);
    if (!d.is_object() || d.contains("error")) return nullptr;
    const json album = d.value("album", json::object());
    return {{"cover", album.value("cover_big", json(nullptr))},
            {"album", album.value("title", json(nullptr))},
            {"durationMs", d.contains("duration") && d["duration"].is_number() ? json(std::llround(d["duration"].get<double>()) * 1000) : json(nullptr)},
            {"date", d.value("release_date", json(nullptr))}};
}

std::optional<std::string> str_of(const json& j, const char* k) {
    return j.is_object() && j.contains(k) && j[k].is_string() ? std::optional(j[k].get<std::string>()) : std::nullopt;
}

fs::path cache_file() { return paths::cache() / L"catalogue_lookup.json"; }

}  // namespace

std::vector<std::vector<std::string>> parse_csv(const std::string& input) {
    const std::string text = input.rfind("\xEF\xBB\xBF", 0) == 0 ? input.substr(3) : input;
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool quoted = false;
    auto end_row = [&] {
        row.push_back(std::move(field));
        field.clear();
        if (std::any_of(row.begin(), row.end(), [](const std::string& f) { return !f.empty(); })) rows.push_back(std::move(row));
        row.clear();
    };
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') field += '"', ++i;
                else quoted = false;
            } else field += c;
        } else if (c == '"') quoted = true;
        else if (c == ',') row.push_back(std::move(field)), field.clear();
        else if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            end_row();
        } else field += c;
    }
    end_row();
    return rows;
}

std::vector<SourcePlaylist> parse_file(const std::string& utf8_path, bool lookup_youtube, const Log& log) {
    const fs::path path(std::u8string(utf8_path.begin(), utf8_path.end()));
    const auto text = paths::read_file(path);
    if (!text) throw std::runtime_error("can't read " + utf8_path);
    const auto rows = parse_csv(*text);
    if (rows.size() < 2) return {};
    const auto& h = rows.front();
    const auto c_title = col(h, {"Track Name", "Track name", "Title", "Song", "Name", "Song Name"});
    const auto c_artist = col(h, {"Artist Name(s)", "Artist name", "Artist", "Artists", "Artist Names"});
    const auto c_album = col(h, {"Album Name", "Album", "Album title"});
    const auto c_isrc = col(h, {"ISRC"});
    const auto c_dur = col(h, {"Duration (ms)", "Duration", "Length", "Time"});
    const auto c_date = col(h, {"Release Date", "Album Release Date", "Year"});
    const auto c_added = col(h, {"Added At", "Date Added", "Playlist Video Creation Timestamp", "Added"});
    const auto c_uri = col(h, {"Track URI", "Spotify - id", "Spotify ID", "Spotify URI"});
    const auto c_playlist = col(h, {"Playlist name", "Playlist"});
    const auto c_video = col(h, {"Video ID", "YouTube ID", "Video Id", "Youtube - id"});
    const auto c_image = col(h, {"Album Image URL", "Image URL", "Artwork"});

    static const std::regex seps(R"([_-]+)");
    const std::string file_name = trim(std::regex_replace(narrow(path.stem().wstring()), seps, " "));
    std::vector<std::string> order;
    std::map<std::string, std::vector<SourceTrack>> by_playlist;
    static const std::regex spotify_id(R"(^[A-Za-z0-9]{22}$)");
    size_t looked_up = 0;
    for (size_t ri = 1; ri < rows.size(); ++ri) {
        const auto& r = rows[ri];
        auto get = [&](const std::optional<size_t>& c) -> std::optional<std::string> {
            if (!c || *c >= r.size()) return std::nullopt;
            const std::string v = trim(r[*c]);
            return v.empty() ? std::nullopt : std::optional(v);
        };
        auto title = get(c_title);
        auto artists = get(c_artist) ? split_artists(*get(c_artist)) : std::vector<std::string>{};
        const auto video = get(c_video);
        if ((!title || artists.empty()) && video && lookup_youtube) {
            if (const auto o = oembed(*video)) {
                auto [a, t] = youtube::parse_title(o->first, o->second);
                if (!title) title = t;
                if (artists.empty()) artists = a;
            }
            if (++looked_up % 25 == 0 && log) log("  looked up " + std::to_string(looked_up) + " YouTube videos…");
        }
        if (!title || title->empty()) continue;
        std::optional<std::string> sp;
        if (const auto uri = get(c_uri)) {
            if (uri->rfind("spotify:track:", 0) == 0) sp = uri->substr(14);
            else if (std::regex_match(*uri, spotify_id)) sp = uri;
        }
        const std::string playlist = get(c_playlist).value_or(file_name);
        if (!by_playlist.contains(playlist)) order.push_back(playlist);
        SourceTrack t;
        t.name = *title;
        t.artists = artists.empty() ? std::vector<std::string>{"Unknown Artist"} : artists;
        t.album = get(c_album);
        if (const auto i = get(c_isrc)) t.isrc = upper(*i);
        if (const auto d = get(c_dur)) t.duration_ms = duration_ms(*d);
        t.release_date = get(c_date);
        t.added_at = get(c_added);
        t.spotify_id = sp;
        t.youtube_id = video;
        t.artwork_url = get(c_image);
        by_playlist[playlist].push_back(std::move(t));
    }
    std::vector<SourcePlaylist> out;
    for (const auto& name : order) out.push_back(SourcePlaylist{name, std::nullopt, false, std::move(by_playlist[name])});
    return out;
}

SourceTrack enrich(const SourceTrack& t, json& cache) {
    if (t.isrc && t.artwork_url && t.duration_ms) return t;
    const std::string key = normalized(t.artists.front() + " " + t.name);
    json hit;
    if (cache.contains(key)) {
        hit = cache[key];
    } else {
        try {
            hit = musicbrainz(t);
            const auto isrc = t.isrc ? t.isrc : str_of(hit, "isrc");
            if (isrc) {
                const json dz = deezer_by_isrc(*isrc);
                if (!hit.is_object()) hit = json::object();
                hit["isrc"] = *isrc;
                // Deezer's data for this exact ISRC wins for length and cover; MusicBrainz fills the gaps.
                if (dz.is_object())
                    for (const auto& [k, v] : dz.items())
                        if (!v.is_null() && (!hit.contains(k) || hit[k].is_null() || k == "cover" || k == "durationMs")) hit[k] = v;
            }
            if (hit.is_object() && (!hit.contains("cover") || hit["cover"].is_null()) && str_of(hit, "releaseId"))
                hit["cover"] = "https://coverartarchive.org/release/" + *str_of(hit, "releaseId") + "/front-500";
            cache[key] = hit;  // null = not found (cached so it isn't asked again)
        } catch (const Offline&) {
            return t;
        }
    }
    if (!hit.is_object()) return t;
    SourceTrack e = t;
    if (!e.album) e.album = str_of(hit, "album");
    if (!e.release_date) e.release_date = str_of(hit, "date");
    if (!e.isrc)
        if (const auto i = str_of(hit, "isrc")) e.isrc = upper(*i);
    if (!e.duration_ms && hit.contains("durationMs") && hit["durationMs"].is_number()) e.duration_ms = hit["durationMs"].get<int64_t>();
    if (!e.artwork_url) e.artwork_url = str_of(hit, "cover");
    return e;
}

namespace {

// Enriches every track of `parsed` through the catalogue cache, reporting progress; counts what was newly matched.
std::vector<SourcePlaylist> enrich_all(const std::vector<SourcePlaylist>& parsed, const Log& log, size_t& tracks, size_t& enriched) {
    json cache = json::object();
    if (const auto text = paths::read_file(cache_file())) {
        const json j = json::parse(*text, nullptr, false);
        if (j.is_object()) cache = j;
    }
    tracks = enriched = 0;
    for (const auto& pl : parsed) tracks += pl.tracks.size();
    std::vector<SourcePlaylist> out;
    size_t n = 0;
    for (const auto& pl : parsed) {
        SourcePlaylist e{pl.name, pl.id, false, {}, pl.file};
        for (const auto& t : pl.tracks) {
            SourceTrack et = enrich(t, cache);
            if (et.isrc && !t.isrc) ++enriched;
            e.tracks.push_back(std::move(et));
            if (++n % 20 == 0) {
                log("Looking up ISRC and covers: " + std::to_string(n) + "/" + std::to_string(tracks) + "…");
                paths::write_atomic(cache_file(), cache.dump());
            }
        }
        out.push_back(std::move(e));
    }
    paths::write_atomic(cache_file(), cache.dump());
    return out;
}

std::string file_name_of(const std::string& utf8_path) { return narrow(fs::path(std::u8string(utf8_path.begin(), utf8_path.end())).filename().wstring()); }

}  // namespace

SourcePlaylist reread(const std::string& utf8_path, const std::string& name, const Log& log) {
    std::error_code ec;
    if (!fs::exists(fs::path(std::u8string(utf8_path.begin(), utf8_path.end())), ec))
        throw std::runtime_error("The CSV file this playlist came from is gone (" + utf8_path + "). Import it again from Settings.");
    log("Reading " + file_name_of(utf8_path) + "…");
    auto pls = parse_file(utf8_path, true, log);
    const auto it = std::find_if(pls.begin(), pls.end(), [&](const SourcePlaylist& p) { return p.name == name; });
    if (it == pls.end())
        throw std::runtime_error(file_name_of(utf8_path) + " no longer has a playlist called \"" + name + "\". Import it again from Settings.");
    it->file = utf8_path;
    size_t tracks = 0, enriched = 0;
    return enrich_all({*it}, log, tracks, enriched).front();
}

Result run(const std::vector<std::string>& utf8_paths, const Log& log) {
    std::vector<SourcePlaylist> parsed;
    for (const auto& p : utf8_paths) {
        log("Reading " + file_name_of(p) + "…");
        for (auto& pl : parse_file(p, true, log)) {
            log("  " + pl.name + ": " + std::to_string(pl.tracks.size()) + " tracks");
            pl.file = p;  // so Sync can re-read it later
            parsed.push_back(std::move(pl));
        }
    }
    Result res;
    res.playlists = enrich_all(parsed, log, res.tracks, res.enriched);

    // Merge with earlier CSV imports: a re-imported playlist replaces the old version of itself.
    std::set<std::string> names;
    for (const auto& pl : res.playlists) names.insert(pl.name);
    std::vector<SourcePlaylist> merged;
    for (auto& pl : sources::load("csv"))
        if (!names.contains(pl.name)) merged.push_back(std::move(pl));
    merged.insert(merged.end(), res.playlists.begin(), res.playlists.end());
    sources::save("csv", "", merged);
    log("Imported " + std::to_string(res.playlists.size()) + " playlists, " + std::to_string(res.tracks) + " tracks (" +
        std::to_string(res.enriched) + " matched to the catalogue).");
    return res;
}

}  // namespace wb::csv
