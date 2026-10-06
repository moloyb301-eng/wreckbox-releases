// BPM, key, energy and loudness analysis (port of core/src/analysis.rs; see that file for the method notes).
#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "engine/decode.h"

namespace wb {

struct Analysis {
    std::optional<double> bpm, bpm_confidence, bpm_alternate;
    std::vector<double> bpm_candidates;  // other plausible tempos, strongest first
    std::optional<std::string> key, camelot;
    std::optional<double> key_strength;
    std::optional<int> key_agreement;
    std::optional<double> energy, loudness_lufs;
    double duration_sec = 0;
};

Analysis analyze(const Audio& audio);

// Exposed for tests.
struct Tempo {
    double bpm = 0, confidence = 0;
    std::vector<double> others;
};
std::optional<Tempo> tempo(std::span<const float> x, uint32_t rate);

// Fold into 70–180; report the half / double tempo when both are plausible for DJ use.
std::pair<double, std::optional<double>> fold_bpm(double bpm);

struct KeyResult {
    std::string name, camelot;
    double strength = 0;
    int agreement = 1;
};
std::optional<KeyResult> key(std::span<const float> x, uint32_t rate);

// Camelot code for a tonic indexed from A. C major = 8B, A minor = 8A.
std::string camelot(size_t tonic_from_a, bool minor);

}  // namespace wb
