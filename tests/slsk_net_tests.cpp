// The Soulseek client over real sockets on 127.0.0.1, against a stand-in server and stand-in peers written here with the same
// (aioslsk-checked) codec: logging in and failing to, searching (answers from peers that connect to us and from peers the
// server relays to us), downloading (direct, through the relay, queued, refused, stalled, cancelled) and closing cleanly.
// Nothing here touches the real Soulseek network.
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

#include "library/store.h"
#include "model/paths.h"
#include "model/settings.h"
#include "net/slsk/client.h"
#include "net/slsk/sync.h"
#include "net/slsk/protocol.h"

namespace fs = std::filesystem;
namespace proto = wb::slsk::proto;
using namespace std::chrono_literals;
using namespace wb::slsk;
static int failures = 0;

#define CHECK(cond, ...)                                                         \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ++failures;                                                          \
            std::fprintf(stderr, "FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond); \
            std::fprintf(stderr, "" __VA_ARGS__);                                \
            std::fputc('\n', stderr);                                            \
        }                                                                        \
    } while (0)

static bool wait_for(const std::function<bool()>& cond, std::chrono::milliseconds limit = 10s) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!cond()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

// MARK: Socket helpers

struct Sock {
    SOCKET s = INVALID_SOCKET;
    Sock() = default;
    explicit Sock(SOCKET x) : s(x) {}
    Sock(const Sock&) = delete;
    Sock(Sock&& o) noexcept : s(o.s) { o.s = INVALID_SOCKET; }
    Sock& operator=(Sock&& o) noexcept {
        close();
        s = o.s, o.s = INVALID_SOCKET;
        return *this;
    }
    ~Sock() { close(); }
    void close() {
        if (s != INVALID_SOCKET) shutdown(s, SD_BOTH), closesocket(s);
        s = INVALID_SOCKET;
    }
    bool send_all(const std::string& d) const {
        size_t at = 0;
        while (at < d.size()) {
            const int n = send(s, d.data() + at, int(d.size() - at), 0);
            if (n <= 0) return false;
            at += size_t(n);
        }
        return true;
    }
    bool recv_exact(void* out, size_t n) const {
        size_t at = 0;
        while (at < n) {
            const int r = recv(s, static_cast<char*>(out) + at, int(n - at), 0);
            if (r <= 0) return false;
            at += size_t(r);
        }
        return true;
    }
    // A frame with a uint32 code (server and peer messages).
    bool frame(uint32_t& code, std::string& payload) const {
        uint32_t len = 0;
        if (!recv_exact(&len, 4) || len < 4) return false;
        std::string body(len, '\0');
        if (!recv_exact(body.data(), len)) return false;
        std::memcpy(&code, body.data(), 4);
        payload = body.substr(4);
        return true;
    }
    // The first frame on a peer connection: uint8 code.
    bool init_frame(uint8_t& code, std::string& payload) const {
        uint32_t len = 0;
        if (!recv_exact(&len, 4) || len < 1) return false;
        std::string body(len, '\0');
        if (!recv_exact(body.data(), len)) return false;
        code = uint8_t(body[0]);
        payload = body.substr(1);
        return true;
    }
};

static Sock listen_local(int& port) {
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    bind(l, reinterpret_cast<sockaddr*>(&a), sizeof a);
    listen(l, 16);
    int len = sizeof a;
    getsockname(l, reinterpret_cast<sockaddr*>(&a), &len);
    port = ntohs(a.sin_port);
    return Sock(l);
}
static Sock connect_local(int port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(uint16_t(port));
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        closesocket(s);
        return Sock();
    }
    return Sock(s);
}

// Runs a function on a thread per accepted connection until stopped.
struct Acceptor {
    Sock listener;
    int port = 0;
    std::thread loop;
    std::mutex m;
    std::vector<std::thread> workers;
    std::vector<std::shared_ptr<Sock>> live;
    explicit Acceptor(std::function<void(std::shared_ptr<Sock>)> on_conn) {
        listener = listen_local(port);
        loop = std::thread([this, on_conn] {
            for (;;) {
                SOCKET c = accept(listener.s, nullptr, nullptr);
                if (c == INVALID_SOCKET) return;
                auto sock = std::make_shared<Sock>(c);
                std::lock_guard lock(m);
                live.push_back(sock);
                workers.emplace_back([on_conn, sock] { on_conn(sock); });
            }
        });
    }
    void stop() {
        listener.close();
        if (loop.joinable()) loop.join();
        {
            std::lock_guard lock(m);
            for (auto& s : live) s->close();
        }
        for (auto& w : workers)
            if (w.joinable()) w.join();
    }
    ~Acceptor() { stop(); }
};

