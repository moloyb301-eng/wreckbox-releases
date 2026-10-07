#include "net/slsk/client.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <thread>

#include "model/model.h"
#include "net/slsk/protocol.h"

namespace wb::slsk {
namespace {

using Clock = std::chrono::steady_clock;
namespace proto = wb::slsk::proto;

struct WsaInit {
    WsaInit() {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    }
};
void ensure_winsock() {
    static const WsaInit once;
    (void)once;
}

// A connected socket and what we know about who is on the other end. Closed from any thread to wake a blocked reader.
struct Conn {
    SOCKET s = INVALID_SOCKET;
    std::mutex send_m;
    std::string username;
    std::atomic<bool> closed{false};

    explicit Conn(SOCKET sock) : s(sock) {}
    ~Conn() { close(); }
    void close() {
        if (!closed.exchange(true) && s != INVALID_SOCKET) {
            shutdown(s, SD_BOTH);
            closesocket(s);
        }
    }
    bool send_all(const std::string& data) {
        std::lock_guard lock(send_m);
        size_t at = 0;
        while (at < data.size()) {
            const int n = send(s, data.data() + at, int(std::min<size_t>(data.size() - at, 1 << 20)), 0);
            if (n <= 0) return false;
            at += size_t(n);
        }
        return true;
    }
    void set_timeout(int ms) {
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
    }
    // Exactly n bytes. False on EOF or error; a timeout counts as "nothing yet" and is retried until `stop` says so.
    bool recv_exact(void* out, size_t n, const std::function<bool()>& stop) {
        size_t at = 0;
        while (at < n) {
            const int r = recv(s, static_cast<char*>(out) + at, int(n - at), 0);
            if (r > 0) {
                at += size_t(r);
                continue;
            }
            if (r < 0 && WSAGetLastError() == WSAETIMEDOUT && !closed && !(stop && stop())) continue;
            return false;
        }
        return true;
    }
};
using ConnPtr = std::shared_ptr<Conn>;

SOCKET tcp_connect(const std::string& host, int port, std::chrono::seconds timeout) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return INVALID_SOCKET;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(res);
        return INVALID_SOCKET;
    }
    u_long nonblocking = 1;
    ioctlsocket(s, FIONBIO, &nonblocking);
    connect(s, res->ai_addr, int(res->ai_addrlen));
    freeaddrinfo(res);
    fd_set w{}, e{};
    FD_ZERO(&w);
    FD_ZERO(&e);
    FD_SET(s, &w);
    FD_SET(s, &e);
    timeval tv{long(timeout.count()), 0};
    const int ready = select(0, nullptr, &w, &e, &tv);
    if (ready <= 0 || FD_ISSET(s, &e) || !FD_ISSET(s, &w)) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    nonblocking = 0;
    ioctlsocket(s, FIONBIO, &nonblocking);
    return s;
}

struct Pierce {  // a peer we asked the server to make connect to us
    std::mutex m;
    std::condition_variable cv;
    ConnPtr conn;
};
struct AddrWait {
    std::mutex m;
    std::condition_variable cv;
    std::optional<proto::PeerAddress> address;
};
struct Search {
    std::mutex m;
    std::vector<UserResult> results;
};

struct Download {
    std::string username, filename, dest;
    DownloadOptions options;
    std::mutex m;
    std::condition_variable cv;
    uint32_t ticket = 0;
    uint64_t size = 0, received = 0;
    int place = -1;
    bool started = false, done = false;
    std::string error;
    std::atomic<bool> aborted{false};
};

}  // namespace

struct Client::Impl {
    ClientOptions opt;
    std::atomic<bool> closing{false}, server_up{false};
    std::string username;

    std::mutex m;  // guards the maps and sets below
    ConnPtr server;
    SOCKET listener = INVALID_SOCKET;
    int port = 0;
    std::map<uint32_t, std::shared_ptr<Pierce>> pierce;
    std::map<std::string, std::shared_ptr<AddrWait>> addr_waits;
    std::map<uint32_t, std::shared_ptr<Search>> searches;
    std::vector<std::shared_ptr<Download>> downloads;
    std::map<uint32_t, std::shared_ptr<Download>> by_ticket;
    std::map<std::string, ConnPtr> peers;
    std::set<ConnPtr> all;

    std::mutex threads_m;
    std::vector<std::thread> threads;
    std::mutex wake_m;
    std::condition_variable wake;  // for the keep-alive thread
    std::mt19937 rng{std::random_device{}()};

    explicit Impl(ClientOptions o) : opt(std::move(o)) { ensure_winsock(); }

