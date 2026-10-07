// A native Soulseek client: logs in, searches, and downloads a file from a peer. Replaces the aioslsk sidecar for what
// slsk_sync.py uses (phase 10). Blocking calls for a worker thread; its own threads do the socket work.
//
// How it talks (docs: the Nicotine+ protocol notes): one connection to the server; a listening port that peers connect to
// (search replies, upload requests, the file itself); and for peers that cannot reach us, the server's "ConnectToPeer"
// relay, which makes *us* connect to *them* and pierce their firewall with a token.
#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "net/slsk/types.h"

namespace wb::slsk {

struct ClientOptions {
    std::string server_host = "server.slsknet.org";
    int server_port = 2242;
    int listen_port = 60000;  // 0: any free port (tests)
    std::chrono::seconds connect_timeout{10};
    std::chrono::seconds indirect_timeout{20};  // waiting for a peer to connect to us after the server relayed our request
    std::chrono::seconds login_timeout{15};
    unsigned shared_folders = 0, shared_files = 0;  // what we tell the server we share
};

struct DownloadOptions {
    std::chrono::seconds queue_timeout{240};  // give up on a peer that hasn't started sending by then
    std::chrono::seconds stall_timeout{180};  // give up on a transfer with no progress for this long
    std::function<bool()> cancel;
    std::function<void(uint64_t done, uint64_t total)> progress;
};

struct DownloadResult {
    bool ok = false;
    std::string error;  // why not, in the sidecar's words ("still queued (place 4)", "stalled", …)
    uint64_t size = 0;
};

class Client {
public:
    explicit Client(ClientOptions options = {});
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // "" when logged in, else a message for the user ("Soulseek login failed: INVALIDPASS", "Can't reach the Soulseek server").
    std::string login(const std::string& username, const std::string& password);
    void close();
    bool connected() const;     // the server connection is up
    int listen_port() const;    // the port peers connect to

    // Sends the search and collects every answer for `wait`. Returns early if `cancel` says so.
    std::vector<UserResult> search(const std::string& query, std::chrono::seconds wait, const std::function<bool()>& cancel = {});
    // Downloads `filename` (the peer's own path) from `username` into `dest` (a partial file: the caller moves it after).
    DownloadResult download(const std::string& username, const std::string& filename, const std::string& dest, const DownloadOptions& options = {});

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace wb::slsk
