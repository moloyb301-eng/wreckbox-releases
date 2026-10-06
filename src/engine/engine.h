// The engine's public surface, in the same JSON shapes as the Rust engine's C ABI (core/src/ffi.rs), so the app,
// wbcore and the analysis cache all speak one format. These never throw: failures come back as {"error": "..."}.
#pragma once
#include <filesystem>

#include <nlohmann/json.hpp>

namespace wb {

// {"bpm", "bpmConfidence", "bpmAlternate", "bpmCandidates", "key", "camelot", "keyStrength", "keyAgreement",
//  "energy", "loudnessLufs", "durationSec"}
nlohmann::json analyze_file(const std::filesystem::path& path);

// {"title", "artists", "album", "year", "genre", "bpm", "key", "isrc", "hasCover"}
nlohmann::json read_tags_json(const std::filesystem::path& path);

// job: {"path", "title", "artists", "album", "year", "genre", "bpm", "key", "isrc", "cover"} → {"ok": true}
nlohmann::json write_tags_json(const nlohmann::json& job);

const char* engine_version();

}  // namespace wb