// MARK: Stand-in server

struct FakeServer {
    std::atomic<int> logins{0};
    std::atomic<int> client_port{0};
    std::string expect_user = "dj", expect_password = "pw";
    std::string peer_ip = "127.0.0.1";
    std::atomic<int> peer_port{0};  // 0: GetPeerAddress says "not logged in", forcing the relay
    std::mutex m;
    std::vector<std::pair<uint32_t, std::string>> searches;       // ticket, query
    std::vector<std::tuple<uint32_t, std::string, std::string>> relays;  // ticket, user, type (ConnectToPeer requests)
    std::function<void(uint32_t ticket, const std::string& query)> on_search;
    std::function<void(uint32_t ticket, const std::string& user, const std::string& type)> on_relay;
    std::shared_ptr<Sock> client;  // the connection to relay things down
    Acceptor acceptor{[this](std::shared_ptr<Sock> s) { serve(s); }};

    void serve(std::shared_ptr<Sock> s) {
        uint32_t code;
        std::string payload;
        if (!s->frame(code, payload) || code != proto::kLogin) return;
        proto::Reader r(payload);
        const std::string user = r.str(), pass = r.str();
        const uint32_t version = r.u32();
        const std::string hash = r.str();
        const uint32_t minor = r.u32();
        const bool ok = user == expect_user && pass == expect_password && hash == proto::md5_hex(user + pass) && version == 175 && minor == 1;
        if (!ok) {
            s->send_all(proto::server_frame(proto::kLogin, proto::Writer().boolean(false).str("INVALIDPASS").data()));
            return;
        }
        s->send_all(proto::server_frame(proto::kLogin, proto::Writer().boolean(true).str("Welcome").ip("127.0.0.1").str(proto::md5_hex(pass)).boolean(false).data()));
        ++logins;
        {
            std::lock_guard lock(m);
            client = s;
        }
        while (s->frame(code, payload)) {
            proto::Reader q(payload);
            switch (code) {
                case proto::kSetListenPort: client_port = int(q.u32()); break;
                case proto::kFileSearch: {
                    const uint32_t ticket = q.u32();
                    const std::string query = q.str();
                    {
                        std::lock_guard lock(m);
                        searches.push_back({ticket, query});
                    }
                    if (on_search) on_search(ticket, query);
                    break;
                }
                case proto::kGetPeerAddress: {
                    const std::string who = q.str();
                    s->send_all(proto::server_frame(proto::kGetPeerAddress, proto::Writer().str(who).ip(peer_port ? peer_ip : "0.0.0.0").u32(uint32_t(peer_port.load())).data()));
                    break;
                }
                case proto::kConnectToPeer: {
                    const uint32_t ticket = q.u32();
                    const std::string who = q.str(), type = q.str();
                    {
                        std::lock_guard lock(m);
                        relays.push_back({ticket, who, type});
                    }
                    if (on_relay) on_relay(ticket, who, type);
                    break;
                }
                default: break;
            }
        }
    }
    // The server telling the client "this peer wants you to connect to it".
    void relay_to_client(const std::string& user, const std::string& type, int port, uint32_t ticket) {
        std::shared_ptr<Sock> c;
        {
            std::lock_guard lock(m);
            c = client;
        }
        if (c) c->send_all(proto::server_frame(proto::kConnectToPeer, proto::Writer().str(user).str(type).ip("127.0.0.1").u32(uint32_t(port)).u32(ticket).boolean(false).data()));
    }
};

// MARK: Stand-in peer (the uploader)

struct FakePeer {
    std::string name = "bob";
    FakeServer& server;
    std::string data;                   // the file it will upload
    std::string filename = "Music\\Skrillex\\Selecta.flac";
    enum class Mode { send_file, queued, refuse, stall, never } mode = Mode::send_file;
    bool request_on_new_connection = true;  // else on the connection the client opened
    bool file_through_relay = false;        // else the peer connects to the client for the file
    std::atomic<int> queue_requests{0}, files_sent{0};
    std::mutex upload_m, reply_m;
    std::condition_variable reply_cv;
    std::optional<proto::TransferReply> last_reply;
    std::vector<std::thread> uploads;
    Acceptor acceptor{[this](std::shared_ptr<Sock> s) { serve(s); }};

