// Audio decoding to mono float at a fixed analysis rate (port of core/src/decode.rs).
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <vector>

namespace wb {

// Analysis sample rate. 22.05 kHz keeps everything up to ~11 kHz, plenty for tempo and key.
inline constexpr uint32_t kAnalysisRate = 22050;

struct Audio {
    std::vector<float> samples;
    uint32_t rate = kAnalysisRate;
    double duration = 0;  // length of the original file in seconds
};

// Decodes the whole file to mono at kAnalysisRate. WAV/AIFF via dr_wav, everything else via Media Foundation.
// Throws std::runtime_error with a readable message.
Audio decode_mono(const std::filesystem::path& path);

// The same samples, handed to `sink` in chunks as they're decoded instead of kept: for analyses that stream through a
// song (a few MB however long it is, instead of ~5 MB a minute). Returns the original's duration in seconds.
double decode_mono_stream(const std::filesystem::path& path, const std::function<void(const float*, size_t)>& sink);

}  // namespace wb
