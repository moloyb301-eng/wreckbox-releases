// The native Soulseek client: matching and ranking (against recorded runs of the Python sidecar), the wire protocol (against
// bytes made by aioslsk), the client over real sockets against a stand-in server and peer, and the sync loop.
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

#include "model/model.h"
#include "net/slsk/match.h"
#include "net/slsk/protocol.h"

namespace fs = std::filesystem;
using wb::json;
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

static std::string read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
static json golden() { return json::parse(read_all(fs::path(WB_FIXTURES) / "slsk_golden.json")); }

static wb::LibraryTrack track_from(const json& j) {
    wb::LibraryTrack t;
    t.id = j["id"];
    for (const auto& a : j["artists"]) t.artists.push_back(a);
    t.title = j["title"];
    if (!j["durationMs"].is_null()) t.duration_ms = j["durationMs"].get<int64_t>();
    return t;
}

// MARK: Matching

static void matching_tests() {
    const json g = golden();
    for (const auto& c : g["norm"]) CHECK(norm(c[0]) == c[1].get<std::string>(), "norm('%s') = '%s', Python says '%s'", c[0].get<std::string>().c_str(), norm(c[0]).c_str(), c[1].get<std::string>().c_str());
    for (const auto& c : g["clean"]) CHECK(clean_title(c[0]) == c[1].get<std::string>(), "clean_title('%s') = '%s', Python says '%s'", c[0].get<std::string>().c_str(), clean_title(c[0]).c_str(), c[1].get<std::string>().c_str());
    for (const auto& c : g["queries"]) {
        const auto qs = search_queries(track_from(c["track"]), c["custom"]);
        CHECK(json(qs) == c["queries"], "queries for %s: %s vs %s", c["track"]["title"].get<std::string>().c_str(), json(qs).dump().c_str(), c["queries"].dump().c_str());
    }
    for (const auto& c : g["quality"]) {
        std::optional<int> br;
        if (!c[1].is_null()) br = c[1].get<int>();
        const auto q = quality_of(c[0], br, c[2]);
        const bool same = c[3].is_null() ? !q : (q && std::fabs(*q - c[3].get<double>()) < 1e-9);
        CHECK(same, "quality(%s, %s) = %s, Python says %s", c[0].get<std::string>().c_str(), c[1].dump().c_str(), q ? std::to_string(*q).c_str() : "none", c[3].dump().c_str());
    }
    // "Smaller files": a good MP3 / AAC beats lossless; lossless still beats nothing; below the limit is still out.
    CHECK(*quality_of("mp3", 320, 256, true) > *quality_of("flac", std::nullopt, 256, true) && *quality_of("m4a", 256, 256, true) > *quality_of("m4a", std::nullopt, 256, true));
    CHECK(*quality_of("flac", std::nullopt, 256) > *quality_of("mp3", 320, 256) && !quality_of("mp3", 192, 256, true) && quality_of("wav", std::nullopt, 256, true));
    for (const auto& c : g["matches"]) {
        std::optional<int> d;
        if (!c["duration"].is_null()) d = c["duration"].get<int>();
        CHECK(file_matches(track_from(c["track"]), c["path"], d, c["tol"]) == c["result"].get<bool>(), "file_matches('%s' vs '%s') should be %d", c["track"]["title"].get<std::string>().c_str(),
              c["path"].get<std::string>().c_str(), int(c["result"].get<bool>()));
    }
    for (const auto& c : g["rank"]) {
        std::vector<UserResult> results;
        for (const auto& r : c["results"]) {
            UserResult u;
            u.username = r["username"], u.free_slots = r["free"], u.avg_speed = r["speed"], u.queue_size = r["queue"];
            for (const auto& f : r["files"]) {
                FileEntry e;
                e.filename = f["filename"], e.size = f["size"], e.extension = f["ext"];
                if (!f["br"].is_null()) e.attributes[kAttrBitrate] = f["br"];
                if (!f["dur"].is_null()) e.attributes[kAttrDuration] = f["dur"];
                u.files.push_back(std::move(e));
            }
            results.push_back(std::move(u));
        }
        std::optional<std::string> loose;
        if (!c["loose"].is_null()) loose = c["loose"].get<std::string>();
        const auto got = rank(track_from(c["track"]), results, MatchConfig{}, loose);
        const auto& want = c["expected"];
        CHECK(got.size() == want.size(), "rank: %zu candidates, Python found %zu", got.size(), want.size());
        for (size_t i = 0; i < std::min(got.size(), want.size()); ++i)
            CHECK(got[i].username == want[i]["username"].get<std::string>() && got[i].path == want[i]["path"].get<std::string>() && got[i].ext == want[i]["ext"].get<std::string>() && got[i].size == want[i]["size"].get<uint64_t>() &&
                      std::fabs(got[i].quality - want[i]["quality"].get<double>()) < 1e-9 && got[i].label() == want[i]["label"].get<std::string>(),
                  "rank #%zu: %s '%s' vs Python %s '%s'", i, got[i].username.c_str(), got[i].label().c_str(), want[i]["username"].get<std::string>().c_str(), want[i]["label"].get<std::string>().c_str());
    }
    std::printf("  matching: %zu norm, %zu clean, %zu query, %zu quality, %zu match and %zu rank cases agree with the Python\n", g["norm"].size(), g["clean"].size(), g["queries"].size(),
                g["quality"].size(), g["matches"].size(), g["rank"].size());
}

