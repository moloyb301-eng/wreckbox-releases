// Soulseek sync: runs the bundled slsk_sync.py sidecar (embedded Python + aioslsk), imports
// what it drops in _inbox through the organiser, and reads its results. The files are the Mac / original apps' own:
// _soulseek/sync.json, sync.log, queue.json, overrides.json (+ sync.pid), and soulseek.toml next to settings.json.
// Phase 10 replaces the sidecar with a native client; this class then keeps only the files and the queue.
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "library/store.h"

namespace wb::slsk {
class Runner;
}

namespace wb::soulseek {

struct SyncRecord {
    std::string status, last_try;  // status: done | not_found | failed
    int attempts = 0;
    std::optional<std::string> reason, format, source;
    std::vector<std::string> queries;
    static SyncRecord from_json(const json& j);
};

struct Process;  // the running sidecar (soulseek.cpp)

class Sync {
public:
    explicit Sync(LibraryStore& store);
    ~Sync();

    std::function<void()> on_changed;  // results, log or running state changed (any thread)

    // Where the sidecar lives: <exe dir>\soulseek\python\python.exe and slsk_sync.py (WRECKBOX_SLSK_DIR overrides, for tests).
    static std::filesystem::path sidecar_dir();
    static std::filesystem::path python();
    static std::filesystem::path script();
    static std::filesystem::path config_file();  // <settings folder>\soulseek.toml
    static bool sidecar_present();               // the interpreter and the script are there
    // The built-in client (net/slsk) runs the sync instead of the sidecar: when asked for (Settings, or
    // WRECKBOX_SLSK_NATIVE=1) or when there is no sidecar. It is the same loop, files and results; it does not upload yet.
    static bool use_native();
    static void set_use_native(bool on);  // saved in settings.json
    static bool available();              // something can run the sync

    bool configured() const;      // username and password are in the config
    std::string username() const;
    // Writes soulseek.toml (the login, the Tracks folder as the shared folder, sync settings). Throws on failure.
    void save_login(const std::string& username, const std::string& password, bool share_tracks = true) const;
    // "Smaller files": MP3 / AAC (256 kbps+) before lossless. Kept in soulseek.toml, which both runners read.
    bool prefer_smaller() const;
    void set_prefer_smaller(bool on);

    // Reads the results every 15 s (and imports finished files) until destroyed.
    void start_watching(std::chrono::seconds every = std::chrono::seconds(15));
    void refresh();

    std::string start();  // "" when started, else why not
    void stop();          // our process and any left over; logs it
    void stop_process();  // at exit: just ends our own process, quietly
    bool running() const;  // our process, or one left over from an earlier run
    std::optional<unsigned long> external_pid() const;

    std::map<std::string, SyncRecord> records() const;
    std::vector<std::string> recent() const;  // newest first, at most 40 log lines
    int done() const { return count("done"); }
    int not_found() const { return count("not_found"); }
    int failed() const { return count("failed"); }

    // Ask the sidecar to retry these tracks on its next pass (it wakes within ~10 s), optionally with a custom search.
    void retry(const std::vector<std::string>& ids, const std::optional<std::string>& query = std::nullopt);
    bool retry_pending(const std::string& id) const;
    // Stop these tracks now if the sync is on them (searching or downloading; it checks every second or two). It doesn't
    // count as a failed try; Ignore keeps them from coming back. A later retry() lifts it.
    void cancel(const std::vector<std::string>& ids);
    std::set<std::string> active() const;  // the tracks the sync is on right now (active.json; empty when it isn't running)
    // The download order for the runner: picked songs / playlists / genres, then everything else unless "only what I
    // pick" (priority_only). Rewrites queue.json only when it changes (it runs on every refresh, so later playlist
    // additions are picked up).
    void write_queue();
    std::vector<std::string> wanted() const;  // the queue's missing songs, in order (the Wanted tab)
    // Files the sidecar finished go through the organiser (tags, rename, move into Tracks).
    int import_inbox();

private:
    int count(const char* status) const;
    static void close(std::unique_ptr<Process> p);  // waits for its watcher thread, closes the handles
    LibraryStore& store_;
    mutable std::mutex m_;
    std::map<std::string, SyncRecord> records_;
    std::map<std::string, json> overrides_;
    std::vector<std::string> recent_;
    std::optional<unsigned long> external_;
    std::set<std::string> inbox_seen_;
    std::vector<std::string> wanted_;
    mutable std::set<std::string> active_;  // active() reads the file at most once a second
    mutable std::chrono::steady_clock::time_point active_read_{};
    json last_queue_;
    std::mutex refresh_m_;

    std::unique_ptr<Process> proc_;
    std::unique_ptr<slsk::Runner> runner_;  // the built-in client, while it runs
    std::thread native_thread_;
    std::atomic<bool> native_active_{false};
    void join_native();
    std::jthread timer_;
    std::condition_variable_any wake_;
    std::mutex wake_m_;
};

}  // namespace wb::soulseek
