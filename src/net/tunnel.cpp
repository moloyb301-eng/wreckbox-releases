#include "net/tunnel.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <regex>
#include <thread>
#include <utility>
#include <vector>

#include "model/paths.h"
#include "net/account.h"
#include "net/http_client.h"

namespace wb::sync {
namespace fs = std::filesystem;
namespace {

struct Handle {
    HANDLE h = nullptr;
    Handle() = default;
    explicit Handle(HANDLE x) : h(x == INVALID_HANDLE_VALUE ? nullptr : x) {}
    Handle(const Handle&) = delete;
    Handle& operator=(Handle&& o) noexcept {
        reset();
        h = std::exchange(o.h, nullptr);
        return *this;
    }
    ~Handle() { reset(); }
    void reset() {
        if (h) CloseHandle(h);
        h = nullptr;
    }
    explicit operator bool() const { return h != nullptr; }
};

std::optional<fs::path> download_binary(const std::function<void(const std::string&)>& say) {
    const fs::path file = Tunnel::binary_path();
    std::error_code ec;
    if (fs::exists(file, ec)) return file;
    say("Downloading Cloudflare tunnel tool…");
    const auto r = http::get("https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-windows-amd64.exe", {},
                             std::chrono::minutes(10));
    if (r.status == 0 || !r.ok()) {
        say("Couldn't connect: download failed (" + (r.status ? std::to_string(r.status) : r.error) + ")");
        return std::nullopt;
    }
    try {
        paths::write_atomic(file, r.body);
    } catch (const std::exception& e) {
        say(std::string("Couldn't connect: ") + e.what());
        return std::nullopt;
    }
    return file;
}

std::wstring quick_tunnel_command(const fs::path& exe, int port) {
    return L"\"" + exe.wstring() + L"\" tunnel --no-autoupdate --url http://127.0.0.1:" + std::to_wstring(port);
}

}  // namespace

std::optional<std::string> find_tunnel_url(const std::string& line) {
    // cloudflared's error lines mention its own service (api.trycloudflare.com): that is not the tunnel.
    static const std::regex re(R"(https://([a-z0-9-]+)\.trycloudflare\.com)");
    for (auto it = std::sregex_iterator(line.begin(), line.end(), re); it != std::sregex_iterator(); ++it)
        if ((*it)[1] != "api") return (*it).str();
    return std::nullopt;
}

fs::path Tunnel::binary_path() { return paths::settings_dir() / L"cloudflared.exe"; }

// One run of the worker. Stopped runs stay around (their threads may still be winding down) until the Tunnel dies.
struct Run {
    std::atomic<bool> stop{false};
    Handle stop_event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::thread thread;
};

struct Tunnel::Impl {
    Tunnel* self = nullptr;
    Server& server;
    Hooks hooks;
    mutable std::mutex m;
    std::string status = "Off";
    std::optional<std::string> url;
    std::vector<std::shared_ptr<Run>> runs;
    std::atomic<bool> dying{false};  // the destructor is waiting: skip the network call that says goodbye

    Impl(Server& s, Hooks h) : server(s), hooks(std::move(h)) {}

    void change(const std::shared_ptr<Run>& run, std::optional<std::string> new_status, std::optional<std::optional<std::string>> new_url = std::nullopt) {
        {
            std::lock_guard lock(m);
            if (run->stop) return;  // a stopped run no longer speaks for the tunnel
            if (new_status) status = std::move(*new_status);
            if (new_url) url = std::move(*new_url);
        }
        if (self->on_changed) self->on_changed();
    }

    void work(const std::shared_ptr<Run>& run) {
        const auto say = [&](const std::string& s) { change(run, s); };
        try {
            go(run, say);
        } catch (const std::exception& e) {
            say(std::string("Couldn't connect: ") + e.what());
        }
        change(run, std::nullopt, std::optional<std::string>{});
        if (run->stop) {  // told to stop: back to Off, and tell the account (off this thread's way: it may take a while)
            {
                std::lock_guard lock(m);
                if (runs.empty() || runs.back() == run) status = "Off", url.reset();
            }
            if (self->on_changed) self->on_changed();
            try {
                if (!dying && hooks.signed_in()) hooks.announce(std::nullopt);
            } catch (const std::exception&) {
                // Offline: the account marks the computer unreachable after its check-ins stop.
            }
        }
    }

    void go(const std::shared_ptr<Run>& run, const std::function<void(const std::string&)>& say) {
        if (!hooks.signed_in()) return say("Sign in to your WreckBox account first.");
        if (!server.running() && !server.start()) return say("Couldn't start the phone-sync server (is port " + std::to_string(Server::configured_port()) + " in use?).");
        const auto exe = hooks.binary(say);
        if (!exe) return;

        // Dies with WreckBox: the job kills its processes when its last handle closes (also if we crash).
        Handle job(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{};
        lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &lim, sizeof lim);

        while (!run->stop) {
            say("Connecting…");
            Handle read, write;
            SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
            if (!CreatePipe(&read.h, &write.h, &sa, 0)) return say("Couldn't connect: no pipe");
            SetHandleInformation(read.h, HANDLE_FLAG_INHERIT, 0);
            STARTUPINFOW si{sizeof si};
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdOutput = si.hStdError = write.h;
            si.hStdInput = INVALID_HANDLE_VALUE;
            PROCESS_INFORMATION pi{};
            std::wstring cmd = hooks.command_line(*exe, server.port());
            if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi))
                return say("Couldn't connect: can't start the tunnel tool");
            Handle proc(pi.hProcess), thread(pi.hThread);
            AssignProcessToJobObject(job.h, proc.h);
            ResumeThread(thread.h);
            write.reset();  // so the pipe ends when the process does

