#include "dasvis/features.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace wb::dasvis {
namespace {

constexpr float kRate = 48000;
constexpr float kBinHz = kRate / float(Features::kFft);
// −60 dB below a full-scale sine's magnitude through the Hann window (kFft / 4): quieter than this, a band's ups and
// downs are noise, however large they look next to its own (tiny) recent peak.
constexpr float kFloor = Features::kFft / 4.f * 1e-3f;

// One-pole smoothing towards `target` with time constant `tc` seconds.
float smooth(float v, float target, float dt, float tc) { return v + (target - v) * (1 - std::exp(-dt / tc)); }

}  // namespace

Features::Features() : window_(kFft), frame_(kFft), mags_(kFft / 2) {
    for (size_t i = 0; i < kFft; ++i)
        window_[i] = 0.5f - 0.5f * std::cos(2 * std::numbers::pi_v<float> * float(i) / float(kFft - 1));
}

double Features::bar_phase() const {
    const double b = std::fmod(beats, 4.0);
    return (b < 0 ? b + 4 : b) / 4;
}

float Features::rms(float lo_hz, float hi_hz) const {
    const size_t lo = std::max<size_t>(1, size_t(lo_hz / kBinHz)), hi = std::min(mags_.size() - 1, size_t(hi_hz / kBinHz));
    double sum = 0;
    for (size_t i = lo; i <= hi; ++i) sum += double(mags_[i]) * mags_[i];
    return float(std::sqrt(sum / double(hi - lo + 1)));
}

void Features::drum(Band& b, float level, float dt, float refractory, float release, float& env, bool& hit) {
    b.prev = b.level;
    b.level = level;
    b.since += dt;
    const float flux = std::max(0.f, b.level - b.prev);
    b.peak = std::max(level, b.peak * std::exp(-dt / 4.f));
    // An onset: the level jumps well above its usual rise, above its average, loud enough to matter, and not straight
    // after the last one (a kick's tail spans several frames).
    hit = flux > 1.8f * b.flux_avg + 0.04f * b.peak && level > 1.2f * b.avg && level > 0.15f * b.peak && level > kFloor &&
          b.since > refractory;
    b.flux_avg = smooth(b.flux_avg, flux, dt, 0.5f);
    b.avg = smooth(b.avg, level, dt, 1.0f);
    env *= std::exp(-dt / release);
    if (hit) {
        b.since = 0;
        const float strength = std::clamp((level - b.avg) / std::max(1e-6f, b.peak - b.avg), 0.f, 1.f);
        env = std::max(env, 0.6f + 0.4f * strength);  // every hit shows; harder ones a bit more
    }
}

void Features::follow(Band& b, float level, float dt, float& env) {
    b.level = level;
    b.peak = std::max(level, b.peak * std::exp(-dt / 6.f));
    const float n = b.peak > 1e-6f ? level / b.peak : 0.f;
    env = smooth(env, n, dt, n > env ? 0.03f : 0.3f);  // fast up, slow down
}

