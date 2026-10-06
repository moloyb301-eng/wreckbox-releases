#include "net/sync_server.h"

#include <httplib.h>  // first: it brings in winsock2.h, which must precede windows.h

#include <windows.h>
#include <bcrypt.h>
#include <iphlpapi.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

#include "model/paths.h"
#include "model/settings.h"
#include "net/oauth.h"

namespace wb::sync {
namespace fs = std::filesystem;
namespace {

bool same(const std::string& a, const std::string& b) {  // no early exit: the time doesn't show how much matched
    unsigned diff = unsigned(a.size() ^ b.size());
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) diff |= unsigned(uint8_t(a[i]) ^ uint8_t(b[i]));
    return diff == 0;
}

const char* mime(const fs::path& p) {
    std::wstring e = p.extension().wstring();
    std::transform(e.begin(), e.end(), e.begin(), ::towlower);
    if (e == L".flac") return "audio/flac";
    if (e == L".mp3") return "audio/mpeg";
    if (e == L".m4a" || e == L".aac" || e == L".alac") return "audio/mp4";
    if (e == L".wav") return "audio/wav";
    if (e == L".aif" || e == L".aiff") return "audio/aiff";
    if (e == L".ogg" || e == L".opus") return "audio/ogg";
    return "application/octet-stream";
}

fs::path to_path(const std::string& utf8) { return fs::path(widen(utf8)); }

}  // namespace

struct Server::Impl {
    httplib::Server svr;
    std::thread thread;
    int port = 0;
    bool running = false;
};

std::string Server::host_name() {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = DWORD(std::size(name));
    return GetComputerNameW(name, &n) ? narrow(std::wstring(name, n)) : "computer";
}

Server::Server(LibraryStore& store) : store_(store), p_(std::make_unique<Impl>()) {}
Server::~Server() { stop(); }

int Server::configured_port() {
    if (const char* v = std::getenv("WRECKBOX_SYNC_PORT")) {
        const int n = std::atoi(v);
        if (n > 0 && n < 65536) return n;
    }
    return kDefaultPort;
}

std::string Server::new_token() {
    unsigned char b[18];
    BCryptGenRandom(nullptr, b, sizeof b, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return oauth::base64url(std::string(reinterpret_cast<char*>(b), sizeof b));
}

std::string Server::token() {
    std::lock_guard lock(token_m_);
    if (token_.empty()) {
        auto& s = Settings::current();
        if (!s.desktop_pair_token) {
            s.desktop_pair_token = new_token();
            try {
                s.save();
            } catch (const std::exception&) {
                // Kept in memory; the next save writes it.
            }
        }
        token_ = *s.desktop_pair_token;
    }
    return token_;
}

void Server::reset_token() {
    auto& s = Settings::current();
    {
        std::lock_guard lock(token_m_);
        token_ = new_token();
        s.desktop_pair_token = token_;
    }
    s.save();
}

std::vector<std::string> Server::local_addresses() {
    std::vector<std::string> out;
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf(size);
    auto* list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        rc = GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size);
    }
    if (rc != NO_ERROR) return out;
    for (auto* a = list; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            const auto* sa = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
            const unsigned char* ip = reinterpret_cast<const unsigned char*>(&sa->sin_addr);
            const bool is_private = ip[0] == 10 || (ip[0] == 192 && ip[1] == 168) || (ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31);
            if (!is_private) continue;
            out.push_back(std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." + std::to_string(ip[2]) + "." + std::to_string(ip[3]));
        }
    }
    return out;
}

std::string Server::pairing_uri() {
    std::string hosts;
    for (const auto& ip : local_addresses()) hosts += (hosts.empty() ? "" : ",") + ip;
    return "wreckbox://pair?hosts=" + hosts + "&port=" + std::to_string(p_->running ? p_->port : configured_port()) + "&t=" + token();
}

bool Server::running() const { return p_->running; }
int Server::port() const { return p_->port; }

void Server::stop() {
    if (!p_->running) return;
    p_->svr.stop();
    if (p_->thread.joinable()) p_->thread.join();
    p_->running = false;
}