    uint32_t next_token() {
        std::lock_guard lock(m);
        return rng() & 0x7FFFFFFF;
    }
    void spawn(std::function<void()> fn) {
        std::lock_guard lock(threads_m);
        if (!closing) threads.emplace_back(std::move(fn));
    }
    ConnPtr track(SOCKET s) {
        auto c = std::make_shared<Conn>(s);
        std::lock_guard lock(m);
        all.insert(c);
        return c;
    }
    void forget(const ConnPtr& c) {
        c->close();
        std::lock_guard lock(m);
        all.erase(c);
        for (auto it = peers.begin(); it != peers.end();)
            it = it->second == c ? peers.erase(it) : std::next(it);
    }

    // MARK: Reading frames
    // A frame after the first on a peer connection: uint32 length, uint32 code, payload.
    bool read_peer_frame(const ConnPtr& c, uint32_t& code, std::string& payload) {
        uint32_t len = 0;
        if (!c->recv_exact(&len, 4, [&] { return closing.load(); }) || len < 4 || len > 64u * 1024 * 1024) return false;
        std::string body(len, '\0');
        if (!c->recv_exact(body.data(), len, [&] { return closing.load(); })) return false;
        std::memcpy(&code, body.data(), 4);
        payload = body.substr(4);
        return true;
    }

    // MARK: The server
    void server_loop(ConnPtr c) {
        while (!closing) {
            uint32_t code = 0;
            std::string payload;
            if (!read_peer_frame(c, code, payload)) break;
            switch (code) {
                case proto::kGetPeerAddress:
                    if (const auto a = proto::parse_peer_address(payload)) {
                        std::shared_ptr<AddrWait> w;
                        {
                            std::lock_guard lock(m);
                            if (const auto it = addr_waits.find(a->username); it != addr_waits.end()) w = it->second;
                        }
                        if (w) {
                            std::lock_guard lock(w->m);
                            w->address = *a;
                            w->cv.notify_all();
                        }
                    }
                    break;
                case proto::kConnectToPeer:  // someone wants us to connect to them
                    if (const auto r = proto::parse_connect_to_peer(payload)) spawn([this, r = *r] { connect_back(r); });
                    break;
                default: break;  // pings, room lists, parents… nothing a downloader needs
            }
        }
        server_up = false;
        wake.notify_all();
    }

    void keep_alive() {
        std::unique_lock lock(wake_m);
        while (!closing && server_up) {
            wake.wait_for(lock, std::chrono::minutes(5));
            if (closing || !server_up) break;
            ConnPtr s;
            {
                std::lock_guard g(m);
                s = server;
            }
            if (s) s->send_all(proto::ping());
        }
    }

    std::optional<proto::PeerAddress> peer_address(const std::string& user) {
        auto w = std::make_shared<AddrWait>();
        ConnPtr s;
        {
            std::lock_guard lock(m);
            addr_waits[user] = w;
            s = server;
        }
        std::optional<proto::PeerAddress> out;
        if (s && s->send_all(proto::get_peer_address(user))) {
            std::unique_lock lock(w->m);
            w->cv.wait_for(lock, opt.connect_timeout, [&] { return w->address.has_value() || closing; });
            out = w->address;
        }
        std::lock_guard lock(m);
        addr_waits.erase(user);
        return out;
    }

    // MARK: Peers
    // We connect to a peer the server relayed to us (or that we asked about), and pierce its firewall with the token.
    void connect_back(const proto::ConnectRequest& r) {
        SOCKET s = tcp_connect(r.ip, int(r.port), opt.connect_timeout);
        if (s == INVALID_SOCKET) return;
        auto c = track(s);
        c->username = r.username;
        if (!c->send_all(proto::pierce_firewall(r.ticket))) return forget(c);
        if (r.type == "P") peer_loop(c);
        else if (r.type == "F") file_connection(c);
        else forget(c);
    }

