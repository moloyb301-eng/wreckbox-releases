// The native sync loop against a stand-in network: the order tracks are tried in, what a pass writes (sync.json, sync.log,
// _inbox), retries with your own words, the lock, and stopping. The same files the sidecar writes, so the app's pages work.
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <thread>

#include "library/store.h"
#include "model/paths.h"
#include "model/settings.h"
#include "net/slsk/sync.h"

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace wb::slsk;
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

static std::string read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

static wb::LibraryTrack make_track(const std::string& id, const std::string& artist, const std::string& title, const std::string& added, int64_t ms = 200000) {
    wb::LibraryTrack t;
    t.id = id;
    t.artists = {artist};
    t.title = title;
    t.first_added = added;
    t.duration_ms = ms;
    t.file_name = wb::safe_file_name(artist + " - " + title);
    return t;
}

static UserResult result(const std::string& user, const std::string& path, uint64_t size, const std::string& ext, int bitrate = 0) {
    UserResult r;
    r.username = user, r.free_slots = true, r.avg_speed = 1000000;
    FileEntry f;
    f.filename = path, f.size = size, f.extension = ext, f.attributes[kAttrDuration] = 200;
    if (bitrate) f.attributes[kAttrBitrate] = uint32_t(bitrate);
    r.files = {f};
    return r;
}

// A network that answers from a table and "downloads" by writing bytes.
struct FakeNet : Backend {
    std::mutex m;
    std::map<std::string, std::vector<UserResult>> answers;  // query → results (any other query: nothing)
    std::map<std::string, std::string> download_error;       // "user" → error
    std::map<std::string, size_t> write_bytes;               // "user" → bytes actually written (default: the candidate's size)
    std::vector<std::string> queries, downloads;
    std::string login_error;
    bool up = false;
    int search_delay_ms = 0;
    std::string login(const std::string&, const std::string&) override {
        if (!login_error.empty()) return login_error;
        up = true;
        return "";
    }
    bool connected() const override { return up; }
    void close() override { up = false; }
    std::vector<UserResult> search(const std::string& q, std::chrono::seconds, const std::function<bool()>& cancel) override {
        {
            std::lock_guard lock(m);
            queries.push_back(q);
        }
        for (int i = 0; i < search_delay_ms / 10 && !(cancel && cancel()); ++i) std::this_thread::sleep_for(10ms);
        std::lock_guard lock(m);
        const auto it = answers.find(q);
        return it == answers.end() ? std::vector<UserResult>{} : it->second;
    }
    DownloadResult download(const std::string& user, const std::string& path, const std::string& dest, const DownloadOptions&) override {
        std::lock_guard lock(m);
        downloads.push_back(user + ":" + path);
        if (const auto e = download_error.find(user); e != download_error.end()) return {false, e->second, 0};
        size_t n = 0;
        for (const auto& [q, rs] : answers)
            for (const auto& r : rs)
                for (const auto& f : r.files)
                    if (r.username == user && f.filename == path) n = size_t(f.size);
        if (const auto w = write_bytes.find(user); w != write_bytes.end()) n = w->second;
        std::ofstream(fs::path(wb::widen(dest)), std::ios::binary) << std::string(n, 'x');
        return {true, "", n};
    }
};

static std::string toml_for(const std::string& extra_sync = "") {
    return "[soulseek]\nusername = \"dj\"\npassword = \"pw\"\nlisten_port = 60000\nshare_dirs = [\"C:\\\\Music\\\\Tracks\", \"D:\\\\x \\\"y\\\"\"]\n\n[sync]\ninterval_minutes = 30\n"
           "max_concurrent = 1\nsearch_wait_seconds = 0\nsearch_gap_seconds = 0\n" + extra_sync;
}

static SyncConfig config(const std::string& extra = "") { return SyncConfig::parse(toml_for(extra)); }

