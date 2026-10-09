#include "net/soulseek.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <set>

#include "model/paths.h"
#include "model/settings.h"
#include "net/slsk/sync.h"

namespace wb::soulseek {
namespace fs = std::filesystem;
namespace {

std::string to_utf8(const fs::path& p) {
    const auto u = p.u8string();
    return {u.begin(), u.end()};
}

std::string quote(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out + "\"";
}

// `key = "value"` from a TOML line, unescaped; nullopt if the line isn't that key.
std::optional<std::string> toml_string(const std::string& line, const std::string& key) {
    const std::regex re("^" + key + R"re(\s*=\s*"((?:[^"\\]|\\.)*)")re");
    std::smatch m;
    if (!std::regex_search(line, m, re)) return std::nullopt;
    std::string out;
    const std::string raw = m[1];
    for (size_t i = 0; i < raw.size(); ++i) out += raw[i] == '\\' && i + 1 < raw.size() ? raw[++i] : raw[i];
    return out;
}

std::string config_text() { return paths::read_file(Sync::config_file()).value_or(""); }

std::optional<std::string> config_value(const std::string& key) {
    std::string text = config_text(), line;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (const auto v = toml_string(line, key)) return v;
            line.clear();
        } else {
            line += text[i];
        }
    }
    return std::nullopt;
}

json read_json(const fs::path& p) {
    const auto text = paths::read_file(p);
    return text ? json::parse(*text, nullptr, false) : json();
}

bool process_alive(unsigned long pid, bool must_be_python) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    DWORD code = 0;
    bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    if (alive && must_be_python) {
        wchar_t name[MAX_PATH];
        DWORD n = MAX_PATH;
        std::wstring image = QueryFullProcessImageNameW(h, 0, name, &n) ? std::wstring(name, n) : L"";
        std::transform(image.begin(), image.end(), image.begin(), ::towlower);
        alive = image.find(L"python") != std::wstring::npos;
    }
    CloseHandle(h);
    return alive;
}

}  // namespace

SyncRecord SyncRecord::from_json(const json& j) {
    SyncRecord r;
    const auto str = [&](const char* k) -> std::optional<std::string> {
        const auto it = j.find(k);
        return it != j.end() && it->is_string() ? std::optional<std::string>(it->get<std::string>()) : std::nullopt;
    };
    r.status = str("status").value_or("");
    r.last_try = str("last_try").value_or("");
    r.attempts = j.value("attempts", 0);
    r.reason = str("reason");
    r.format = str("format");
    r.source = str("source");
    if (const auto it = j.find("queries"); it != j.end() && it->is_array())
        for (const auto& q : *it)
            if (q.is_string()) r.queries.push_back(q.get<std::string>());
    return r;
}

struct Process {
    HANDLE process = nullptr, job = nullptr;
    std::thread waiter;  // waits for the process to end, then says so
    std::atomic<bool> ended{false};
};

void Sync::close(std::unique_ptr<Process> p) {
    if (!p) return;
    TerminateProcess(p->process, 1);  // no-op once it has ended; the job would kill it too
    if (p->waiter.joinable()) p->waiter.join();
    CloseHandle(p->process);
    CloseHandle(p->job);
}

Sync::Sync(LibraryStore& store) : store_(store) {}

Sync::~Sync() {
    if (timer_.joinable()) {
        timer_.request_stop();
        wake_.notify_all();
        timer_.join();
    }
    if (runner_) runner_->stop();
    join_native();
    std::unique_ptr<Process> p;
    {
        std::lock_guard lock(m_);
        p = std::move(proc_);
    }
    close(std::move(p));
}

fs::path Sync::sidecar_dir() {
    if (const char* v = std::getenv("WRECKBOX_SLSK_DIR"); v && *v) return fs::path(widen(v));
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return fs::path(exe).parent_path() / L"soulseek";
}
fs::path Sync::python() {
    if (const char* v = std::getenv("WRECKBOX_SLSK_PYTHON"); v && *v) return fs::path(widen(v));
    return sidecar_dir() / L"python" / L"python.exe";
}
fs::path Sync::script() { return sidecar_dir() / L"slsk_sync.py"; }
fs::path Sync::config_file() { return paths::settings_dir() / L"soulseek.toml"; }

bool Sync::sidecar_present() {
    std::error_code ec;
    return fs::exists(python(), ec) && fs::exists(script(), ec);
}

bool Sync::use_native() {
    if (const char* v = std::getenv("WRECKBOX_SLSK_NATIVE"); v && *v) return *v != '0';
    return Settings::current().extra.value("soulseekNative", false) || !sidecar_present();
}

void Sync::set_use_native(bool on) {
    Settings::current().extra["soulseekNative"] = on;
    try {
        Settings::current().save();
    } catch (const std::exception&) {
        // Kept for this run.
    }
}

bool Sync::available() { return use_native() || sidecar_present(); }

void Sync::join_native() {
    if (native_thread_.joinable()) native_thread_.join();
}

bool Sync::configured() const {
    const auto u = config_value("username"), p = config_value("password");
    return u && !u->empty() && p && !p->empty();
}
std::string Sync::username() const { return config_value("username").value_or(""); }

void Sync::save_login(const std::string& username, const std::string& password, bool share_tracks) const {
    const std::string share = share_tracks ? "[" + quote(to_utf8(paths::tracks())) + "]" : "[]";
    paths::write_atomic(config_file(), "[soulseek]\nusername = " + quote(username) + "\npassword = " + quote(password) +
                                           "\nlisten_port = 60000\nshare_dirs = " + share +
                                           "\n\n[sync]\ninterval_minutes = 30\nmax_concurrent = 3\nmin_lossy_kbps = 256\nprefer_smaller = " +
                                           (prefer_smaller() ? "true" : "false") + "\n");
}

bool Sync::prefer_smaller() const { return slsk::SyncConfig::load(config_file()).prefer_smaller; }

void Sync::set_prefer_smaller(bool on) {
    // Rewrites just that line of soulseek.toml (in [sync]); a running sync restarts to use it.
    std::string text = std::regex_replace(config_text(), std::regex(R"((^|\n)prefer_smaller *=[^\n]*)"), "");
    const std::string line = std::string("prefer_smaller = ") + (on ? "true" : "false") + "\n";
    if (const auto at = text.find("[sync]"); at != std::string::npos) text.insert(text.find('\n', at) == std::string::npos ? text.size() : text.find('\n', at) + 1, line);
    else text += "\n[sync]\n" + line;
    paths::write_atomic(config_file(), text);
    if (running()) {
        stop();
        start();
    }
}

// MARK: Watching

void Sync::start_watching(std::chrono::seconds every) {
    if (timer_.joinable()) return;
    timer_ = std::jthread([this, every](std::stop_token stop) {
        while (!stop.stop_requested()) {
            refresh();
            std::unique_lock lock(wake_m_);
            wake_.wait_for(lock, stop, every, [] { return false; });
        }
    });
}

void Sync::refresh() {
    std::lock_guard run(refresh_m_);
    const fs::path dir = paths::soulseek_dir();
    std::map<std::string, SyncRecord> records;
    std::map<std::string, json> overrides;
    if (const json j = read_json(dir / L"sync.json"); j.is_object())
        for (const auto& [k, v] : j.items())
            if (v.is_object()) records[k] = SyncRecord::from_json(v);
    if (const json j = read_json(dir / L"overrides.json"); j.is_object())
        for (const auto& [k, v] : j.items())
            if (v.is_object()) overrides[k] = v;

    std::vector<std::string> recent;
    {   // the end of the log (it only grows)
        std::ifstream f(dir / L"sync.log", std::ios::binary | std::ios::ate);
        if (f) {
            const auto len = std::streamoff(f.tellg());
            const auto start = len > 65536 ? len - 65536 : 0;
            std::string text(size_t(len - start), '\0');
            f.seekg(start);
            f.read(text.data(), std::streamsize(text.size()));
            std::vector<std::string> lines;
            for (size_t i = 0, from = 0; i <= text.size(); ++i)
                if (i == text.size() || text[i] == '\n') {
                    std::string line = text.substr(from, i - from);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.find_first_not_of(" \t") != std::string::npos) lines.push_back(std::move(line));
                    from = i + 1;
                }
            for (auto it = lines.rbegin(); it != lines.rend() && recent.size() < 40; ++it) recent.push_back(*it);
        }
    }
    std::optional<unsigned long> external;
    {
        std::lock_guard lock(m_);
        if (!proc_ || proc_->ended)
            if (const auto text = paths::read_file(dir / L"sync.pid")) {
                const unsigned long pid = std::strtoul(text->c_str(), nullptr, 10);
                if (pid && process_alive(pid, true)) external = pid;
            }
    }
    {
        std::lock_guard lock(m_);
        records_ = std::move(records);
        overrides_ = std::move(overrides);
        recent_ = std::move(recent);
        external_ = external;
    }
    import_inbox();
    if (!store_.busy()) write_queue();  // picks the playlists' new songs up; a no-op when nothing changed
    if (on_changed) on_changed();
}

