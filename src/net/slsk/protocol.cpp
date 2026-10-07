#include "net/slsk/protocol.h"

#include <windows.h>
#include <bcrypt.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <zlib.h>

#include <cstring>

namespace wb::slsk::proto {
namespace {

bool valid_utf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const size_t n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : size_t(-1);
        if (n == size_t(-1)) return false;
        for (size_t k = 1; k <= n; ++k)
            if (i + k >= s.size() || (static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        i += n + 1;
    }
    return true;
}

std::string from_cp1252(const std::string& s) {
    if (s.empty()) return s;
    const int w = MultiByteToWideChar(1252, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring wide(size_t(w), L'\0');
    MultiByteToWideChar(1252, 0, s.data(), int(s.size()), wide.data(), w);
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), out.data(), n, nullptr, nullptr);
    return out;
}

void write_file(Writer& w, const FileEntry& f) {
    w.u8(1).str(f.filename).u64(f.size).str(f.extension).u32(uint32_t(f.attributes.size()));
    for (const auto& [k, v] : f.attributes) w.u32(k).u32(v);
}

}  // namespace

// MARK: Writer / Reader

Writer& Writer::u8(uint8_t v) { return buf_.push_back(char(v)), *this; }
Writer& Writer::u32(uint32_t v) { return buf_.append(reinterpret_cast<const char*>(&v), 4), *this; }  // little-endian PCs
Writer& Writer::u64(uint64_t v) { return buf_.append(reinterpret_cast<const char*>(&v), 8), *this; }
Writer& Writer::boolean(bool v) { return u8(v ? 1 : 0); }
Writer& Writer::str(const std::string& v) { return u32(uint32_t(v.size())), buf_.append(v), *this; }
Writer& Writer::ip(const std::string& dotted) {
    in_addr a{};
    inet_pton(AF_INET, dotted.c_str(), &a);
    const auto* b = reinterpret_cast<const unsigned char*>(&a);  // network order: a.b.c.d
    for (int i = 3; i >= 0; --i) u8(b[i]);
    return *this;
}

bool Reader::need(size_t n) {
    if (!ok_ || pos_ + n > d_.size()) ok_ = false;
    return ok_;
}
uint8_t Reader::u8() { return need(1) ? static_cast<uint8_t>(d_[pos_++]) : 0; }
uint32_t Reader::u32() {
    uint32_t v = 0;
    if (need(4)) std::memcpy(&v, d_.data() + pos_, 4), pos_ += 4;
    return v;
}
uint64_t Reader::u64() {
    uint64_t v = 0;
    if (need(8)) std::memcpy(&v, d_.data() + pos_, 8), pos_ += 8;
    return v;
}
bool Reader::boolean() { return u8() != 0; }
std::string Reader::str() {
    const uint32_t n = u32();
    if (!ok_ || !need(n)) return {};
    std::string s = d_.substr(pos_, n);
    pos_ += n;
    return valid_utf8(s) ? s : from_cp1252(s);
}
std::string Reader::ip() {
    if (!need(4)) return {};
    char buf[16];
    std::snprintf(buf, sizeof buf, "%u.%u.%u.%u", unsigned(uint8_t(d_[pos_ + 3])), unsigned(uint8_t(d_[pos_ + 2])), unsigned(uint8_t(d_[pos_ + 1])), unsigned(uint8_t(d_[pos_])));
    pos_ += 4;
    return buf;
}

// MARK: Helpers

std::string md5_hex(const std::string& s) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0);
    unsigned char out[16]{};
    BCryptHash(alg, nullptr, 0, PUCHAR(s.data()), ULONG(s.size()), out, sizeof out);
    BCryptCloseAlgorithmProvider(alg, 0);
    static const char* d = "0123456789abcdef";
    std::string hex;
    for (const unsigned char b : out) hex += {d[b >> 4], d[b & 15]};
    return hex;
}

std::string server_frame(uint32_t code, const std::string& payload) {
    Writer w;
    w.u32(uint32_t(4 + payload.size())).u32(code);
    return w.data() + payload;
}
std::string init_frame(uint8_t code, const std::string& payload) {
    Writer w;
    w.u32(uint32_t(1 + payload.size())).u8(code);
    return w.data() + payload;
}

