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

#include <atomic>
#include <chrono>
#include <complex>
#include <cstring>
#include <thread>

#include "engine/analysis.h"
#include "engine/engine.h"
#include "engine/fft.h"
#include "engine/scan_plan.h"
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

// The FFT as it was before it went four-at-a-time: std::complex, one butterfly and one bin at a time. Kept here as
// the reference the fast one must match bit for bit (analysis results are tuned against it).
static std::vector<float> reference_magnitudes(const std::vector<float>& in) {
    const size_t n = in.size(), h = n / 2;
    auto twiddle = [](size_t k, size_t len) {
        const double a = -2.0 * std::numbers::pi * double(k) / double(len);
        return std::complex<float>{float(std::cos(a)), float(std::sin(a))};
    };
    std::vector<size_t> rev(h);
    for (size_t i = 1, j = 0; i < h; ++i) {
        size_t bit = h >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        rev[i] = j;
    }
    std::vector<std::complex<float>> buf(h);
    for (size_t i = 0; i < h; ++i) buf[rev[i]] = {in[2 * i], in[2 * i + 1]};
    for (size_t len = 2; len <= h; len <<= 1) {
        const size_t half = len / 2;
        for (size_t i = 0; i < h; i += len)
            for (size_t k = 0; k < half; ++k) {
                const std::complex<float> tw = twiddle(k, len);
                std::complex<float>& a = buf[i + k];
                std::complex<float>& b = buf[i + k + half];
                const float tr = tw.real() * b.real() - tw.imag() * b.imag();
                const float ti = tw.real() * b.imag() + tw.imag() * b.real();
                b = {a.real() - tr, a.imag() - ti};
                a = {a.real() + tr, a.imag() + ti};
            }
    }
    std::vector<float> out(h);
    for (size_t k = 0; k < h; ++k) {
        const std::complex<float> z = buf[k], zc = std::conj(buf[k == 0 ? 0 : h - k]);
        const std::complex<float> e = 0.5f * (z + zc);
        const std::complex<float> d = z - zc;
        const std::complex<float> o{0.5f * d.imag(), -0.5f * d.real()};
        const std::complex<float> x = e + twiddle(k, n) * o;
        out[k] = std::sqrt(x.real() * x.real() + x.imag() * x.imag());
    }
    return out;
}

static void fft_matches_the_reference_bit_for_bit() {
    uint32_t seed = 0x2545F491u;
    for (const size_t n : {size_t(1024), size_t(8192), size_t(16), size_t(8)}) {
        std::vector<float> in(n);
        for (size_t i = 0; i < n; ++i) {
            seed = seed * 1664525u + 1013904223u;
            in[i] = float(seed >> 8) / float(1u << 24) - 0.5f + 0.4f * std::sin(float(i) * 0.37f);
        }
        const auto want = reference_magnitudes(in);
        wb::RealFft fft(n);
        std::vector<float> got(n / 2, -1.0f);
        fft.magnitudes(in.data(), got.data());
        CHECK(std::memcmp(got.data(), want.data(), want.size() * sizeof(float)) == 0, "whole spectrum, n=%zu", n);
        // A band only: the bins asked for match, the others are left alone.
        for (const auto& [first, last] : {std::pair<size_t, size_t>{1, n / 2 - 3}, {5, 6}, {n / 8, n / 4 + 1}, {0, 1}}) {
            if (first >= last || last > n / 2) continue;
            std::vector<float> band(n / 2, -1.0f);
            fft.magnitudes(in.data(), band.data(), first, last);
            bool same = true, untouched = true;
            for (size_t k = 0; k < n / 2; ++k) {
                if (k >= first && k < last) same = same && std::memcmp(&band[k], &want[k], sizeof(float)) == 0;
                else untouched = untouched && band[k] == -1.0f;
            }
            CHECK(same && untouched, "band %zu..%zu of n=%zu", first, last, n);
        }
    }
}

static void the_scan_plan_follows_cores_power_and_memory() {
    auto machine = [](unsigned logical, unsigned physical, bool battery, bool saver, uint64_t free_mb) {
        wb::Machine m;
        m.logical = logical, m.physical = physical, m.on_battery = battery, m.battery_saver = saver, m.free_mb = free_mb;
        return m;
    };
    // A plugged-in 8-core / 16-thread laptop with 8 GB free: 15 workers, 4 GB of audio between them.
    auto p = wb::plan_scan(machine(16, 8, false, false, 8192), 5000);
    CHECK(p.workers == 15 && p.memory_budget_mb == 4096 && !p.eco, "%u workers, %llu MB", p.workers, (unsigned long long)p.memory_budget_mb);
    // On battery: half the cores, efficiency mode. Battery saver: a quarter.
    p = wb::plan_scan(machine(16, 8, true, false, 8192), 5000);
    CHECK(p.workers == 4 && p.eco);
    p = wb::plan_scan(machine(16, 8, true, true, 8192), 5000);
    CHECK(p.workers == 2 && p.eco);
    // Never more workers than files, never fewer than one.
    CHECK(wb::plan_scan(machine(16, 8, false, false, 8192), 3).workers == 3);
    CHECK(wb::plan_scan(machine(1, 1, false, false, 8192), 100).workers == 1);
    CHECK(wb::plan_scan(machine(2, 2, true, true, 8192), 100).workers == 1);
    // Memory: half of what's free, within 256 MB … 8 GB; 1 GB when the system doesn't say.
    CHECK(wb::plan_scan(machine(16, 8, false, false, 300), 10).memory_budget_mb == 256);
    CHECK(wb::plan_scan(machine(16, 8, false, false, 64000), 10).memory_budget_mb == 8192);
    CHECK(wb::plan_scan(machine(16, 8, false, false, 0), 10).memory_budget_mb == 1024);
    // This PC can be asked.
    const wb::Machine here = wb::detect_machine();
    CHECK(here.logical >= 1 && here.physical >= 1 && here.physical <= here.logical && here.free_mb > 0);
    // An hour of 320 kbit/s MP3 (144 MB) is estimated well above a 5-minute FLAC (30 MB).
    CHECK(wb::estimated_audio_bytes(L"mix.mp3", 144ull << 20) > 500ull << 20);
    CHECK(wb::estimated_audio_bytes(L"track.flac", 30ull << 20) < 60ull << 20);
}

static void the_memory_budget_holds_work_back_and_lets_it_go() {
    wb::MemoryBudget budget(100);
    const std::atomic<bool> never{false};
    CHECK(budget.reserve(60, never) == 60);
    // An oversized request is trimmed to the whole budget — and waits for the budget to be whole.
    std::atomic<uint64_t> got{0};
    std::thread waiter([&] { got = budget.reserve(500, never); });
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(got == 0, "the big request should wait while 60 are held");
    budget.release(60);
    waiter.join();
    CHECK(got == 100);
    // A stopped scan doesn't wait.
    const std::atomic<bool> stopped{true};
    CHECK(budget.reserve(10, stopped) == 0);
}

int main() {
    camelot_codes();
    folding();
    click_track_tempo();
    a_minor_chord_key();
    short_keys();
    wav_analyse_and_tag_round_trip();
    errors_are_json();
    fft_matches_the_reference_bit_for_bit();
    the_scan_plan_follows_cores_power_and_memory();
    the_memory_budget_holds_work_back_and_lets_it_go();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all engine tests passed");
    return 0;
}
