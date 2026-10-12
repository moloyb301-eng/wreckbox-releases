// DasVis's live audio features: what the music is doing right now, from the samples being heard, as smooth values a
// shader can use directly. Phase 1 works from the live audio only (phase 2 adds the song map):
//   - kick / snare / hats: onsets from spectral flux in their bands, each an envelope with a fast attack and a slower
//     release, so pictures spring with the hit and ease back instead of jittering with the raw level
//   - bass / lead: smoothed band levels; energy: the overall loudness, slow
//   - a beat clock: the tempo from the library's analysis when known (else from the kicks), its phase pulled towards
//     the kicks (a phase-locked loop)
//   - the spectrum (256 log bins) and a trigger-aligned waveform, for the shaders' textures
// Levels are normalised by their own recent peaks (automatic gain), so quiet and loud songs both fill the range.
// Costs one 2,048-point FFT and a few hundred multiply-adds a frame.
#pragma once
#include <array>
#include <cstddef>
#include <vector>

#include "engine/fft.h"

namespace wb::dasvis {

class Features {
public:
    static constexpr size_t kFft = 2048;  // ~43 ms at 48 kHz
    static constexpr size_t kBins = 256, kWave = 512;

    Features();

    // `mono`: the newest kFft samples heard (48 kHz, oldest first; zeros while paused); `dt`: seconds since the last call;
    // `known_bpm`: the song's analysed tempo, 0 if unknown (radio, not analysed yet).
    void update(const float* mono, float dt, double known_bpm);

    // Envelopes, 0–1.
    float kick = 0, snare = 0, hats = 0, bass = 0, lead = 0, energy = 0;
    bool kick_hit = false, snare_hit = false, hats_hit = false;  // an onset in this frame
    int kicks = 0, snares = 0, hatses = 0;                       // onsets so far (tests)

    // The beat clock: `beats` counts beats (its fraction is the phase within the beat, 0 on the beat).
    double bpm = 120, beats = 0;
    double beat_phase() const { return beats - double(long long(beats)); }
    double bar_phase() const;  // 0→1 across each 4-beat bar (live, the bar's start is a guess until phase 2)

    const std::array<float, kBins>& spectrum() const { return spectrum_; }        // 0–1, log-spaced 30 Hz – 16 kHz
    const std::array<float, kBins>& spectrum_peaks() const { return peaks_; }     // the same, falling slowly
    const std::array<float, kWave>& wave() const { return wave_; }                // about −1…1, from a rising zero crossing

private:
    struct Band {
        float level = 0, prev = 0;  // RMS magnitude, this frame and the last
        float avg = 0;              // ~1 s average
        float peak = 0;             // automatic gain: jumps up, decays over seconds
        float flux_avg = 0;         // average positive flux
        float since = 1;            // seconds since the last onset
    };
    float rms(float lo_hz, float hi_hz) const;
    // Updates a drum band from its new level: onset detection and the envelope.
    void drum(Band& b, float level, float dt, float refractory, float release, float& env, bool& hit);
    void follow(Band& b, float level, float dt, float& env);  // a smoothed, gain-normalised level (bass, lead)
    void beat_clock(float dt, double known_bpm);

    RealFft fft_{kFft};
    std::vector<float> window_, frame_, mags_;
    Band kick_band_, snare_band_, hats_band_, bass_band_, lead_band_;
    float loud_peak_ = 0, wave_peak_ = 0;
    double time_ = 0;
    int missed_ = 0;                  // kicks in a row far from the beat (the clock is half a beat off: re-lock)
    double nudge_ = 0;                // phase correction still to apply
    std::vector<double> kick_times_;  // recent kicks (seconds), for the tempo when the library doesn't know it
    std::array<float, kBins> spectrum_{}, peaks_{};
    std::array<float, kWave> wave_{};
};

}  // namespace wb::dasvis
