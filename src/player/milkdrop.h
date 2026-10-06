// The MilkDrop engine (projectM, plays Winamp's .milk presets) rendered off-screen. projectM 4.1 always draws its final
// image to framebuffer 0, so we give it a WGL pbuffer: its framebuffer 0 is off-screen, projectM runs unmodified, and
// the pixels come back through two PBOs (asynchronous: a frame is read while the next one renders). The caller draws
// the pixels however it likes (Direct2D, in the app). No UI dependencies; everything must run on one thread.
// Any failure (no OpenGL 3.3, no pbuffers: old Intel drivers, Remote Desktop) leaves ok() == false with error().
#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace wb::player {

struct MilkDropConfig {
    int width = 1280, height = 720;
    std::vector<std::filesystem::path> preset_dirs;  // scanned recursively for .milk files
    std::filesystem::path texture_dir;
};

class MilkDrop {
public:
    explicit MilkDrop(const MilkDropConfig& cfg);
    ~MilkDrop();
    MilkDrop(const MilkDrop&) = delete;
    MilkDrop& operator=(const MilkDrop&) = delete;

    bool ok() const;
    const std::string& error() const;  // why !ok(); empty otherwise

    struct Frame {
        const uint8_t* bgra = nullptr;  // bottom-up rows, width * 4 bytes each; valid until the next render()
        int width = 0, height = 0;
        explicit operator bool() const { return bgra != nullptr; }
    };
    // Feeds the newest mono samples, renders one frame, and returns the previous frame's pixels (empty on the very first
    // call: the readback runs one frame behind so the GPU never stalls).
    Frame render(const float* mono, size_t n);
    bool resize(int width, int height);  // false (and !ok()) if the new pbuffer can't be made

    void next();
    void previous();
    void random();
    void lock(bool on);  // hold the current preset (manual next / previous still work)
    bool locked() const;
    void set_auto_advance(double seconds);  // 0 = never switch by itself

    std::string preset_name() const;  // file name without folders or extension
    size_t preset_count() const;
    int failed_presets() const;  // presets projectM couldn't compile since the start (it skips to the next)

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace wb::player
