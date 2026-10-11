// The director (docs/DASVIS-PLAN.md §9): which scene plays, with which layers, and how it changes. It picks at each
// section boundary of the song map (sections start on a 4-bar downbeat), on 16-bar phrases in long sections, and on
// drops; only scenes that fit this PC's frame budget, weighted by the music's energy and your likes, never one of the
// last 4. Deterministic: the same song (seed), map, tier and costs give the same scenes, so it's testable.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "dasvis/control.h"
#include "dasvis/songmap.h"

namespace wb::dasvis {

enum : uint8_t { kCalm = 1, kMid = 2, kPeak = 4 };  // the energy a scene suits (a mask)

struct SceneTags {
    std::string name;
    uint8_t energy = kCalm | kMid | kPeak;
    char cost = 'L';      // L (< 2 ms on the Intel UHD at 720p, quality 0.3), M (< 5 ms), H (more)
    bool hero = false;    // a drop's scene
    bool layers = true;   // a mid layer (particles or the ribbon) may go over it
    bool ring = false;    // the spectrum ring suits it
};

enum class Transition : uint8_t { crossfade, wipe, zoom, shatter, glitch };  // the shaders' TRANSITION_KIND
enum class Mid : uint8_t { none, particles, ribbon };                        // the layer over the scene
enum : uint8_t { kShockwave = 1, kWhiteout = 2, kCoverShatter = 4 };         // a drop's events (the shaders' EVENT bits)

struct Cue {
    size_t scene = 0;
    Transition transition = Transition::wipe;
    float beats = 2;         // how long the transition takes
    Mid mid = Mid::none;     // the mid layer
    bool ring = false;       // overlays: the spectrum ring
    bool sigils = false;     // and the beat sigils
};

// Where the music is when the director picks.
struct Moment {
    uint32_t seed = 0;       // the song's
    uint32_t index = 0;      // the how-manyth pick in this song: with the seed, the random numbers
    Section section = Section::verse, from = Section::verse;  // the new section, and the one before
    bool drop = false;       // the new section is a drop
    uint8_t energy = kMid;
    Tier tier = Tier::medium;
    float budget_ms = 13;    // GPU time a frame may take
    int intensity = 1;       // 0 chill, 1 normal, 2 insane
    bool glitchy = false;    // the genre suits glitches (dubstep, drum & bass)
};

class Director {
public:
    explicit Director(std::vector<SceneTags> scenes = {});
    void set_scenes(std::vector<SceneTags> scenes);  // the user's scenes were reloaded
    const std::vector<SceneTags>& scenes() const { return scenes_; }

    // What a scene costs on this PC at a tier (GPU ms a frame at the tier's smallest render size); 0: not measured. Kept
    // as "<scene>|<tier>" → ms.
    void set_cost(const std::string& scene, Tier tier, float ms);
    float cost(size_t scene, Tier tier) const;
    const std::map<std::string, float>& costs() const { return costs_; }
    void set_costs(std::map<std::string, float> costs) { costs_ = std::move(costs); }
    // Measured: within the budget. Not measured: anything but a heavy scene on Low.
    bool fits(size_t scene, Tier tier, float budget_ms) const;
    // Your taste for this genre: scene name → weight (1 neutral, more liked, less skipped). nullptr: neutral.
    void set_taste(const std::map<std::string, float>* taste) { taste_ = taste; }

    // A new scene for `m` (never `current`, nor one of the last 4 if there's anything else), dressed for it.
    Cue pick(const Moment& m, size_t current);
    // `scene` as chosen by you (N / P): its layers and transition for `m`.
    Cue dress(size_t scene, const Moment& m) const;
    void reset() { recent_.clear(); }  // a new song

    // The events on a song's drop number `drop` (0, 1, …): kShockwave / kWhiteout / kCoverShatter bits. Chill: the
    // shockwave; normal: one of shockwave, white-out, or cover shatter (with a shockwave); insane: all three.
    static uint8_t events(uint32_t seed, size_t drop, int intensity);
    // The energy a section suits: calm (intro, breakdown, outro or a quiet part), peak (drop, or a loud chorus), mid.
    static uint8_t energy_of(Section s, float part_energy);
    // Bars a scene stays on within one section before the next phrase changes it: chill 64, normal 32, insane 16.
    static int phrase_bars(int intensity) { return 64 >> std::min(intensity, 2); }

private:
    std::vector<SceneTags> scenes_;
    std::map<std::string, float> costs_;
    const std::map<std::string, float>* taste_ = nullptr;
    std::vector<size_t> recent_;  // the last 4 picked, newest last
    uint8_t last_energy_ = kMid;
};

}  // namespace wb::dasvis