    explicit FakePeer(FakeServer& srv) : server(srv) { server.peer_port = acceptor.port; }
    ~FakePeer() {
        acceptor.stop();
        std::lock_guard lock(upload_m);
        for (auto& t : uploads) t.join();
    }

    // The client connected to us (directly, or after the relay: PierceFirewall).
    void serve(std::shared_ptr<Sock> s) {
        uint8_t code;
        std::string payload;
        if (!s->init_frame(code, payload)) return;
        if (code == proto::kPeerInit || code == proto::kPierceFirewall) peer_session(s);
    }

    void peer_session(std::shared_ptr<Sock> s) {
        uint32_t code;
        std::string payload;
        while (s->frame(code, payload)) {
            if (code == proto::kTransferReply) {  // the answer to a request we made on this very connection
                std::lock_guard lock(reply_m);
                last_reply = proto::parse_transfer_reply(payload);
                reply_cv.notify_all();
                continue;
            }
            if (code != proto::kQueueUpload) continue;
            ++queue_requests;
            const std::string wanted = proto::parse_filename(payload).value_or("");
            if (wanted != filename || mode == Mode::refuse) {
                s->send_all(proto::queue_failed(wanted, "File not shared."));
                continue;
            }
            if (mode == Mode::queued) {
                s->send_all(proto::place_in_queue(wanted, 4));
                continue;
            }
            if (mode == Mode::never) continue;
            std::lock_guard lock(upload_m);
            uploads.emplace_back([this, s] { upload(s); });
        }
    }

    // Our side of an upload: ask permission (TransferRequest), then deliver on a file connection.
    void upload(std::shared_ptr<Sock> client_conn) {
        const uint32_t ticket = 9000 + uint32_t(files_sent.load());
        std::shared_ptr<Sock> request_conn = client_conn;
        Sock own;
        if (request_on_new_connection) {
            own = connect_local(server.client_port);
            if (own.s == INVALID_SOCKET || !own.send_all(proto::peer_init(name, "P", 0))) return;
        }
        const Sock& rc = request_on_new_connection ? own : *request_conn;
        if (!rc.send_all(proto::transfer_request(1, ticket, filename, data.size()))) return;
        // The reply: on the connection we asked on.
        std::optional<proto::TransferReply> reply;
        if (request_on_new_connection) {
            uint32_t code;
            std::string payload;
            if (!rc.frame(code, payload) || code != proto::kTransferReply) return;
            reply = proto::parse_transfer_reply(payload);
        } else {  // peer_session is reading this connection: it hands the reply over
            std::unique_lock lock(reply_m);
            reply_cv.wait_for(lock, 5s, [&] { return last_reply.has_value(); });
            reply = last_reply;
            last_reply.reset();
        }
        if (!reply || !reply->allowed || reply->ticket != ticket) return;

        Sock file;
        if (file_through_relay) {
            // Ask the server to relay "connect to me" to the client; the client then connects here and pierces our firewall.
            std::mutex dm;
            std::condition_variable dcv;
            std::shared_ptr<Sock> accepted;
            Acceptor relay_acceptor([&](std::shared_ptr<Sock> c) {
                uint8_t ic;
                std::string ip;
                if (!c->init_frame(ic, ip) || ic != proto::kPierceFirewall) return;
                uint32_t token = 0;
                std::memcpy(&token, ip.data(), 4);
                if (token != 777) return;
                uint32_t t = ticket;
                c->send_all(std::string(reinterpret_cast<char*>(&t), 4));
                uint64_t offset = 1;
                if (!c->recv_exact(&offset, 8) || offset != 0) return;
                deliver(*c);
            });
            server.relay_to_client(name, "F", relay_acceptor.port, 777);
            std::this_thread::sleep_for(1500ms);  // (the acceptor's worker does the rest)
            return;
        }
        file = connect_local(server.client_port);
        if (file.s == INVALID_SOCKET || !file.send_all(proto::peer_init(name, "F", 0))) return;
        uint32_t t = ticket;
        if (!file.send_all(std::string(reinterpret_cast<char*>(&t), 4))) return;
        uint64_t offset = 1;
        if (!file.recv_exact(&offset, 8) || offset != 0) return;
        deliver(file);
    }

    void deliver(const Sock& file) {
        if (mode == Mode::stall) {
            file.send_all(data.substr(0, data.size() / 2));
            std::this_thread::sleep_for(3s);
            return;
        }
        file.send_all(data);
        ++files_sent;
        std::this_thread::sleep_for(100ms);  // the downloader closes first, as on the real network
    }
};

