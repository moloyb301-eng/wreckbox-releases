// Computer → phone sync over the local network. Serves the crate
// (the tracks you have, with their analysis) over HTTP on port 47390; every request must carry the pairing token shown
// in the QR code (header `x-wreckbox-token` or `?t=`), so only phones you paired can browse or download. The endpoints
// are in docs/DESIGN.md §4; the Android app uses them unchanged.
#pragma once
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "library/store.h"

namespace wb::sync {

constexpr int kDefaultPort = 47390;

class Server {
public:
    explicit Server(LibraryStore& store);
    ~Server();

    // The sync port: WRECKBOX_SYNC_PORT overrides the default (to test next to another WreckBox).
    static int configured_port();
    // Starts listening on `host` (all IPv4 interfaces by default); port 0 = any free one. False if the port is taken.
    bool start(int port = configured_port(), const std::string& host = "0.0.0.0");
    void stop();
    bool running() const;
    int port() const;

    // The pairing token (created and saved on first use). reset_token() unpairs every phone.
    std::string token();
    void reset_token();
    // `wreckbox://pair?hosts=<private IPv4s>&port=47390&t=<token>`
    std::string pairing_uri();
    // Private-network IPv4 addresses of this computer (what a phone on the same Wi-Fi can reach).
    static std::vector<std::string> local_addresses();

    static std::string new_token();  // 18 random bytes, base64url without padding
    static std::string host_name();  // this computer's name, for "WreckBox on <name>"
    LibraryStore& store() { return store_; }

private:
    struct Impl;
    LibraryStore& store_;
    std::unique_ptr<Impl> p_;
    std::mutex token_m_;
    std::string token_;
};

}  // namespace wb::sync