    ConnPtr connect_peer(const std::string& user) {
        {
            std::lock_guard lock(m);
            if (const auto it = peers.find(user); it != peers.end() && !it->second->closed) return it->second;
        }
        // Directly, where the server says the peer is.
        if (const auto a = peer_address(user); a && a->port && a->ip != "0.0.0.0") {
            SOCKET s = tcp_connect(a->ip, int(a->port), opt.connect_timeout);
            if (s != INVALID_SOCKET) {
                auto c = track(s);
                c->username = user;
                if (c->send_all(proto::peer_init(username, "P", 0))) {
                    {
                        std::lock_guard lock(m);
                        peers[user] = c;
                    }
                    spawn([this, c] { peer_loop(c); });
                    return c;
                }
                forget(c);
            }
        }
        // Or have the server ask the peer to connect to us.
        const uint32_t token = next_token();
        auto w = std::make_shared<Pierce>();
        ConnPtr server_conn;
        {
            std::lock_guard lock(m);
            pierce[token] = w;
            server_conn = server;
        }
        ConnPtr got;
        if (server_conn && server_conn->send_all(proto::connect_to_peer(token, user, "P"))) {
            std::unique_lock lock(w->m);
            w->cv.wait_for(lock, opt.indirect_timeout, [&] { return w->conn != nullptr || closing; });
            got = w->conn;
        }
        {
            std::lock_guard lock(m);
            pierce.erase(token);
            if (got) peers[user] = got;
        }
        if (got) {
            got->username = user;
            spawn([this, got] { peer_loop(got); });
        }
        return got;
    }

    // Messages on a peer connection until it closes.
    void peer_loop(ConnPtr c) {
        uint32_t code = 0;
        std::string payload;
        while (!closing && read_peer_frame(c, code, payload)) {
            switch (code) {
                case proto::kSearchReply:
                    if (auto r = proto::parse_search_reply(payload)) {
                        std::shared_ptr<Search> s;
                        {
                            std::lock_guard lock(m);
                            if (const auto it = searches.find(r->ticket); it != searches.end()) s = it->second;
                        }
                        if (s) {
                            std::lock_guard lock(s->m);
                            s->results.push_back(std::move(*r));
                        }
                    }
                    break;
                case proto::kTransferRequest:
                    if (const auto t = proto::parse_transfer_request(payload)) on_transfer_request(c, *t);
                    break;
                case proto::kPlaceInQueue:
                    if (const auto p = proto::parse_place_in_queue(payload)) {
                        if (const auto d = find_download(c->username, p->filename)) {
                            std::lock_guard lock(d->m);
                            d->place = int(p->place);
                        }
                    }
                    break;
                case proto::kUploadFailed:
                    if (const auto f = proto::parse_filename(payload)) fail_download(c->username, *f, "the peer's upload failed");
                    break;
                case proto::kQueueFailed:
                    if (const auto q = proto::parse_queue_failed(payload)) fail_download(c->username, q->filename, q->reason.empty() ? "the peer refused the download" : q->reason);
                    break;
                default: break;
            }
        }
        forget(c);
    }

    std::shared_ptr<Download> find_download(const std::string& user, const std::string& filename) {
        std::lock_guard lock(m);
        for (const auto& d : downloads)
            if (d->username == user && d->filename == filename) return d;
        return nullptr;
    }
    void fail_download(const std::string& user, const std::string& filename, const std::string& why) {
        if (const auto d = find_download(user, filename)) {
            std::lock_guard lock(d->m);
            if (d->error.empty() && !d->done) d->error = why;
            d->cv.notify_all();
        }
    }

    void on_transfer_request(const ConnPtr& c, const proto::TransferRequest& t) {
        const auto d = t.direction == 1 ? find_download(c->username, t.filename) : nullptr;
        if (!d) {  // we upload nothing, and we don't know this download
            c->send_all(proto::transfer_reply(t.ticket, false, t.direction == 1 ? "Cancelled" : "File not shared."));
            return;
        }
        {
            std::lock_guard lock(d->m);
            d->ticket = t.ticket, d->size = t.size;
        }
        {
            std::lock_guard lock(m);
            by_ticket[t.ticket] = d;
        }
        c->send_all(proto::transfer_reply(t.ticket, true));  // the peer now opens a file connection with this ticket
    }

    // The connection a file arrives on: the uploader sends the transfer ticket, we answer with the offset, then the bytes.
    void file_connection(ConnPtr c) {
        uint32_t ticket = 0;
        std::shared_ptr<Download> d;
        if (c->recv_exact(&ticket, 4, [&] { return closing.load(); })) {
            std::lock_guard lock(m);
            if (const auto it = by_ticket.find(ticket); it != by_ticket.end()) d = it->second;
        }
        if (!d) return forget(c);
        receive_file(c, d);
        forget(c);
    }

