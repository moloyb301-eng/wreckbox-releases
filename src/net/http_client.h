// Minimal blocking HTTPS client on WinHTTP (built into Windows: system certificates, proxy settings, gzip).
// Call from worker threads only — every call blocks until the response or the timeout.
#pragma once
#include <chrono>
#include <map>
#include <string>

namespace wb::http {

using Headers = std::map<std::string, std::string>;

struct Response {
    int status = 0;     // 0 when the request never got a response
    std::string body;
    std::string error;  // readable reason when status == 0 ("No internet connection.", "timed out", …)
    Headers headers;    // response headers, names lower-cased
    bool ok() const { return status >= 200 && status < 300; }
    std::string header(const std::string& lower_name) const {
        const auto it = headers.find(lower_name);
        return it == headers.end() ? "" : it->second;
    }
};

Response request(const std::string& method, const std::string& url, const Headers& headers = {}, const std::string& body = {},
                 std::chrono::seconds timeout = std::chrono::seconds(30));

inline Response get(const std::string& url, const Headers& headers = {}, std::chrono::seconds timeout = std::chrono::seconds(30)) {
    return request("GET", url, headers, {}, timeout);
}

// Percent-encodes a URL component (RFC 3986 unreserved characters kept).
std::string url_encode(const std::string& s);

}  // namespace wb::http
