#include "player/visualizer.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "player/audio_output.h"

namespace wb::player {
namespace {

constexpr float kLowHz = 20, kHighHz = 16000;
constexpr float kFloorDb = -60;  // bottom of the bars
// Music has less energy the higher it goes; analysers tilt +3 dB per octave (around 1 kHz) so the highs show and the
// bass doesn't swamp the picture. (Winamp got there with linear bands, which squash the bass into a few bars instead.)
constexpr float kTiltDbPerOctave = 3;
constexpr float kHold = 0.4f;    // seconds a peak cap hangs before it drops
// Units of full height per second, slowest → fastest (Winamp's five falloff settings).
constexpr float kBarFall[] = {0.8f, 1.4f, 2.2f, 3.2f, 4.5f};
constexpr float kPeakFall[] = {0.15f, 0.3f, 0.5f, 0.8f, 1.2f};

template <class E>
const char* name(E v, std::initializer_list<const char*> names) {
    return names.begin()[size_t(v)];
}
template <class E>
E parse(const json& j, const char* key, E fallback, std::initializer_list<const char*> names) {
    if (!j.contains(key) || !j[key].is_string()) return fallback;
    for (size_t i = 0; i < names.size(); ++i)
        if (j[key].get<std::string>() == names.begin()[i]) return E(i);
    return fallback;
}

}  // namespace

json VisOptions::to_json() const {
    return {{"mode", name(mode, {"spectrum", "scope", "both", "off", "milkdrop"})},
            {"bars", name(bars, {"normal", "fire", "line"})},
            {"scope", name(scope, {"dots", "lines", "solid"})},
            {"barFalloff", bar_falloff},
            {"peakFalloff", peak_falloff},
            {"peaks", peaks},
            {"look", classic ? "classic" : "wreckbox"},
            {"quality", quality},
            {"autoAdvance", auto_advance},
            {"pulse", pulse},
            {"hardCuts", hard_cuts},
            {"presets", all_presets ? "all" : "beat"},
            {"beatSensitivity", beat_sensitivity},
            {"syncMs", sync_ms}};
}

VisOptions VisOptions::from_json(const json& j) {
    VisOptions o;
    if (!j.is_object()) return o;
    o.mode = parse(j, "mode", o.mode, {"spectrum", "scope", "both", "off", "milkdrop"});
    o.bars = parse(j, "bars", o.bars, {"normal", "fire", "line"});
    o.scope = parse(j, "scope", o.scope, {"dots", "lines", "solid"});
    o.bar_falloff = std::clamp(j.value("barFalloff", o.bar_falloff), 0, 4);
    o.peak_falloff = std::clamp(j.value("peakFalloff", o.peak_falloff), 0, 4);
    o.peaks = j.value("peaks", o.peaks);
    o.classic = j.value("look", std::string("classic")) != "wreckbox";
    const int q = j.value("quality", o.quality);
    o.quality = q <= 540 ? 540 : q <= 720 ? 720 : 1080;
    o.auto_advance = std::clamp(j.value("autoAdvance", o.auto_advance), 0, 600);
    o.pulse = std::clamp(j.value("pulse", o.pulse), 0, 2);
    o.hard_cuts = j.value("hardCuts", o.hard_cuts);
    o.all_presets = j.value("presets", std::string("beat")) == "all";
    o.beat_sensitivity = std::clamp(j.value("beatSensitivity", o.beat_sensitivity), 1, 3);
    o.sync_ms = std::clamp(j.value("syncMs", o.sync_ms), -300, 300);
    return o;
}

float BeatPulse::update(const float* mono, size_t n, float dt) {
    constexpr float kAlpha = 0.0194f;  // 1 - exp(-2π · 150 Hz / 48 kHz)
    env_ *= std::exp(-std::max(dt, 0.f) / 0.06f);
    since_ += std::max(dt, 0.f);
    if (!n) return env_;
    double e = 0;
    for (size_t i = 0; i < n; ++i) {
        lp1_ += kAlpha * (mono[i] - lp1_);
        lp2_ += kAlpha * (lp1_ - lp2_);
        e += double(lp2_) * lp2_;
    }
    const float energy = float(e / double(n));
    if (energy > 1.6f * avg_ && energy > last_ && energy > 1e-4f && since_ > 0.15f) {
        env_ = 1;
        since_ = 0;
        ++kicks_;
    }
    avg_ += (energy - avg_) * (1 - std::exp(-std::max(dt, 0.f) / 1.0f));  // ~1 s memory
    last_ = energy;
    return env_;
}

float Visualizer::band_hz(size_t i) const {
    const float n = float(std::max<size_t>(bars_.size(), 1));
    return kLowHz * std::pow(kHighHz / kLowHz, (float(i) + 0.5f) / n);
}

void Visualizer::set_band_count(size_t n) {
    n = std::clamp<size_t>(n, 8, 256);
    if (n == bars_.size()) return;
    bars_.assign(n, 0);
    peaks_.assign(n, 0);
    hold_.assign(n, 0);
}

void Visualizer::update(const float* samples, float dt) {
    if (bars_.empty()) set_band_count(32);
    if (window_.empty()) {
        window_.resize(kFft);
        for (size_t i = 0; i < kFft; ++i) window_[i] = 0.5f - 0.5f * std::cos(2 * std::numbers::pi_v<float> * float(i) / float(kFft - 1));
        frame_.resize(kFft);
        mags_.resize(kFft / 2);
    }
    dt = std::clamp(dt, 0.f, 0.25f);

    // Spectrum: Hann window → magnitudes, scaled so a full-scale sine reads 0 dB (Hann's coherent gain is 1/2).
    for (size_t i = 0; i < kFft; ++i) frame_[i] = samples[i] * window_[i];
    fft_.magnitudes(frame_.data(), mags_.data());
    const float scale = 4.f / float(kFft), bin_hz = float(kRate) / float(kFft);
    const size_t n = bars_.size();
    const float fall = kBarFall[std::clamp(opt.bar_falloff, 0, 4)] * dt, peak_fall = kPeakFall[std::clamp(opt.peak_falloff, 0, 4)] * dt;
    for (size_t b = 0; b < n; ++b) {
        const float lo = kLowHz * std::pow(kHighHz / kLowHz, float(b) / float(n));
        const float hi = kLowHz * std::pow(kHighHz / kLowHz, float(b + 1) / float(n));
        // The loudest bin in the band. Bass bands are narrower than a bin: interpolate at the band's centre instead, so
        // they slope smoothly rather than several bars reading the same bin.
        float m = 0;
        if (hi - lo < bin_hz) {
            const float at = std::sqrt(lo * hi) / bin_hz;
            const size_t k = std::min(size_t(at), mags_.size() - 2);
            m = std::lerp(mags_[k], mags_[k + 1], at - float(k));
        } else {
            for (size_t k = size_t(lo / bin_hz + 0.5f); k <= size_t(hi / bin_hz + 0.5f) && k < mags_.size(); ++k) m = std::max(m, mags_[k]);
        }
        const float db = 20 * std::log10(std::max(m * scale, 1e-9f)) + kTiltDbPerOctave * std::log2(std::sqrt(lo * hi) / 1000);
        const float v = std::clamp((db - kFloorDb) / -kFloorDb, 0.f, 1.f);
        // Instant attack, steady fall — Winamp's feel.
        bars_[b] = std::max(v, bars_[b] - fall);
        if (bars_[b] >= peaks_[b]) {
            peaks_[b] = bars_[b];
            hold_[b] = kHold;
        } else if (hold_[b] > 0) {
            hold_[b] -= dt;
        } else {
            peaks_[b] = std::max(bars_[b], peaks_[b] - peak_fall);
        }
    }

    // Oscilloscope: 1024 samples from the first rising zero crossing in the older half.
    size_t start = 0;
    for (size_t i = 1; i < kFft / 2; ++i)
        if (samples[i - 1] < 0 && samples[i] >= 0) {
            start = i;
            break;
        }
    wave_.assign(samples + start, samples + start + kFft / 2);
}

}  // namespace wb::player
