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

    json to_json() const;
    static VisOptions from_json(const json& j);
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
