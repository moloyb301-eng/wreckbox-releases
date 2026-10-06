#include "engine/engine.h"

#include <exception>
#include <optional>

#include "engine/analysis.h"
#include "engine/decode.h"
#include "engine/tags.h"

using nlohmann::json;
namespace fs = std::filesystem;

namespace wb {
namespace {

template <class T>
json opt(const std::optional<T>& v) {
    return v ? json(*v) : json(nullptr);
}

template <class T>
std::optional<T> get_opt(const json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) return std::nullopt;
    try {
        return it->get<T>();
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

fs::path path_from_utf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

// Errors must never escape: same contract as the Rust `guarded`.
template <class F>
json guarded(F&& f) {
    try {
        return f();
    } catch (const std::exception& e) {
        return {{"error", e.what()}};
    } catch (...) {
        return {{"error", "internal error in the analysis engine"}};
    }
}

}  // namespace

json analyze_file(const fs::path& path) {
    return guarded([&] {
        const Analysis a = analyze(decode_mono(path));
        return json{
            {"bpm", opt(a.bpm)},
            {"bpmConfidence", opt(a.bpm_confidence)},
            {"bpmAlternate", opt(a.bpm_alternate)},
            {"bpmCandidates", a.bpm_candidates},
            {"key", opt(a.key)},
            {"camelot", opt(a.camelot)},
            {"keyStrength", opt(a.key_strength)},
            {"keyAgreement", opt(a.key_agreement)},
            {"energy", opt(a.energy)},
            {"loudnessLufs", opt(a.loudness_lufs)},
            {"durationSec", a.duration_sec},
        };
    });
}

json read_tags_json(const fs::path& path) {
    return guarded([&] {
        const TrackTags t = read_tags(path);
        return json{
            {"title", opt(t.title)}, {"artists", t.artists}, {"album", opt(t.album)}, {"year", opt(t.year)},
            {"genre", opt(t.genre)}, {"bpm", opt(t.bpm)},    {"key", opt(t.key)},     {"isrc", opt(t.isrc)},
            {"cover", nullptr},      {"hasCover", t.has_cover},
        };
    });
}

json write_tags_json(const json& job) {
    return guarded([&] {
        if (!job.is_object()) return json{{"error", "bad job: expected an object"}};
        TrackTags t;
        t.title = get_opt<std::string>(job, "title");
        if (const auto a = get_opt<std::vector<std::string>>(job, "artists")) t.artists = *a;
        t.album = get_opt<std::string>(job, "album");
        t.year = get_opt<std::string>(job, "year");
        t.genre = get_opt<std::string>(job, "genre");
        t.bpm = get_opt<double>(job, "bpm");
        t.key = get_opt<std::string>(job, "key");
        t.isrc = get_opt<std::string>(job, "isrc");
        t.cover = get_opt<std::string>(job, "cover");
        write_tags(path_from_utf8(get_opt<std::string>(job, "path").value_or("")), t);
        return json{{"ok", true}};
    });
}

const char* engine_version() { return WB_VERSION; }

}  // namespace wb
