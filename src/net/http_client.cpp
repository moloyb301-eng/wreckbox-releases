#include "net/http_client.h"

#include <windows.h>
#include <winhttp.h>

#include <cctype>

#include "model/model.h"

namespace wb::http {
namespace {

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET x) : h(x) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
    explicit operator bool() const { return h != nullptr; }
};

// One session for the process; WinHTTP sessions are thread-safe.
HINTERNET session() {
    static Handle s([] {
        HINTERNET h = WinHttpOpen(L"WreckBox", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (h) {
            DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;
            WinHttpSetOption(h, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof decompress);
        }
        return h;
    }());
    return s.h;
}

std::string describe(DWORD err) {
    switch (err) {
        case ERROR_WINHTTP_TIMEOUT: return "The server is not responding — try again.";
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        case ERROR_WINHTTP_CANNOT_CONNECT:
        case ERROR_WINHTTP_CONNECTION_ERROR: return "No internet connection.";
        case ERROR_WINHTTP_SECURE_FAILURE: return "Secure connection failed.";
        default: return "Network error " + std::to_string(err) + ".";
    }
}

Response fail() { return Response{0, {}, describe(GetLastError())}; }

}  // namespace

Response request(const std::string& method, const std::string& url, const Headers& headers, const std::string& body,
                 std::chrono::seconds timeout) {
    if (!session()) return fail();
    const std::wstring wurl = widen(url);
    // WinHttpCrackUrl copies each part, NUL-terminated, into these buffers.
    std::wstring host(256, L'\0'), path(wurl.size() + 1, L'\0'), query(wurl.size() + 1, L'\0');
    URL_COMPONENTS u{sizeof u};
    u.lpszHostName = host.data();
    u.dwHostNameLength = DWORD(host.size());
    u.lpszUrlPath = path.data();
    u.dwUrlPathLength = DWORD(path.size());
    u.lpszExtraInfo = query.data();
    u.dwExtraInfoLength = DWORD(query.size());
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &u)) return Response{0, {}, "Bad address: " + url};
    const std::wstring target = path.substr(0, u.dwUrlPathLength) + query.substr(0, u.dwExtraInfoLength);

    Handle connect(WinHttpConnect(session(), host.c_str(), u.nPort, 0));
    if (!connect) return fail();
    Handle req(WinHttpOpenRequest(connect.h, widen(method).c_str(), target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES, u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    if (!req) return fail();
    const int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count());
    WinHttpSetTimeouts(req.h, ms, ms, ms, ms);

    std::wstring head;
    for (const auto& [k, v] : headers) head += widen(k) + L": " + widen(v) + L"\r\n";
    if (!WinHttpSendRequest(req.h, head.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : head.c_str(), DWORD(-1L),
                            body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()), DWORD(body.size()),
                            DWORD(body.size()), 0) ||
        !WinHttpReceiveResponse(req.h, nullptr))
        return fail();

    Response r;
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                        WINHTTP_NO_HEADER_INDEX);
    r.status = int(status);
    DWORD hsize = 0;
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &hsize, WINHTTP_NO_HEADER_INDEX);
    if (hsize) {
        std::wstring raw(hsize / sizeof(wchar_t), L'\0');
        if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &hsize, WINHTTP_NO_HEADER_INDEX)) {
            const std::string text = narrow(raw.substr(0, hsize / sizeof(wchar_t)));
            size_t start = text.find("\r\n");  // skip the status line
            while (start != std::string::npos && start + 2 < text.size()) {
                const size_t end = text.find("\r\n", start + 2);
                const std::string line = text.substr(start + 2, (end == std::string::npos ? text.size() : end) - start - 2);
                if (const size_t colon = line.find(':'); colon != std::string::npos) {
                    std::string name = line.substr(0, colon);
                    for (auto& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
                    r.headers[name] = line.substr(line.find_first_not_of(' ', colon + 1) == std::string::npos ? line.size()
                                                                                                                : line.find_first_not_of(' ', colon + 1));
                }
                start = end;
            }
        }
    }
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail)) return fail();
        if (avail == 0) break;
        const size_t at = r.body.size();
        r.body.resize(at + avail);
        DWORD read = 0;
        if (!WinHttpReadData(req.h, r.body.data() + at, avail, &read)) return fail();
        r.body.resize(at + read);
    }
    return r;
}

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
        else out += {'%', hex[c >> 4], hex[c & 15]};
    }
    return out;
}

}  // namespace wb::http
