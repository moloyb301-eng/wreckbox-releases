// Import tests: ports of the original csv_test / app_test cases (same fixtures), plus checks for the pieces of the Spotify
// and YouTube sign-in that can run without anyone's developer keys (PKCE, the loopback redirect, response parsing).
// Live catalogue lookups only run with WRECKBOX_NET_TESTS=1.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>

#include "model/paths.h"
#include "net/http_client.h"
#include "net/oauth.h"
#include "sources/csv_import.h"
#include "sources/sources.h"
#include "sources/spotify.h"
#include "sources/sync.h"
#include "sources/youtube.h"

namespace fs = std::filesystem;
using wb::json;
static int failures = 0;

#define CHECK(cond, ...)                                                         \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ++failures;                                                          \
            std::fprintf(stderr, "FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond); \
            std::fprintf(stderr, "" __VA_ARGS__);                                \
            std::fputc('\n', stderr);                                            \
        }                                                                        \
    } while (0)

static std::string fixture(const char* name) { return std::string(WB_FIXTURES) + "/" + name; }

static void csv_parser() {
    const auto rows = wb::csv::parse_csv("a,b\n\"x, y\",\"say \"\"hi\"\"\nthere\"\r\n");
    CHECK(rows == (std::vector<std::vector<std::string>>{{"a", "b"}, {"x, y", "say \"hi\"\nthere"}}));
    CHECK(wb::csv::parse_csv("\xEF\xBB\xBFh\n1\n\n,\n").size() == 2);  // BOM skipped, empty rows dropped
}

static void csv_files() {
    const auto ex = wb::csv::parse_file(fixture("exportify_bangers.csv"), false);
    CHECK(ex.size() == 1 && ex[0].name == "exportify bangers", "%s", ex.empty() ? "" : ex[0].name.c_str());
    if (!ex.empty() && !ex[0].tracks.empty()) {
        const auto& t = ex[0].tracks[0];
        CHECK((t.artists == std::vector<std::string>{"Skrillex", "BEAM"}));
        CHECK(t.spotify_id == "0FJ6sfZPS9zNxRz8OHwnKy");
        CHECK(t.duration_ms == 190000);
        CHECK(t.added_at == "2026-01-02T10:00:00Z" && t.release_date == "2023-02-17" && t.album == "Selecta");
        CHECK((ex[0].tracks[1].artists == std::vector<std::string>{"Fred again..", "070 Shake"}));
    }
    const auto tm = wb::csv::parse_file(fixture("tunemymusic.csv"), false);
    CHECK(tm.size() == 2 && tm[0].name == "Chill" && tm[0].tracks.size() == 2 && tm[1].name == "Gym" && tm[1].tracks.size() == 1);
    if (tm.size() == 2) {
        CHECK((tm[1].tracks[0].artists == std::vector<std::string>{"MPH", "Skrillex"}));
        CHECK((tm[0].tracks[0].artists == std::vector<std::string>{"RÜFÜS DU SOL"}));
        CHECK(tm[0].tracks[1].name == "Tyler, The Creator - EARFQUAKE" && tm[0].tracks[1].artists == std::vector<std::string>{"Unknown Artist"});
    }
    // Takeout with lookups off: video ids only, no titles → nothing to import (titles come from oEmbed).
    CHECK(wb::csv::parse_file(fixture("Takeout - Liked videos.csv"), false).empty());
}

static void youtube_titles() {
    auto check = [](const char* video, const char* channel, std::vector<std::string> artists, const char* title) {
        const auto [a, t] = wb::youtube::parse_title(video, channel);
        CHECK(a == artists && t == title, "%s → [%s] %s", video, a.empty() ? "" : a[0].c_str(), t.c_str());
    };
    check("Fred again.. - Danielle (smile on my face) [Official Video]", "Fred again..", {"Fred again.."}, "Danielle (smile on my face)");
    check("Innerbloom", "RÜFÜS DU SOL - Topic", {"RÜFÜS DU SOL"}, "Innerbloom");
    check("Skrillex & BEAM - Selecta (Official Visualizer) | OWSLA", "Skrillex", {"Skrillex", "BEAM"}, "Selecta");
    check("Mirage (Lyrics)", "MPH Music", {"MPH"}, "Mirage");
    check("Kaytranada x Kali Uchis – 10%", "KAYTRANADA", {"Kaytranada", "Kali Uchis"}, "10%");
    CHECK(wb::youtube::parse_iso_duration("PT3M20S") == 200000);
    CHECK(wb::youtube::parse_iso_duration("PT1H2S") == 3602000);
    CHECK(!wb::youtube::parse_iso_duration("3:20"));
}

static wb::SourceTrack st(std::optional<std::string> sp, std::optional<std::string> yt, std::optional<std::string> isrc, const char* name,
                          std::vector<std::string> artists, std::optional<int64_t> ms = std::nullopt) {
    wb::SourceTrack t;
    t.spotify_id = std::move(sp);
    t.youtube_id = std::move(yt);
    t.isrc = std::move(isrc);
    t.name = name;
    t.artists = std::move(artists);
    t.duration_ms = ms;
    return t;
}

static void merging() {
    // original: "a song in both Spotify and YouTube playlists becomes one library entry".
    const auto lib = wb::sources::build("me", {
        wb::SourcePlaylist{"Bangers", "sp1", false, {st("abc", std::nullopt, "USRC1", "Selecta", {"Skrillex", "BEAM"}, 190000)}},
        wb::SourcePlaylist{"YT: Gym", "yt1", false, {st(std::nullopt, "v1", std::nullopt, "Selecta", {"Skrillex"}, 201000),
                                                     st(std::nullopt, "v2", std::nullopt, "Some YouTube Only Song", {"Someone"})}},
    });
    CHECK(lib.tracks.size() == 2, "%zu", lib.tracks.size());
    if (lib.tracks.size() == 2) {
        CHECK(lib.tracks[0].id == "USRC1" && lib.tracks[0].playlists == std::vector<std::string>({"Bangers", "YT: Gym"}));
        CHECK(lib.tracks[1].id == "youtube:v2");
        CHECK(lib.tracks[0].file_name == "Skrillex, BEAM - Selecta");
    }
    // Different ISRCs stay apart even with the same artist and title (radio edit vs original).
    const auto two = wb::sources::build("", {wb::SourcePlaylist{"P", {}, false, {st({}, {}, "AAA1", "Song", {"X"}), st({}, {}, "BBB2", "Song", {"X"})}}});
    CHECK(two.tracks.size() == 2);
    // Repeated playlist names get a number; local files and nameless tracks are skipped.
    auto local = st("l1", {}, {}, "Local", {"Me"});
    local.is_local = true;
    const auto dup = wb::sources::build("", {wb::SourcePlaylist{" Mix ", {}, false, {local}}, wb::SourcePlaylist{"Mix", {}, false, {}}});
    CHECK(dup.playlists.size() == 2 && dup.playlists[0].name == "Mix" && dup.playlists[1].name == "Mix (2)" && dup.tracks.empty());
}

static void legacy_library_survives() {
    // original: "importing YouTube into a library made before sources keeps the Spotify playlists".
    const fs::path root = fs::temp_directory_path() / L"wreckbox-sources-tests";
    fs::remove_all(root);
    wb::paths::init(root);
    wb::Library old = wb::sources::build("me", {wb::SourcePlaylist{"Old Favourites", "sp9", false, {st("abc", {}, "USRC1", "Selecta", {"Skrillex"}, 190000)}},
                                                 wb::SourcePlaylist{"YT: Stale", {}, false, {st({}, "v0", {}, "Gone", {"Nobody"})}}});
    wb::paths::write_atomic(wb::paths::library_file(), old.to_json().dump());
    const auto lib = wb::sources::save("youtube", "", {wb::SourcePlaylist{"YT: Test", "x", false, {st({}, "zz", {}, "New Song", {"New Artist"})}}});
    std::vector<std::string> names;
    for (const auto& p : lib.playlists) names.push_back(p.name);
    CHECK((names == std::vector<std::string>{"Old Favourites", "YT: Test"}), "%zu playlists", names.size());  // old YT playlists are replaced
    CHECK(lib.tracks.size() == 2 && lib.tracks[0].id == "USRC1", "%zu", lib.tracks.size());  // ids survive, so download state lines up
    CHECK(lib.spotify_user == "me");
    CHECK(fs::exists(root / L"_sources" / L"library-legacy.json") && fs::exists(root / L"_sources" / L"youtube.json"));
    // Re-importing CSV keeps YouTube and the legacy playlists (one source never drops another).
    const auto again = wb::sources::save("csv", "", {wb::SourcePlaylist{"From CSV", {}, false, {st({}, {}, {}, "Csv Song", {"Csv Artist"})}}});
    CHECK(again.playlists.size() == 3, "%zu", again.playlists.size());
    const auto reread = wb::Library::from_json(json::parse(*wb::paths::read_file(wb::paths::library_file())));
    CHECK(reread.tracks.size() == again.tracks.size());
    fs::remove_all(root);
}

static void pkce_and_forms() {
    // PKCE challenge = base64url(SHA-256(verifier)); SHA-256 vectors from FIPS 180-2 ("abc", ""), checked with Python.
    CHECK(wb::oauth::pkce_challenge("abc") == "ungWv48Bz-pBQUDeXa4iI7ADYaOWF3qctBD_YfIAFa0", "%s", wb::oauth::pkce_challenge("abc").c_str());
    CHECK(wb::oauth::pkce_challenge("") == "47DEQpj8HBSa-_TImW-5JCeuQeRkm5NMpJWZG3hSuFU", "%s", wb::oauth::pkce_challenge("").c_str());
    const auto v = wb::oauth::random_string(64);
    CHECK(v.size() == 64 && v.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~") == std::string::npos);
    CHECK(wb::oauth::random_string(16) != wb::oauth::random_string(16));
    const std::map<std::string, std::string> f{{"scope", "a b/c"}, {"x", "ü&="}};
    CHECK(wb::oauth::form(f) == "scope=a%20b%2Fc&x=%C3%BC%26%3D", "%s", wb::oauth::form(f).c_str());
    CHECK(wb::oauth::parse_query(wb::oauth::form(f)) == f);
    CHECK(wb::oauth::parse_query("code=a+b&state=1")["code"] == "a b");
}

static void loopback_redirect() {
    // The browser's redirect after sign-in, simulated with our own HTTP client.
    wb::oauth::Loopback lb(0);
    CHECK(lb.ok() && lb.port() > 0, "%s", lb.error().c_str());
    std::optional<std::map<std::string, std::string>> got;
    std::thread server([&] { got = lb.wait("/callback", "Connected.", std::chrono::seconds(10)); });
    const std::string base = "http://127.0.0.1:" + std::to_string(lb.port());
    const auto miss = wb::http::get(base + "/favicon.ico", {}, std::chrono::seconds(5));
    CHECK(miss.status == 404, "%d %s", miss.status, miss.error.c_str());
    const auto hit = wb::http::get(base + "/callback?code=abc%2B1&state=xyz", {}, std::chrono::seconds(5));
    server.join();
    CHECK(hit.status == 200 && hit.body.find("Connected.") != std::string::npos, "%d", hit.status);
    CHECK(got && got->at("code") == "abc+1" && got->at("state") == "xyz");
    // Spotify's redirect needs the fixed port 8888: if something holds it, the user gets a readable message.
    wb::oauth::Loopback taken(lb.port());
    CHECK(!taken.ok() && taken.error().find("in use") != std::string::npos, "%s", taken.error().c_str());
}

static void spotify_parsing() {
    const json item = json::parse(R"({"added_at":"2026-01-02T10:00:00Z","track":{"type":"track","id":"0FJ6","name":"Selecta",
        "artists":[{"name":"Skrillex"},{"name":"BEAM"}],"duration_ms":190000,"is_local":false,"external_ids":{"isrc":"USAT22300854"},
        "album":{"name":"Selecta","release_date":"2023-02-17","images":[{"url":"big","width":640},{"url":"mid","width":300},{"url":"small","width":64}]}}})");
    const auto t = wb::spotify::parse_track(item["track"], item["added_at"].get<std::string>());
    CHECK(t && t->spotify_id == "0FJ6" && t->isrc == "USAT22300854" && t->artwork_url == "mid" && t->duration_ms == 190000);
    CHECK(t && t->artists == std::vector<std::string>({"Skrillex", "BEAM"}) && t->added_at == "2026-01-02T10:00:00Z" && t->release_date == "2023-02-17");
    CHECK(!wb::spotify::parse_track(json::parse(R"({"type":"episode","name":"Podcast"})")));
    CHECK(!wb::spotify::parse_track(json()));
}

