#include "net/oauth.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>

#include "model/model.h"
#include "net/http_client.h"

namespace wb::oauth {
namespace {

struct WinsockInit {
    WinsockInit() {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    }
};

int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') out += ' ';
        else if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
            out += char(hex(s[i + 1]) * 16 + hex(s[i + 2]));
            i += 2;
        } else out += s[i];
    }
    return out;
}

}  // namespace

std::string random_string(size_t n) {
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    std::string bytes(n, '\0');
    BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(bytes.data()), ULONG(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    std::string out;
    for (const unsigned char b : bytes) out += chars[b % (sizeof chars - 1)];
    return out;
}

std::string sha256(const std::string& data) {
    std::string out(32, '\0');
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), ULONG(data.size()),
               reinterpret_cast<PUCHAR>(out.data()), 32);
    return out;
}

std::string base64url(const std::string& bytes) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    uint32_t v = 0;
    int bits = 0;
    for (const unsigned char c : bytes) {
        v = (v << 8) | c;
        bits += 8;
        while (bits >= 6) out += t[(v >> (bits -= 6)) & 63];
    }
    if (bits > 0) out += t[(v << (6 - bits)) & 63];
    return out;
}

std::string pkce_challenge(const std::string& verifier) { return base64url(sha256(verifier)); }

std::string form(const std::map<std::string, std::string>& fields) {
    std::string out;
    for (const auto& [k, v] : fields) out += (out.empty() ? "" : "&") + http::url_encode(k) + "=" + http::url_encode(v);
    return out;
}

std::map<std::string, std::string> parse_query(const std::string& query) {
    std::map<std::string, std::string> out;
    size_t start = 0;
    while (start <= query.size()) {
        const size_t amp = std::min(query.find('&', start), query.size());
        const std::string part = query.substr(start, amp - start);
        if (!part.empty()) {
            const size_t eq = part.find('=');
            out[decode(part.substr(0, eq))] = eq == std::string::npos ? "" : decode(part.substr(eq + 1));
        }
        start = amp + 1;
    }
    return out;
}

void open_in_browser(const std::string& url) { ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

// MARK: Loopback

Loopback::Loopback(int port) {
    static WinsockInit init;
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(u_short(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (s == INVALID_SOCKET || bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(s, 4) != 0) {
        error_ = port ? "Port " + std::to_string(port) + " is in use — close whatever is using it and try again." : "Can't listen for the sign-in.";
        if (s != INVALID_SOCKET) closesocket(s);
        return;
    }
    int len = sizeof addr;
    getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    sock_ = s;
}

Loopback::~Loopback() {
    if (ok()) closesocket(SOCKET(sock_));
}

std::optional<std::map<std::string, std::string>> Loopback::wait(const std::string& path, const std::string& done_title,
                                                                 std::chrono::seconds timeout) {
    if (!ok()) return std::nullopt;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(SOCKET(sock_), &fds);
        timeval tv{1, 0};
        if (select(0, &fds, nullptr, nullptr, &tv) <= 0) continue;
        const SOCKET c = accept(SOCKET(sock_), nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        DWORD to = 5000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
        std::string req;
        char buf[2048];
        while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384) {
            const int n = recv(c, buf, sizeof buf, 0);
            if (n <= 0) break;
            req.append(buf, size_t(n));
        }
        // "GET /callback?code=…&state=… HTTP/1.1"
        std::optional<std::map<std::string, std::string>> params;
        if (req.rfind("GET ", 0) == 0) {
            const std::string target = req.substr(4, req.find(' ', 4) - 4);
            const size_t q = target.find('?');
            if (target.substr(0, q) == path && q != std::string::npos) {
                auto p = parse_query(target.substr(q + 1));
                if (p.contains("code") || p.contains("error")) params = std::move(p);
            }
        }
        const std::string body = params ? "<html><body style=\"font-family:sans-serif;background:#08080a;color:#eee;padding:40px\"><h2>" +
                                              done_title + "</h2><p>You can close this tab and go back to the app.</p></body></html>"
                                        : "Not found";
        const std::string resp = std::string("HTTP/1.1 ") + (params ? "200 OK" : "404 Not Found") +
                                 "\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) +
                                 "\r\n\r\n" + body;
        send(c, resp.data(), int(resp.size()), 0);
        shutdown(c, SD_SEND);
        closesocket(c);
        if (params) return params;
    }
    return std::nullopt;
}

}  // namespace wb::oauth
