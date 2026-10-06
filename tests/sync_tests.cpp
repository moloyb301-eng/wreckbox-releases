// Phone sync, account and tunnel tests. The sync server is exercised over real HTTP on 127.0.0.1 (tokens, the crate, whole
// files, Range requests, ids that need URL-decoding); the account against a small fake of the account service (nothing here
// ever touches the real one); the tunnel with a stand-in for cloudflared (a .cmd script that prints an address and keeps
// running). Live checks of the real services are the user's: see docs/PLAN.md phase 6.
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <thread>

#include <httplib.h>

#include "library/store.h"
#include "model/paths.h"
#include "model/settings.h"
#include "net/account.h"
#include "net/http_client.h"
#include "net/sync_server.h"
#include "net/tunnel.h"

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using wb::json;
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
        std::this_thread::sleep_for(20ms);
    }
    return true;
}

static void write_click_wav(const fs::path& p, double bpm, int seconds) {
    const uint32_t rate = 22050;
    std::vector<int16_t> s(size_t(rate) * seconds);
    const size_t period = size_t(rate * 60.0 / bpm);
    for (size_t i = 0; i < s.size(); ++i) s[i] = i % period < 200 ? int16_t(12000 * std::sin(double(i) * 0.3)) : 0;
    std::ofstream f(p, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t data = uint32_t(s.size() * 2);
    f.write("RIFF", 4), u32(36 + data), f.write("WAVEfmt ", 8), u32(16), u16(1), u16(1), u32(rate), u32(rate * 2), u16(2), u16(16);
    f.write("data", 4), u32(data), f.write(reinterpret_cast<const char*>(s.data()), data);
}

static wb::LibraryTrack make_track(std::string id, std::string artist, std::string title) {
    wb::LibraryTrack t;
    t.id = std::move(id);
    t.artists = {std::move(artist)};
    t.title = std::move(title);
    t.file_name = wb::safe_file_name(t.artist() + " - " + t.title);
    return t;
}

static std::string read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

static std::string path_utf8(const fs::path& p) { return reinterpret_cast<const char*>(p.u8string().c_str()); }

// MARK: Library fixture

static const std::string kOddId = "isrc:odd id/1";  // a colon, a space and a slash: the phone sends it URL-encoded

static void make_library(const fs::path& root, wb::LibraryStore& store) {
    fs::remove_all(root);
    wb::paths::init(root);
    wb::Settings::load();
    wb::Library lib;
    lib.built_at = wb::iso_seconds_now();
    lib.spotify_user = "me";
    lib.tracks = {make_track("spotify:a", "Test Artist", "Click Song"), make_track("spotify:b", "Other", "Missing Song"), make_track(kOddId, "Odd", "Name")};
    wb::LibraryPlaylist pl;
    pl.name = "Bangers";
    pl.track_ids = {"spotify:a", "spotify:b", kOddId};
    lib.playlists = {pl};
    wb::paths::write_atomic(wb::paths::library_file(), lib.to_json().dump());
    fs::create_directories(wb::paths::tracks());
    write_click_wav(wb::paths::tracks() / L"Test Artist - Click Song.wav", 120, 8);
    write_click_wav(wb::paths::tracks() / L"Odd - Name.wav", 128, 4);
    store.load();
    store.set_scan_folders({path_utf8(wb::paths::tracks())});
    store.rescan();
}

// MARK: Server

static void server_tests(wb::LibraryStore& store) {
    wb::sync::Server server(store);
    CHECK(server.start(0, "127.0.0.1") && server.running() && server.port() > 0);
    const std::string base = "http://127.0.0.1:" + std::to_string(server.port());
    const std::string token = server.token();
    CHECK(token.size() == 24 && std::regex_match(token, std::regex("[A-Za-z0-9_-]+")), "token '%s'", token.c_str());
    CHECK(wb::Settings::current().desktop_pair_token == token);
    CHECK(wb::sync::Server::new_token() != wb::sync::Server::new_token());

    // The token check comes first, on every route.
    for (const char* path : {"/info", "/library.json", "/crate", "/file/spotify%3Aa", "/art/spotify%3Aa", "/nothing"}) {
        CHECK(wb::http::get(base + path).status == 403, "%s without a token", path);
        CHECK(wb::http::get(base + path, {{"x-wreckbox-token", "wrong"}}).status == 403, "%s with a wrong token", path);
        CHECK(wb::http::get(base + path + "?t=wrong").status == 403);
    }
    const wb::http::Headers auth{{"x-wreckbox-token", token}};

    const auto info = wb::http::get(base + "/info", auth);
    const json ij = json::parse(info.body, nullptr, false);
    CHECK(info.status == 200 && ij.value("tracks", -1) == 2 && ij.value("name", "").rfind("WreckBox on ", 0) == 0, "%s", info.body.c_str());
    CHECK(wb::http::get(base + "/info?t=" + token).status == 200);  // the query parameter works too

    const auto lib = wb::http::get(base + "/library.json", auth);
    CHECK(lib.status == 200 && lib.body == read_all(wb::paths::library_file()) && lib.header("content-type").rfind("application/json", 0) == 0);

    // The crate: downloaded tracks whose file exists, with size and analysis.
    const auto crate = wb::http::get(base + "/crate", auth);
    const json cj = json::parse(crate.body, nullptr, false);
    CHECK(crate.status == 200 && cj.is_array() && cj.size() == 2, "%s", crate.body.c_str());
    const fs::path click = wb::paths::tracks() / L"Test Artist - Click Song.wav";
    for (const auto& c : cj) {
        CHECK(c["ext"] == ".wav" && c["size"].is_number());
        if (c["id"] == "spotify:a") {
            CHECK(c["size"].get<uintmax_t>() == fs::file_size(click));
            CHECK(c["analysis"].is_object() && std::abs(c["analysis"].value("bpm", 0.0) - 120.0) < 1.0, "%s", c["analysis"].dump().c_str());
        } else {
            CHECK(c["id"] == kOddId);
        }
    }

    // A whole file, then ranges.
    const std::string bytes = read_all(click);
    const auto whole = wb::http::get(base + "/file/spotify%3Aa", auth);
    CHECK(whole.status == 200 && whole.body == bytes && whole.header("accept-ranges") == "bytes" && whole.header("content-type") == "audio/wav",
          "status %d, %zu of %zu bytes", whole.status, whole.body.size(), bytes.size());
    const auto odd = wb::http::get(base + "/file/" + wb::http::url_encode(kOddId), auth);
    CHECK(odd.status == 200 && odd.body == read_all(wb::paths::tracks() / L"Odd - Name.wav"), "odd id: status %d", odd.status);

    const size_t len = bytes.size();
    const auto range = [&](const std::string& spec) { return wb::http::get(base + "/file/spotify%3Aa", {{"x-wreckbox-token", token}, {"Range", spec}}); };
    auto r = range("bytes=100-199");
    CHECK(r.status == 206 && r.body == bytes.substr(100, 100) && r.header("content-range") == "bytes 100-199/" + std::to_string(len), "206: %d '%s'", r.status,
          r.header("content-range").c_str());
    r = range("bytes=" + std::to_string(len - 10) + "-");
    CHECK(r.status == 206 && r.body == bytes.substr(len - 10), "open-ended: %d %zu", r.status, r.body.size());
    r = range("bytes=-50");
    CHECK(r.status == 206 && r.body == bytes.substr(len - 50), "suffix: %d %zu", r.status, r.body.size());
    r = range("bytes=0-" + std::to_string(len * 2));  // an end past the file is cut at the end
    CHECK(r.status == 206 && r.body == bytes, "end past the file: %d %zu", r.status, r.body.size());
    r = range("bytes=" + std::to_string(len + 5) + "-" + std::to_string(len + 9));
    CHECK(r.status == 416, "start past the end: %d", r.status);

    CHECK(wb::http::get(base + "/file/spotify%3Ab", auth).status == 404);  // in the library, not downloaded
    CHECK(wb::http::get(base + "/file/nope", auth).status == 404);
    CHECK(wb::http::get(base + "/art/spotify%3Aa", auth).status == 404);  // no cover and no URL to fetch it from
    CHECK(wb::http::get(base + "/nothing", auth).status == 404);

    // The pairing link.
    const std::string uri = server.pairing_uri();
    CHECK(std::regex_match(uri, std::regex("wreckbox://pair\\?hosts=[0-9.,]*&port=" + std::to_string(server.port()) + "&t=" + token)), "%s", uri.c_str());

    // Unpairing: the old token stops working at once, the new one works, and it is saved.
    server.reset_token();
    CHECK(wb::http::get(base + "/info", auth).status == 403);
    CHECK(server.token() != token && wb::http::get(base + "/info", {{"x-wreckbox-token", server.token()}}).status == 200);
    CHECK(wb::Settings::current().desktop_pair_token == server.token());
    wb::Settings::load();
    CHECK(wb::Settings::current().desktop_pair_token == server.token(), "saved to settings.json");

    server.stop();
    CHECK(!server.running() && wb::http::get(base + "/info", {{"x-wreckbox-token", server.token()}}, 2s).status == 0);
    CHECK(server.start(0, "127.0.0.1") && wb::http::get("http://127.0.0.1:" + std::to_string(server.port()) + "/info?t=" + server.token()).status == 200,
          "restart");
    const auto addrs = wb::sync::Server::local_addresses();
    for (const auto& a : addrs) CHECK(std::regex_match(a, std::regex("(10\\.|192\\.168\\.|172\\.(1[6-9]|2[0-9]|3[01])\\.).*")), "%s", a.c_str());
}

// MARK: Account (against a fake service)

struct FakeService {
    httplib::Server svr;
    std::thread thread;
    int port = 0;
    std::mutex m;
    std::string key = "", email, name;  // the one account
    std::string session;
    std::map<std::string, std::string> blobs;
    json device;
    std::vector<std::string> log;  // "METHOD path"
    std::string last_device_header;
    std::atomic<int> library_puts{0};
    bool revoke_next = false;

    FakeService() {
        auto reply = [](httplib::Response& res, int status, const json& j) {
            res.status = status;
            res.set_content(j.dump(), "application/json");
        };
        svr.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response&) {
            std::lock_guard lock(m);
            log.push_back(req.method + " " + req.path);
            last_device_header = req.get_header_value("x-wreckbox-device");
            return httplib::Server::HandlerResponse::Unhandled;
        });
        svr.Post("/v1/signup", [this, reply](const httplib::Request& req, httplib::Response& res) {
            const json b = json::parse(req.body);
            std::lock_guard lock(m);
            if (b["key"].get<std::string>().size() != 64) return reply(res, 400, {{"error", "bad request"}});
            key = b["key"], email = b["email"], name = b["name"], session = "tok-signup";
            reply(res, 200, {{"token", session}, {"user", {{"id", "u1"}, {"email", email}, {"name", name}}}});
        });
        svr.Post("/v1/login", [this, reply](const httplib::Request& req, httplib::Response& res) {
            const json b = json::parse(req.body);
            std::lock_guard lock(m);
            std::string given = b["email"];
            for (auto& c : given) c = char(std::tolower(static_cast<unsigned char>(c)));  // the real service normalises emails too
            if (given != email || b["key"] != key) return reply(res, 401, {{"error", "Wrong email or password."}});
            session = "tok-login";
            reply(res, 200, {{"token", session}, {"user", {{"id", "u1"}, {"email", email}, {"name", name}}}});
        });
        auto authed = [this](const httplib::Request& req) {
            std::lock_guard lock(m);
            if (revoke_next) return revoke_next = false, false;
            return req.get_header_value("authorization") == "Bearer " + session;
        };
        svr.Post("/v1/logout", [authed, reply](const httplib::Request& req, httplib::Response& res) {
            reply(res, authed(req) ? 200 : 401, {{"ok", true}});
        });
        svr.Post("/v1/password", [this, authed, reply](const httplib::Request& req, httplib::Response& res) {
            if (!authed(req)) return reply(res, 401, {{"error", "Please sign in again."}});
            const json b = json::parse(req.body);
            std::lock_guard lock(m);
            if (b["oldKey"] != key) return reply(res, 401, {{"error", "Current password is wrong."}});
            key = b["newKey"];
            reply(res, 200, {{"ok", true}});
        });
        svr.Put(R"(/v1/blob/(\w+))", [this, authed, reply](const httplib::Request& req, httplib::Response& res) {
            if (!authed(req)) return reply(res, 401, {{"error", "Please sign in again."}});
            std::lock_guard lock(m);
            blobs[req.matches[1]] = req.body;
            if (req.matches[1] == "library") ++library_puts;
            reply(res, 200, {{"ok", true}});
        });
        svr.Post("/v1/devices", [this, authed, reply](const httplib::Request& req, httplib::Response& res) {
            if (!authed(req)) return reply(res, 401, {{"error", "Please sign in again."}});
            std::lock_guard lock(m);
            device = json::parse(req.body);
            reply(res, 200, {{"ok", true}});
        });
        port = svr.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { svr.listen_after_bind(); });
    }
    ~FakeService() {
        svr.stop();
        thread.join();
    }
};