    void receive_file(const ConnPtr& c, const std::shared_ptr<Download>& d) {
        uint64_t size;
        {
            std::lock_guard lock(d->m);
            size = d->size;
            d->started = true;
            d->cv.notify_all();
        }
        const auto fail = [&](const std::string& why) {
            std::lock_guard lock(d->m);
            if (d->error.empty() && !d->done) d->error = why;
            d->cv.notify_all();
        };
        if (!c->send_all(proto::Writer().u64(0).data())) return fail("couldn't start the transfer");
        std::ofstream out(std::filesystem::path(widen(d->dest)), std::ios::binary | std::ios::trunc);
        if (!out) return fail("can't write the file");
        c->set_timeout(5000);
        std::string buf(64 * 1024, '\0');
        uint64_t got = 0;
        while (got < size && !d->aborted && !closing) {
            const int r = recv(c->s, buf.data(), int(std::min<uint64_t>(buf.size(), size - got)), 0);
            if (r > 0) {
                out.write(buf.data(), r);
                got += uint64_t(r);
                std::lock_guard lock(d->m);
                d->received = got;
                if (d->options.progress) d->options.progress(got, size);
            } else if (r < 0 && WSAGetLastError() == WSAETIMEDOUT) {
                continue;  // the stall timer is the download's
            } else {
                break;
            }
        }
        out.close();
        std::lock_guard lock(d->m);
        if (got >= size) d->done = true;
        else if (d->error.empty() && !d->aborted) d->error = "the connection closed before the file was complete";
        d->cv.notify_all();
    }

    // MARK: Connections from peers
    void accept_loop() {
        while (!closing) {
            sockaddr_in from{};
            int len = sizeof from;
            SOCKET s = accept(listener, reinterpret_cast<sockaddr*>(&from), &len);
            if (s == INVALID_SOCKET) break;
            auto c = track(s);
            spawn([this, c] { incoming(c); });
        }
    }

    void incoming(ConnPtr c) {
        uint32_t len = 0;
        uint8_t code = 0;
        c->set_timeout(30000);
        if (!c->recv_exact(&len, 4, [&] { return closing.load(); }) || len < 1 || len > 4096 || !c->recv_exact(&code, 1, [&] { return closing.load(); })) return forget(c);
        std::string body(len - 1, '\0');
        if (!body.empty() && !c->recv_exact(body.data(), body.size(), [&] { return closing.load(); })) return forget(c);
        if (code == proto::kPierceFirewall) {
            proto::Reader r(body);
            const uint32_t token = r.u32();
            std::shared_ptr<Pierce> w;
            {
                std::lock_guard lock(m);
                if (const auto it = pierce.find(token); it != pierce.end()) w = it->second;
            }
            if (!w) return forget(c);  // nobody is waiting for that token
            std::lock_guard lock(w->m);
            w->conn = c;  // the requester takes it from here
            w->cv.notify_all();
            return;
        }
        if (code != proto::kPeerInit) return forget(c);
        proto::Reader r(body);
        const std::string user = r.str(), type = r.str();
        if (!r.ok()) return forget(c);
        c->username = user;
        if (type == "P") peer_loop(c);
        else if (type == "F") file_connection(c);
        else forget(c);
    }