// MARK: Wire format

static std::string hex_of(const std::string& s) {
    static const char* d = "0123456789abcdef";
    std::string out;
    for (const unsigned char c : s) out += {d[c >> 4], d[c & 15]};
    return out;
}
static std::string unhex(const std::string& h) {
    std::string out;
    for (size_t i = 0; i + 1 < h.size(); i += 2) out += char(std::stoi(h.substr(i, 2), nullptr, 16));
    return out;
}

static void protocol_tests() {
    namespace p = wb::slsk::proto;
    const json g = json::parse(read_all(fs::path(WB_FIXTURES) / "slsk_wire.json"));
    const auto want = [&](const char* name) { return g[name].get<std::string>(); };
    const auto same = [&](const char* name, const std::string& frame) { CHECK(hex_of(frame) == want(name), "%s:\n   ours    %s\n   aioslsk %s", name, hex_of(frame).c_str(), want(name).c_str()); };
    const auto payload = [&](const char* name, size_t header = 8) { return unhex(want(name)).substr(header); };

    // What we send, byte for byte as aioslsk sends it.
    same("login_request", p::login_request("dj_test", "p@ssw0rd"));
    same("set_listen_port", p::set_listen_port(60000));
    same("set_status", p::set_status(2));
    same("shared_folders_files", p::shared_folders_files(3, 1234));
    same("accept_children", p::accept_children(false));
    same("toggle_parent_search", p::toggle_parent_search(false));
    same("ping", p::ping());
    same("file_search", p::file_search(4242, "skrillex selecta"));
    same("get_peer_address", p::get_peer_address("bob"));
    same("connect_to_peer_request", p::connect_to_peer(77, "bob", "P"));
    same("peer_init", p::init_frame(p::kPeerInit, wb::slsk::proto::Writer().str("dj_test").str("P").u32(0).data()));
    same("peer_init", p::peer_init("dj_test", "P", 0));
    same("pierce_firewall", p::pierce_firewall(1234567));
    same("queue_upload", p::queue_upload("Music\\Skrillex\\Selecta \xE2\x9C\x93.flac"));
    same("transfer_request_upload", p::transfer_request(1, 555, "Music\\Skrillex\\Selecta.flac", 30000000));
    same("transfer_request_download", p::transfer_request(0, 556, "a\\b.mp3"));
    same("transfer_reply_ok", p::transfer_reply(555, true));
    same("transfer_reply_size", p::transfer_reply(555, true, std::nullopt, 30000000));
    same("transfer_reply_no", p::transfer_reply(555, false, "Cancelled"));
    same("place_in_queue_request", p::place_in_queue_request("a\\b.mp3"));
    same("place_in_queue", p::place_in_queue("a\\b.mp3", 7));
    same("upload_failed", p::upload_failed("a\\b.mp3"));
    same("queue_failed", p::queue_failed("a\\b.mp3", "File not shared."));

    // What we receive, as aioslsk makes it.
    const auto ok = p::parse_login_reply(payload("login_ok"));
    CHECK(ok && ok->success && ok->greeting == "Welcome!" && ok->ip == "203.0.113.7" && ok->md5 == p::md5_hex("p@ssw0rd") && !ok->supporter);
    const auto bad = p::parse_login_reply(payload("login_fail"));
    CHECK(bad && !bad->success && bad->reason == "INVALIDPASS");
    const auto addr = p::parse_peer_address(payload("peer_address"));
    CHECK(addr && addr->username == "bob" && addr->ip == "198.51.100.20" && addr->port == 2234);
    const auto con = p::parse_connect_to_peer(payload("connect_to_peer_response"));
    CHECK(con && con->username == "bob" && con->type == "F" && con->ip == "198.51.100.20" && con->port == 2234 && con->ticket == 99 && !con->privileged);
    const auto up = p::parse_transfer_request(payload("transfer_request_upload"));
    CHECK(up && up->direction == 1 && up->ticket == 555 && up->filename == "Music\\Skrillex\\Selecta.flac" && up->size == 30000000);
    const auto down = p::parse_transfer_request(payload("transfer_request_download"));
    CHECK(down && down->direction == 0 && down->ticket == 556 && down->size == 0);
    const auto r_ok = p::parse_transfer_reply(payload("transfer_reply_ok")), r_size = p::parse_transfer_reply(payload("transfer_reply_size")),
               r_no = p::parse_transfer_reply(payload("transfer_reply_no"));
    CHECK(r_ok && r_ok->allowed && r_ok->ticket == 555 && r_size && r_size->size == 30000000 && r_no && !r_no->allowed && r_no->reason == "Cancelled");
    const auto place = p::parse_place_in_queue(payload("place_in_queue"));
    CHECK(place && place->filename == "a\\b.mp3" && place->place == 7);
    const auto qf = p::parse_queue_failed(payload("queue_failed"));
    CHECK(qf && qf->filename == "a\\b.mp3" && qf->reason == "File not shared.");
    CHECK(p::parse_filename(payload("upload_failed")) == std::optional<std::string>("a\\b.mp3"));
    // A frame's header is its length (counting what follows) and the code.
    CHECK(unhex(want("ping")) == std::string("\x04\0\0\0\x20\0\0\0", 8) && unhex(want("pierce_firewall")).size() == 4 + 1 + 4);

    // A search reply: what we parse out of aioslsk's, and our own body, inflated, byte for byte.
    CHECK(g["search_reply_inflated_matches"].get<bool>());
    const auto sr = p::parse_search_reply(payload("search_reply"));
    CHECK(sr && sr->username == "bob" && sr->ticket == 4242 && sr->files.size() == 3 && sr->free_slots && sr->avg_speed == 2000000 && sr->queue_size == 3);
    if (sr && sr->files.size() == 3) {
        CHECK(sr->files[0].filename == "Music\\Skrillex\\Selecta.flac" && sr->files[0].size == 30000000 && sr->files[0].extension == "flac" && sr->files[0].attributes.at(1) == 190 &&
              sr->files[0].attributes.at(4) == 44100 && sr->files[0].attributes.at(5) == 16);
        CHECK(sr->files[1].attributes.at(wb::slsk::kAttrBitrate) == 320 && sr->files[2].filename == "\xC3\x9Cn\xC3\xAF" "code\\Zo\xC3\xAB \xE2\x9C\x93.ogg");
        const auto ours = p::search_reply(*sr);
        const auto inflated = p::zlib_decompress(ours.substr(8));
        CHECK(inflated && hex_of(*inflated) == want("search_reply_uncompressed_body"), "our search reply body differs");
    }
    // Garbage never crashes the parsers.
    for (const std::string junk : {std::string(), std::string("\xFF\xFF\xFF\xFF"), std::string("\x05\0\0\0abc", 8), std::string(40, '\x7F')}) {
        p::parse_login_reply(junk), p::parse_peer_address(junk), p::parse_connect_to_peer(junk), p::parse_transfer_request(junk), p::parse_transfer_reply(junk),
            p::parse_search_reply(junk), p::parse_place_in_queue(junk), p::parse_queue_failed(junk), p::parse_filename(junk);
    }
    CHECK(!p::parse_search_reply("not zlib at all") && !p::parse_filename(std::string("\x10\0\0\0short", 9)));
    // A name that isn't UTF-8 (a Windows-1252 client) is decoded to UTF-8.
    CHECK(p::parse_filename(std::string("\x05\0\0\0" "Caf\xE9!", 9)) == std::optional<std::string>("Caf\xC3\xA9!"));
    std::printf("  protocol: %zu messages agree with aioslsk's bytes\n", g.size() - 1);
}

int main() {
    matching_tests();
    protocol_tests();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all slsk tests passed");
    return 0;
}
