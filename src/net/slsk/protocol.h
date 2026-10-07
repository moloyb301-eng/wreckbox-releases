// The Soulseek wire protocol, the part a downloader needs: byte-exact encoders and decoders for the server, peer and
// peer-initialisation messages (tests compare them with bytes made by aioslsk). A frame is a little-endian uint32 length, a
// message code (uint32 for server and peer messages, uint8 for peer initialisation), then the payload; the search reply's
// payload is zlib-compressed.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "net/slsk/types.h"

namespace wb::slsk::proto {

constexpr uint32_t kClientVersion = 175, kMinorVersion = 1;  // what aioslsk (and the sidecar) report

enum ServerCode : uint32_t {
    kLogin = 1, kSetListenPort = 2, kGetPeerAddress = 3, kConnectToPeer = 18, kFileSearch = 26, kSetStatus = 28, kPing = 32, kSharedFoldersFiles = 35,
    kToggleParentSearch = 71, kAcceptChildren = 100, kCantConnectToPeer = 1001
};
enum PeerCode : uint32_t {
    kSearchReply = 9, kTransferRequest = 40, kTransferReply = 41, kQueueUpload = 43, kPlaceInQueue = 44, kUploadFailed = 46, kQueueFailed = 50, kPlaceInQueueRequest = 51
};
enum InitCode : uint8_t { kPierceFirewall = 0, kPeerInit = 1 };

class Writer {
public:
    Writer& u8(uint8_t v);
    Writer& u32(uint32_t v);
    Writer& u64(uint64_t v);
    Writer& boolean(bool v);
    Writer& str(const std::string& v);  // uint32 length + bytes
    Writer& ip(const std::string& dotted);  // four bytes, last octet first
    const std::string& data() const { return buf_; }

private:
    std::string buf_;
};

class Reader {
public:
    explicit Reader(const std::string& data, size_t pos = 0) : d_(data), pos_(pos) {}
    uint8_t u8();
    uint32_t u32();
    uint64_t u64();
    bool boolean();
    std::string str();  // UTF-8, or Windows-1252 if it isn't valid UTF-8
    std::string ip();
    bool ok() const { return ok_; }          // false once something ran past the end
    bool more() const { return ok_ && pos_ < d_.size(); }

private:
    bool need(size_t n);
    const std::string& d_;
    size_t pos_;
    bool ok_ = true;
};

std::string md5_hex(const std::string& s);
std::string server_frame(uint32_t code, const std::string& payload);
std::string init_frame(uint8_t code, const std::string& payload);
std::string zlib_compress(const std::string& s);
std::optional<std::string> zlib_decompress(const std::string& s);

// MARK: Server messages we send
std::string login_request(const std::string& username, const std::string& password);
std::string set_listen_port(uint32_t port);
std::string set_status(uint32_t status);  // 2 = online
std::string shared_folders_files(uint32_t folders, uint32_t files);
std::string accept_children(bool accept);
std::string toggle_parent_search(bool enable);
std::string ping();
std::string file_search(uint32_t ticket, const std::string& query);
std::string get_peer_address(const std::string& username);
std::string connect_to_peer(uint32_t ticket, const std::string& username, const std::string& type);

// MARK: Server messages we receive (payloads, after the code)
struct LoginReply {
    bool success = false;
    std::string greeting, ip, md5, reason;
    bool supporter = false;
};
std::optional<LoginReply> parse_login_reply(const std::string& payload);
struct PeerAddress {
    std::string username, ip;
    uint32_t port = 0;
};
std::optional<PeerAddress> parse_peer_address(const std::string& payload);
struct ConnectRequest {  // the server passing on someone's ConnectToPeer
    std::string username, type, ip;
    uint32_t port = 0, ticket = 0;
    bool privileged = false;
};
std::optional<ConnectRequest> parse_connect_to_peer(const std::string& payload);
std::optional<uint32_t> parse_cant_connect(const std::string& payload);  // the ticket

// MARK: Peer initialisation
std::string pierce_firewall(uint32_t token);
std::string peer_init(const std::string& username, const std::string& type, uint32_t token = 0);

// MARK: Peer messages
std::string queue_upload(const std::string& filename);
std::string transfer_request(uint32_t direction, uint32_t ticket, const std::string& filename, std::optional<uint64_t> size = std::nullopt);
std::string transfer_reply(uint32_t ticket, bool allowed, const std::optional<std::string>& reason = std::nullopt, std::optional<uint64_t> size = std::nullopt);
std::string place_in_queue_request(const std::string& filename);
std::string place_in_queue(const std::string& filename, uint32_t place);
std::string upload_failed(const std::string& filename);
std::string queue_failed(const std::string& filename, const std::string& reason);
std::string search_reply(const UserResult& r);  // compressed

struct TransferRequest {
    uint32_t direction = 0, ticket = 0;
    std::string filename;
    uint64_t size = 0;
};
std::optional<TransferRequest> parse_transfer_request(const std::string& payload);
struct TransferReply {
    uint32_t ticket = 0;
    bool allowed = false;
    std::string reason;
    uint64_t size = 0;
};
std::optional<TransferReply> parse_transfer_reply(const std::string& payload);
std::optional<UserResult> parse_search_reply(const std::string& compressed_payload);
struct Place {
    std::string filename;
    uint32_t place = 0;
};
std::optional<Place> parse_place_in_queue(const std::string& payload);
struct NameAndReason {
    std::string filename, reason;
};
std::optional<NameAndReason> parse_queue_failed(const std::string& payload);
std::optional<std::string> parse_filename(const std::string& payload);  // upload_failed / queue_upload / place_in_queue_request

}  // namespace wb::slsk::proto
