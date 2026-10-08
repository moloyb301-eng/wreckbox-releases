// "Use from anywhere": a Cloudflare quick tunnel gives this computer's phone-sync server a
// temporary https address, which is registered in the account so the account's phones can stream and download from
// anywhere. Requests still need the phone-sync token, which only the account's devices receive.
//
// start() / stop() return at once; a worker thread downloads cloudflared once (~40 MB, into the settings folder), runs it
// with the quick-tunnel arguments, reads its output for the address, registers it, repeats that every 5 minutes, and
// brings the tunnel back 10 seconds after it drops. The process dies with WreckBox (a job object), even on a crash.
#pragma once
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "net/sync_server.h"

namespace wb::sync {

// "https://abc-def.trycloudflare.com" if the line has one.
std::optional<std::string> find_tunnel_url(const std::string& line);

class Tunnel {
public:
    // Everything the tests replace; the defaults are the real thing.
    struct Hooks {
        std::function<bool()> signed_in;
        // The tool's path (downloading it if needed), or nullopt after saying why.
        std::function<std::optional<std::filesystem::path>(const std::function<void(const std::string&)>& say)> binary;
        std::function<std::wstring(const std::filesystem::path&, int port)> command_line;
        // Register this computer's address (nullopt: it's no longer reachable). Throws on failure.
        std::function<void(const std::optional<std::string>& url)> announce;
        std::chrono::milliseconds heartbeat = std::chrono::minutes(5), retry = std::chrono::seconds(10), url_timeout = std::chrono::seconds(45);
    };

    explicit Tunnel(Server& server);  // real hooks
    Tunnel(Server& server, Hooks hooks);
    ~Tunnel();

    std::function<void()> on_changed;  // status or address changed (called on the worker thread)

    void start();  // keep it up until stop()
    void stop();   // returns at once; the worker winds down and tells the account
    bool running() const;  // up, with an address
    std::string status() const;  // "Off", "Connecting…", "Reachable from anywhere", why it failed, …
    std::optional<std::string> url() const;

    static std::filesystem::path binary_path();  // <settings folder>\cloudflared.exe

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace wb::sync
