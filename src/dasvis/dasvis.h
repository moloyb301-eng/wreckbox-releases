// DasVis, WreckBox's own visualizer (docs/DASVIS-PLAN.md). Renders with Direct3D 11 on the app's own device (the one
// Direct2D draws the window with), straight into the window's buffer: nothing is copied or read back. Shader Model 4
// only: it runs at feature level 10_0, on graphics cards from about 2008 on.
//
// Each frame: the scene (one pixel shader, drawn at a fraction of the screen's size that the QualityControl picks) into
// an HDR target that's also the next frame's feedback; a bloom chain; the final pass at the screen's size (tone
// mapping, finish, the spectrum ring) straight into the target it's given, the window's own back buffer (no
// full-screen texture of its own: 8 MB less at 1080p); a 16 × 16 brightness measure read back two frames later for the
// FlashLimiter; and GPU timestamps for the QualityControl. No UI dependencies; everything runs on the UI thread.
#pragma once
#include <d3d11.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "dasvis/control.h"
#include "dasvis/features.h"
#include "dasvis/songmap.h"

namespace wb::dasvis {

struct Config {
    Tier start = Tier::medium;  // the tier remembered for this graphics card, or Medium
    bool auto_tier = true;      // false: stay in `start` (the Quality setting)
    Tier max_tier = Tier::high; // on battery: Medium
    bool reduce_flash = false;  // at most 1 big flash a second (instead of 3), and gentler flashes
    double auto_advance = 30;   // seconds per scene, 0 = stay
    std::filesystem::path user_dir;  // the user's own scenes (.hlsl); empty = none
};

// What the app knows this frame.
struct Inputs {
    const Features* f = nullptr;
    float dt = 1 / 60.f;
    float budget_ms = 13;          // GPU time a frame may take (most of one frame at the display's pace)
    std::string song;              // changes → a new look (seed, scene, palette)
    std::optional<std::string> camelot;  // the song's key ("8A"): the palette's hue
    int intensity = 1;             // 0 chill, 1 normal, 2 insane
    // The song map, once it's ready (nullptr: live only), and where the song is (seconds) for the frame being drawn.
    // The beat, bar, phrase, section, build-up and drop come from the map; the live kicks keep the two in step.
    const SongMap* map = nullptr;
    double song_time = 0;
};

class DasVis {
public:
    DasVis(ID3D11Device* device, const Config& cfg);
    ~DasVis();
    DasVis(const DasVis&) = delete;
    DasVis& operator=(const DasVis&) = delete;

    bool ok() const;
    const std::string& error() const;

    // Renders one frame into `target`: a B8G8R8A8 render-target texture on the same device, the screen's size (the
    // window's back buffer).
    void render(const Inputs& in, ID3D11Texture2D* target);
    // Draws the last frame into `target` again, without moving it on (paused: the back buffer is refilled each paint).
    void redraw(ID3D11Texture2D* target);

    // The current song's cover: opaque BGRA, any size (a square ~512 px is plenty). nullptr: no cover.
    void set_cover(const uint8_t* bgra, int w, int h);

    void next();
    void previous();
    void lock(bool on);
    bool locked() const;
    void set_auto_advance(double seconds);
    void set_reduce_flash(bool on);
    std::string scene_name() const;
    size_t scene_count() const;
    // Compiles the user's scenes again (F5). Returns the errors, "" if none.
    std::string reload_user_scenes();
    const std::string& user_errors() const;

    Tier tier() const;
    float scale() const;
    float gpu_ms() const;       // measured, smoothed
    bool tier_settled() const;  // measured long enough to remember the tier
    float flash_budget() const;
    double map_offset() const;  // seconds the song time is moved by to match the kicks heard (the dev view shows it)

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// dasvis.hlsli's text: written next to the user's scenes so they can #include it.
const char* library_source();
// Writes dasvis.hlsli (always, so it matches this version) and, in an empty folder, an example scene.
void prepare_user_dir(const std::filesystem::path& dir);

}  // namespace wb::dasvis