int Sync::import_inbox() {
    const auto lib = store_.library();
    std::error_code ec;
    if (!lib || store_.busy() || !fs::exists(paths::inbox(), ec)) return 0;
    std::map<std::string, std::string> by_name;
    for (const auto& t : lib->tracks) by_name[t.file_name] = t.id;
    int added = 0;
    for (fs::directory_iterator it(paths::inbox(), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || !paths::is_audio(it->path())) continue;
        const std::string key = to_utf8(it->path()) + "|" + std::to_string(it->file_size(ec));
        {
            std::lock_guard lock(m_);
            if (!inbox_seen_.insert(key).second) continue;
        }
        const std::string stem = narrow(it->path().stem().wstring());
        const std::string base = std::regex_replace(stem, std::regex(R"( \(\d+\)$)"), "");
        std::optional<std::string> id;
        if (const auto a = by_name.find(stem); a != by_name.end()) id = a->second;
        else if (const auto b = by_name.find(base); b != by_name.end()) id = b->second;
        if (store_.organise(to_utf8(it->path()), "soulseek", id).rfind("Added", 0) == 0) ++added;
    }
    return added;
}

// MARK: Running the sidecar

std::string Sync::start() {
    if (running()) return "";
    {   // a process that ended on its own is still held until here
        std::unique_ptr<Process> old;
        {
            std::lock_guard lock(m_);
            old = std::move(proc_);
        }
        close(std::move(old));
    }
    if (!available()) return "The Soulseek component isn't installed next to the app.";
    write_queue();
    if (!configured()) return "Add your Soulseek username and password first.";
    std::error_code ec;
    fs::create_directories(paths::soulseek_dir(), ec);
    if (use_native()) {  // the built-in client: the same loop on a thread of ours
        join_native();
        auto cfg = slsk::SyncConfig::load(config_file());
        runner_ = std::make_unique<slsk::Runner>(store_, cfg, slsk::make_network_backend(cfg, 0, 0));
        native_active_ = true;
        native_thread_ = std::thread([this] {
            runner_->run();
            native_active_ = false;
            refresh();  // the results, and that it has stopped
        });
        store_.log("soulseek", "sync started (built-in client)");
        store_.save();
        if (on_changed) on_changed();
        return "";
    }

    // The current environment plus what the sidecar reads. Our own values replace any inherited ones: a block holding a
    // name twice gives the child the first, so a PYTHONIOENCODING set on this PC would otherwise win over utf-8.
    const std::wstring ours[] = {L"WRECKBOX_ROOT=" + paths::root().wstring(), L"WRECKBOX_SLSK_CONFIG=" + config_file().wstring(),
                                 L"PYTHONIOENCODING=utf-8"};
    auto overridden = [&](const wchar_t* e) {
        for (const auto& o : ours) {
            const size_t name = o.find(L'=') + 1;  // compare "NAME=" case-insensitively, as Windows does
            if (wcslen(e) >= name && _wcsnicmp(e, o.c_str(), name) == 0) return true;
        }
        return false;
    };
    std::wstring env;
    if (wchar_t* block = GetEnvironmentStringsW()) {
        for (const wchar_t* e = block; *e; e += wcslen(e) + 1)
            if (!overridden(e)) env.append(e, wcslen(e) + 1);
        FreeEnvironmentStringsW(block);
    }
    for (const std::wstring& add : ours) env.append(add.c_str(), add.size() + 1);
    env.push_back(L'\0');

    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = si.hStdOutput = si.hStdError = nul;
    std::wstring cmd = L"\"" + python().wstring() + L"\" \"" + script().wstring() + L"\" run";
    HANDLE job = CreateJobObjectW(nullptr, nullptr);  // dies with WreckBox, even on a crash
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{};
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof lim);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, env.data(),
                                   sidecar_dir().c_str(), &si, &pi);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(job);
        return "Couldn't start the Soulseek component.";
    }
    AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    auto p = std::make_unique<Process>();
    p->process = pi.hProcess;
    p->job = job;
    Process* raw = p.get();
    p->waiter = std::thread([this, raw] {
        WaitForSingleObject(raw->process, INFINITE);
        raw->ended = true;
        refresh();  // reads what it left and the running state
    });
    {
        std::lock_guard lock(m_);
        proc_ = std::move(p);
    }
    store_.log("soulseek", "sync started");
    store_.save();
    if (on_changed) on_changed();
    return "";
}

