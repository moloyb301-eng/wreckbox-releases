#include "net/wled.h"

#include <ws2tcpip.h>

#include <algorithm>

namespace wb::lights {

namespace {

struct WinsockOnce {
    WinsockOnce() {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    }
};

void winsock() { static WinsockOnce once; }

}  // namespace

std::optional<sockaddr_in> resolve(const std::string& host_port) {
    winsock();
    std::string host = host_port, port = std::to_string(kWledPort);
    if (const size_t colon = host_port.rfind(':'); colon != std::string::npos) host = host_port.substr(0, colon), port = host_port.substr(colon + 1);
    if (host.empty()) return std::nullopt;
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET, hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) return std::nullopt;
    sockaddr_in a = *reinterpret_cast<const sockaddr_in*>(res->ai_addr);
    freeaddrinfo(res);
    return a;
}

Wled::Wled() {
    winsock();
    s_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
}

Wled::~Wled() {
    if (s_ != INVALID_SOCKET) closesocket(s_);
}

bool Wled::send(const sockaddr_in& to, const std::vector<uint8_t>& rgb) {
    if (s_ == INVALID_SOCKET) return false;
    const size_t n = std::min(rgb.size(), kMaxLeds * 3);
    std::vector<char> packet(2 + n);
    packet[0] = 2;  // DRGB
    packet[1] = 2;  // seconds without a packet before WLED goes back to its own effect
    std::copy(rgb.begin(), rgb.begin() + std::ptrdiff_t(n), packet.begin() + 2);
    return sendto(s_, packet.data(), int(packet.size()), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to) == int(packet.size());
}

}  // namespace wb::lights