    bool start_listener() {
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET) return false;
        BOOL reuse = TRUE;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof reuse);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons(uint16_t(opt.listen_port));
        if (bind(listener, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(listener, 32) != 0) {
            // The port is taken (another Soulseek client): peers can't reach us directly, but the relay still works.
            closesocket(listener);
            listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            a.sin_port = 0;
            if (bind(listener, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(listener, 32) != 0) return false;
        }
        int len = sizeof a;
        getsockname(listener, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        spawn([this] { accept_loop(); });
        return true;
    }
};

Client::Client(ClientOptions options) : p_(std::make_unique<Impl>(std::move(options))) {}
Client::~Client() { close(); }

int Client::listen_port() const { return p_->port; }
bool Client::connected() const { return p_->server_up && !p_->closing; }

std::string Client::login(const std::string& username, const std::string& password) {
    auto& s = *p_;
    if (s.server_up) return "";
    s.username = username;
    if (s.listener == INVALID_SOCKET && !s.start_listener()) return "Can't open a port for Soulseek peers.";
    SOCKET sock = tcp_connect(s.opt.server_host, s.opt.server_port, s.opt.connect_timeout);
    if (sock == INVALID_SOCKET) return "Can't reach the Soulseek server (" + s.opt.server_host + ").";
    auto c = s.track(sock);
    c->set_timeout(int(std::chrono::milliseconds(s.opt.login_timeout).count()));
    if (!c->send_all(proto::login_request(username, password))) return s.forget(c), "Can't reach the Soulseek server.";
    uint32_t code = 0;
    std::string payload;
    if (!s.read_peer_frame(c, code, payload) || code != proto::kLogin) return s.forget(c), "The Soulseek server didn't answer the login.";
    const auto reply = proto::parse_login_reply(payload);
    if (!reply) return s.forget(c), "The Soulseek server sent something unexpected.";
    if (!reply->success)
        return s.forget(c), "Soulseek login failed: " + (reply->reason.empty() ? std::string("refused") : reply->reason) +
                                ". Check the username and password (a new username is created on first login; if it's taken by someone else, pick another).";
    c->set_timeout(0);
    {
        std::lock_guard lock(s.m);
        s.server = c;
    }
    s.server_up = true;
    c->send_all(proto::set_listen_port(uint32_t(s.port)));
    c->send_all(proto::set_status(2));
    c->send_all(proto::shared_folders_files(s.opt.shared_folders, s.opt.shared_files));
    c->send_all(proto::accept_children(false));
    c->send_all(proto::toggle_parent_search(false));
    s.spawn([&s, c] { s.server_loop(c); });
    s.spawn([&s] { s.keep_alive(); });
    return "";
}

void Client::close() {
    auto& s = *p_;
    if (s.closing.exchange(true)) return;
    s.wake.notify_all();
    {
        std::lock_guard lock(s.m);
        if (s.listener != INVALID_SOCKET) closesocket(s.listener);
        s.listener = INVALID_SOCKET;
        for (const auto& c : s.all) c->close();
        for (auto& [token, w] : s.pierce) w->cv.notify_all();
        for (auto& [user, w] : s.addr_waits) w->cv.notify_all();
        for (auto& d : s.downloads) {
            d->aborted = true;
            d->cv.notify_all();
        }
    }
    std::vector<std::thread> threads;
    {
        std::lock_guard lock(s.threads_m);
        threads.swap(s.threads);
    }
    for (auto& t : threads)
        if (t.joinable()) t.join();
    s.server_up = false;
}

std::vector<UserResult> Client::search(const std::string& query, std::chrono::seconds wait, const std::function<bool()>& cancel) {
    auto& s = *p_;
    auto state = std::make_shared<Search>();
    const uint32_t ticket = s.next_token();
    ConnPtr server;
    {
        std::lock_guard lock(s.m);
        s.searches[ticket] = state;
        server = s.server;
    }
    std::vector<UserResult> out;
    if (server && server->send_all(proto::file_search(ticket, query))) {
        const auto end = Clock::now() + wait;
        while (Clock::now() < end && !s.closing && !(cancel && cancel())) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    {
        std::lock_guard lock(s.m);
        s.searches.erase(ticket);
    }
    std::lock_guard lock(state->m);
    return state->results;
}

DownloadResult Client::download(const std::string& user, const std::string& filename, const std::string& dest, const DownloadOptions& options) {
    auto& s = *p_;
    if (!connected()) return {false, "not connected to Soulseek"};
    auto d = std::make_shared<Download>();
    d->username = user, d->filename = filename, d->dest = dest, d->options = options;
    {
        std::lock_guard lock(s.m);
        s.downloads.push_back(d);
    }
    const auto cleanup = [&] {
        std::lock_guard lock(s.m);
        s.downloads.erase(std::remove(s.downloads.begin(), s.downloads.end(), d), s.downloads.end());
        for (auto it = s.by_ticket.begin(); it != s.by_ticket.end();)
            it = it->second == d ? s.by_ticket.erase(it) : std::next(it);
    };
    const auto finish = [&](bool ok, std::string error) {
        d->aborted = true;
        cleanup();
        DownloadResult r{ok, std::move(error), d->size};
        return r;
    };

    const ConnPtr peer = s.connect_peer(user);
    if (!peer) return finish(false, "can't connect to " + user);
    if (!peer->send_all(proto::queue_upload(filename))) return finish(false, "can't reach " + user);

    const auto started = Clock::now();
    auto last_progress = started;
    uint64_t last_bytes = 0;
    std::unique_lock lock(d->m);
    for (;;) {
        d->cv.wait_for(lock, std::chrono::milliseconds(250));
        if (s.closing || (options.cancel && options.cancel())) return lock.unlock(), finish(false, "cancelled");
        if (d->done) break;
        if (!d->error.empty()) {
            std::string e = d->error;
            lock.unlock();
            return finish(false, e);
        }
        const auto now = Clock::now();
        if (d->started) {
            if (d->received > last_bytes) last_bytes = d->received, last_progress = now;
            else if (now - last_progress > options.stall_timeout) return lock.unlock(), finish(false, "stalled");
        } else if (now - started > options.queue_timeout) {
            const int place = d->place;
            lock.unlock();
            return finish(false, "still queued (place " + (place >= 0 ? std::to_string(place) : std::string("?")) + ")");
        }
    }
    lock.unlock();
    return finish(true, "");
}

}  // namespace wb::slsk