void Sync::stop() {
    {
        std::lock_guard lock(m_);
        if (proc_ && !proc_->ended) TerminateProcess(proc_->process, 1);
        if (runner_ && native_active_) runner_->stop();
        if (external_)
            if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, *external_)) {
                TerminateProcess(h, 1);
                CloseHandle(h);
            }
    }
    store_.log("soulseek", "sync stopped");
    store_.save();
    if (on_changed) on_changed();
}

void Sync::stop_process() {
    std::lock_guard lock(m_);
    if (proc_ && !proc_->ended) TerminateProcess(proc_->process, 1);
    if (runner_ && native_active_) runner_->stop();
}

bool Sync::running() const {
    std::lock_guard lock(m_);
    return (proc_ && !proc_->ended) || native_active_ || external_.has_value();
}
std::optional<unsigned long> Sync::external_pid() const {
    std::lock_guard lock(m_);
    return external_;
}

// MARK: Results

std::map<std::string, SyncRecord> Sync::records() const {
    std::lock_guard lock(m_);
    return records_;
}
std::vector<std::string> Sync::recent() const {
    std::lock_guard lock(m_);
    return recent_;
}
int Sync::count(const char* status) const {
    std::lock_guard lock(m_);
    return int(std::count_if(records_.begin(), records_.end(), [&](const auto& r) { return r.second.status == status; }));
}

