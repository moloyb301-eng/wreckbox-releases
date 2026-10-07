// The Winamp-style visualizer's maths: spectrum bars with falling peak caps, and the oscilloscope trace. Fed with the
// audible samples (AudioOutput::tap), so it moves with what you hear. Drawing is in src/ui/view_visualizer.cpp.
#pragma once
#include <vector>

#include "engine/fft.h"
#include "model/model.h"

namespace wb::player {

struct VisOptions {
    // milkdrop last so the Winamp modes keep their numbers (click cycles the first four).
    enum class Mode { spectrum, scope, both, off, milkdrop };
    enum class Bars { normal, fire, line };
    enum class Scope { dots, lines, solid };
    Mode mode = Mode::milkdrop;
    Bars bars = Bars::normal;
    Scope scope = Scope::lines;
    int bar_falloff = 2;   // 0 slowest … 4 fastest, as in Winamp's options
    int peak_falloff = 2;
    bool peaks = true;
    bool classic = true;   // Classic Winamp colours; false = WreckBox pastel
    int quality = 720;      // MilkDrop render height: 540 / 720 / 1080
    int auto_advance = 30;  // MilkDrop: seconds per preset, 0 = stay on one
    int pulse = 2;          // WreckBox's beat pulse over MilkDrop: 0 off, 1 subtle, 2 strong
    bool hard_cuts = true;  // MilkDrop switches preset on a big beat after a quiet part (at most every 15 s)
    bool all_presets = false;   // false: the beat-heavy categories only
    int beat_sensitivity = 2;   // 1 low, 2 normal, 3 high
    int sync_ms = 0;            // your calibration: + draws the pictures later, − earlier (−300…300)

    json to_json() const;
    static VisOptions from_json(const json& j);
};

// The beat pulse: a kick detector on the audio being heard. Two one-pole low-passes (~150 Hz) give the bass energy of
// each frame's samples; a kick is energy well above its last-second average and rising, at least 150 ms after the last
// one. Each kick sets the envelope to 1; it decays with a 60 ms time constant (gone after ~200 ms).
class BeatPulse {
public:
    // `mono`: the samples heard since the last call (may be none); `dt`: seconds since the last call. Returns envelope().
    float update(const float* mono, size_t n, float dt);
    float envelope() const { return env_; }
    int kicks() const { return kicks_; }  // detected so far (tests)

private:
    float lp1_ = 0, lp2_ = 0, avg_ = 0, last_ = 0, env_ = 0, since_ = 1;
    int kicks_ = 0;
};

class Visualizer {
public:
    static constexpr size_t kFft = 2048;  // ~43 ms at 48 kHz: 23 Hz per bin

    VisOptions opt;

    // `samples`: the newest kFft mono samples (oldest first); `dt`: seconds since the last update.
    void update(const float* samples, float dt);
    void set_band_count(size_t n);  // follows the drawing width

    // 0–1 per band, low to high frequency (20 Hz – 16 kHz, log spaced).
    const std::vector<float>& bars() const { return bars_; }
    const std::vector<float>& peaks() const { return peaks_; }
    // Up to 1024 samples for the oscilloscope, starting at a rising zero crossing so the trace stands still.
    const std::vector<float>& wave() const { return wave_; }
    float band_hz(size_t i) const;  // centre frequency of band i

private:
    RealFft fft_{kFft};
    std::vector<float> window_, frame_, mags_;
    std::vector<float> bars_, peaks_, hold_, wave_;
};

}  // namespace wb::player