int main() {
    const fs::path root = fs::temp_directory_path() / L"wreckbox-slsk-sync-tests";
    fs::remove_all(root);
    wb::paths::init(root);
    wb::Settings::load();

    // The config file.
    {
        const auto c = SyncConfig::parse(toml_for("min_lossy_kbps = 320\ncandidates_per_track = 2\n"));
        CHECK(c.username == "dj" && c.password == "pw" && c.listen_port == 60000 && c.interval_minutes == 30 && c.max_concurrent == 1 && c.min_lossy_kbps == 320 && c.candidates_per_track == 2);
        CHECK((c.share_dirs == std::vector<std::string>{"C:\\Music\\Tracks", "D:\\x \"y\""}), "share_dirs");
        const auto d = SyncConfig::parse("");
        CHECK(d.username.empty() && d.max_concurrent == 3 && d.search_wait_seconds == 12 && d.retry_after_hours == 24 && d.max_attempts == 5 && d.min_lossy_kbps == 256 && d.candidates_per_track == 4);
    }

    wb::Library lib;
    lib.built_at = wb::iso_seconds_now();
    lib.spotify_user = "me";
    lib.tracks = {make_track("a", "Artist A", "Song A", "2026-05-01"), make_track("b", "Artist B", "Song B", "2026-04-01"), make_track("c", "Artist C", "Song C", "2026-03-01"),
                  make_track("d", "Artist D", "Song D", "2026-02-01"), make_track("e", "Artist E", "Song E", "2026-01-01"), make_track("f", "Artist F", "Song F", "2025-12-01"),
                  make_track("g", "Artist G", "Song G", "2025-11-01"), make_track("h", "Artist H", "Song H", "2025-10-01")};
    wb::paths::write_atomic(wb::paths::library_file(), lib.to_json().dump());
    wb::LibraryStore store;
    store.load();

    // Who is due, and in what order.
    {
        wb::TrackState have, ignored;
        have.status = wb::TrackStatus::downloaded, ignored.status = wb::TrackStatus::ignored;
        store.set_track_state("a", have);
        store.set_track_state("b", ignored);
        fs::create_directories(wb::paths::inbox());
        std::ofstream(wb::paths::inbox() / L"Artist C - Song C.flac") << "x";  // already delivered, waiting to be filed
        fs::create_directories(wb::paths::soulseek_dir());
        const auto now = wb::iso_seconds_now();
        wb::paths::write_atomic(wb::paths::soulseek_dir() / L"sync.json",
                                json{{"d", {{"status", "done"}, {"last_try", now}}},
                                     {"e", {{"status", "failed"}, {"last_try", now}, {"attempts", 1}}},                              // tried an hour ago: not yet
                                     {"f", {{"status", "failed"}, {"last_try", "2020-01-01T00:00:00Z"}, {"attempts", 5}}},          // too many attempts
                                     {"g", {{"status", "not_found"}, {"last_try", "2020-01-01T00:00:00Z"}, {"attempts", 2}}}}       // due again
                                    .dump());
        Runner r(store, config(), std::make_unique<FakeNet>());
        auto ids = [&] {
            std::vector<std::string> out;
            for (const auto& t : r.missing_tracks()) out.push_back(t.id);
            return out;
        };
        CHECK((ids() == std::vector<std::string>{"g", "h"}), "due: %zu", ids().size());  // newest first among those due: g (2025-11), h (2025-10)
        // An explicit retry (newer than the last attempt) is due at once, and goes first.
        wb::paths::write_atomic(wb::paths::soulseek_dir() / L"overrides.json", json{{"e", {{"retryAt", "2999-01-01T00:00:00Z"}}}, {"d", {{"retryAt", "2999-01-01T00:00:00Z"}}}}.dump());
        CHECK((ids() == std::vector<std::string>{"e", "d", "g", "h"}) || (ids() == std::vector<std::string>{"d", "e", "g", "h"}), "retries first");
        // The app's queue decides the order of the rest; "only priority" drops what isn't on it, but not retries.
        wb::paths::write_atomic(wb::paths::soulseek_dir() / L"queue.json", json{{"ids", {"h", "g"}}, {"onlyPriority", false}}.dump());
        CHECK(ids().size() == 4 && ids()[2] == "h" && ids()[3] == "g", "queue order: %s %s", ids()[2].c_str(), ids()[3].c_str());
        wb::paths::write_atomic(wb::paths::soulseek_dir() / L"queue.json", json{{"ids", {"h"}}, {"onlyPriority", true}}.dump());
        CHECK((ids().size() == 3 && ids()[2] == "h"), "only priority keeps the retries and h: %zu", ids().size());
        // Nothing picked means nothing (but the retries), not everything.
        wb::paths::write_atomic(wb::paths::soulseek_dir() / L"queue.json", json{{"ids", json::array()}, {"onlyPriority", true}}.dump());
        CHECK(ids().size() == 2, "nothing picked: %zu", ids().size());
        fs::remove(wb::paths::soulseek_dir() / L"queue.json");
        fs::remove(wb::paths::soulseek_dir() / L"overrides.json");
        fs::remove(wb::paths::inbox() / L"Artist C - Song C.flac");
        fs::remove(wb::paths::soulseek_dir() / L"sync.json");
    }

    // A pass.
    {
        auto net = std::make_unique<FakeNet>();
        FakeNet* n = net.get();
        // a is downloaded already; the rest:
        n->answers["artist b song b"] = {};  // b is ignored: never searched
        n->answers["artist c song c"] = {result("u1", "Music\\Artist C\\Song C.flac", 20000000, "flac")};
        n->answers["artist d song d"] = {result("badpeer", "x\\Artist D - Song D.flac", 20000000, "flac"), result("goodpeer", "y\\Artist D - Song D.mp3", 9000000, "mp3", 320)};
        n->download_error["badpeer"] = "still queued (place 12)";
        n->answers["artist e song e"] = {result("p1", "Artist E - Song E.flac", 20000000, "flac"), result("p2", "Artist E - Song E.mp3", 9000000, "mp3", 320)};
        n->download_error["p1"] = "stalled", n->download_error["p2"] = "File not shared.";
        n->answers["artist f song f"] = {result("short", "Artist F - Song F.flac", 20000000, "flac")};
        n->write_bytes["short"] = 1000;  // the file ends early
        // g and h have no answers.
        Runner r(store, config("max_concurrent = 1\n"), std::move(net));
        CHECK(r.login());
        wb::TrackState st;
        store.set_track_state("c", st), store.set_track_state("d", st), store.set_track_state("e", st), store.set_track_state("f", st);  // missing
        const auto counts = r.run_pass();
        CHECK(counts.done == 2 && counts.failed == 2 && counts.not_found == 2, "%d done / %d failed / %d not found", counts.done, counts.failed, counts.not_found);
        const json sync = json::parse(read_all(wb::paths::soulseek_dir() / L"sync.json"));
        CHECK(sync["c"]["status"] == "done" && sync["c"]["file"] == "Artist C - Song C.flac" && sync["c"]["format"] == "flac" && sync["c"]["sizeBytes"] == 20000000 &&
                  sync["c"]["source"] == "u1:Music\\Artist C\\Song C.flac" && sync["c"]["reason"].is_null() && sync["c"]["bitrate"].is_null() && sync["c"]["name"] == "Artist C - Song C" &&
                  sync["c"]["queries"][0] == "artist c song c",
              "%s", sync["c"].dump().c_str());
        CHECK(sync["d"]["status"] == "done" && sync["d"]["source"] == "goodpeer:y\\Artist D - Song D.mp3" && sync["d"]["bitrate"] == 320, "second source tried: %s", sync["d"].dump().c_str());
        CHECK(sync["e"]["status"] == "failed" && sync["e"]["reason"] == "2 sources tried, none delivered" && sync["e"]["attempts"] == 1, "%s", sync["e"].dump().c_str());
        CHECK(sync["f"]["status"] == "failed" && sync["f"]["attempts"] == 1, "an incomplete file is a failure");
        CHECK(sync["g"]["status"] == "not_found" && sync["g"]["reason"] == "no results" && sync["g"]["attempts"] == 1 && sync["g"]["last_try"].is_string());
        CHECK(fs::exists(wb::paths::inbox() / L"Artist C - Song C.flac") && fs::file_size(wb::paths::inbox() / L"Artist C - Song C.flac") == 20000000);
        CHECK(fs::exists(wb::paths::inbox() / L"Artist D - Song D.mp3"));
        CHECK(!fs::exists(wb::paths::inbox() / L"Artist F - Song F.flac"), "nothing partial reaches _inbox");
        size_t parts = 0;
        for (const auto& f : fs::directory_iterator(wb::paths::soulseek_dir() / L"incoming")) parts += f.path().extension() == L".part";
        CHECK(parts == 0, "partial files are cleaned up");
        const std::string log = read_all(wb::paths::soulseek_dir() / L"sync.log");
        for (const char* line : {"Logged in to Soulseek as dj", "Pass: 6 tracks to look for", "↓ Artist C – Song C  ←  FLAC  19.1MB  u1  free  976KB/s", "✓ Artist C – Song C → _inbox/Artist C - Song C.flac",
                                 "  ✗ badpeer: still queued (place 12)", "· not found: Artist G – Song G (no results)", "Pass finished: 2 downloaded, 2 not found, 2 failed"})
            CHECK(log.find(line) != std::string::npos, "log lacks '%s'", line);
        CHECK(std::regex_match(log.substr(0, 19), std::regex(R"(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)")), "the log's timestamps");
        // Next pass: the finished are skipped, the failed wait a day, the not-found wait too.
        n->queries.clear();
        const auto again = r.run_pass();
        CHECK(again.done + again.failed + again.not_found == 0 && n->queries.empty(), "nothing is due again at once");
    }

    // A retry with the user's own words: the custom search first, judged by its words.
    {
        auto net = std::make_unique<FakeNet>();
        FakeNet* n = net.get();
        n->answers["weird name here"] = {result("w", "Some Folder\\weird name here (final).flac", 20000000, "flac")};
        wb::paths::write_atomic(wb::paths::soulseek_dir() / L"overrides.json", json{{"g", {{"query", "  Weird Name Here "}, {"retryAt", "2999-01-01T00:00:00Z"}}}}.dump());
        Runner r(store, config(), std::move(net));
        CHECK(r.login());
        const auto counts = r.run_pass();
        CHECK(counts.done == 1 && !n->queries.empty() && n->queries[0] == "weird name here", "custom query first: %s", n->queries.empty() ? "" : n->queries[0].c_str());
        const json sync = json::parse(read_all(wb::paths::soulseek_dir() / L"sync.json"));
        CHECK(sync["g"]["status"] == "done" && sync["g"]["attempts"] == 1, "%s", sync["g"].dump().c_str());  // (attempts from the not-found stay)
        fs::remove(wb::paths::soulseek_dir() / L"overrides.json");
    }

    // The login, the lock, and stopping.
    {
        auto net = std::make_unique<FakeNet>();
        net->login_error = "Soulseek login failed: INVALIDPASS. Check the username and password.";
        Runner r(store, config(), std::move(net));
        r.run(true);
        CHECK(read_all(wb::paths::soulseek_dir() / L"sync.log").find("✗ Soulseek login failed: INVALIDPASS") != std::string::npos);
    }
    {
        Runner none(store, SyncConfig::parse(""), std::make_unique<FakeNet>());
        CHECK(!none.login());
        CHECK(read_all(wb::paths::soulseek_dir() / L"sync.log").find("✗ Add your Soulseek username and password") != std::string::npos);
    }
    {
        Runner first(store, config(), std::make_unique<FakeNet>());
        CHECK(first.login());
        Runner second(store, config(), std::make_unique<FakeNet>());
        CHECK(!second.login(), "one sync at a time");
        CHECK(read_all(wb::paths::soulseek_dir() / L"sync.log").find("✗ slsk-sync is already running (pid ") != std::string::npos);
        CHECK(read_all(wb::paths::soulseek_dir() / L"sync.pid") == std::to_string(GetCurrentProcessId()));
    }
    {  // stopping in the middle of a slow pass
        fs::remove(wb::paths::soulseek_dir() / L"sync.json");
        auto net = std::make_unique<FakeNet>();
        net->search_delay_ms = 20000;
        Runner r(store, config(), std::move(net));
        std::thread t([&] { r.run(); });
        std::this_thread::sleep_for(700ms);
        const auto t0 = std::chrono::steady_clock::now();
        r.stop();
        t.join();
        CHECK(std::chrono::steady_clock::now() - t0 < 3s, "stop takes %lld ms", (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    }
    {  // "run" loops: a pass, a sleep, woken early by a retry request, stopped
        fs::remove(wb::paths::soulseek_dir() / L"sync.json");
        auto net = std::make_unique<FakeNet>();
        FakeNet* n = net.get();
        Runner r(store, config(), std::move(net));
        std::thread t([&] { r.run(); });
        const auto passes = [&] { return read_all(wb::paths::soulseek_dir() / L"sync.log"); };
        const auto count = [&](const std::string& s) {
            size_t k = 0;
            for (size_t at = passes().find(s); at != std::string::npos; at = passes().find(s, at + 1)) ++k;
            return k;
        };
        for (int i = 0; i < 100 && count("Sleeping 30 min") < 1; ++i) std::this_thread::sleep_for(50ms);
        CHECK(count("Sleeping 30 min") >= 1, "sleeps between passes");
        const size_t before = count("Pass finished");
        wb::paths::write_atomic(wb::paths::soulseek_dir() / L"overrides.json", json{{"h", {{"retryAt", "2999-01-01T00:00:00Z"}}}}.dump());
        for (int i = 0; i < 100 && count("Pass finished") == before; ++i) std::this_thread::sleep_for(50ms);
        CHECK(count("Pass finished") > before && passes().find("Queue or retry requests changed — starting a new pass") != std::string::npos, "a retry request wakes it");
        r.stop();
        t.join();
        (void)n;
    }

    fs::remove_all(root);
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all slsk sync tests passed");
    return 0;
}