void Sync::retry(const std::vector<std::string>& ids, const std::optional<std::string>& query) {
    const fs::path file = paths::soulseek_dir() / L"overrides.json";
    json all = read_json(file);
    if (!all.is_object()) all = json::object();
    const std::string now = iso_seconds_now();
    for (const auto& id : ids) {
        json o = all.contains(id) && all[id].is_object() ? all[id] : json::object();
        o["retryAt"] = now;
        o.erase("cancelAt");
        if (query) {
            std::string q = *query;
            q.erase(0, q.find_first_not_of(" \t\r\n"));
            q.erase(q.find_last_not_of(" \t\r\n") + 1);
            if (q.empty()) o.erase("query");
            else o["query"] = q;
        }
        all[id] = o;
    }
    std::error_code ec;
    fs::create_directories(paths::soulseek_dir(), ec);
    paths::write_atomic(file, all.dump(2));
    refresh();
}

void Sync::cancel(const std::vector<std::string>& ids) {
    const fs::path file = paths::soulseek_dir() / L"overrides.json";
    json all = read_json(file);
    if (!all.is_object()) all = json::object();
    const std::string now = iso_seconds_now();
    for (const auto& id : ids) {
        json o = all.contains(id) && all[id].is_object() ? all[id] : json::object();
        o["cancelAt"] = now;
        all[id] = o;
    }
    std::error_code ec;
    fs::create_directories(paths::soulseek_dir(), ec);
    paths::write_atomic(file, all.dump(2));
    refresh();
}

