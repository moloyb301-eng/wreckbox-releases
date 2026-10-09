#include "net/slsk/sync.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <random>
#include <regex>
#include <set>
#include <thread>

#include "model/paths.h"

namespace wb::slsk {
namespace fs = std::filesystem;
namespace {

std::string utf8(const fs::path& p) {
    const auto u = p.u8string();
    return {u.begin(), u.end()};
}
fs::path from_utf8(const std::string& s) { return fs::path(widen(s)); }

fs::path work_dir() { return paths::soulseek_dir(); }
fs::path incoming_dir() { return work_dir() / L"incoming"; }
fs::path sync_file() { return work_dir() / L"sync.json"; }
fs::path overrides_file() { return work_dir() / L"overrides.json"; }
fs::path queue_file() { return work_dir() / L"queue.json"; }
fs::path log_file() { return work_dir() / L"sync.log"; }

json read_json(const fs::path& p) {
    const auto text = paths::read_file(p);
    return text ? json::parse(*text, nullptr, false) : json();
}

double hours_since(const std::string& iso) {
    if (iso.empty()) return 1e9;
    std::tm tm{};
    if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%dZ", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) return 1e9;
    tm.tm_year -= 1900, tm.tm_mon -= 1;
    const time_t then = _mkgmtime(&tm);
    return std::difftime(std::time(nullptr), then) / 3600.0;
}

std::string trim(std::string s) {
    s.erase(0, s.find_first_not_of(" \t\r\n"));
    s.erase(s.find_last_not_of(" \t\r\n") + 1);
    return s;
}

// A TOML string at `at` ("…" with \\ and \" escapes): value and the position after it.
std::pair<std::string, size_t> toml_string(const std::string& s, size_t at) {
    std::string out;
    size_t i = at + 1;
    for (; i < s.size() && s[i] != '"'; ++i) out += s[i] == '\\' && i + 1 < s.size() ? s[++i] : s[i];
    return {out, i + 1};
}

class NetworkBackend : public Backend {
public:
    NetworkBackend(const SyncConfig& cfg, unsigned folders, unsigned files) : cfg_(cfg), folders_(folders), files_(files) {}
    std::string login(const std::string& user, const std::string& password) override {
        if (client_) client_->close();
        ClientOptions o;
        o.listen_port = cfg_.listen_port;
        o.server_host = cfg_.server_host, o.server_port = cfg_.server_port;
        o.shared_folders = folders_, o.shared_files = files_;
        client_ = std::make_unique<Client>(o);
        return client_->login(user, password);
    }
    bool connected() const override { return client_ && client_->connected(); }
    void close() override {
        if (client_) client_->close();
    }
    std::vector<UserResult> search(const std::string& q, std::chrono::seconds wait, const std::function<bool()>& cancel) override { return client_->search(q, wait, cancel); }
    DownloadResult download(const std::string& u, const std::string& p, const std::string& d, const DownloadOptions& o) override { return client_->download(u, p, d, o); }

private:
    SyncConfig cfg_;
    unsigned folders_, files_;
    std::unique_ptr<Client> client_;
};

}  // namespace

// MARK: Config

SyncConfig SyncConfig::parse(const std::string& text) {
    SyncConfig c;
    std::string section;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = trim(text.substr(at, end - at));
        at = end + 1;
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        const auto number = [&] { return std::atoi(value.c_str()); };
        const auto text_value = [&] { return value.size() && value[0] == '"' ? toml_string(value, 0).first : value; };
        if (section == "soulseek") {
            if (key == "username") c.username = text_value();
            else if (key == "password") c.password = text_value();
            else if (key == "listen_port") c.listen_port = number();
            else if (key == "server_host") c.server_host = text_value();
            else if (key == "server_port") c.server_port = number();
            else if (key == "share_dirs") {
                c.share_dirs.clear();
                for (size_t i = value.find('"'); i != std::string::npos && i < value.size(); i = value.find('"', i)) {
                    auto [s, next] = toml_string(value, i);
                    c.share_dirs.push_back(s);
                    i = next;
                }
            }
        } else if (section == "sync") {
            struct Field {
                const char* name;
                int SyncConfig::*member;
            };
            static const Field fields[] = {{"interval_minutes", &SyncConfig::interval_minutes},       {"max_concurrent", &SyncConfig::max_concurrent},
                                           {"search_wait_seconds", &SyncConfig::search_wait_seconds}, {"search_gap_seconds", &SyncConfig::search_gap_seconds},
                                           {"queue_timeout_minutes", &SyncConfig::queue_timeout_minutes}, {"stall_timeout_minutes", &SyncConfig::stall_timeout_minutes},
                                           {"candidates_per_track", &SyncConfig::candidates_per_track}, {"retry_after_hours", &SyncConfig::retry_after_hours},
                                           {"max_attempts", &SyncConfig::max_attempts},               {"min_lossy_kbps", &SyncConfig::min_lossy_kbps},
                                           {"duration_tolerance_seconds", &SyncConfig::duration_tolerance_seconds}};
            for (const auto& f : fields)
                if (key == f.name) c.*f.member = number();
            if (key == "prefer_smaller") c.prefer_smaller = value.starts_with("true");
        }
    }
    return c;
}

