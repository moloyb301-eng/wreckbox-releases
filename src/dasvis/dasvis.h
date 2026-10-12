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
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dasvis/control.h"
#include "dasvis/director.h"
#include "dasvis/features.h"
#include "dasvis/palette.h"
#include "dasvis/songmap.h"

namespace wb::dasvis {

struct Config {
    Tier start = Tier::medium;  // the tier remembered for this graphics card, or Medium
    bool auto_tier = true;      // false: stay in `start` (the Quality setting)
    Tier max_tier = Tier::high; // on battery: Medium
    bool reduce_flash = false;  // at most 1 big flash a second (instead of 3), and gentler flashes
    double auto_advance = 30;   // seconds per scene, 0 = stay
    std::filesystem::path user_dir;  // the user's own scenes (.hlsl); empty = none
    std::map<std::string, float> costs;  // what each scene measured on this graphics card before (costs())
    float fixed_scale = 0;  // > 0: always this render scale (video export: 1, full size), never adjusted
    bool milkdrop = false;  // a "MilkDrop" scene too: the app renders projectM while it's on screen (set_milkdrop)
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
    // Your likes and skips for this song's genre: scene name → weight (1 neutral). nullptr: neutral.
    const std::map<std::string, float>* taste = nullptr;
    bool glitchy = false;  // the genre suits glitch transitions (dubstep, drum & bass)
};

// What moves the pictures this frame: the kick, snare, hats, bass and lead. From the song map when there is one (exact
// timing, each stem separated from the rest), with the live analysis on top for anything the map missed; else from the
// live analysis alone.
struct Drive {
    float kick = 0, snare = 0, hats = 0, bass = 0, lead = 0;  // envelopes, 0–1
    bool kick_hit = false, snare_hit = false, hats_hit = false;  // a hit in this frame
    float since_kick = 10;                                       // seconds since the last kick (10: a while)
};
// `t0` / `t1`: the song time of the last frame and of this one; the map's hits in (t0, t1] fire now (none on a seek).
Drive drive(const Features& live, const SongMap* map, double t0, double t1, const Drive& last, float dt);

// Room lights: what DasVis is showing, in a few numbers; led_frame() makes a strip's colours from them.
struct LightState {
    Palette pal{};          // the palette on screen (linear)
    float kick = 0, snare = 0, bass = 0, drop = 0;
    double beats = 0;       // the beat count (the colours travel along the strip, a step a beat)
    float flash = 1;        // the flash limiter's budget (0–1): bright changes are held back when it's spent
    bool reduce_flash = false;
};
// `leds` LEDs, 3 bytes each (sRGB): the palette spread along the strip and moving with the beat, brighter on the kick and
// the bass, a white glint on the snare and the whole strip lit on a drop, all within the flash budget.
std::vector<uint8_t> led_frame(const LightState& s, int leds);

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

    // The current song's cover: opaque BGRA, any size (a square ~512 px is plenty). nullptr: no cover. Its colours become
    // the palette (fading in over about a second), unless it's grey.
    void set_cover(const uint8_t* bgra, int w, int h);
    // The title card shown as the song starts: title_target() gives a cleared BGRA render-target texture for the app to
    // draw the title over the artist into (red: coverage; green: the letter's number × coverage); then show_title() with
    // how many letters and the title line's share of the height. show_title(0, …): none (radio, or no title).
    ID3D11Texture2D* title_target(int w, int h);
    void show_title(int letters, float title_share);
    // A lyric line in kinetic type, in place of the title card: drawn into title_target() the same way (the line alone),
    // sung from `start` to `end` (song seconds). show_lyric(0, …): none.
    void show_lyric(int letters, double start, double end);

    // N / P: the next or previous scene (any: your choice). skip(): the director picks another that fits, now (X).
    void next();
    void previous();
    void skip();
    // The phone deck.
    std::vector<std::string> scene_names() const;
    void select(size_t scene);                // that scene now, dressed by the director
    void drop_now();                          // a drop: a hero scene, its events, the punch, for a bar
    void shockwave();
    void set_camera(float turn, float zoom);  // −1…1 each, 0 at rest; eased
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
    bool particles() const;     // the particle layer is on
    // What each scene measured on this graphics card ("<scene>|<tier>" → GPU ms at the tier's smallest size): saved,
    // and given back in Config::costs next time, so the director knows what fits from the start.
    std::map<std::string, float> costs() const;
    LightState lights() const;  // for the room lights, after render()
    // The MilkDrop scene (Config::milkdrop) is on screen: the app renders a projectM frame for each DasVis frame and hands
    // it over before render(). BGRA, rows bottom-up (as OpenGL reads them), any size.
    bool wants_milkdrop() const;
    void set_milkdrop(const uint8_t* bgra, int w, int h);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// The built-in scenes' tags, in their order (what the director knows of them).
std::vector<SceneTags> builtin_scenes();

// dasvis.hlsli's text: written next to the user's scenes so they can #include it.
const char* library_source();
// Writes dasvis.hlsli (always, so it matches this version) and, in an empty folder, an example scene.
void prepare_user_dir(const std::filesystem::path& dir);

}  // namespace wb::dasvis