static std::string make_data(size_t n) {
    std::string d(n, '\0');
    uint32_t x = 12345;
    for (auto& c : d) x = x * 1664525u + 1013904223u, c = char(x >> 24);
    return d;
}
static std::string read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

static ClientOptions options_for(FakeServer& srv) {
    ClientOptions o;
    o.server_host = "127.0.0.1";
    o.server_port = srv.acceptor.port;
    o.listen_port = 0;
    o.connect_timeout = 3s;
    o.indirect_timeout = 3s;
    o.login_timeout = 3s;
    return o;
}

// MARK: Tests

static void login_tests() {
    FakeServer srv;
    {
        Client c(options_for(srv));
        const std::string err = c.login("dj", "wrong");
        CHECK(err.find("Soulseek login failed: INVALIDPASS") == 0, "%s", err.c_str());
        CHECK(!c.connected() && srv.logins == 0);
    }
    {
        Client c(options_for(srv));
        CHECK(c.login("dj", "pw").empty() && c.connected());
        CHECK(wait_for([&] { return srv.client_port == c.listen_port() && srv.client_port > 0; }), "the server learns our listening port");
        CHECK(c.login("dj", "pw").empty(), "logging in again is a no-op");
        c.close();
        CHECK(!c.connected());
    }
    {
        ClientOptions o = options_for(srv);
        o.server_port = 1;  // nothing there
        Client c(o);
        const std::string err = c.login("dj", "pw");
        CHECK(err.find("Can't reach the Soulseek server") == 0, "%s", err.c_str());
    }
    std::puts("  login: ok, wrong password, no server");
}

static void search_tests() {
    FakeServer srv;
    FakePeer peer(srv);
    peer.name = "bob";
    UserResult from_bob;
    from_bob.username = "bob", from_bob.free_slots = true, from_bob.avg_speed = 1000, from_bob.queue_size = 2;
    FileEntry f;
    f.filename = "Music\\Skrillex\\Selecta.flac", f.size = 30000000, f.extension = "flac", f.attributes[kAttrDuration] = 190;
    from_bob.files = {f};

    // Bob connects to us with the answer; Carol can't, so the server relays and we connect to her.
    srv.on_search = [&](uint32_t ticket, const std::string&) {
        std::thread([&, ticket] {
            auto r = from_bob;
            r.ticket = ticket;
            Sock c = connect_local(srv.client_port);
            c.send_all(proto::peer_init("bob", "P", 0));
            c.send_all(proto::search_reply(r));
            std::this_thread::sleep_for(300ms);
        }).detach();
        std::thread([&, ticket] {
            int port = 0;
            Sock l = listen_local(port);
            srv.relay_to_client("carol", "P", port, 4321);
            Sock c(accept(l.s, nullptr, nullptr));
            uint8_t code;
            std::string payload;
            if (!c.init_frame(code, payload) || code != proto::kPierceFirewall) return;
            UserResult r = from_bob;
            r.username = "carol", r.ticket = ticket;
            c.send_all(proto::search_reply(r));
            std::this_thread::sleep_for(300ms);
        }).detach();
    };
    Client c(options_for(srv));
    CHECK(c.login("dj", "pw").empty());
    wait_for([&] { return srv.client_port > 0; });
    const auto results = c.search("skrillex selecta", 2s);
    CHECK(results.size() == 2, "%zu answers", results.size());
    bool bob = false, carol = false;
    for (const auto& r : results) {
        bob |= r.username == "bob", carol |= r.username == "carol";
        CHECK(r.files.size() == 1 && r.files[0].size == 30000000 && r.files[0].attributes.at(kAttrDuration) == 190 && r.free_slots);
    }
    CHECK(bob && carol, "answers from the peer that connected to us and from the one the server relayed");
    {
        std::lock_guard lock(srv.m);
        CHECK(srv.searches.size() == 1 && srv.searches[0].second == "skrillex selecta");
    }
    // A cancelled search returns at once; an answer for a finished search is ignored.
    const auto t0 = std::chrono::steady_clock::now();
    c.search("x", 30s, [] { return true; });
    CHECK(std::chrono::steady_clock::now() - t0 < 2s);
    std::puts("  search: answers from a direct peer and a relayed peer");
}

struct DownloadCase {
    const char* name;
    std::function<void(FakeServer&, FakePeer&)> setup;
    bool direct = true;  // GetPeerAddress knows the peer; else the relay is needed
    bool ok = true;
    std::string error_has;
    DownloadOptions options;
};