// Opt-in (WRECKBOX_NET_TESTS=1): the original "full import with Deezer + YouTube lookups" test.
static void network_import() {
    const char* on = std::getenv("WRECKBOX_NET_TESTS");
    if (!on || std::string(on) != "1") {
        std::puts("(skipping network import: WRECKBOX_NET_TESTS not set)");
        return;
    }
    // WRECKBOX_IMPORT_ROOT keeps the imported library there (to open it in the app afterwards).
    const char* keep = std::getenv("WRECKBOX_IMPORT_ROOT");
    const fs::path root = keep && *keep ? fs::path(keep) : fs::temp_directory_path() / L"wreckbox-csv-net-tests";
    if (!keep || !*keep) fs::remove_all(root);
    wb::paths::init(root);
    const auto r = wb::csv::run({fixture("exportify_bangers.csv"), fixture("Takeout - Liked videos.csv")}, [](const std::string& l) { std::printf("  %s\n", l.c_str()); });
    std::vector<wb::SourceTrack> all;
    for (const auto& pl : r.playlists) all.insert(all.end(), pl.tracks.begin(), pl.tracks.end());
    for (const auto& t : all)
        std::printf("  %s – %s | isrc %s | cover %s | %lld\n", t.artists.empty() ? "" : t.artists[0].c_str(), t.name.c_str(), t.isrc.value_or("-").c_str(),
                    t.artwork_url ? "yes" : "no", t.duration_ms.value_or(-1));
    const auto rick = std::find_if(all.begin(), all.end(), [](const auto& t) { return t.youtube_id == "dQw4w9WgXcQ"; });
    CHECK(rick != all.end() && rick->artists == std::vector<std::string>{"Rick Astley"} && rick->name == "Never Gonna Give You Up");
    // Any ISRC found must be the real one's registrant (a wrong-length version must not lend its ISRC).
    const std::map<std::string, std::string> real{{"Selecta", "USAT22300854"}, {"Danielle (smile on my face)", "GBAHS2300020"}};
    for (const auto& t : all)
        if (real.contains(t.name) && t.isrc) CHECK(t.isrc->substr(0, 5) == real.at(t.name).substr(0, 5), "%s %s", t.name.c_str(), t.isrc->c_str());
    // Lengths come only from ISRC-confirmed matches.
    for (const auto& t : all)
        if (t.duration_ms && !real.contains(t.name)) CHECK(t.isrc.has_value(), "%s: length without a confirmed match", t.name.c_str());
    CHECK(std::count_if(all.begin(), all.end(), [](const auto& t) { return t.artwork_url.has_value(); }) >= 2);
    CHECK(wb::sources::rebuild().tracks.size() == 3);
    if (!keep || !*keep) fs::remove_all(root);
}