void Features::update(const float* mono, float dt, double known_bpm) {
    dt = std::clamp(dt, 0.f, 0.25f);
    time_ += dt;
    double sq = 0;
    for (size_t i = 0; i < kFft; ++i) {
        frame_[i] = mono[i] * window_[i];
        sq += double(mono[i]) * mono[i];
    }
    fft_.magnitudes(frame_.data(), mags_.data());

    drum(kick_band_, rms(40, 120), dt, 0.15f, 0.12f, kick, kick_hit);
    drum(snare_band_, rms(150, 400) + rms(1500, 4000), dt, 0.1f, 0.15f, snare, snare_hit);
    drum(hats_band_, rms(6000, 11000), dt, 0.06f, 0.06f, hats, hats_hit);
    follow(bass_band_, rms(40, 250), dt, bass);
    follow(lead_band_, rms(300, 4000), dt, lead);
    kicks += kick_hit, snares += snare_hit, hatses += hats_hit;

    const float loud = float(std::sqrt(sq / kFft));
    loud_peak_ = std::max(loud, loud_peak_ * std::exp(-dt / 8.f));
    energy = smooth(energy, loud_peak_ > 1e-5f ? loud / loud_peak_ : 0.f, dt, 1.5f);

    beat_clock(dt, known_bpm);

    // The spectrum: the loudest bin in each log-spaced slice, in dB, tilted +3 dB an octave around 1 kHz so the highs
    // show (as the Winamp bars do). A full-scale sine through a Hann window peaks at kFft / 4.
    constexpr float kLo = 30, kHi = 16000;
    for (size_t i = 0; i < kBins; ++i) {
        const float f0 = kLo * std::pow(kHi / kLo, float(i) / kBins), f1 = kLo * std::pow(kHi / kLo, float(i + 1) / kBins);
        const size_t b0 = std::min(mags_.size() - 1, size_t(f0 / kBinHz)), b1 = std::min(mags_.size() - 1, std::max(b0, size_t(f1 / kBinHz)));
        float m = 0;
        for (size_t b = b0; b <= b1; ++b) m = std::max(m, mags_[b]);
        const float db = 20 * std::log10(std::max(m / (kFft / 4.f), 1e-6f)) + 3 * std::log2(std::sqrt(f0 * f1) / 1000);
        spectrum_[i] = std::clamp((db + 70) / 70, 0.f, 1.f);
        peaks_[i] = std::max(spectrum_[i], peaks_[i] - dt * 0.4f);
    }

    // The waveform from a rising zero crossing, so it stands still; scaled by its recent peak.
    size_t start = kFft - kWave;
    for (size_t i = kFft - kWave; i > kFft - kWave - 512 && i > 0; --i)
        if (mono[i - 1] < 0 && mono[i] >= 0) {
            start = i;
            break;
        }
    float peak = 0;
    for (size_t i = 0; i < kWave; ++i) peak = std::max(peak, std::fabs(mono[start + i]));
    wave_peak_ = std::max(peak, wave_peak_ * std::exp(-dt / 2.f));
    const float k = 1 / std::max(0.02f, wave_peak_);
    for (size_t i = 0; i < kWave; ++i) wave_[i] = mono[start + i] * k;
}

void Features::beat_clock(float dt, double known_bpm) {
    if (known_bpm > 0) {
        double b = known_bpm;
        while (b < 70) b *= 2;
        while (b > 180) b /= 2;
        bpm = b;
    } else if (kick_hit) {
        kick_times_.push_back(time_);
        while (!kick_times_.empty() && time_ - kick_times_.front() > 8) kick_times_.erase(kick_times_.begin());
        if (kick_times_.size() >= 6) {
            // The tempo from the kicks: their gaps, folded into one beat at 70–180 BPM (kicks on 1 and 3, or doubled),
            // the median taken so the odd missed or extra kick doesn't move it.
            std::vector<double> gaps;
            for (size_t i = 1; i < kick_times_.size(); ++i) {
                double g = kick_times_[i] - kick_times_[i - 1];
                while (g > 60.0 / 70) g /= 2;
                while (g < 60.0 / 180) g *= 2;
                gaps.push_back(g);
            }
            std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
            bpm += (60 / gaps[gaps.size() / 2] - bpm) * 0.3;
        }
    }
    // Corrections are spread over ~150 ms and never make the clock run backwards (a tunnel would visibly jerk).
    const double advance = dt * bpm / 60;
    const double d = std::max(nudge_ * std::min(1.0, dt / 0.15), -0.8 * advance);
    beats += advance + d;
    nudge_ -= d;
    if (!kick_hit) return;
    // The phase-locked loop: a kick near a beat pulls the clock a fifth of the way onto it. Kicks far from the beat
    // four times in a row mean the clock is off by about half a beat: move onto them.
    const double err = beats + nudge_ - std::round(beats + nudge_);
    if (std::fabs(err) < 0.3) {
        nudge_ -= err * 0.2;
        missed_ = 0;
    } else if (++missed_ >= 4) {
        nudge_ -= err;
        missed_ = 0;
    }
}

}  // namespace wb::dasvis