static void download_cases(const fs::path& dir) {
    const std::string data = make_data(300000);
    std::vector<DownloadCase> cases;
    cases.push_back({"direct, request on a new connection", [](FakeServer&, FakePeer&) {}});
    cases.push_back({"direct, request on our connection", [](FakeServer&, FakePeer& p) { p.request_on_new_connection = false; }});
    cases.push_back({"file connection through the relay", [](FakeServer&, FakePeer& p) { p.file_through_relay = true; }});
    cases.push_back({"peer behind a firewall (relay for the first connection)", [](FakeServer&, FakePeer&) {}, false});
    {
        DownloadCase c{"stays queued", [](FakeServer&, FakePeer& p) { p.mode = FakePeer::Mode::queued; }, true, false, "still queued (place 4)"};
        c.options.queue_timeout = 1s;
        cases.push_back(std::move(c));
    }
    cases.push_back({"refused", [](FakeServer&, FakePeer& p) { p.mode = FakePeer::Mode::refuse; }, true, false, "File not shared."});
    {
        DownloadCase c{"stalls half way", [](FakeServer&, FakePeer& p) { p.mode = FakePeer::Mode::stall; }, true, false, "stalled"};
        c.options.stall_timeout = 1s;
        cases.push_back(std::move(c));
    }
    {
        DownloadCase c{"peer never answers", [](FakeServer&, FakePeer& p) { p.mode = FakePeer::Mode::never; }, true, false, "still queued (place ?)"};
        c.options.queue_timeout = 1s;
        cases.push_back(std::move(c));
    }

    for (auto& dc : cases) {
        FakeServer srv;
        FakePeer peer(srv);
        peer.data = data;
        dc.setup(srv, peer);
        if (!dc.direct) {
            srv.peer_port = 0;  // "not logged in": the client must ask the server to relay
            srv.on_relay = [&](uint32_t ticket, const std::string&, const std::string& type) {
                if (type == "P")  // the peer connects to the client and pierces its firewall
                    std::thread([&, ticket] {
                        Sock c = connect_local(srv.client_port);
                        c.send_all(proto::pierce_firewall(ticket));
                        peer.peer_session(std::make_shared<Sock>(std::move(c)));
                    }).detach();
            };
        }
        Client c(options_for(srv));
        CHECK(c.login("dj", "pw").empty(), "%s", dc.name);
        wait_for([&] { return srv.client_port > 0; });
        const fs::path dest = dir / "dl.part";
        fs::remove(dest);
        uint64_t last_done = 0, last_total = 0;
        dc.options.progress = [&](uint64_t d, uint64_t t) { last_done = d, last_total = t; };
        const auto r = c.download("bob", peer.filename, dest.string(), dc.options);
        if (dc.ok) {
            CHECK(r.ok && r.size == data.size(), "%s: %s", dc.name, r.error.c_str());
            CHECK(read_all(dest) == data, "%s: the file differs (%zu bytes)", dc.name, read_all(dest).size());
            CHECK(last_done == data.size() && last_total == data.size(), "%s: progress %llu / %llu", dc.name, (unsigned long long)last_done, (unsigned long long)last_total);
        } else {
            CHECK(!r.ok && r.error.find(dc.error_has) != std::string::npos, "%s: expected '%s', got ok=%d '%s'", dc.name, dc.error_has.c_str(), int(r.ok), r.error.c_str());
        }
        std::printf("  download, %s: %s\n", dc.name, r.ok ? "file delivered" : r.error.c_str());
    }

    // Cancelling, and asking for a file the peer doesn't have.
    {
        FakeServer srv;
        FakePeer peer(srv);
        peer.data = data;
        peer.mode = FakePeer::Mode::never;
        Client c(options_for(srv));
        CHECK(c.login("dj", "pw").empty());
        std::atomic<bool> cancel{false};
        std::thread t([&] {
            std::this_thread::sleep_for(500ms);
            cancel = true;
        });
        DownloadOptions o;
        o.cancel = [&] { return cancel.load(); };
        const auto t0 = std::chrono::steady_clock::now();
        const auto r = c.download("bob", peer.filename, (dir / "c.part").string(), o);
        t.join();
        CHECK(!r.ok && r.error == "cancelled" && std::chrono::steady_clock::now() - t0 < 3s, "cancel: '%s'", r.error.c_str());
    }
    {
        FakeServer srv;
        FakePeer peer(srv);
        Client c(options_for(srv));
        CHECK(c.login("dj", "pw").empty());
        const auto r = c.download("bob", "not\\shared.mp3", (dir / "x.part").string());
        CHECK(!r.ok && r.error == "File not shared.", "%s", r.error.c_str());
        c.close();
        const auto after = c.download("bob", "a", (dir / "y.part").string());
        CHECK(!after.ok && after.error == "not connected to Soulseek");
    }
    // Closing while a download is waiting ends it and returns promptly.
    {
        FakeServer srv;
        FakePeer peer(srv);
        peer.mode = FakePeer::Mode::never;
        auto c = std::make_unique<Client>(options_for(srv));
        CHECK(c->login("dj", "pw").empty());
        DownloadResult r;
        std::thread t([&] { r = c->download("bob", peer.filename, (dir / "z.part").string()); });
        std::this_thread::sleep_for(500ms);
        const auto t0 = std::chrono::steady_clock::now();
        c->close();
        t.join();
        CHECK(!r.ok && std::chrono::steady_clock::now() - t0 < 3s, "close while downloading");
    }
}