// Playlist Sync mirrors the source: sync::playlist refetches, then sources::replace swaps the playlist in. Songs added
// at the source appear, removed ones drop out; other playlists and every track's state (downloads) are untouched.
static void playlist_sync() {
    const fs::path root = fs::temp_directory_path() / L"wreckbox-sync-tests";
    fs::remove_all(root);
    wb::paths::init(root);
    const auto a = st("spA", {}, "ISRCA", "Alpha", {"One"}), b = st("spB", {}, "ISRCB", "Beta", {"Two"}), c = st("spC", {}, "ISRCC", "Gamma", {"Three"});
    const wb::SourcePlaylist mix{"Mix", {}, false, {a, b}, "C:\\exports\\mix.csv"}, other{"Other", {}, false, {a}, "C:\\exports\\other.csv"};
    wb::sources::save("csv", "", {mix, other});
    const std::string state = R"({"tracks":{"ISRCB":{"status":"downloaded","localPath":"C:\\Music\\b.mp3"}}})";
    wb::paths::write_atomic(wb::paths::state_file(), state);

    auto find = [](const wb::Library& lib, const std::string& name) {
        const auto it = std::find_if(lib.playlists.begin(), lib.playlists.end(), [&](const auto& p) { return p.name == name; });
        return it == lib.playlists.end() ? std::optional<wb::LibraryPlaylist>{} : std::optional(*it);
    };
    const auto before = wb::Library::from_json(json::parse(*wb::paths::read_file(wb::paths::library_file())));
    const auto pl = find(before, "Mix");
    CHECK(pl && pl->track_ids == (std::vector<std::string>{"ISRCA", "ISRCB"}));
    if (!pl) return;
    const auto loc = wb::sources::locate(*pl);
    CHECK(loc && loc->kind == "csv" && loc->playlist.file == mix.file);
    CHECK(!wb::sync::unavailable(*pl), "%s", wb::sync::unavailable(*pl).value_or("").c_str());

    // At the source: Alpha removed, Gamma added.
    wb::SourcePlaylist fresh = mix;
    fresh.tracks = {b, c};
    const auto after = wb::sources::replace(loc->kind, loc->index, fresh);
    const auto now = find(after, "Mix"), kept = find(after, "Other");
    CHECK(now && now->track_ids == (std::vector<std::string>{"ISRCB", "ISRCC"}), "%zu", now ? now->track_ids.size() : 0);
    CHECK(kept && kept->track_ids == std::vector<std::string>{"ISRCA"});  // still in another playlist, so still in the library
    CHECK(after.tracks.size() == 3);
    CHECK(*wb::paths::read_file(wb::paths::state_file()) == state);  // downloads, analysis and status: never touched
    CHECK(wb::sources::load("csv")[0].file == mix.file);              // still knows its file for the next Sync

    // Why a playlist can't sync.
    wb::LibraryPlaylist ghost;
    ghost.name = "Nowhere";
    CHECK(wb::sync::unavailable(ghost).value_or("").find("isn't in any import source") != std::string::npos);
    wb::sources::save("csv", "", {fresh, other, wb::SourcePlaylist{"Old", {}, false, {c}, std::nullopt}});
    const auto old = find(wb::sources::rebuild(), "Old");
    CHECK(old && wb::sync::unavailable(*old).value_or("").find("Imported before Sync") != std::string::npos);
    wb::sources::save("spotify", "me", {wb::SourcePlaylist{"Liked Songs", "liked", false, {a}, std::nullopt}});
    const auto liked = find(wb::sources::rebuild(), "Liked Songs");
    CHECK(liked && wb::sync::unavailable(*liked).value_or("").find("Spotify client ID") != std::string::npos,  // no key in settings
          "%s", liked ? wb::sync::unavailable(*liked).value_or("(available)").c_str() : "(no playlist)");
    fs::remove_all(root);
}

int main() {
    playlist_sync();
    csv_parser();
    csv_files();
    youtube_titles();
    merging();
    legacy_library_survives();
    pkce_and_forms();
    loopback_redirect();
    spotify_parsing();
    network_import();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all sources tests passed");
    return 0;
}