bool Server::start(int port, const std::string& host) {
    if (p_->running) return true;
    p_ = std::make_unique<Impl>();  // a fresh httplib::Server: routes are added below, and stop() left the old one used
    auto& svr = p_->svr;

    // The token check runs before anything else, for every route.
    svr.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
        std::string given = req.get_header_value("x-wreckbox-token");
        if (given.empty() && req.has_param("t")) given = req.get_param_value("t");
        if (same(given, token())) return httplib::Server::HandlerResponse::Unhandled;
        res.status = 403;
        res.set_content("not paired", "text/plain");
        return httplib::Server::HandlerResponse::Handled;
    });

    auto json_reply = [](httplib::Response& res, const json& j) { res.set_content(j.dump(), "application/json"); };

    svr.Get("/info", [this, json_reply](const httplib::Request&, httplib::Response& res) {
        json_reply(res, {{"name", "WreckBox on " + Server::host_name()}, {"tracks", store_.count(TrackStatus::downloaded)}});
    });

    svr.Get("/library.json", [](const httplib::Request&, httplib::Response& res) {
        if (const auto text = paths::read_file(paths::library_file())) res.set_content(*text, "application/json");
        else res.status = 404;
    });

    svr.Get("/crate", [this, json_reply](const httplib::Request&, httplib::Response& res) {
        json out = json::array();
        const auto lib = store_.library();
        const auto state = store_.state_copy();
        if (lib)
            for (const auto& t : lib->tracks) {
                const auto st = state.tracks.find(t.id);
                if (st == state.tracks.end() || st->second.status != TrackStatus::downloaded || !st->second.local_path) continue;
                const fs::path file = to_path(*st->second.local_path);
                std::error_code ec;
                const auto size = fs::file_size(file, ec);
                if (ec) continue;  // the file is gone
                std::wstring ext = file.extension().wstring();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
                const auto analysis = store_.analysis_of(*st->second.local_path);
                out.push_back({{"id", t.id}, {"ext", narrow(ext)}, {"size", size}, {"analysis", analysis ? analysis->to_json() : json(nullptr)}});
            }
        json_reply(res, out);
    });

    // A track, with Range support (206 / 416) so the phone's player can seek while streaming.
    svr.Get(R"(/file/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
        const auto state = store_.state_copy();
        const auto st = state.tracks.find(req.matches[1].str());
        const fs::path file = st != state.tracks.end() && st->second.local_path ? to_path(*st->second.local_path) : fs::path();
        std::error_code ec;
        const auto size = file.empty() ? 0 : fs::file_size(file, ec);
        if (file.empty() || ec) {
            res.status = 404;
            res.set_content("no such track", "text/plain");
            return;
        }
        res.set_header("Accept-Ranges", "bytes");
        auto in = std::make_shared<std::ifstream>(file, std::ios::binary);
        res.set_content_provider(size_t(size), mime(file), [in](size_t offset, size_t length, httplib::DataSink& sink) {
            char buf[64 * 1024];
            in->clear();
            in->seekg(std::streamoff(offset));
            in->read(buf, std::streamsize(std::min(length, sizeof buf)));
            const auto got = size_t(in->gcount());
            if (!got) return false;
            return sink.write(buf, got);
        });
    });

    svr.Get(R"(/art/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
        const auto t = store_.track(req.matches[1].str());
        const auto file = t ? store_.ensure_artwork(*t) : std::nullopt;
        const auto bytes = file ? paths::read_file(*file) : std::nullopt;
        if (!bytes) {
            res.status = 404;
            return;
        }
        res.set_content(*bytes, "image/jpeg");
    });

    svr.set_read_timeout(15);
    svr.set_write_timeout(60);
    if (port == 0 ? (p_->port = svr.bind_to_any_port(host)) < 0 : !svr.bind_to_port(host, port)) return false;
    if (port != 0) p_->port = port;
    p_->running = true;
    p_->thread = std::thread([this] { p_->svr.listen_after_bind(); });
    return true;
}

}  // namespace wb::sync
