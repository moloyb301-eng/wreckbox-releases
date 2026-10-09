// The Soulseek sync loop, native (a port of Syncer in sidecar/slsk_sync.py): looks for the library's missing tracks on
// Soulseek, downloads the best match into _inbox and keeps the same files the sidecar keeps (sync.json, sync.log, sync.pid),
// so the Soulseek page, the queue and the retries work unchanged. The network is behind `Backend`, so the loop is tested
// without it. While a track is being worked on it is listed in active.json; a `cancelAt` in overrides.json newer than
// its start stops it (search or transfer) without counting as a failed try.
#pragma once
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "library/store.h"
#include "net/slsk/client.h"
#include "net/slsk/match.h"

namespace wb::slsk {

struct SyncConfig {
    std::string username, password;
    int listen_port = 60000;
    std::string server_host = "server.slsknet.org";  // (server_host / server_port in soulseek.toml: for tests)
    int server_port = 2242;
    std::vector<std::string> share_dirs;
    int interval_minutes = 30, max_concurrent = 3, search_wait_seconds = 12, search_gap_seconds = 4, queue_timeout_minutes = 4, stall_timeout_minutes = 3,
        candidates_per_track = 4, retry_after_hours = 24, max_attempts = 5, min_lossy_kbps = 256, duration_tolerance_seconds = 5;
    bool prefer_smaller = false;

    // soulseek.toml (the format the app writes); missing keys keep the defaults.
    static SyncConfig parse(const std::string& toml_text);
    static SyncConfig load(const std::filesystem::path& file);
};

// What the loop needs from the network.
class Backend {
public:
    virtual ~Backend() = default;
    virtual std::string login(const std::string& user, const std::string& password) = 0;  // "" or why not
    virtual bool connected() const = 0;
    virtual void close() = 0;
    virtual std::vector<UserResult> search(const std::string& query, std::chrono::seconds wait, const std::function<bool()>& cancel) = 0;
    virtual DownloadResult download(const std::string& user, const std::string& path, const std::string& dest, const DownloadOptions& options) = 0;
};
std::unique_ptr<Backend> make_network_backend(const SyncConfig& cfg, unsigned shared_folders, unsigned shared_files);

struct PassCounts {
    int done = 0, failed = 0, not_found = 0;
};

class Runner {
public:
    Runner(LibraryStore& store, SyncConfig cfg, std::unique_ptr<Backend> backend);
    ~Runner();

    // Logs in, then passes with a sleep in between, until stop(). Returns when stopped or if the login fails.
    void run(bool once = false, std::optional<int> limit = std::nullopt);
    void stop();
    bool stopping() const { return stop_; }

    PassCounts run_pass(std::optional<int> limit = std::nullopt);  // login first (run() does it); public for tests
    bool login();                                                   // false after logging why

    // The tracks the app hasn't got that are due for a try, in the order they'll be tried.
    std::vector<LibraryTrack> missing_tracks() const;

    // Lines go to sync.log with a timestamp, as the sidecar writes them.
    void log_line(const std::string& message);

private:
    std::string process(const LibraryTrack& track);  // "done" | "not_found" | "failed" | "cancelled"
    std::optional<std::filesystem::path> download(const LibraryTrack& track, const Candidate& c, const std::string& since);
    bool cancelled(const std::string& id, const std::string& since);  // the app asked to stop it (read at most once a second)
    void set_active(const LibraryTrack& track, std::optional<std::string> started);  // active.json
    void mark(const LibraryTrack& track, const std::string& status, const json& extra = json::object());
    std::pair<std::vector<Candidate>, json> find(const LibraryTrack& track);
    std::vector<UserResult> search(const std::string& query);
    void sleep_until_nudged(std::chrono::seconds total);
    bool acquire_lock();

    LibraryStore& store_;
    SyncConfig cfg_;
    std::unique_ptr<Backend> backend_;
    std::atomic<bool> stop_{false};
    std::mutex sync_m_;
    json sync_ = json::object();  // sync.json
    std::mutex search_m_;
    std::chrono::steady_clock::time_point last_search_{};
    std::mutex log_m_;
    void* lock_file_ = nullptr;  // the sync.lock handle
    std::mutex cancel_m_;
    std::map<std::string, std::string> cancel_at_;
    std::chrono::steady_clock::time_point cancel_read_{};
    std::mutex active_m_;
    json active_ = json::object();
};

}  // namespace wb::slsk
