// Soulseek bridge tests: the login file, the download queue (priorities), retry requests, reading the sidecar's results,
// filing what it leaves in _inbox, and running a sidecar process (a stand-in script run by whatever python.exe is on PATH:
// the real sidecar needs a Soulseek account, which tests never use).
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

#include "library/store.h"
#include "model/paths.h"
#include "model/settings.h"
#include "net/soulseek.h"

namespace fs = std::filesystem;
using namespace std::chrono_literals;
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

static bool wait_for(const std::function<bool()>& cond, std::chrono::milliseconds limit = 10s) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!cond()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(20ms);
    }
    return true;
}

static std::string read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
static std::string utf8(const fs::path& p) { return reinterpret_cast<const char*>(p.u8string().c_str()); }

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

static wb::LibraryTrack make_track(const std::string& id, const std::string& artist, const std::string& title, const std::string& added) {
    wb::LibraryTrack t;
    t.id = id;
    t.artists = {artist};
    t.title = title;
    t.first_added = added;
    t.duration_ms = 8000;
    t.file_name = wb::safe_file_name(artist + " - " + title);
    return t;
}

static std::string find_python() {
    wchar_t buf[MAX_PATH];
    const DWORD n = SearchPathW(nullptr, L"python", L".exe", MAX_PATH, buf, nullptr);
    return n ? wb::narrow(std::wstring(buf, n)) : "";
}