SyncConfig SyncConfig::load(const fs::path& file) { return parse(paths::read_file(file).value_or("")); }

std::unique_ptr<Backend> make_network_backend(const SyncConfig& cfg, unsigned folders, unsigned files) { return std::make_unique<NetworkBackend>(cfg, folders, files); }

// MARK: Runner

Runner::Runner(LibraryStore& store, SyncConfig cfg, std::unique_ptr<Backend> backend) : store_(store), cfg_(std::move(cfg)), backend_(std::move(backend)) {
    sync_ = read_json(sync_file());
    if (!sync_.is_object()) sync_ = json::object();
}

Runner::~Runner() {
    stop_ = true;
    if (lock_file_) CloseHandle(static_cast<HANDLE>(lock_file_));
}

void Runner::stop() { stop_ = true; }

void Runner::log_line(const std::string& message) {
    std::lock_guard lock(log_m_);
    std::error_code ec;
    fs::create_directories(work_dir(), ec);
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);
    std::ofstream(log_file(), std::ios::app | std::ios::binary) << stamp << " " << message << "\n";
}

bool Runner::acquire_lock() {
    std::error_code ec;
    fs::create_directories(work_dir(), ec);
    HANDLE h = CreateFileW((work_dir() / L"sync.lock").c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    OVERLAPPED ov{};
    if (h == INVALID_HANDLE_VALUE || !LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        const std::string pid = paths::read_file(work_dir() / L"sync.pid").value_or("?");
        log_line("✗ slsk-sync is already running (pid " + trim(pid) + ") — not starting a second copy.");
        return false;
    }
    lock_file_ = h;
    paths::write_atomic(work_dir() / L"sync.pid", std::to_string(GetCurrentProcessId()));
    return true;
}

bool Runner::login() {
    if (cfg_.username.empty() || cfg_.password.empty()) {
        log_line("✗ Add your Soulseek username and password to " + utf8(paths::settings_dir() / L"soulseek.toml"));
        return false;
    }
    if (!lock_file_ && !acquire_lock()) return false;
    std::error_code ec;
    fs::create_directories(incoming_dir(), ec);
    for (fs::recursive_directory_iterator it(incoming_dir(), ec), end; !ec && it != end; it.increment(ec))  // partial files from a crash
        if (it->is_regular_file(ec)) fs::remove(it->path(), ec);
    fs::create_directories(paths::inbox(), ec);
    const std::string err = backend_->login(cfg_.username, cfg_.password);
    if (!err.empty()) {
        log_line("✗ " + err);
        return false;
    }
    log_line("Logged in to Soulseek as " + cfg_.username);
    return true;
}

std::vector<LibraryTrack> Runner::missing_tracks() const {
    const auto lib = store_.library();
    std::vector<LibraryTrack> out;
    if (!lib) return out;
    const auto state = store_.state_copy();
    const json overrides = read_json(overrides_file());
    json sync;
    {
        std::lock_guard lock(const_cast<std::mutex&>(sync_m_));
        sync = sync_;
    }
    std::set<std::string> inbox_stems, retried;
    std::error_code ec;
    for (fs::directory_iterator it(paths::inbox(), ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) inbox_stems.insert(utf8(it->path().stem()));
    for (const auto& t : lib->tracks) {
        if (const auto st = state.tracks.find(t.id); st != state.tracks.end() && (st->second.status == TrackStatus::downloaded || st->second.status == TrackStatus::ignored)) continue;
        const json rec = sync.contains(t.id) && sync[t.id].is_object() ? sync[t.id] : json::object();
        if (inbox_stems.contains(t.file_name)) continue;
        const std::string retry_at = overrides.is_object() && overrides.contains(t.id) && overrides[t.id].is_object() ? overrides[t.id].value("retryAt", std::string()) : "";
        const std::string last_try = rec.contains("last_try") && rec["last_try"].is_string() ? rec["last_try"].get<std::string>() : "";
        if (retry_at > last_try) {  // "Retry" in the app: a request newer than the last attempt makes it due right away
            retried.insert(t.id);
            out.push_back(t);
            continue;
        }
        const std::string status = rec.value("status", std::string());
        if (status == "done") continue;
        if (status == "failed" || status == "not_found")
            if (rec.value("attempts", 0) >= cfg_.max_attempts || hours_since(last_try) < cfg_.retry_after_hours) continue;
        out.push_back(t);
    }
    // Newest additions first, so fresh playlist adds arrive quickly.
    std::stable_sort(out.begin(), out.end(), [](const LibraryTrack& a, const LibraryTrack& b) { return a.first_added.value_or("") > b.first_added.value_or(""); });
    // The app's Download queue (playlist / genre priorities) overrides that order.
    const json queue = read_json(queue_file());
    if (queue.is_object() && queue.contains("ids") && queue["ids"].is_array()) {
        std::map<std::string, size_t> rank;
        for (size_t i = 0; i < queue["ids"].size(); ++i) rank.emplace(queue["ids"][i].get<std::string>(), i);
        if (queue.value("onlyPriority", false))  // only what was picked (nothing picked: nothing); explicit retries still run
            out.erase(std::remove_if(out.begin(), out.end(), [&](const LibraryTrack& t) { return !rank.contains(t.id) && !retried.contains(t.id); }), out.end());
        std::stable_sort(out.begin(), out.end(), [&](const LibraryTrack& a, const LibraryTrack& b) {
            const auto ra = rank.find(a.id), rb = rank.find(b.id);
            return (ra == rank.end() ? rank.size() : ra->second) < (rb == rank.end() ? rank.size() : rb->second);
        });
    }
    std::stable_sort(out.begin(), out.end(), [&](const LibraryTrack& a, const LibraryTrack& b) { return retried.contains(a.id) && !retried.contains(b.id); });  // explicit retries first
    return out;
}

void Runner::mark(const LibraryTrack& track, const std::string& status, const json& extra) {
    std::lock_guard lock(sync_m_);
    json& rec = sync_[track.id];
    if (!rec.is_object()) rec = json::object();
    if (status == "failed" || status == "not_found") rec["attempts"] = rec.value("attempts", 0) + 1;
    rec["status"] = status;
    rec["last_try"] = iso_seconds_now();
    rec["name"] = track.file_name;
    for (const auto& [k, v] : extra.items()) rec[k] = v;
    std::error_code ec;
    fs::create_directories(work_dir(), ec);
    paths::write_atomic(sync_file(), sync_.dump(2));
}

std::vector<UserResult> Runner::search(const std::string& query) {
    {
        std::lock_guard lock(search_m_);  // space searches out to stay under the server's limits
        const auto wait = std::chrono::seconds(cfg_.search_gap_seconds) - (std::chrono::steady_clock::now() - last_search_);
        if (wait > std::chrono::seconds(0) && last_search_.time_since_epoch().count() != 0) {
            const auto end = std::chrono::steady_clock::now() + wait;
            while (std::chrono::steady_clock::now() < end && !stop_) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        last_search_ = std::chrono::steady_clock::now();
    }
    return backend_->search(query, std::chrono::seconds(cfg_.search_wait_seconds), [this] { return stop_.load(); });
}

std::pair<std::vector<Candidate>, json> Runner::find(const LibraryTrack& track) {
    json info = {{"queries", json::array()}, {"filesSeen", 0}, {"usersSeen", 0}};
    const json overrides = read_json(overrides_file());
    std::string custom;
    if (overrides.is_object() && overrides.contains(track.id) && overrides[track.id].is_object() && overrides[track.id].contains("query") && overrides[track.id]["query"].is_string())
        custom = trim(overrides[track.id]["query"].get<std::string>());
    MatchConfig mc;
    mc.min_lossy_kbps = cfg_.min_lossy_kbps, mc.duration_tolerance_seconds = cfg_.duration_tolerance_seconds, mc.prefer_smaller = cfg_.prefer_smaller;
    for (const auto& q : search_queries(track, custom)) {
        if (stop_) break;
        const auto results = search(q);
        info["queries"].push_back(q);
        size_t files = 0;
        for (const auto& r : results) files += r.files.size();
        info["filesSeen"] = info["filesSeen"].get<size_t>() + files;
        info["usersSeen"] = info["usersSeen"].get<size_t>() + results.size();
        // A custom query is the user's own wording: trust it for the title / artist check, keep the quality rules.
        const bool loose = !custom.empty() && info["queries"].size() == 1;
        auto cands = rank(track, results, mc, loose ? std::optional<std::string>(q) : std::nullopt);
        if (!cands.empty()) return {std::move(cands), info};
    }
    return {{}, info};
}

std::optional<fs::path> Runner::download(const LibraryTrack& track, const Candidate& c) {
    std::error_code ec;
    fs::create_directories(incoming_dir(), ec);
    static std::atomic<unsigned> counter{0};
    const fs::path part = incoming_dir() / (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++counter) + L".part");
    DownloadOptions o;
    o.queue_timeout = std::chrono::minutes(cfg_.queue_timeout_minutes);
    o.stall_timeout = std::chrono::minutes(cfg_.stall_timeout_minutes);
    o.cancel = [this] { return stop_.load(); };
    const auto res = backend_->download(c.username, c.path, utf8(part), o);
    if (!res.ok) {
        log_line("  ✗ " + c.username + ": " + res.error);
        fs::remove(part, ec);
        return std::nullopt;
    }
    const auto size = fs::exists(part, ec) ? fs::file_size(part, ec) : 0;
    if (double(size) < 0.9 * double(c.size)) {
        log_line("  ✗ " + c.username + ": incomplete file");
        fs::remove(part, ec);
        return std::nullopt;
    }
    fs::path dest = paths::inbox() / from_utf8(track.file_name + "." + c.ext);
    for (int n = 2; fs::exists(dest, ec); ++n) dest = paths::inbox() / from_utf8(track.file_name + " (" + std::to_string(n) + ")." + c.ext);
    fs::rename(part, dest, ec);  // same volume: atomic, so the app never sees a partial file
    if (ec) {
        log_line("  ✗ " + c.username + ": can't move the file into _inbox (" + ec.message() + ")");
        fs::remove(part, ec);
        return std::nullopt;
    }
    return dest;
}

std::string Runner::process(const LibraryTrack& track) {
    std::string who;
    for (size_t i = 0; i < track.artists.size(); ++i) who += (i ? ", " : "") + track.artists[i];
    who += " – " + track.title;
    auto [cands, info] = find(track);
    const size_t files = info["filesSeen"].get<size_t>(), users = info["usersSeen"].get<size_t>();
    const std::string reason = files == 0 ? "no results" : std::to_string(files) + " files from " + std::to_string(users) + " users, none matched";
    if (stop_) return "failed";
    if (cands.empty()) {
        log_line("· not found: " + who + " (" + reason + ")");
        mark(track, "not_found", {{"reason", reason}, {"queries", info["queries"]}});
        return "not_found";
    }
    const size_t tries = std::min<size_t>(cands.size(), size_t(cfg_.candidates_per_track));
    for (size_t i = 0; i < tries && !stop_; ++i) {
        const Candidate& c = cands[i];
        log_line("↓ " + who + "  ←  " + c.label());
        if (const auto dest = download(track, c)) {
            const std::string name = utf8(dest->filename());
            log_line("✓ " + who + " → _inbox/" + name);
            mark(track, "done",
                 {{"file", name}, {"source", c.username + ":" + c.path}, {"format", c.ext}, {"bitrate", c.bitrate ? json(*c.bitrate) : json(nullptr)}, {"sizeBytes", c.size},
                  {"queries", info["queries"]}, {"reason", nullptr}});
            return "done";
        }
    }
    if (stop_) return "failed";
    mark(track, "failed", {{"reason", std::to_string(tries) + " sources tried, none delivered"}, {"queries", info["queries"]}});
    return "failed";
}

PassCounts Runner::run_pass(std::optional<int> limit) {
    auto tracks = missing_tracks();
    std::error_code ec;
    if (fs::exists(queue_file(), ec)) log_line("Following the app's download queue");
    if (limit && size_t(*limit) < tracks.size()) tracks.resize(size_t(*limit));
    log_line("Pass: " + std::to_string(tracks.size()) + " tracks to look for");
    PassCounts counts;
    std::mutex cm, qm;
    size_t next = 0;
    const auto worker = [&] {
        for (;;) {
            LibraryTrack t;
            {
                std::lock_guard lock(qm);
                if (next >= tracks.size() || stop_) return;
                t = tracks[next++];
            }
            std::string result;
            try {
                result = process(t);
            } catch (const std::exception& e) {  // keep the pass going if one track blows up
                log_line("error on " + t.file_name + ": " + e.what());
                mark(t, "failed", {{"error", e.what()}});
                result = "failed";
            }
            std::lock_guard lock(cm);
            if (result == "done") ++counts.done;
            else if (result == "not_found") ++counts.not_found;
            else ++counts.failed;
        }
    };
    std::vector<std::thread> workers;
    for (int i = 0; i < std::max(1, cfg_.max_concurrent); ++i) workers.emplace_back(worker);
    for (auto& w : workers) w.join();
    log_line("Pass finished: " + std::to_string(counts.done) + " downloaded, " + std::to_string(counts.not_found) + " not found, " + std::to_string(counts.failed) + " failed");
    return counts;
}

void Runner::sleep_until_nudged(std::chrono::seconds total) {
    const auto stamp = [] {
        std::error_code ec;
        const auto t = [&](const fs::path& p) { return fs::exists(p, ec) ? fs::last_write_time(p, ec).time_since_epoch().count() : 0; };
        return std::pair(t(overrides_file()), t(queue_file()));
    };
    const auto before = stamp();
    const auto start = std::chrono::steady_clock::now();
    while (!stop_ && std::chrono::steady_clock::now() - start < total) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        if (stamp() != before) {
            log_line("Queue or retry requests changed — starting a new pass");
            return;
        }
    }
}

void Runner::run(bool once, std::optional<int> limit) {
    if (!login()) return;
    while (!stop_) {
        if (!backend_->connected()) {
            log_line("Reconnecting to Soulseek");
            if (!login()) break;
        }
        run_pass(limit);
        if (once || stop_) break;
        log_line("Sleeping " + std::to_string(cfg_.interval_minutes) + " min (checks the library again for new playlist tracks)");
        sleep_until_nudged(std::chrono::minutes(cfg_.interval_minutes));
    }
    backend_->close();
}

}  // namespace wb::slsk
