// Spout output: shares the full-screen visualizer's picture with other programs on this PC (OBS's Spout2 plugin,
// Resolume, TouchDesigner…) as a Spout 2 sender named "WreckBox DasVis". The sender side of the protocol, written
// here rather than taking in the Spout SDK: a shared Direct3D 11 texture, the sender's name in the "SpoutSenderNames"
// list, and its details (texture handle, size, format) in a small shared memory block under its own name. Receivers
// open the texture on their own device; nothing is copied through the CPU.
#pragma once
#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <string>

namespace wb::dasvis {

class SpoutSender {
public:
    explicit SpoutSender(std::string name = "WreckBox DasVis");
    ~SpoutSender();  // leaves the list: receivers see the sender go
    SpoutSender(const SpoutSender&) = delete;
    SpoutSender& operator=(const SpoutSender&) = delete;

    // Shares `frame` (a B8G8R8A8 texture on `dev`, e.g. the window's back buffer): copied into the shared texture,
    // which is made again when the size changes. False if sharing isn't possible.
    bool send(ID3D11Device* dev, ID3D11Texture2D* frame);
    const std::string& name() const { return name_; }

private:
    bool start(ID3D11Device* dev, UINT w, UINT h);
    void stop();
    std::string name_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> shared_;
    HANDLE info_ = nullptr, names_ = nullptr, active_ = nullptr, names_mutex_ = nullptr, access_ = nullptr;
    UINT w_ = 0, h_ = 0;
};

}  // namespace wb::dasvis
