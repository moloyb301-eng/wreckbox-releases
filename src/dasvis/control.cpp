#include "dasvis/control.h"

#include <algorithm>
#include <cmath>

namespace wb::dasvis {

bool FlashCounter::update(float lum, float dt) {
    if (!high_) {
        low_ = std::min(lum, low_ + (lum - low_) * (1 - std::exp(-dt / 0.08f)));
        if (lum - low_ >= 0.1f && low_ < 0.8f) return high_ = true, peak_ = lum, true;
    } else {
        peak_ = std::max(peak_, lum);
        if (peak_ - lum >= 0.1f) high_ = false, low_ = lum;
    }
    return false;
}

void FlashLimiter::update(float lum, float dt) {
    t_ += dt;
    while (!times_.empty() && t_ - times_.front() > 1.0) times_.pop_front();
    if (counter_.update(lum, dt)) times_.push_back(t_);
    budget_ = std::clamp(1.f - float(times_.size()) / float(std::max(1, max_)), 0.f, 1.f);
    // Budget spent: hold the brightness near the dark level until the second is over. `lum` already has the gain in
    // it, so the scene's own brightness is lum / gain.
    float target = 1;
    if (budget_ <= 0) {
        const float raw = lum / std::max(gain_, 0.05f);
        target = std::clamp((counter_.low() + 0.06f) / std::max(raw, 1e-3f), 0.05f, 1.f);
    }
    const float tc = target < gain_ ? 0.02f : 0.5f;  // down at once, back up gently
    gain_ += (target - gain_) * (1 - std::exp(-dt / tc));
}

const TierSpec& tier_spec(Tier t) {
    // High stops at 90 %: the final pass sharpens the upscale, and full size cost ~3 MB more than its +30 MB budget.
    static const TierSpec specs[] = {{0.33f, 0.6f, 0.3f, 3}, {0.5f, 0.85f, 0.6f, 5}, {0.7f, 0.9f, 1.0f, 6}};
    return specs[int(t)];
}

const char* tier_name(Tier t) {
    static const char* names[] = {"Low", "Medium", "High"};
    return names[int(t)];
}

QualityControl::QualityControl(Tier start, bool automatic, Tier max_tier)
    : tier_(std::min(start, max_tier)), max_(max_tier), auto_(automatic), scale_(tier_spec(tier_).scale_max) {}

bool QualityControl::update(float gpu_ms, float budget_ms, float dt) {
    time_ += dt;
    ms_ = measured_ ? ms_ + (gpu_ms - ms_) * (1 - std::exp(-dt / 0.3f)) : gpu_ms;
    measured_ = true;
    const TierSpec& s = tier_spec(tier_);
    at_min_ = scale_ <= s.scale_min + 1e-3f ? at_min_ + dt : 0;
    at_max_ = scale_ >= s.scale_max - 1e-3f ? at_max_ + dt : 0;
    if ((wait_ -= dt) > 0) return false;  // let the last change show in the measurements first
    const auto step = [](float v) { return std::round(v * 20) / 20; };  // 5 % steps: few re-made targets
    const Tier before = tier_;
    const float scale_before = scale_;
    if (ms_ > 0.9f * budget_ms) {
        if (scale_ > s.scale_min + 1e-3f) scale_ = std::max(s.scale_min, step(scale_ * 0.85f));
        else if (auto_ && tier_ > Tier::low && at_min_ > 0.5) tier_ = Tier(int(tier_) - 1), last_down_ = time_;
    } else if (ms_ < 0.55f * budget_ms) {
        if (scale_ < s.scale_max - 1e-3f) scale_ = std::min(s.scale_max, step(scale_ * 1.1f));
        // Up a tier only after holding the top of this one, and never soon after coming down (no see-sawing).
        else if (auto_ && tier_ < max_ && at_max_ > 2 && time_ - last_down_ > 10) tier_ = Tier(int(tier_) + 1);
    }
    if (tier_ != before) scale_ = std::clamp(scale_, tier_spec(tier_).scale_min, tier_spec(tier_).scale_max);
    if (tier_ == before && scale_ == scale_before) return false;
    wait_ = 0.5;
    at_min_ = at_max_ = 0;
    return true;
}

}  // namespace wb::dasvis
