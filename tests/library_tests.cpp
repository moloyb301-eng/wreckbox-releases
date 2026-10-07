// Library tests: the matcher (cases mirror the Dart behaviour) and the store's organise + rescan flow on a throwaway
// library folder. No network: test tracks have no artwork URL and no ISRC lookup is needed for the checks.
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include "engine/engine.h"
#include "library/downloads_watcher.h"
#include "library/matcher.h"
#include "library/store.h"
#include "model/paths.h"
#include "model/settings.h"
#include "net/http_client.h"

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

static wb::LibraryTrack make_track(std::string id, std::vector<std::string> artists, std::string title, std::optional<int64_t> ms,
                                   std::optional<std::string> isrc = std::nullopt) {
    wb::LibraryTrack t;
    t.id = std::move(id);
    t.artists = std::move(artists);
    t.title = std::move(title);
    t.duration_ms = ms;
    t.isrc = std::move(isrc);
    t.file_name = wb::safe_file_name(t.artist() + " - " + t.title);
    return t;
}

static void clean_titles() {
    using M = wb::TrackMatcher;
    CHECK(M::clean_title("Selecta (feat. BEAM)") == "selecta", "%s", M::clean_title("Selecta (feat. BEAM)").c_str());
    CHECK(M::clean_title("Song ft. Someone") == "song");
    CHECK(M::clean_title("Track (Original Mix)") == "track");
    CHECK(M::clean_title("Track - Original Mix") == "track");
    CHECK(M::clean_title("Hit - Remastered 2011") == "hit");
    CHECK(M::clean_title("Tune (Official Video)") == "tune");
    CHECK(M::clean_title("Tune [Free Download]") == "tune");
    CHECK(M::clean_title("Tune (Extended Mix)") == "tune extended mix");  // remix / edit names are kept
}

static void matching() {
    const std::vector<wb::LibraryTrack> tracks = {
        make_track("USRC1", {"Skrillex", "BEAM"}, "Selecta", 190000, "USRC1"),
        make_track("spotify:2", {"Fred again.."}, "Danielle (smile on my face)", 240000),
        make_track("spotify:3", {"RÜFÜS DU SOL"}, "Innerbloom", 577000),
    };
    const wb::TrackMatcher m(tracks);
    CHECK(m.match({"C:\\x\\whatever.mp3", std::nullopt, "usrc1"}) == 0u);  // ISRC, case-insensitive
    CHECK(m.match({"C:\\x\\Fred again.. - Danielle (smile on my face).flac"}) == 1u);
    CHECK(m.match({"C:\\x\\Danielle (smile on my face) - Fred again...mp3"}) == 1u);  // "Title - Artist" too
    CHECK(m.match({"C:\\x\\Rufus Du Sol - Innerbloom (Official Audio).mp3"}) == 2u);  // accents + noise
    CHECK(m.match({"C:\\x\\track01.mp3", "Selecta", std::nullopt, {"Skrillex"}}) == 0u);  // tags
    // Duration check: same names but 30 s off → no match.
    CHECK(!m.match({"C:\\x\\Skrillex - Selecta.mp3", std::nullopt, std::nullopt, {}, 220.0}));
    CHECK(m.match({"C:\\x\\Skrillex - Selecta.mp3", std::nullopt, std::nullopt, {}, 193.0}) == 0u);
    CHECK(!m.match({"C:\\x\\Someone Else - Selecta.mp3"}));

    CHECK(wb::compatible_keys("8A") == std::set<std::string>({"8A", "9A", "7A", "8B"}));
    CHECK(wb::compatible_keys("12B") == std::set<std::string>({"12B", "1B", "11B", "12A"}));
    CHECK(wb::compatible_keys("x").empty());
    CHECK(wb::camelot_order("1A") < wb::camelot_order("1B") && wb::camelot_order("1B") < wb::camelot_order("2A"));
}

