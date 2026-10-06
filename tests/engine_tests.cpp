// Engine tests: ports of the Rust unit tests plus an end-to-end analyse + tag round trip on a generated WAV
// (the same click track as the original CI smoke test). Plain asserts; exit code 1 on any failure.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <string>
#include <vector>

#include "engine/analysis.h"
#include "engine/engine.h"
#include "engine/tags.h"

namespace fs = std::filesystem;
static int failures = 0;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            ++failures;                                               \
            std::fprintf(stderr, "FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond); \
            std::fprintf(stderr, "" __VA_ARGS__);                     \
            std::fputc('\n', stderr);                                 \
        }                                                             \
    } while (0)

static void camelot_codes() {
    CHECK(wb::camelot(3, false) == "8B");   // C major
    CHECK(wb::camelot(0, true) == "8A");    // A minor
    CHECK(wb::camelot(7, true) == "9A");    // E minor
    CHECK(wb::camelot(8, true) == "4A");    // F minor
    CHECK(wb::camelot(7, false) == "12B");  // E major
}

static void folding() {
    auto [a, a_alt] = wb::fold_bpm(87.0);
    CHECK(a == 87.0 && a_alt && *a_alt == 174.0);
    auto [b, b_alt] = wb::fold_bpm(256.0);
    CHECK(b == 128.0 && !b_alt);
    auto [c, c_alt] = wb::fold_bpm(160.0);
    CHECK(c == 160.0 && c_alt && *c_alt == 80.0);
}

static void click_track_tempo() {
    // 128 BPM clicks for 30 s.
    const uint32_t rate = 22050;
    std::vector<float> x(size_t(rate) * 30, 0.0f);
    const size_t period = size_t(double(rate) * 60.0 / 128.0);
    for (size_t i = 0; i + 200 < x.size(); i += period)
        for (size_t k = 0; k < 200; ++k) x[i + k] = std::sin(float(k) * 0.3f) * (1.0f - float(k) / 200.0f);
    const auto t = wb::tempo(x, rate);
    CHECK(t.has_value());
    if (t) {
        const double bpm = wb::fold_bpm(t->bpm).first;
        CHECK(std::abs(bpm - 128.0) < 1.0, "got %.2f", bpm);
    }
}

static void a_minor_chord_key() {
    // Sustained A minor triad (A3, C4, E4) with a few harmonics.
    const uint32_t rate = 22050;
    const float notes[] = {220.0f, 261.63f, 329.63f};
    std::vector<float> x(size_t(rate) * 10);
    for (size_t i = 0; i < x.size(); ++i) {
        const float t = float(i) / float(rate);
        float s = 0;
        for (const float f : notes)
            s += std::sin(2.0f * std::numbers::pi_v<float> * f * t) + 0.3f * std::sin(4.0f * std::numbers::pi_v<float> * f * t);
        x[i] = s;
    }
    const auto k = wb::key(x, rate);
    CHECK(k && k->camelot == "8A", "got %s", k ? k->name.c_str() : "nothing");
}

static void short_keys() {
    CHECK(wb::short_key("A minor") == "Am");
    CHECK(wb::short_key("F# major") == "F#");
    CHECK(wb::short_key("Ebm") == "Ebm");
}

// 16-bit mono PCM WAV writer, enough for test fixtures.
static void write_wav(const fs::path& p, const std::vector<int16_t>& s, uint32_t rate) {
    std::ofstream f(p, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t data = uint32_t(s.size() * 2);
    f.write("RIFF", 4), u32(36 + data), f.write("WAVEfmt ", 8), u32(16), u16(1), u16(1), u32(rate), u32(rate * 2), u16(2), u16(16);
    f.write("data", 4), u32(data), f.write(reinterpret_cast<const char*>(s.data()), data);
}

static void wav_analyse_and_tag_round_trip() {
    // The original CI smoke test: 20 s at 44.1 kHz, a click every 0.5 s (120 BPM).
    const fs::path dir = fs::temp_directory_path() / "wreckbox-tests";
    fs::create_directories(dir);
    const fs::path wav = dir / L"smöke test.wav";  // non-ASCII on purpose
    std::vector<int16_t> s(44100 * 20);
    for (size_t i = 0; i < s.size(); ++i) s[i] = i % 22050 < 400 ? int16_t(12000 * std::sin(double(i) * 0.3)) : 0;
    write_wav(wav, s, 44100);

    const auto a = wb::analyze_file(wav);
    CHECK(!a.contains("error"), "%s", a.dump().c_str());
    CHECK(a.value("bpm", 0.0) > 119.0 && a.value("bpm", 0.0) < 121.0, "%s", a.dump().c_str());
    CHECK(a.value("durationSec", 0.0) == 20.0, "%s", a.dump().c_str());

    const auto w = wb::write_tags_json({{"path", reinterpret_cast<const char*>(wav.u8string().c_str())},
                                        {"title", "Smoke"},
                                        {"artists", {"CI", "Bot"}},
                                        {"bpm", 120.4},
                                        {"key", "A minor"},
                                        {"isrc", "USRC17607839"}});
    CHECK(w.value("ok", false), "%s", w.dump().c_str());
    const auto t = wb::read_tags_json(wav);
    CHECK(t.value("title", "") == "Smoke", "%s", t.dump().c_str());
    CHECK(t["bpm"] == 120.0, "%s", t.dump().c_str());
    CHECK(t["key"] == "Am", "%s", t.dump().c_str());
    CHECK(t["isrc"] == "USRC17607839", "%s", t.dump().c_str());
    CHECK(!t["artists"].empty() && t["artists"][0] == "CI / Bot", "%s", t.dump().c_str());

    // The tagged file still decodes to the same audio.
    const auto again = wb::analyze_file(wav);
    CHECK(again.value("bpm", 0.0) == a.value("bpm", 0.0), "%s", again.dump().c_str());
    // No temporary copies left behind.
    for (const auto& e : fs::directory_iterator(dir))
        CHECK(e.path().filename().wstring().rfind(L".wreckbox-", 0) != 0, "leftover %ls", e.path().c_str());
    fs::remove_all(dir);
}

static void errors_are_json() {
    const auto a = wb::analyze_file(L"C:\\does\\not\\exist.mp3");
    CHECK(a.contains("error"), "%s", a.dump().c_str());
    const auto w = wb::write_tags_json({{"path", "C:\\does\\not\\exist.mp3"}, {"title", "x"}});
    CHECK(w.contains("error"), "%s", w.dump().c_str());
}

int main() {
    camelot_codes();
    folding();
    click_track_tempo();
    a_minor_chord_key();
    short_keys();
    wav_analyse_and_tag_round_trip();
    errors_are_json();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all engine tests passed");
    return 0;
}