// The whole stack: the sync loop, the network backend, the client, a stand-in server and peer.
static void end_to_end(const fs::path& dir) {
    const fs::path root = dir / "lib";
    wb::paths::init(root);
    wb::Settings::load();
    wb::Library lib;
    lib.built_at = wb::iso_seconds_now();
    lib.spotify_user = "me";
    wb::LibraryTrack t;
    t.id = "spotify:1", t.artists = {"Skrillex"}, t.title = "Selecta", t.duration_ms = 200000, t.file_name = "Skrillex - Selecta", t.first_added = "2026-01-01";
    lib.tracks = {t};
    wb::paths::write_atomic(wb::paths::library_file(), lib.to_json().dump());
    wb::LibraryStore store;
    store.load();

    FakeServer srv;
    FakePeer peer(srv);
    peer.filename = "Music\\Skrillex\\Skrillex - Selecta.flac";
    peer.data = make_data(700000);
    srv.on_search = [&](uint32_t ticket, const std::string&) {
        std::lock_guard lock(peer.upload_m);
        peer.uploads.emplace_back([&, ticket] {
            UserResult r;
            r.username = "bob", r.ticket = ticket, r.free_slots = true, r.avg_speed = 500000;
            FileEntry f;
            f.filename = peer.filename, f.size = peer.data.size(), f.extension = "flac", f.attributes[kAttrDuration] = 200;
            r.files = {f};
            Sock c = connect_local(srv.client_port);
            c.send_all(proto::peer_init("bob", "P", 0));
            c.send_all(proto::search_reply(r));
            std::this_thread::sleep_for(300ms);
        });
    };
    const std::string toml = std::string("[soulseek]\nusername = \"dj\"\npassword = \"pw\"\nlisten_port = 0\nserver_host = \"127.0.0.1\"\nserver_port = ") +
                             std::to_string(srv.acceptor.port) + "\n[sync]\nmax_concurrent = 1\nsearch_wait_seconds = 1\nsearch_gap_seconds = 0\n";
    Runner runner(store, SyncConfig::parse(toml), make_network_backend(SyncConfig::parse(toml), 0, 0));
    runner.run(true);  // login, one pass, close
    const auto inbox_file = wb::paths::inbox() / L"Skrillex - Selecta.flac";
    CHECK(fs::exists(inbox_file) && read_all(inbox_file) == peer.data, "the track arrived in _inbox");
    const std::string sync = read_all(wb::paths::soulseek_dir() / L"sync.json");
    CHECK(sync.find("\"status\": \"done\"") != std::string::npos && sync.find("bob:Music") != std::string::npos, "%s", sync.c_str());
    const std::string log = read_all(wb::paths::soulseek_dir() / L"sync.log");
    CHECK(log.find("Logged in to Soulseek as dj") != std::string::npos && log.find("\xE2\x9C\x93 Skrillex \xE2\x80\x93 Selecta \xE2\x86\x92 _inbox/Skrillex - Selecta.flac") != std::string::npos, "%s", log.c_str());
    std::puts("  end to end: the sync loop found, downloaded and delivered a track through the real client");
}


int main() {
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
    const fs::path dir = fs::temp_directory_path() / L"wreckbox-slsk-net-tests";
    fs::remove_all(dir);
    fs::create_directories(dir);
    login_tests();
    search_tests();
    download_cases(dir);
    end_to_end(dir);
    fs::remove_all(dir);
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all slsk network tests passed");
    return 0;
}