std::string zlib_compress(const std::string& s) {
    uLongf n = compressBound(uLong(s.size()));
    std::string out(n, '\0');
    compress(reinterpret_cast<Bytef*>(out.data()), &n, reinterpret_cast<const Bytef*>(s.data()), uLong(s.size()));
    out.resize(n);
    return out;
}
std::optional<std::string> zlib_decompress(const std::string& s) {
    z_stream z{};
    if (inflateInit(&z) != Z_OK) return std::nullopt;
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(s.data()));
    z.avail_in = uInt(s.size());
    std::string out;
    char buf[16384];
    int rc;
    do {
        z.next_out = reinterpret_cast<Bytef*>(buf);
        z.avail_out = sizeof buf;
        rc = inflate(&z, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            inflateEnd(&z);
            return std::nullopt;
        }
        out.append(buf, sizeof buf - z.avail_out);
        if (out.size() > 64u * 1024 * 1024) {  // a hostile peer: nobody answers a search with 64 MB
            inflateEnd(&z);
            return std::nullopt;
        }
    } while (rc != Z_STREAM_END && (z.avail_in > 0 || z.avail_out == 0));
    inflateEnd(&z);
    return rc == Z_STREAM_END ? std::optional<std::string>(out) : std::nullopt;
}

// MARK: Server messages we send

std::string login_request(const std::string& username, const std::string& password) {
    Writer w;
    w.str(username).str(password).u32(kClientVersion).str(md5_hex(username + password)).u32(kMinorVersion);
    return server_frame(kLogin, w.data());
}
std::string set_listen_port(uint32_t port) { return server_frame(kSetListenPort, Writer().u32(port).data()); }
std::string set_status(uint32_t status) { return server_frame(kSetStatus, Writer().u32(status).data()); }
std::string shared_folders_files(uint32_t folders, uint32_t files) { return server_frame(kSharedFoldersFiles, Writer().u32(folders).u32(files).data()); }
std::string accept_children(bool accept) { return server_frame(kAcceptChildren, Writer().boolean(accept).data()); }
std::string toggle_parent_search(bool enable) { return server_frame(kToggleParentSearch, Writer().boolean(enable).data()); }
std::string ping() { return server_frame(kPing, ""); }
std::string file_search(uint32_t ticket, const std::string& query) { return server_frame(kFileSearch, Writer().u32(ticket).str(query).data()); }
std::string get_peer_address(const std::string& username) { return server_frame(kGetPeerAddress, Writer().str(username).data()); }
std::string connect_to_peer(uint32_t ticket, const std::string& username, const std::string& type) {
    return server_frame(kConnectToPeer, Writer().u32(ticket).str(username).str(type).data());
}

// MARK: Server messages we receive

std::optional<LoginReply> parse_login_reply(const std::string& payload) {
    Reader r(payload);
    LoginReply l;
    l.success = r.boolean();
    if (l.success) {
        l.greeting = r.str(), l.ip = r.ip(), l.md5 = r.str();
        if (r.more()) l.supporter = r.boolean();
    } else {
        l.reason = r.str();
    }
    return r.ok() ? std::optional(l) : std::nullopt;
}
std::optional<PeerAddress> parse_peer_address(const std::string& payload) {
    Reader r(payload);
    PeerAddress a;
    a.username = r.str(), a.ip = r.ip(), a.port = r.u32();
    return r.ok() ? std::optional(a) : std::nullopt;
}
std::optional<ConnectRequest> parse_connect_to_peer(const std::string& payload) {
    Reader r(payload);
    ConnectRequest c;
    c.username = r.str(), c.type = r.str(), c.ip = r.ip(), c.port = r.u32(), c.ticket = r.u32(), c.privileged = r.boolean();
    return r.ok() ? std::optional(c) : std::nullopt;
}
std::optional<uint32_t> parse_cant_connect(const std::string& payload) {
    Reader r(payload);
    const uint32_t t = r.u32();
    return r.ok() ? std::optional(t) : std::nullopt;
}

// MARK: Peer initialisation

std::string pierce_firewall(uint32_t token) { return init_frame(kPierceFirewall, Writer().u32(token).data()); }
std::string peer_init(const std::string& username, const std::string& type, uint32_t token) {
    return init_frame(kPeerInit, Writer().str(username).str(type).u32(token).data());
}