            // cloudflared prints the quick-tunnel address in its log.
            std::mutex fm;
            std::condition_variable fcv;
            std::optional<std::string> found;
            std::thread reader([&] {
                std::string pending;
                char buf[4096];
                DWORD n = 0;
                while (ReadFile(read.h, buf, sizeof buf, &n, nullptr) && n) {
                    pending.append(buf, n);
                    for (size_t nl; (nl = pending.find('\n')) != std::string::npos; pending.erase(0, nl + 1))
                        if (const auto u = find_tunnel_url(pending.substr(0, nl))) {
                            std::lock_guard lock(fm);
                            if (!found) found = u;
                            fcv.notify_all();
                        }
                }
            });
            const auto finish = [&] {
                TerminateProcess(proc.h, 0);
                read.reset();  // unblocks the reader if the process left children holding the pipe open
                if (reader.joinable()) reader.join();
            };

            const HANDLE waits[2] = {run->stop_event.h, proc.h};
            const auto deadline = std::chrono::steady_clock::now() + hooks.url_timeout;
            bool ok = false;
            while (!run->stop && std::chrono::steady_clock::now() < deadline) {
                {
                    std::unique_lock lock(fm);
                    if (fcv.wait_for(lock, std::chrono::milliseconds(100), [&] { return found.has_value(); })) {
                        ok = true;
                        break;
                    }
                }
                if (WaitForMultipleObjects(2, waits, FALSE, 0) != WAIT_TIMEOUT) break;  // stopped, or the tool quit
            }
            if (!ok) {
                finish();
                if (!run->stop) say("Couldn't connect: the tunnel tool gave no address");
                return;  // like the original build: no automatic retry before the first address
            }
            const std::string address = [&] {
                std::lock_guard lock(fm);
                return *found;
            }();
            try {
                hooks.announce(address);
            } catch (const std::exception& e) {
                finish();
                return say(std::string("Couldn't connect: ") + e.what());
            }
            change(run, "Reachable from anywhere", std::optional<std::string>{address});

            for (;;) {  // up: check in every few minutes until the process quits or we're stopped
                const DWORD w = WaitForMultipleObjects(2, waits, FALSE, DWORD(hooks.heartbeat.count()));
                if (w != WAIT_TIMEOUT) break;
                try {
                    hooks.announce(address);  // also keeps the account's copy of the library current
                } catch (const std::exception&) {
                    // Try again at the next beat.
                }
            }
            finish();
            if (run->stop) return;
            change(run, "Reconnecting…", std::optional<std::string>{});
            WaitForSingleObject(run->stop_event.h, DWORD(hooks.retry.count()));  // dropped: bring it back
        }
    }
};

Tunnel::Tunnel(Server& server) : Tunnel(server, Hooks{}) {}

Tunnel::Tunnel(Server& server, Hooks hooks) : p_(std::make_unique<Impl>(server, std::move(hooks))) {
    p_->self = this;
    auto& h = p_->hooks;
    if (!h.signed_in) h.signed_in = [] { return account::signed_in(); };
    if (!h.binary) h.binary = download_binary;
    if (!h.command_line) h.command_line = quick_tunnel_command;
    if (!h.announce)
        h.announce = [&server](const std::optional<std::string>& url) {
            const std::string name = "WreckBox on " + Server::host_name();
            if (!url) return account::register_computer(name, "windows");
            account::register_computer(name, "windows", url, server.token());
            try {
                account::upload_library(server.store());  // keep the account's copy current
            } catch (const std::exception&) {
            }
        };
}

Tunnel::~Tunnel() {
    p_->dying = true;
    for (auto& r : p_->runs) {
        r->stop = true;
        SetEvent(r->stop_event.h);
    }
    for (auto& r : p_->runs)
        if (r->thread.joinable()) r->thread.join();
}

void Tunnel::start() {
    std::lock_guard lock(p_->m);
    if (!p_->runs.empty() && !p_->runs.back()->stop) return;  // already wanted
    auto run = std::make_shared<Run>();
    p_->runs.push_back(run);
    p_->status = "Connecting…";
    run->thread = std::thread([impl = p_.get(), run] { impl->work(run); });
}

void Tunnel::stop() {
    {
        std::lock_guard lock(p_->m);
        if (p_->runs.empty() || p_->runs.back()->stop) return;
        p_->runs.back()->stop = true;
        SetEvent(p_->runs.back()->stop_event.h);
        p_->status = "Off";
        p_->url.reset();
    }
    if (on_changed) on_changed();
}

bool Tunnel::running() const {
    std::lock_guard lock(p_->m);
    return p_->url.has_value();
}
std::string Tunnel::status() const {
    std::lock_guard lock(p_->m);
    return p_->status;
}
std::optional<std::string> Tunnel::url() const {
    std::lock_guard lock(p_->m);
    return p_->url;
}

}  // namespace wb::sync