static void account_tests(wb::LibraryStore& store) {
    // PBKDF2-HMAC-SHA256 against the published test vectors.
    CHECK(wb::account::pbkdf2_hex("password", "salt", 1) == "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    CHECK(wb::account::pbkdf2_hex("password", "salt", 4096) == "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
    // The salt is "wreckbox:" + the trimmed, lower-cased email (what the Flutter build and the server expect).
    CHECK(wb::account::derive_key("  Me@Example.COM ", "hunter22", 7) == wb::account::pbkdf2_hex("hunter22", "wreckbox:me@example.com", 7));
    CHECK(wb::account::derive_key("a@b.co", "x").size() == 64);

    FakeService fake;
    _putenv_s("WRECKBOX_API", ("http://127.0.0.1:" + std::to_string(fake.port)).c_str());
    auto& s = wb::Settings::current();
    CHECK(!wb::account::signed_in());
    const std::string device = wb::account::device_id();
    CHECK(device.size() == 24 && wb::account::device_id() == device && s.device_id == device);

    auto fails = [](const std::function<void()>& f) -> std::string {
        try {
            f();
        } catch (const wb::account::Error& e) {
            return e.what();
        }
        return "";
    };
    CHECK(fails([] { wb::account::sign_up("me@example.com", "short", "Me"); }) == "Use at least 8 characters for the password.");
    CHECK(fake.log.empty(), "a short password never reaches the server");
    CHECK(fails([] { wb::account::sign_up("me@example.com", "longenough", " Me "); }).empty());
    CHECK(wb::account::signed_in() && s.account_token == "tok-signup" && s.account_email == "me@example.com" && s.account_name == "Me");
    CHECK(fake.last_device_header == device);
    CHECK(fake.key == wb::account::derive_key("me@example.com", "longenough") && fake.key.find("longenough") == std::string::npos);

    // Uploading: the library as is, plus a summary of the crate with no file paths.
    wb::account::upload_library(store);
    const json summary = json::parse(fake.blobs["state"]);
    CHECK(fake.blobs["library"] == store.library()->to_json().dump());
    const json& tracks = summary.at("tracks");
    CHECK(summary.at("from") == device && tracks.at("spotify:a").at("s") == "downloaded" && tracks.at(kOddId).at("s") == "downloaded" && !tracks.contains("spotify:b"),
          "%s", fake.blobs["state"].c_str());  // a track the computer has never seen has no entry
    CHECK(std::abs(tracks.at("spotify:a").value("bpm", 0.0) - 120.0) < 1.0 && tracks.at("spotify:a").contains("camelot"));
    CHECK(fake.blobs["state"].find("\\\\") == std::string::npos && fake.blobs["state"].find("wav") == std::string::npos, "no paths in the summary");

    // This computer's address, and taking it back.
    wb::account::register_computer("WreckBox on PC", "windows", "https://x.trycloudflare.com", "tok");
    CHECK(fake.device["id"] == device && fake.device["url"] == "https://x.trycloudflare.com" && fake.device["syncToken"] == "tok" && fake.device["platform"] == "windows");
    wb::account::register_computer("WreckBox on PC", "windows");
    CHECK(fake.device["url"].is_null() && fake.device["syncToken"].is_null());

    // Changing the password: both keys are derived here.
    CHECK(fails([] { wb::account::change_password("longenough", "tiny"); }) == "Use at least 8 characters for the password.");
    CHECK(fails([] { wb::account::change_password("wrong-old", "brandnewpass"); }) == "Current password is wrong.");
    CHECK(fails([] { wb::account::change_password("longenough", "brandnewpass"); }).empty());
    CHECK(fake.key == wb::account::derive_key("me@example.com", "brandnewpass"));

    // Signing out, a wrong password, signing in.
    wb::account::sign_out();
    CHECK(!wb::account::signed_in() && !s.account_token);
    CHECK(fails([] { wb::account::sign_in("me@example.com", "longenough"); }) == "Wrong email or password.");
    CHECK(!wb::account::signed_in());
    CHECK(fails([] { wb::account::sign_in("ME@example.com ", "brandnewpass"); }).empty() && s.account_token == "tok-login");
    wb::Settings::load();
    CHECK(wb::account::signed_in() && s.account_email == "me@example.com", "the session is saved to settings.json");

    // A session the server no longer knows ends here too.
    fake.revoke_next = true;
    CHECK(!fails([&] { wb::account::upload_library(store); }).empty() && !wb::account::signed_in(), "401 signs out");
    CHECK(fails([] { wb::account::sign_in("me@example.com", "brandnewpass"); }).empty());

    // Scheduled upload: many changes in a burst → one upload, after the quiet period.
    const int before = fake.library_puts;
    for (int i = 0; i < 4; ++i) {
        wb::account::schedule_upload(store, 700ms);
        std::this_thread::sleep_for(150ms);
    }
    CHECK(fake.library_puts == before, "nothing yet");
    CHECK(wait_for([&] { return fake.library_puts == before + 1; }, 5s), "one upload after the burst");
    std::this_thread::sleep_for(1200ms);
    CHECK(fake.library_puts == before + 1, "and only one");
    wb::account::shutdown();

    // No service at all: a readable message.
    _putenv_s("WRECKBOX_API", "http://127.0.0.1:1");
    CHECK(!fails([&] { wb::account::upload_library(store); }).empty());
    wb::account::sign_out();  // offline: still signs out locally
    CHECK(!wb::account::signed_in());
    _putenv_s("WRECKBOX_API", "");
}

// MARK: Tunnel (with a stand-in for cloudflared)

static void tunnel_tests(wb::LibraryStore& store) {
    using wb::sync::find_tunnel_url;
    CHECK(find_tunnel_url("2026-10-06T10:00:00Z INF |  https://quiet-river-1234.trycloudflare.com                          |") ==
          std::optional<std::string>("https://quiet-river-1234.trycloudflare.com"));
    CHECK(!find_tunnel_url("INF Requesting new quick Tunnel on trycloudflare.com...") && !find_tunnel_url(""));
    CHECK(!find_tunnel_url("ERR Post \"https://api.trycloudflare.com/tunnel\": dial tcp: no such host"), "the service's own address isn't the tunnel");
    CHECK(wb::sync::Tunnel::binary_path().filename() == L"cloudflared.exe");

    const fs::path dir = fs::temp_directory_path() / L"wreckbox-tunnel-tests";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path alive = dir / L"alive.txt";
    // "cloudflared": says its address on stderr, then keeps a heartbeat file growing until it's killed.
    auto script = [&](const std::string& body) {
        const fs::path f = dir / L"fake.cmd";
        std::ofstream(f) << "@echo off\r\n" << body;
        return f;
    };
    auto hooks_for = [&](const fs::path& cmd, std::vector<std::optional<std::string>>& announced, std::mutex& m, bool signed_in = true) {
        wb::sync::Tunnel::Hooks h;
        h.signed_in = [signed_in] { return signed_in; };
        h.binary = [](const auto&) { return std::optional<fs::path>(L"cmd.exe"); };
        h.command_line = [cmd](const fs::path&, int) { return L"cmd.exe /d /c " + cmd.wstring(); };
        h.announce = [&announced, &m](const std::optional<std::string>& url) {
            std::lock_guard lock(m);
            announced.push_back(url);
        };
        h.heartbeat = 400ms, h.retry = 300ms, h.url_timeout = 3s;
        return h;
    };
    wb::sync::Server server(store);
    CHECK(server.start(0, "127.0.0.1"));  // so the tunnel finds a server running
    std::mutex m;

    {  // Up, heartbeats, then stop: gone at once, process killed, the account told.
        std::vector<std::optional<std::string>> announced;
        wb::sync::Tunnel t(server, hooks_for(script("echo INF  https://stand-in-one.trycloudflare.com 1>&2\r\n:loop\r\necho x>>\"" + path_utf8(alive) + "\"\r\nping -n 2 127.0.0.1 >nul\r\ngoto loop\r\n"), announced, m));
        CHECK(!t.running() && t.status() == "Off");
        std::atomic<int> changes{0};
        t.on_changed = [&] { ++changes; };
        t.start();
        CHECK(wait_for([&] { return t.running(); }), "status '%s'", t.status().c_str());
        CHECK(t.url() == std::optional<std::string>("https://stand-in-one.trycloudflare.com") && t.status() == "Reachable from anywhere");
        CHECK(wait_for([&] {
            std::lock_guard lock(m);
            return announced.size() >= 3;
        }), "heartbeat: %zu announcements", announced.size());
        {
            std::lock_guard lock(m);
            CHECK(announced[0] == t.url() && announced[1] == t.url());
        }
        CHECK(changes >= 2);
        const auto grew = fs::file_size(alive);
        std::this_thread::sleep_for(1500ms);
        CHECK(fs::file_size(alive) > grew, "the stand-in is running");
        t.start();  // already wanted: nothing new
        const auto t0 = std::chrono::steady_clock::now();
        t.stop();
        CHECK(std::chrono::steady_clock::now() - t0 < 500ms && !t.running() && t.status() == "Off" && !t.url(), "stop returns at once");
        CHECK(wait_for([&] {
            std::lock_guard lock(m);
            return !announced.empty() && !announced.back();
        }), "the account is told it's gone");
        std::this_thread::sleep_for(800ms);
        const auto after = fs::file_size(alive);
        std::this_thread::sleep_for(1500ms);
        CHECK(fs::file_size(alive) == after, "the process is killed");
    }

    {  // The process quits: it's brought back after the retry delay.
        std::vector<std::optional<std::string>> announced;
        wb::sync::Tunnel t(server, hooks_for(script("echo https://flaky.trycloudflare.com 1>&2\r\nping -n 2 127.0.0.1 >nul\r\n"), announced, m));
        t.start();
        CHECK(wait_for([&] {
            std::lock_guard lock(m);
            return announced.size() >= 2;
        }, 15s), "reconnected: %zu announcements, status '%s'", announced.size(), t.status().c_str());
        t.stop();
    }

    {  // No address in time, and not signed in: said plainly, nothing left running.
        std::vector<std::optional<std::string>> announced;
        wb::sync::Tunnel t(server, hooks_for(script("ping -n 6 127.0.0.1 >nul\r\n"), announced, m));
        t.start();
        CHECK(wait_for([&] { return t.status().rfind("Couldn't connect", 0) == 0; }, 8s), "status '%s'", t.status().c_str());
        CHECK(!t.running() && announced.empty());
        std::vector<std::optional<std::string>> none;
        wb::sync::Tunnel off(server, hooks_for(script("echo hi\r\n"), none, m, false));
        off.start();
        CHECK(wait_for([&] { return off.status() == "Sign in to your WreckBox account first."; }), "status '%s'", off.status().c_str());
        CHECK(!off.running() && none.empty());
    }
    server.stop();
    fs::remove_all(dir);
}

int main() {
    const fs::path root = fs::temp_directory_path() / L"wreckbox-sync-tests";
    wb::LibraryStore store;
    make_library(root, store);
    server_tests(store);
    account_tests(store);
    tunnel_tests(store);
    fs::remove_all(root);
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all sync tests passed");
    return 0;
}