// MARK: Peer messages

std::string queue_upload(const std::string& filename) { return server_frame(kQueueUpload, Writer().str(filename).data()); }
std::string transfer_request(uint32_t direction, uint32_t ticket, const std::string& filename, std::optional<uint64_t> size) {
    Writer w;
    w.u32(direction).u32(ticket).str(filename);
    if (size) w.u64(*size);
    return server_frame(kTransferRequest, w.data());
}
std::string transfer_reply(uint32_t ticket, bool allowed, const std::optional<std::string>& reason, std::optional<uint64_t> size) {
    Writer w;
    w.u32(ticket).boolean(allowed);
    if (allowed && size) w.u64(*size);
    if (!allowed && reason) w.str(*reason);
    return server_frame(kTransferReply, w.data());
}
std::string place_in_queue_request(const std::string& filename) { return server_frame(kPlaceInQueueRequest, Writer().str(filename).data()); }
std::string place_in_queue(const std::string& filename, uint32_t place) { return server_frame(kPlaceInQueue, Writer().str(filename).u32(place).data()); }
std::string upload_failed(const std::string& filename) { return server_frame(kUploadFailed, Writer().str(filename).data()); }
std::string queue_failed(const std::string& filename, const std::string& reason) { return server_frame(kQueueFailed, Writer().str(filename).str(reason).data()); }

std::string search_reply(const UserResult& r) {
    Writer w;
    w.str(r.username).u32(r.ticket).u32(uint32_t(r.files.size()));
    for (const auto& f : r.files) write_file(w, f);
    w.boolean(r.free_slots).u32(r.avg_speed).u32(r.queue_size).u32(0);  // then an unknown word; no locked results
    return server_frame(kSearchReply, zlib_compress(w.data()));
}

std::optional<TransferRequest> parse_transfer_request(const std::string& payload) {
    Reader r(payload);
    TransferRequest t;
    t.direction = r.u32(), t.ticket = r.u32(), t.filename = r.str();
    if (r.more()) t.size = r.u64();
    return r.ok() ? std::optional(t) : std::nullopt;
}
std::optional<TransferReply> parse_transfer_reply(const std::string& payload) {
    Reader r(payload);
    TransferReply t;
    t.ticket = r.u32(), t.allowed = r.boolean();
    if (r.more()) {
        if (t.allowed) t.size = r.u64();
        else t.reason = r.str();
    }
    return r.ok() ? std::optional(t) : std::nullopt;
}
std::optional<UserResult> parse_search_reply(const std::string& compressed_payload) {
    const auto raw = zlib_decompress(compressed_payload);
    if (!raw) return std::nullopt;
    Reader r(*raw);
    UserResult u;
    u.username = r.str(), u.ticket = r.u32();
    const uint32_t n = r.u32();
    if (n > 200000) return std::nullopt;  // not a real result list
    for (uint32_t i = 0; i < n && r.ok(); ++i) {
        FileEntry f;
        r.u8();
        f.filename = r.str(), f.size = r.u64(), f.extension = r.str();
        const uint32_t attrs = r.u32();
        if (attrs > 64) return std::nullopt;
        for (uint32_t k = 0; k < attrs && r.ok(); ++k) {
            const uint32_t key = r.u32(), value = r.u32();
            f.attributes[key] = value;
        }
        u.files.push_back(std::move(f));
    }
    u.free_slots = r.boolean(), u.avg_speed = r.u32(), u.queue_size = r.u32();
    return r.ok() ? std::optional(std::move(u)) : std::nullopt;  // (the unknown word and locked results are ignored)
}
std::optional<Place> parse_place_in_queue(const std::string& payload) {
    Reader r(payload);
    Place p;
    p.filename = r.str(), p.place = r.u32();
    return r.ok() ? std::optional(p) : std::nullopt;
}
std::optional<NameAndReason> parse_queue_failed(const std::string& payload) {
    Reader r(payload);
    NameAndReason n;
    n.filename = r.str(), n.reason = r.str();
    return r.ok() ? std::optional(n) : std::nullopt;
}
std::optional<std::string> parse_filename(const std::string& payload) {
    Reader r(payload);
    std::string f = r.str();
    return r.ok() ? std::optional(std::move(f)) : std::nullopt;
}

}  // namespace wb::slsk::proto