// 16-bit mono WAV with clicks at `bpm`.
static void write_click_wav(const fs::path& p, double bpm, int seconds) {
    const uint32_t rate = 22050;
    std::vector<int16_t> s(size_t(rate) * seconds);
    const size_t period = size_t(rate * 60.0 / bpm);
    for (size_t i = 0; i < s.size(); ++i) s[i] = i % period < 200 ? int16_t(12000 * std::sin(double(i) * 0.3)) : 0;
    std::ofstream f(p, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t data = uint32_t(s.size() * 2);
    f.write("RIFF", 4), u32(36 + data), f.write("WAVEfmt ", 8), u32(16), u16(1), u16(1), u32(rate), u32(rate * 2), u16(2), u16(16);
    f.write("data", 4), u32(data), f.write(reinterpret_cast<const char*>(s.data()), data);
}

static void organise_and_rescan() {
    const fs::path root = fs::temp_directory_path() / L"wreckbox-library-tests";
    fs::remove_all(root);
    wb::paths::init(root);
    wb::Settings::load();

    wb::Library lib;
    lib.built_at = wb::iso_seconds_now();
    lib.spotify_user = "me";
    lib.tracks = {make_track("spotify:a", {"Test Artist"}, "Click Song", 20000), make_track("spotify:b", {"Other"}, "Kept Song", 8000)};
    wb::LibraryPlaylist pl;
    pl.name = "Bangers";
    pl.track_ids = {"spotify:a", "spotify:b", "spotify:a"};
    lib.playlists = {pl};
    wb::paths::write_atomic(wb::paths::library_file(), lib.to_json().dump());

    wb::LibraryStore store;
    int changes = 0;
    store.on_changed = [&] { ++changes; };
    store.load();
    CHECK(!store.load_error(), "%s", store.load_error().value_or("").c_str());
    store.set_scan_folders({reinterpret_cast<const char*>(wb::paths::tracks().u8string().c_str())});
    CHECK(store.rows().size() == 2 && store.rows(wb::ListFilter::all, "Bangers").size() == 2);  // playlist de-duplicated
    CHECK(store.rows(wb::ListFilter::all, std::nullopt, "click").size() == 1);
    CHECK(changes > 0);

    // A junk-named download that matches by tags → analysed, tagged, renamed, filed.
    const fs::path downloads = root / L"Downloads";
    fs::create_directories(downloads);
    const fs::path junk = downloads / L"track_01 (1).wav";
    write_click_wav(junk, 120, 20);
    {
        json job = {{"path", reinterpret_cast<const char*>(junk.u8string().c_str())}, {"title", "Click Song"}, {"artists", {"Test Artist"}}};
        CHECK(wb::write_tags_json(job).value("ok", false));
    }
    const std::string msg = store.organise(reinterpret_cast<const char*>(junk.u8string().c_str()), "downloads");
    CHECK(msg.rfind("Added", 0) == 0, "%s", msg.c_str());
    const auto r = store.row("spotify:a");
    CHECK(r && r->status() == wb::TrackStatus::downloaded);
    const fs::path filed = wb::paths::tracks() / L"Test Artist - Click Song.wav";
    CHECK(fs::exists(filed) && !fs::exists(junk));
    CHECK(r && r->bpm() && std::abs(*r->bpm() - 120.0) < 1.0, "bpm %.1f", r && r->bpm() ? *r->bpm() : -1.0);
    const json tags = wb::read_tags_json(filed);
    CHECK(tags["bpm"] == 120.0 && tags["title"] == "Click Song", "%s", tags.dump().c_str());

    // Something unrelated stays where it is.
    const fs::path stranger = downloads / L"Nobody - Nothing.wav";
    write_click_wav(stranger, 100, 6);
    CHECK(store.organise(reinterpret_cast<const char*>(stranger.u8string().c_str()), "downloads").rfind("Not in your library", 0) == 0);
    CHECK(fs::exists(stranger));

    // Rescan finds a file named "Artist - Title" in Tracks and notices a deleted one.
    write_click_wav(wb::paths::tracks() / L"Other - Kept Song.wav", 128, 8);
    fs::remove(filed);
    const fs::path mine = fs::temp_directory_path() / L"wreckbox-library-tests-mine";  // a folder the user picked
    fs::remove_all(mine);
    fs::create_directories(mine);
    store.add_scan_folder(reinterpret_cast<const char*>(mine.u8string().c_str()));
    const fs::path loose = mine / L"Somebody Else - Loose Tune.wav";  // on the PC, not in the library
    write_click_wav(loose, 110, 6);
    store.rescan();
    CHECK(store.row("spotify:b")->status() == wb::TrackStatus::downloaded);
    CHECK(store.row("spotify:a")->status() == wb::TrackStatus::missing);
    CHECK(!store.busy());

    // My folders: the files in the user's folders as rows with their file type; WreckBox's own downloads (Tracks) aren't.
    const auto on_pc = store.row_ids(wb::ListFilter::on_pc);
    const std::string loose_id = wb::kFileIdPrefix + std::string(reinterpret_cast<const char*>(loose.u8string().c_str()));
    const auto lr = store.row(loose_id);
    CHECK(lr && lr->id() == loose_id && lr->track.title == "Somebody Else - Loose Tune" && lr->format() == "WAV" && !lr->is_flac());
    CHECK(lr && lr->status() == wb::TrackStatus::downloaded && lr->file && !lr->file->library_track_id && lr->duration_text() == "0:06");
    CHECK(on_pc == std::vector{loose_id}, "%zu: %s", on_pc.size(), on_pc.empty() ? "" : on_pc.back().c_str());
    CHECK(store.row_ids(wb::ListFilter::on_pc, std::nullopt, "kept").empty());  // in Tracks
    // A file in the user's folder that matches a library track shows the library's names.
    write_click_wav(mine / L"Other - Kept Song.wav", 128, 8);
    store.rescan();
    const auto kept = store.row_ids(wb::ListFilter::on_pc, std::nullopt, "kept");
    CHECK(kept.size() == 1 && store.row(kept[0])->track.id == "spotify:b" && store.row(kept[0])->track.title == "Kept Song");  // the library's names
    CHECK(store.row_ids(wb::ListFilter::on_pc, std::nullopt, "loose") == std::vector{loose_id});
    CHECK(store.describe(loose_id) == "Somebody Else - Loose Tune.wav");
    CHECK(store.rows().size() == 2);  // the library lists are unchanged
    wb::TrackRow fl;
    fl.state = wb::TrackState{};
    fl.state->local_path = "C:\\Music\\a.b - Song.Flac";
    CHECK(fl.format() == "FLAC" && fl.is_flac() && wb::TrackRow{}.format().empty());

    // State and analysis are saved in the shared formats and load back.
    wb::LibraryStore again;
    again.load();
    CHECK(again.row("spotify:b")->status() == wb::TrackStatus::downloaded);
    CHECK(again.row("spotify:b")->file.has_value());
    const auto log = again.state_copy().log;
    CHECK(!log.empty() && log.back().event == "rescan", "%s", log.empty() ? "" : log.back().event.c_str());

    // Downloads watcher: files the re-downloaded song, leaves the stranger, and doesn't redo either next time.
    const fs::path redownload = downloads / L"dl.wav";
    write_click_wav(redownload, 120, 20);
    CHECK(wb::write_tags_json({{"path", reinterpret_cast<const char*>(redownload.u8string().c_str())},
                               {"title", "Click Song"},
                               {"artists", {"Test Artist"}}})
              .value("ok", false));
    wb::DownloadsWatcher watcher(again, downloads);
    CHECK(watcher.run_once() == 1);
    CHECK(again.row("spotify:a")->status() == wb::TrackStatus::downloaded && fs::exists(filed) && fs::exists(stranger));
    CHECK(watcher.recent().size() == 2, "%zu", watcher.recent().size());
    CHECK(watcher.run_once() == 0 && watcher.recent().size() == 2);
    CHECK(fs::exists(wb::paths::cache() / L"organiser_seen.json"));
    CHECK(wb::dart_iso("2026-01-02T03:04:05Z") == "2026-01-02T03:04:05.000Z" && wb::dart_iso("2026-01-02T03:04:05.123Z") == "2026-01-02T03:04:05.123Z");
    fs::remove_all(root);
    fs::remove_all(mine);
}

// Opt-in (WRECKBOX_NET_TESTS=1): the real services the store calls.
static void network() {
    const char* on = std::getenv("WRECKBOX_NET_TESTS");
    if (!on || std::string(on) != "1") {
        std::puts("(skipping network checks: WRECKBOX_NET_TESTS not set)");
        return;
    }
    const auto r = wb::http::get("https://api.deezer.com/track/isrc:GBAYE0601498", {}, std::chrono::seconds(15));
    CHECK(r.status == 200, "status %d %s", r.status, r.error.c_str());
    const json j = json::parse(r.body, nullptr, false);
    CHECK(j.is_object() && j.contains("bpm"), "%s", r.body.substr(0, 200).c_str());
    std::printf("deezer: %s, bpm %s\n", j.value("title", "?").c_str(), j.contains("bpm") ? j["bpm"].dump().c_str() : "-");
    const auto bad = wb::http::get("https://no-such-host.wreckbox.invalid/", {}, std::chrono::seconds(5));
    CHECK(bad.status == 0 && !bad.error.empty(), "%d %s", bad.status, bad.error.c_str());
}

// Opt-in (WRECKBOX_BENCH=1): DESIGN §5 budgets on a synthetic 5,000-track library — load < 400 ms, search < 16 ms.
static void bench() {
    const char* on = std::getenv("WRECKBOX_BENCH");
    if (!on || std::string(on) != "1") return;
    const fs::path root = fs::temp_directory_path() / L"wreckbox-bench";
    fs::remove_all(root);
    wb::paths::init(root);
    wb::Library lib;
    lib.built_at = wb::iso_seconds_now();
    wb::AppState state;
    json analysis = json::object();
    for (int i = 0; i < 5000; ++i) {
        auto t = make_track("spotify:" + std::to_string(i), {"Artist " + std::to_string(i % 700), "Feat " + std::to_string(i % 13)},
                            "Some Song Title Number " + std::to_string(i), 200000 + i, "ISRC" + std::to_string(i));
        t.album = "Album " + std::to_string(i % 400);
        t.artwork_url = "https://i.scdn.co/image/ab67616d00001e02" + std::to_string(i);
        t.playlists = {"Playlist " + std::to_string(i % 30)};
        lib.tracks.push_back(t);
        const std::string path = "C:\\Users\\x\\Music\\WreckBox\\Tracks\\" + t.file_name + ".mp3";
        wb::TrackState s;
        s.status = i % 3 ? wb::TrackStatus::downloaded : wb::TrackStatus::missing;
        s.local_path = path;
        s.source = "soulseek";
        s.updated_at = wb::iso_seconds_now();
        state.tracks[t.id] = s;
        wb::FileAnalysis a;
        a.path = path, a.size_bytes = 9000000, a.modified = wb::iso_seconds_now(), a.duration_sec = 200, a.bpm = 120 + i % 30;
        a.bpm_candidates = {60, 240}, a.key = "A minor", a.camelot = std::to_string(i % 12 + 1) + "A", a.energy = 0.7;
        analysis[path] = a.to_json();
    }
    for (int p = 0; p < 30; ++p) {
        wb::LibraryPlaylist pl;
        pl.name = "Playlist " + std::to_string(p);
        for (int i = p; i < 5000; i += 30) pl.track_ids.push_back("spotify:" + std::to_string(i));
        lib.playlists.push_back(pl);
    }
    for (int i = 0; i < 5000; ++i) state.log.push_back(wb::LogEntry::make("found", "something happened " + std::to_string(i)));
    wb::paths::write_atomic(wb::paths::library_file(), lib.to_json().dump());
    wb::paths::write_atomic(wb::paths::state_file(), state.to_json().dump(2));
    wb::paths::write_atomic(wb::paths::analysis_cache(), analysis.dump());
    std::printf("bench files: library %zu KB, state %zu KB, analysis %zu KB\n", size_t(fs::file_size(wb::paths::library_file()) / 1024),
                size_t(fs::file_size(wb::paths::state_file()) / 1024), size_t(fs::file_size(wb::paths::analysis_cache()) / 1024));

    using clock = std::chrono::steady_clock;
    auto ms = [](clock::time_point a) { return std::chrono::duration<double, std::milli>(clock::now() - a).count(); };
    wb::LibraryStore store;
    auto t0 = clock::now();
    store.load();
    std::printf("load 5000 tracks: %.0f ms (budget 400)\n", ms(t0));
    t0 = clock::now();
    const auto all = store.rows();
    std::printf("rows() all: %.1f ms (%zu rows)\n", ms(t0), all.size());
    t0 = clock::now();
    const auto hits = store.rows(wb::ListFilter::all, std::nullopt, "artist 12");
    std::printf("search keystroke: %.1f ms (%zu hits, budget 16)\n", ms(t0), hits.size());
    t0 = clock::now();
    store.save();
    std::printf("save state: %.0f ms\n", ms(t0));
    fs::remove_all(root);
}

int main() {
    bench();
    clean_titles();
    matching();
    organise_and_rescan();
    network();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all library tests passed");
    return 0;
}
