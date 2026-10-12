// DasVis's two feedback controllers, plain maths so they're unit-tested:
//   - FlashLimiter keeps full-screen brightness flashes to a few a second (WCAG 2.3.1: at most 3). It counts flashes in
//     the measured picture and hands the scenes a budget (they scale their flashes by it), and, once the budget is
//     spent, caps the exposure so brightness can't jump.
//   - QualityControl picks the tier (Low / Medium / High) and the render scale from the GPU time each frame takes, so
//     the picture stays smooth on any PC: a softer picture rather than a stutter.
#pragma once
#include <deque>

namespace wb::dasvis {

// A flash, as WCAG counts it: relative luminance rising quickly by 0.1 or more from a level below 0.8, then falling back
// by 0.1. "Quickly": the dark level follows the picture up with an 80 ms lag, so a slow brightening isn't a flash. The
// tests count what reached the screen with it too.
class FlashCounter {
public:
    bool update(float lum, float dt);  // true when a new flash starts
    float low() const { return low_; }  // the dark level the next (or current) flash rises from

private:
    bool high_ = false;
    float low_ = 1, peak_ = 0;
};

class FlashLimiter {
public:
    explicit FlashLimiter(int per_second = 3) : max_(per_second) {}
    void set_max(int per_second) { max_ = per_second; }

    // `lum`: the measured relative luminance (0–1) of a recent frame, after this limiter's gain; `dt`: seconds.
    void update(float lum, float dt);
    float budget() const { return budget_; }  // 0–1: how much the scenes may still flash this second
    float gain() const { return gain_; }      // ≤ 1: the exposure cap while the budget is spent
    int flashes() const { return int(times_.size()); }  // in the last second

private:
    int max_;
    double t_ = 0;
    std::deque<double> times_;  // when the flashes of the last second happened
    FlashCounter counter_;
    float budget_ = 1, gain_ = 1;
};

enum class Tier { low, medium, high };

struct TierSpec {
    float scale_min, scale_max;  // internal render size, as a fraction of the screen
    float quality;               // the shaders' `quality` knob: loop counts, taps
    int bloom_levels;
};
const TierSpec& tier_spec(Tier t);
const char* tier_name(Tier t);

class QualityControl {
public:
    // `automatic`: the tier moves with the measurements (the "Auto" setting); otherwise only the scale does, within the
    // tier. `max_tier`: on battery, one below High.
    QualityControl(Tier start, bool automatic, Tier max_tier = Tier::high);

    // `gpu_ms`: one frame's GPU time (measured a few frames late); `budget_ms`: what a frame may take. True when the
    // render scale or the tier changed (the render targets are made again).
    bool update(float gpu_ms, float budget_ms, float dt);

    Tier tier() const { return tier_; }
    float scale() const { return scale_; }
    float quality() const { return tier_spec(tier_).quality; }
    bool settled() const { return time_ > 2; }  // the first 2 s have been measured: worth remembering the tier

private:
    Tier tier_, max_;
    bool auto_;
    float scale_, ms_ = 0;
    double time_ = 0, wait_ = 0, at_min_ = 0, at_max_ = 0, last_down_ = -100;
    bool measured_ = false;
};

}  // namespace wb::dasvis