std::set<std::string> Sync::active() const {
    if (!running()) return {};
    std::lock_guard lock(m_);
    if (std::chrono::steady_clock::now() - active_read_ > std::chrono::seconds(1)) {
        active_read_ = std::chrono::steady_clock::now();
        active_.clear();
        if (const json j = read_json(paths::soulseek_dir() / L"active.json"); j.is_object())
            for (const auto& [k, v] : j.items()) active_.insert(k);
    }
    return active_;
}

bool Sync::retry_pending(const std::string& id) const {
    std::lock_guard lock(m_);
    const auto o = overrides_.find(id);
    if (o == overrides_.end()) return false;
    const auto r = records_.find(id);
    return o->second.value("retryAt", std::string()) > (r == records_.end() ? std::string() : r->second.last_try);
}

void Sync::write_queue() {
    const auto lib = store_.library();
    if (!lib) return;
    const auto state = store_.state_copy();
    std::vector<const LibraryTrack*> missing;
    for (const auto& t : lib->tracks) {
        const auto st = state.tracks.find(t.id);
        if (st == state.tracks.end() || st->second.status == TrackStatus::missing) missing.push_back(&t);
    }
    std::stable_sort(missing.begin(), missing.end(), [](const LibraryTrack* a, const LibraryTrack* b) { return a->first_added.value_or("") > b->first_added.value_or(""); });
    std::set<std::string> missing_ids, seen;
    for (const auto* t : missing) missing_ids.insert(t->id);
    json ids = json::array();
    auto add = [&](const std::string& id) {
        if (seen.insert(id).second) ids.push_back(id);
    };
    for (const auto& key : state.download_priority) {
        const auto colon = key.find(':');
        if (colon == std::string::npos) continue;
        const std::string kind = key.substr(0, colon), name = key.substr(colon + 1);
        if (kind == "playlist") {
            for (const auto& pl : lib->playlists)
                if (pl.name == name) {
                    for (const auto& id : pl.track_ids)
                        if (missing_ids.contains(id)) add(id);
                    break;
                }
        } else if (kind == "track") {
            if (missing_ids.contains(name)) add(name);
        } else if (kind == "genre") {
            for (const auto* t : missing)
                if (const auto g = state.genre_overrides.find(t->id); g != state.genre_overrides.end() && g->second == name) add(t->id);
        }
    }
    if (!state.priority_only)
        for (const auto* t : missing) add(t->id);
    json body{{"onlyPriority", state.priority_only}, {"priorities", state.download_priority}, {"ids", ids}};
    const fs::path file = paths::soulseek_dir() / L"queue.json";
    {
        std::lock_guard lock(m_);
        wanted_ = ids.get<std::vector<std::string>>();
        std::error_code ec;
        if (body == last_queue_ && fs::exists(file, ec)) return;  // unchanged: don't wake the runner
        last_queue_ = body;
    }
    std::error_code ec;
    fs::create_directories(paths::soulseek_dir(), ec);
    body["generatedAt"] = iso_seconds_now();
    paths::write_atomic(file, body.dump());
}

std::vector<std::string> Sync::wanted() const {
    std::lock_guard lock(m_);
    return wanted_;
}

}  // namespace wb::soulseek