int main() {
    const fs::path root = fs::temp_directory_path() / L"wreckbox-soulseek-tests";
    fs::remove_all(root);
    wb::paths::init(root);
    wb::Settings::load();

    wb::Library lib;
    lib.built_at = wb::iso_seconds_now();
    lib.spotify_user = "me";
    lib.tracks = {make_track("t1", "Artist One", "Song One", "2026-01-01T00:00:00Z"), make_track("t2", "Artist Two", "Song Two", "2026-03-01T00:00:00Z"),
                  make_track("t3", "Artist Three", "Song Three", "2026-02-01T00:00:00Z"), make_track("t4", "Artist Four", "Song Four", "2026-04-01T00:00:00Z")};
    wb::LibraryPlaylist a, b;
    a.name = "Alpha", a.track_ids = {"t1", "t3"};
    b.name = "Beta", b.track_ids = {"t2", "t3", "t4"};
    lib.playlists = {a, b};
    wb::paths::write_atomic(wb::paths::library_file(), lib.to_json().dump());
    wb::LibraryStore store;
    store.load();
    wb::soulseek::Sync sl(store);

    // The login file.
    CHECK(!sl.configured() && sl.username().empty());
    sl.save_login("dj \"quoted\" \\name", "p@ss\\word\"", true);
    CHECK(sl.configured() && sl.username() == "dj \"quoted\" \\name", "'%s'", sl.username().c_str());
    const std::string toml = read_all(wb::soulseek::Sync::config_file());
    CHECK(toml.find("[soulseek]") == 0 && toml.find("listen_port = 60000") != std::string::npos && toml.find("[sync]") != std::string::npos && toml.find("max_concurrent = 3") != std::string::npos);
    CHECK(toml.find("share_dirs = [\"") != std::string::npos && toml.find("Tracks") != std::string::npos, "the Tracks folder is shared");
    sl.save_login("someone", "", false);
    CHECK(!sl.configured() && read_all(wb::soulseek::Sync::config_file()).find("share_dirs = []") != std::string::npos);

    // The queue. By default only what's picked: nothing yet.
    auto queue = [&] { return json::parse(read_all(wb::paths::soulseek_dir() / L"queue.json")); };
    sl.write_queue();
    json q = queue();
    CHECK(q["ids"].empty() && q["onlyPriority"] == true && sl.wanted().empty(), "%s", q.dump().c_str());
    // Everything: priorities first (in order, only what's missing, no repeats), then everything else, newest first.
    store.set_download_priority({}, false);
    sl.write_queue();
    q = queue();
    CHECK((q["ids"] == json::array({"t4", "t2", "t3", "t1"})) && q["onlyPriority"] == false && q.contains("generatedAt"), "%s", q.dump().c_str());
    CHECK((sl.wanted() == std::vector<std::string>{"t4", "t2", "t3", "t1"}));
    {
        auto st = store.state_copy();
        wb::TrackState have;
        have.status = wb::TrackStatus::downloaded;
        store.set_track_state("t2", have);  // one is already in the crate
        wb::TrackState ignored;
        ignored.status = wb::TrackStatus::ignored;
        store.set_track_state("t4", ignored);
    }
    sl.write_queue();
    CHECK(queue()["ids"] == json::array({"t3", "t1"}), "%s", queue().dump().c_str());  // downloaded and ignored are left out
    wb::TrackState back;
    store.set_track_state("t2", back), store.set_track_state("t4", back);  // missing again
    // Priorities need the store's own setter (they live in state.json): go through the saved file.
    store.save();
    {
        json st = json::parse(read_all(wb::paths::state_file()));
        st["downloadPriority"] = json::array({"playlist:Beta", "playlist:Alpha", "playlist:Nope", "nocolon"});
        st["downloadMode"] = "picked";
        wb::paths::write_atomic(wb::paths::state_file(), st.dump());
        store.load();
    }
    sl.write_queue();
    q = queue();
    CHECK((q["ids"] == json::array({"t2", "t3", "t4", "t1"})) && q["onlyPriority"] == true && q["priorities"].size() == 4, "%s", q.dump().c_str());
    // A song picked on its own; an ignored one stays out even when picked.
    store.set_download_priority({"track:t3", "track:t1", "track:zz"});
    sl.write_queue();
    CHECK(queue()["ids"] == json::array({"t3", "t1"}), "%s", queue().dump().c_str());
    store.set_download_priority({"track:t3", "playlist:Beta", "playlist:Alpha", "playlist:Nope", "nocolon"});
    sl.write_queue();
    CHECK(queue()["ids"][0] == "t3", "%s", queue().dump().c_str());
    store.set_download_priority({"playlist:Beta", "playlist:Alpha", "playlist:Nope", "nocolon"});

    // Retry requests.
    CHECK(!sl.retry_pending("t1"));
    sl.retry({"t1", "t2"}, std::string("  artist one song  "));
    const json ov = json::parse(read_all(wb::paths::soulseek_dir() / L"overrides.json"));
    CHECK(ov["t1"]["query"] == "artist one song" && ov["t2"]["query"] == "artist one song" && ov["t1"]["retryAt"].is_string());
    CHECK(sl.retry_pending("t1") && sl.retry_pending("t2") && !sl.retry_pending("t3"));
    sl.retry({"t1"}, std::string("   "));  // an empty search clears the custom words
    CHECK(!json::parse(read_all(wb::paths::soulseek_dir() / L"overrides.json"))["t1"].contains("query") && sl.retry_pending("t1"));
    // A newer attempt than the request means it was handled.
    wb::paths::write_atomic(wb::paths::soulseek_dir() / L"sync.json",
                            json{{"t1", {{"status", "not_found"}, {"last_try", "2999-01-01T00:00:00Z"}, {"attempts", 2}, {"reason", "no good match"}, {"format", "flac"}, {"queries", {"a", "b"}}}},
                                 {"t2", {{"status", "done"}, {"last_try", "2026-01-01T00:00:00Z"}, {"attempts", 1}}},
                                 {"t3", {{"status", "failed"}, {"last_try", "2026-01-02T00:00:00Z"}, {"attempts", 5}}}}
                                .dump());
    std::string log;
    for (int i = 0; i < 60; ++i) log += "line " + std::to_string(i) + (i % 7 == 0 ? " ✓" : "") + "\r\n\r\n";
    wb::paths::write_atomic(wb::paths::soulseek_dir() / L"sync.log", log);
    sl.refresh();
    CHECK(sl.done() == 1 && sl.not_found() == 1 && sl.failed() == 1, "%d/%d/%d", sl.done(), sl.not_found(), sl.failed());
    const auto recs = sl.records();
    CHECK(recs.at("t1").attempts == 2 && recs.at("t1").reason == "no good match" && recs.at("t1").format == "flac" && recs.at("t1").queries.size() == 2);
    CHECK(!sl.retry_pending("t1") && sl.retry_pending("t2"), "t1's attempt is newer than the request; t2's is older");
    const auto recent = sl.recent();
    CHECK(recent.size() == 40 && recent.front() == "line 59" && recent.back() == "line 20", "%zu '%s' '%s'", recent.size(), recent.empty() ? "" : recent.front().c_str(),
          recent.empty() ? "" : recent.back().c_str());

    // What the sidecar leaves in _inbox is filed: by its exact name, or "Name (2)".
    fs::create_directories(wb::paths::inbox());
    write_click_wav(wb::paths::inbox() / (std::wstring(wb::widen(lib.tracks[0].file_name)) + L".wav"), 120, 8);
    write_click_wav(wb::paths::inbox() / (std::wstring(wb::widen(lib.tracks[2].file_name)) + L" (2).wav"), 128, 8);
    write_click_wav(wb::paths::inbox() / L"Nobody - Nothing.wav", 100, 6);
    CHECK(sl.import_inbox() == 2);
    CHECK(store.row("t1")->status() == wb::TrackStatus::downloaded && store.row("t3")->status() == wb::TrackStatus::downloaded);
    CHECK(fs::exists(wb::paths::tracks() / (wb::widen(lib.tracks[0].file_name) + L".wav")) && !fs::exists(wb::paths::inbox() / (wb::widen(lib.tracks[0].file_name) + L".wav")));
    CHECK(fs::exists(wb::paths::inbox() / L"Nobody - Nothing.wav") && sl.import_inbox() == 0, "an unknown file stays, and isn't tried again");

    // Without the component installed.
    _putenv_s("WRECKBOX_SLSK_DIR", utf8(root / L"no-such-dir").c_str());
    _putenv_s("WRECKBOX_SLSK_PYTHON", "");
    CHECK(!wb::soulseek::Sync::sidecar_present() && wb::soulseek::Sync::use_native() && wb::soulseek::Sync::available(), "no sidecar: the built-in client takes over");
    CHECK(sl.start() == "Add your Soulseek username and password first.", "the login is still needed");

    // A sidecar process: a stand-in script says what it was given, writes results and a pid file, and runs until killed.
    const std::string python = find_python();
    if (python.empty()) {
        std::puts("  no python.exe on PATH: the process checks are skipped");
    } else {
        const fs::path dir = root / L"sidecar";
        fs::create_directories(dir);
        std::ofstream(dir / L"slsk_sync.py") << R"(import os, sys, time, json, pathlib
root = pathlib.Path(os.environ["WRECKBOX_ROOT"]); work = root / "_soulseek"; work.mkdir(exist_ok=True)
(work / "args.txt").write_text(" ".join(sys.argv[1:]) + "|" + os.environ["WRECKBOX_SLSK_CONFIG"] + "|" + os.environ["PYTHONIOENCODING"], encoding="utf-8")
(work / "sync.pid").write_text(str(os.getpid()))
(work / "sync.json").write_text(json.dumps({"t4": {"status": "done", "last_try": "2026-05-01T00:00:00Z", "attempts": 1}}))
(work / "sync.log").write_text("started\n", encoding="utf-8")
while True:
    time.sleep(0.2)
)";
        _putenv_s("WRECKBOX_SLSK_DIR", utf8(dir).c_str());
        _putenv_s("WRECKBOX_SLSK_PYTHON", python.c_str());
        CHECK(wb::soulseek::Sync::available());
        sl.save_login("user", "pass", true);
        std::atomic<int> changes{0};
        sl.on_changed = [&] { ++changes; };
        CHECK(!sl.running() && sl.start().empty());
        CHECK(sl.running() && sl.start().empty(), "starting twice is a no-op");
        CHECK(wait_for([&] { return fs::exists(wb::paths::soulseek_dir() / L"args.txt"); }), "the sidecar ran");
        const std::string args = read_all(wb::paths::soulseek_dir() / L"args.txt");
        CHECK(args == "run|" + utf8(wb::soulseek::Sync::config_file()) + "|utf-8", "%s", args.c_str());
        CHECK(wait_for([&] {  // the script writes its files one after another
            sl.refresh();
            return sl.done() == 1 && sl.recent().size() == 1;
        }));
        CHECK(sl.done() == 1 && sl.recent().size() == 1 && sl.recent()[0] == "started", "its results are read: %d done", sl.done());
        CHECK(changes > 0);
        const auto t0 = std::chrono::steady_clock::now();
        sl.stop();
        CHECK(std::chrono::steady_clock::now() - t0 < 2s);
        CHECK(wait_for([&] { return !sl.running(); }), "stopped");
        // The process is really gone: the pid file's process isn't alive.
        const unsigned long pid = std::strtoul(read_all(wb::paths::soulseek_dir() / L"sync.pid").c_str(), nullptr, 10);
        HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
        CHECK(!h || WaitForSingleObject(h, 3000) == WAIT_OBJECT_0, "killed");
        if (h) CloseHandle(h);
        // And again after it ended.
        fs::remove(wb::paths::soulseek_dir() / L"args.txt");
        CHECK(sl.start().empty() && wait_for([&] { return fs::exists(wb::paths::soulseek_dir() / L"args.txt"); }), "restart");
        sl.stop();
        // A sync left over from an earlier run (its pid file, process still alive) shows as running.
        PROCESS_INFORMATION pi{};
        STARTUPINFOW si{sizeof si};
        std::wstring cmd = wb::widen(python) + L" -c \"import time; time.sleep(30)\"";
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            // (the stopped stand-in may still be closing its own sync.pid: try again for a moment)
            CHECK(wait_for([&] {
                try {
                    wb::paths::write_atomic(wb::paths::soulseek_dir() / L"sync.pid", std::to_string(pi.dwProcessId));
                    return true;
                } catch (const std::exception&) {
                    return false;
                }
            }));
            CHECK(wait_for([&] { sl.refresh(); return sl.running(); }) && sl.external_pid() == pi.dwProcessId, "external sync noticed");
            sl.stop();  // kills it too
            CHECK(WaitForSingleObject(pi.hProcess, 3000) == WAIT_OBJECT_0, "the leftover is stopped");
            CloseHandle(pi.hProcess), CloseHandle(pi.hThread);
        }
    }

    _putenv_s("WRECKBOX_SLSK_DIR", "");
    _putenv_s("WRECKBOX_SLSK_PYTHON", "");
    fs::remove_all(root);
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all soulseek tests passed");
    return 0;
}
