// OAuth sign-in for desktop apps (PKCE + loopback redirect), shared by the Spotify and YouTube importers, plus the
// small crypto helpers it needs (Windows CNG — nothing to ship).
#pragma once
#include <chrono>
#include <map>
#include <optional>
#include <string>

namespace wb::oauth {

std::string random_string(size_t n);  // from A–Z a–z 0–9 - . _ ~ (the PKCE verifier alphabet)
std::string sha256(const std::string& data);  // raw 32 bytes
std::string base64url(const std::string& bytes);  // no padding
std::string pkce_challenge(const std::string& verifier);  // base64url(sha256(verifier))

// "a=1&b=2" with each value percent-encoded (for URLs and application/x-www-form-urlencoded bodies).
std::string form(const std::map<std::string, std::string>& fields);
std::map<std::string, std::string> parse_query(const std::string& query);  // also decodes %xx and '+'

void open_in_browser(const std::string& url);

// Listens on 127.0.0.1:<port> (0 = any free port) for the browser's redirect after sign-in.
class Loopback {
public:
    explicit Loopback(int port = 0);
    ~Loopback();
    Loopback(const Loopback&) = delete;
    Loopback& operator=(const Loopback&) = delete;

    bool ok() const { return sock_ != ~uintptr_t(0); }
    int port() const { return port_; }
    const std::string& error() const { return error_; }
    // Waits for a GET whose path is `path` and which carries `code` or `error`; answers with a "you can close this tab"
    // page titled `done_title`. Returns its query parameters, or nullopt on timeout.
    std::optional<std::map<std::string, std::string>> wait(const std::string& path, const std::string& done_title,
                                                           std::chrono::seconds timeout = std::chrono::minutes(5));

private:
    uintptr_t sock_ = ~uintptr_t(0);
    int port_ = 0;
    std::string error_;
};

}  // namespace wb::oauth
