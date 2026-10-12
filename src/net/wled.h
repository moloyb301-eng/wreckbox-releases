// Room lights: WLED's realtime protocol (UDP port 21324, "DRGB": a header of 2 bytes, then red, green and blue for each
// LED, up to 490). No account, nothing to install; a few hundred bytes a frame on the local network. Each packet says
// how long WLED should wait for the next before going back to its own effect (2 s), so closing DasVis hands the lights
// back by themselves.
#pragma once
#include <winsock2.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wb::lights {

constexpr int kWledPort = 21324;
constexpr size_t kMaxLeds = 490;  // what one DRGB packet carries

// "192.168.1.50", "wled.local" or "wled.local:21324" → an address to send to. Blocks while a name is looked up: call it
// off the UI thread. nullopt if it can't be found.
std::optional<sockaddr_in> resolve(const std::string& host);

class Wled {
public:
    Wled();
    ~Wled();
    Wled(const Wled&) = delete;
    Wled& operator=(const Wled&) = delete;
    // Sends one frame: `rgb` is 3 bytes an LED (more than kMaxLeds are left off). False if it couldn't be sent.
    bool send(const sockaddr_in& to, const std::vector<uint8_t>& rgb);

private:
    SOCKET s_ = INVALID_SOCKET;
};

}  // namespace wb::lights
