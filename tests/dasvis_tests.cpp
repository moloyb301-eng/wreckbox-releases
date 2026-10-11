// DasVis tests: the song map on a synthetic song whose structure is known, the live audio features on synthetic drum
// tracks, the flash limiter against a strobe, the quality controller on fake slow and fast GPUs, and every scene
// rendered on WARP (Windows' software GPU, so it runs on any PC and in CI), at full Direct3D 11 and limited to feature
// level 10_0 (old graphics cards).
#include <windows.h>
#include <psapi.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <map>
#include <fstream>
#include <numbers>
#include <random>
#include <set>
#include <vector>

#include "dasvis/control.h"
#include "dasvis/dasvis.h"
#include "dasvis/director.h"
#include "dasvis/features.h"
#include "dasvis/palette.h"
#include "dasvis/songmap.h"
#include "engine/decode.h"

using namespace wb::dasvis;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
static int failures = 0;

#define CHECK(cond, ...)                                                         \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ++failures;                                                          \
            std::fprintf(stderr, "FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond); \
            std::fprintf(stderr, "" __VA_ARGS__);                                \
            std::fputc('\n', stderr);                                            \
        }                                                                        \
    } while (0)

constexpr int kRate = 48000;
constexpr int kHop = 800;  // samples per frame at 60 fps

// A drum track: kicks (a 60 → 45 Hz thump) on every beat and/or hats (bright noise) on the off-beats.
std::vector<float> drums(double seconds, double bpm, bool kick, bool hats) {
    std::vector<float> s(size_t(seconds * kRate));
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> noise(-1, 1);
    const double beat = 60.0 / bpm;
    float last = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const double t = double(i) / kRate, in_beat = std::fmod(t, beat), off = std::fmod(t + beat / 2, beat);
        float v = 0;
        if (kick) v += 0.8f * float(std::exp(-in_beat / 0.12) * std::sin(2 * std::numbers::pi * (60 - 60 * in_beat) * in_beat));
        if (hats) {
            const float n = noise(rng), hp = n - last;  // a difference: mostly high frequencies
            last = n;
            v += 0.25f * float(std::exp(-off / 0.03)) * hp;
        }
        s[i] = v;
    }
    return s;
}

// Feeds `s` to `f` frame by frame (the newest kFft samples each time); calls `each(frame_end_sample)` after each frame.
template <class Fn>
void play(Features& f, const std::vector<float>& s, double known_bpm, Fn each) {
    std::vector<float> win(Features::kFft);
    for (size_t end = kHop; end <= s.size(); end += kHop) {
        for (size_t i = 0; i < Features::kFft; ++i) win[i] = end >= Features::kFft - i ? s[end - Features::kFft + i] : 0.f;
        f.update(win.data(), float(kHop) / kRate, known_bpm);
        each(end);
    }
}

void features() {
    {  // Kicks: found (all but maybe the first, while the levels are learnt), the tempo learnt, the clock locked to them.
        Features f;
        const auto s = drums(12, 120, true, true);
        int locked = 0, checked = 0;
        float before_next = 1;
        bool released = true;
        play(f, s, 0, [&](size_t end) {
            const double t = double(end) / kRate;
            if (f.kick_hit && t > 6) {
                ++checked;
                const double err = std::fabs(f.beats - std::round(f.beats));
                locked += err < 0.1;
                released &= before_next < 0.15f;  // the envelope fell away before this kick
            }
            before_next = f.kick_hit ? 1.f : std::min(before_next, f.kick);
        });
        CHECK(f.kicks >= 22 && f.kicks <= 24, "kicks %d of 24", f.kicks);
        CHECK(std::fabs(f.bpm - 120) < 2, "bpm %.1f", f.bpm);
        CHECK(checked >= 10 && locked == checked, "beat clock on %d of %d kicks", locked, checked);
        CHECK(released, "the kick envelope falls away between kicks");
        CHECK(f.hatses >= 15, "hats %d", f.hatses);
    }
    {  // Hats alone are not kicks.
        Features f;
        play(f, drums(6, 120, false, true), 0, [](size_t) {});
        CHECK(f.kicks == 0, "kicks in hats: %d", f.kicks);
        CHECK(f.hatses >= 8, "hats %d", f.hatses);
    }
    {  // Silence: nothing fires, the envelopes settle at nothing.
        Features f;
        play(f, drums(4, 120, true, false), 0, [](size_t) {});
        const int before = f.kicks;
        play(f, std::vector<float>(kRate * 2), 0, [](size_t) {});
        CHECK(f.kicks == before, "kicks in silence: %d", f.kicks - before);
        CHECK(f.kick < 0.01f && f.bass < 0.05f, "kick %.3f bass %.3f after silence", f.kick, f.bass);
    }
    {  // A known tempo is used as is (folded into 70–180), and the beat count never runs backwards.
        Features f;
        double last = 0;
        bool forward = true;
        play(f, drums(4, 128, true, false), 64, [&](size_t) {
            forward &= f.beats >= last;
            last = f.beats;
        });
        CHECK(f.bpm == 128, "bpm %.1f", f.bpm);
        CHECK(forward, "the beat count only goes forward");
        CHECK(f.bar_phase() >= 0 && f.bar_phase() < 1, "bar phase %.2f", f.bar_phase());
    }
}

// The flash limiter against a scene that asks for a full-screen flash 8 times a second: what reaches the screen
// (measured two frames late, as on the GPU) must have at most 3 flashes (1 with "Reduce flashing") in any second.
int worst_second(int per_second) {
    FlashLimiter lim(per_second);
    FlashCounter seen;
    std::vector<float> shown;
    std::vector<double> flashes;
    const float dt = 1 / 60.f;
    for (int i = 0; i < 60 * 6; ++i) {
        const bool asks = (i % 8) < 3;  // 3 frames on, 5 off: 7.5 Hz
        const float scene = 0.05f + (asks ? 0.85f * lim.budget() : 0.f);
        shown.push_back(std::min(1.f, scene * lim.gain()));
        if (seen.update(shown.back(), dt)) flashes.push_back(i * dt);
        if (shown.size() > 2) lim.update(shown[shown.size() - 3], dt);
    }
    int worst = 0;
    for (size_t a = 0; a < flashes.size(); ++a)
        worst = std::max(worst, int(std::count_if(flashes.begin() + long(a), flashes.end(), [&](double t) { return t - flashes[a] < 1.0; })));
    return worst;
}

void limiter() {
    CHECK(worst_second(3) <= 3, "%d flashes in a second", worst_second(3));
    CHECK(worst_second(1) <= 1, "%d flashes in a second (reduce flashing)", worst_second(1));
    {  // A steady bright picture isn't dimmed.
        FlashLimiter lim;
        for (int i = 0; i < 300; ++i) lim.update(0.8f, 1 / 60.f);
        CHECK(lim.gain() > 0.99f && lim.budget() == 1, "gain %.2f budget %.2f", lim.gain(), lim.budget());
    }
    {  // A slow brightening (over a second) isn't a flash.
        FlashCounter c;
        int n = 0;
        for (int i = 0; i <= 60; ++i) n += c.update(0.1f + 0.8f * float(i) / 60, 1 / 60.f);
        CHECK(n == 0, "a slow fade counted as %d flashes", n);
    }
}

// The quality controller on a fake GPU whose frame cost grows with the render area and the quality knob.
struct Sim {
    Tier tier;
    float scale, ms;
    int tier_changes;
};
Sim simulate(float cost, Tier start, bool automatic = true) {
    QualityControl qc(start, automatic);
    const float budget = 13;
    Tier last = qc.tier();
    int changes = 0;
    float ms = 0;
    for (int i = 0; i < 60 * 60; ++i) {
        ms = cost * qc.scale() * qc.scale() * (0.4f + 0.6f * qc.quality());
        qc.update(ms, budget, 1 / 60.f);
        changes += qc.tier() != last;
        last = qc.tier();
    }
    return {qc.tier(), qc.scale(), cost * qc.scale() * qc.scale() * (0.4f + 0.6f * qc.quality()), changes};
}

void quality() {
    const Sim weak = simulate(80, Tier::medium);
    CHECK(weak.tier == Tier::low && weak.ms <= 13 * 0.9f, "weak GPU: %s at %.2f, %.1f ms", tier_name(weak.tier), weak.scale, weak.ms);
    const Sim strong = simulate(4, Tier::medium);
    CHECK(strong.tier == Tier::high && strong.scale == tier_spec(Tier::high).scale_max, "strong GPU: %s at %.2f", tier_name(strong.tier), strong.scale);
    const Sim middling = simulate(25, Tier::medium);
    CHECK(middling.ms <= 13 * 0.9f, "middling GPU: %.1f ms", middling.ms);
    for (const Sim& s : {weak, strong, middling}) CHECK(s.tier_changes <= 2, "%d tier changes (see-sawing)", s.tier_changes);
    const Sim fixed = simulate(80, Tier::high, false);
    CHECK(fixed.tier == Tier::high && fixed.scale == tier_spec(Tier::high).scale_min, "a fixed tier only moves the scale");
}

// MARK: Song map

// A synthetic 128 BPM song, 22.05 kHz, starting 0.37 s in, with a known structure (bars, repeating every 56):
//   0–15 intro (soft kick, hats, bass, chords) · 16–23 build-up (a snare roll getting faster and louder, a noise
//   riser, then bar 23 silent: the breath) · 24–39 drop (kick, bass, claps, hats) · 40–47 breakdown (chords and a
//   melody) · 48–55 drop. The bass and the chord change on every bar's downbeat.
struct Synth {
    static constexpr double kRate = 22050, kBpm = 128, kBeat = 60 / kBpm, kBar = 4 * kBeat, kStart = 0.37;
    std::vector<float> s;
    std::mt19937 rng{3};
    std::uniform_real_distribution<float> noise{-1, 1};

    explicit Synth(int bars) : s(size_t((kStart + bars * kBar + 1) * kRate)) {
        const double chords[4][3] = {{220, 277.2, 329.6}, {196, 246.9, 293.7}, {174.6, 220, 261.6}, {164.8, 207.7, 246.9}};
        const double bass[4] = {55, 49, 43.65, 41.2};
        for (int bar = 0; bar < bars; ++bar) {
            const int b = bar % 56, c = bar % 4;
            const double t = kStart + bar * kBar;
            const bool intro = b < 16, build = b >= 16 && b < 23, gap = b == 23, drop = (b >= 24 && b < 40) || b >= 48, breakdown = b >= 40 && b < 48;
            if (gap) continue;
            if (!build) {
                for (const double f : chords[c]) tone(t, kBar, f, breakdown ? 0.05 : 0.03);
                tone(t, kBar * 0.9, bass[c], drop ? 0.35 : intro ? 0.12 : 0.0);
            }
            for (int k = 0; k < 4; ++k) {
                const double tb = t + k * kBeat;
                if (intro || drop) kick(tb, drop ? 0.8 : 0.35);
                if (intro || drop || build) hit(tb + kBeat / 2, intro ? 0.06 : drop ? 0.12 : 0.08, 0.03, true);  // hats
                if (drop && (k == 1 || k == 3)) hit(tb, 0.3, 0.08, false);                                     // claps
                if (breakdown) tone(tb, kBeat * 0.8, 440 * std::pow(2.0, ((bar * 4 + k) * 5 % 12) / 12.0), 0.12);
            }
            if (build) {  // the roll: quarters, then eighths, then sixteenths, louder as it goes; and the riser
                const int per_beat = b < 20 ? 1 : b < 22 ? 2 : 4;
                const float amp = 0.1f + 0.25f * float(b - 16) / 7;
                for (int k = 0; k < 4 * per_beat; ++k) hit(t + k * kBeat / per_beat, amp, 0.06, false);
                for (size_t i = size_t(t * kRate); i < size_t((t + kBar) * kRate); ++i)
                    s[i] += 0.15f * float((double(i) / kRate - (kStart + (bar - b + 16) * kBar)) / (7 * kBar)) * noise(rng);
            }
        }
    }
    void kick(double t, double amp) {
        for (size_t i = 0; i < size_t(0.3 * kRate); ++i) {
            const double x = double(i) / kRate;
            s[size_t(t * kRate) + i] += float(amp * std::exp(-x / 0.12) * std::sin(2 * std::numbers::pi * (60 - 60 * x) * x));
        }
    }
    void hit(double t, double amp, double decay, bool bright) {
        float last = 0;
        for (size_t i = 0; i < size_t(5 * decay * kRate); ++i) {
            const float n = noise(rng), v = bright ? n - last : n;
            last = n;
            s[size_t(t * kRate) + i] += float(amp * std::exp(-double(i) / kRate / decay)) * v;
        }
    }
    void tone(double t, double dur, double f, double amp) {
        if (amp <= 0) return;
        for (size_t i = 0; i < size_t(dur * kRate); ++i) {
            const double x = double(i) / kRate, env = std::min({1.0, x / 0.01, (dur - x) / 0.05});
            s[size_t(t * kRate) + i] += float(amp * env * std::sin(2 * std::numbers::pi * f * x));
        }
    }
};

SongMap map_of(const std::vector<float>& s, double bpm) {
    MapBuilder b(bpm);
    for (size_t i = 0; i < s.size(); i += 4096) b.push(&s[i], std::min<size_t>(4096, s.size() - i));  // as a decoder hands it over
    return b.finish();
}

// A drop that starts off the bar grid (two beats late, as when the downbeat is guessed wrong): it's still found, and
// the part starts on the beat where the loudness steps up, not a bar early with the quiet before it in it.
void songmap_off_grid() {
    using S = Synth;
    Synth x(0);
    const double drop_t = S::kStart + 16 * S::kBar + 2 * S::kBeat;  // bar 16½
    x.s.assign(size_t((drop_t + 24 * S::kBar + 1) * S::kRate), 0.f);
    for (int beat = 0; S::kStart + beat * S::kBeat < drop_t + 24 * S::kBar; ++beat) {
        const double t = S::kStart + beat * S::kBeat;
        if (t < drop_t) {  // the intro: a chord and bass note that change on each bar's first beat (its downbeats)
            static const double roots[4] = {220, 196, 174.6, 164.8};
            x.hit(t + S::kBeat / 2, 0.05, 0.03, true);
            if (beat % 4 == 0) x.tone(t, S::kBar, roots[beat / 4 % 4], 0.05), x.tone(t, S::kBar, roots[beat / 4 % 4] * 1.5, 0.03),
                               x.tone(t, S::kBar * 0.9, roots[beat / 4 % 4] / 4, 0.15);
        } else {  // the drop, two beats off that grid: a kick, clap and bass on every beat, nothing to mark a bar
            x.kick(t, 0.8);
            x.hit(t + S::kBeat / 2, 0.12, 0.03, true);
            x.hit(t, 0.2, 0.08, false);
            x.tone(t, S::kBeat * 0.9, 55, 0.3);
        }
    }
    const SongMap m = map_of(x.s, 0);
    CHECK(m.drops.size() == 1 && std::fabs(m.drops[0] - drop_t) < S::kBeat * 1.01, "an off-grid drop at %.2f s: found at %.2f s (%zu drops)",
          drop_t, m.drops.empty() ? -1.0 : m.drops[0], m.drops.size());
    std::printf("  off-grid drop: at %.2f s, found at %.2f s\n", drop_t, m.drops.empty() ? -1.0 : m.drops[0]);
}

// Memory and time on a 6-minute song: the samples stream through, so the analysis needs only a few MB. Runs first,
// so the process's peak memory so far is just the test song.
void songmap_cost() {
    const Synth song(192);  // 6 min
    PROCESS_MEMORY_COUNTERS pm{sizeof pm};
    GetProcessMemoryInfo(GetCurrentProcess(), &pm, sizeof pm);
    const size_t before = pm.PagefileUsage;
    const auto t0 = std::chrono::steady_clock::now();
    const SongMap m = map_of(song.s, 0);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    GetProcessMemoryInfo(GetCurrentProcess(), &pm, sizeof pm);
    const double extra = double(pm.PeakPagefileUsage - std::min(before, pm.PeakPagefileUsage)) / 1048576.0;
    std::printf("  song map of a 6-minute song: %.2f s, %.1f MB at most while analysing, %zu beats, %zu parts\n", secs, extra, m.beats.size(),
                m.parts.size());
    CHECK(extra < 30, "analysis memory %.1f MB", extra);
    CHECK(secs < 15, "analysis took %.1f s", secs);
    CHECK(std::fabs(m.bpm - 128) < 1, "6-minute song: bpm %.2f", m.bpm);
}

void songmap() {
    const Synth song(56);
    using S = Synth;
    const auto bar_t = [](double bar) { return S::kStart + bar * S::kBar; };
    for (const double known : {0.0, 64.0}) {  // found by itself, and given by the library (as half: folded to 128)
        const SongMap m = map_of(song.s, known);
        CHECK(std::fabs(m.bpm - 128) < 0.5, "bpm %.2f (known %.0f)", m.bpm, known);
        // Beats: every true beat has a detected one close by.
        std::vector<double> err, signed_err;
        for (int k = 0; k < 56 * 4; ++k) {
            const double truth = S::kStart + k * S::kBeat;
            double best = 1e9;
            for (const double b : m.beats)
                if (std::fabs(b - truth) < std::fabs(best)) best = b - truth;
            err.push_back(std::fabs(best)), signed_err.push_back(best);
        }
        std::vector<double> sorted = err;
        std::sort(sorted.begin(), sorted.end());
        std::sort(signed_err.begin(), signed_err.end());
        std::printf("  beats: signed median error %+.1f ms\n", signed_err[signed_err.size() / 2] * 1000);
        const double median = sorted[sorted.size() / 2], within = double(std::count_if(err.begin(), err.end(), [](double e) { return e < 0.025; })) / double(err.size());
        std::printf("  beats (known bpm %.0f): median error %.1f ms, %.0f %% within 25 ms\n", known, median * 1000, within * 100);
        CHECK(median < 0.012 && within > 0.95, "beats: median %.1f ms, %.0f %% within 25 ms", median * 1000, within * 100);
        // Downbeats: the beat at each bar start is a downbeat.
        int down_ok = 0, down_n = 0;
        for (int bar = 2; bar < 56; ++bar) {
            const auto it = std::min_element(m.beats.begin(), m.beats.end(), [&](double a, double b) { return std::fabs(a - bar_t(bar)) < std::fabs(b - bar_t(bar)); });
            ++down_n;
            down_ok += ((it - m.beats.begin()) - m.downbeat) % 4 == 0;
        }
        CHECK(down_ok > down_n * 9 / 10, "downbeats right on %d of %d bars", down_ok, down_n);
        // Sections: the build-up, both drops (within a beat), the breakdown, and the breath before the first drop.
        const auto covers = [&](Section k, double a, double b) {
            double in = 0;
            for (const auto& p : m.parts)
                if (p.kind == k) in += std::max(0.0, std::min(p.end, bar_t(b)) - std::max(p.start, bar_t(a)));
            return in / (bar_t(b) - bar_t(a));
        };
        std::string parts;
        for (const auto& p : m.parts) parts += std::string(section_name(p.kind)) + " " + std::to_string(int(std::lround((p.start - S::kStart) / S::kBar))) + ", ";
        std::printf("  parts (bar where each starts): %s\n", parts.c_str());
        CHECK(covers(Section::build, 16, 24) > 0.6, "build-up found (%s)", parts.c_str());
        CHECK(covers(Section::breakdown, 40, 48) > 0.6, "breakdown found (%s)", parts.c_str());
        CHECK(m.drops.size() == 2, "%zu drops (%s)", m.drops.size(), parts.c_str());
        if (m.drops.size() == 2) {
            CHECK(std::fabs(m.drops[0] - bar_t(24)) < S::kBeat && std::fabs(m.drops[1] - bar_t(48)) < S::kBeat, "drops at %.2f, %.2f s (truth %.2f, %.2f)",
                  m.drops[0], m.drops[1], bar_t(24), bar_t(48));
            CHECK(std::fabs(m.gaps[0] - bar_t(23)) < S::kBeat, "the breath before the drop starts at %.2f s (truth %.2f)", m.gaps[0], bar_t(23));
        }
        // What a frame sees: two bars before the drop; on it; in the breath.
        const auto build = m.at(bar_t(22));
        CHECK(build.section == Section::build && build.build > 0.5f && build.build < 1 && std::fabs(build.to_drop - 2) < 0.3f,
              "at bar 22: %s, build %.2f, drop in %.2f bars", section_name(build.section), build.build, build.to_drop);
        const auto drop = m.at(bar_t(24) + 0.05);
        CHECK(drop.section == Section::drop && drop.drop > 0.9f, "on the drop: %s, drop %.2f", section_name(drop.section), drop.drop);
        const auto gap = m.at(bar_t(23.5));
        CHECK(gap.gap && gap.build == 1, "in the breath: gap %d, build %.2f", int(gap.gap), gap.build);
        CHECK(drop.stems[SongMap::kick] > 0.3f || m.at(bar_t(24) + 0.02).stems[SongMap::kick] > 0.3f, "kick envelope on the drop");
        CHECK(m.hits_between(SongMap::kick, bar_t(30), bar_t(34)) >= 15, "kick hits in 4 drop bars: %d", m.hits_between(SongMap::kick, bar_t(30), bar_t(34)));
        CHECK(m.hits_between(SongMap::kick, bar_t(41), bar_t(47)) <= 1, "kick hits in the breakdown: %d", m.hits_between(SongMap::kick, bar_t(41), bar_t(47)));

        // The cache: round-trips; another version, or a damaged file, is refused.
        const fs::path file = fs::temp_directory_path() / L"wb-dasvis-test.map";
        CHECK(m.save(file), "save");
        const auto back = SongMap::load(file);
        CHECK(back && back->beats == m.beats && back->drops == m.drops && back->parts.size() == m.parts.size() && back->env[0] == m.env[0] &&
                  back->hits[0] == m.hits[0] && back->downbeat == m.downbeat && back->bpm == m.bpm,
              "the map round-trips");
        {
            std::fstream f(file, std::ios::in | std::ios::out | std::ios::binary);
            f.seekp(4);
            const uint32_t old = SongMap::kVersion + 1;
            f.write(reinterpret_cast<const char*>(&old), 4);
        }
        CHECK(!SongMap::load(file), "another version is made again");
        std::ofstream(file, std::ios::binary) << "DVMP";
        CHECK(!SongMap::load(file), "a damaged file is refused");
        fs::remove(file);
    }
    // The cache name follows the song and its file.
    const fs::path a = fs::path(WB_FIXTURES) / "audio" / "tone.wav", b = fs::path(WB_FIXTURES) / "audio" / "tone.flac";
    CHECK(map_cache_file("x", "id1", a) != map_cache_file("x", "id2", a) && map_cache_file("x", "id1", a) != map_cache_file("x", "id1", b) &&
              map_cache_file("x", "id1", a) == map_cache_file("x", "id1", a),
          "cache names");
    // Streaming decode hands over exactly what decode_mono returns.
    for (const char* name : {"tone.wav", "tone.mp3", "tone.flac", "tone.m4a"}) {
        const fs::path f = fs::path(WB_FIXTURES) / "audio" / name;
        const wb::Audio whole = wb::decode_mono(f);
        std::vector<float> streamed;
        const double dur = wb::decode_mono_stream(f, [&](const float* s, size_t n) { streamed.insert(streamed.end(), s, s + n); });
        CHECK(streamed == whole.samples && dur == whole.duration, "streamed %s matches", name);
    }
    std::string why;
    CHECK(!analyse_file(fs::path(WB_FIXTURES) / "audio" / "tone.wav", 0, &why) && !why.empty(), "a tone has no beat: %s", why.c_str());
}

// MARK: Colours and stems

float luminance(const Rgb& c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

void colours() {
    // A cover: half red, half blue, a white band along the top.
    std::vector<uint8_t> px(128 * 128 * 4, 255);
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x) {
            uint8_t* p = &px[(size_t(y) * 128 + size_t(x)) * 4];
            if (y < 16) p[0] = p[1] = p[2] = 245;
            else if (x < 64) p[0] = 30, p[1] = 30, p[2] = 220;  // BGRA: red
            else p[0] = 200, p[1] = 60, p[2] = 40;              // blue
        }
    const auto p = cover_palette(px.data(), 128, 128);
    CHECK(p.has_value(), "a colourful cover gives a palette");
    if (p) {
        bool red = false, blue = false, ordered = true;
        for (size_t i = 0; i < 5; ++i) {
            red |= (*p)[i][0] > 2 * (*p)[i][2] && (*p)[i][0] > (*p)[i][1];
            blue |= (*p)[i][2] > 1.5f * (*p)[i][0];
            if (i) ordered &= luminance((*p)[i]) > luminance((*p)[i - 1]);
            for (const float v : (*p)[i]) CHECK(v >= 0 && v <= 1, "inside sRGB");
        }
        CHECK(red && blue, "the cover's red and blue are in its palette");
        CHECK(ordered, "dark to light");
        CHECK(cover_palette(px.data(), 128, 128) == p, "the same cover, the same colours");
    }
    // A grey cover (black and white photo): no palette from it; the key's colours are used instead.
    std::vector<uint8_t> grey(64 * 64 * 4, 255);
    for (size_t i = 0; i < 64 * 64; ++i) grey[i * 4] = grey[i * 4 + 1] = grey[i * 4 + 2] = uint8_t(i % 251);
    CHECK(!cover_palette(grey.data(), 64, 64), "a grey cover gives no palette");
    CHECK(!cover_palette(nullptr, 0, 0), "no cover, no palette");
    // The key: minor keys are cooler (bluer) than the major key with the same number.
    const Rgb minor = key_tint(std::string("1A"), 0), major = key_tint(std::string("1B"), 0);
    CHECK(minor[2] / (minor[0] + minor[1] + minor[2]) > major[2] / (major[0] + major[1] + major[2]), "1A cooler than 1B");
    CHECK(key_palette(std::string("8A"), 0) != key_palette(std::string("3A"), 0), "different keys, different colours");
}

void drives() {
    SongMap m;
    m.bpm = 120;
    for (int i = 0; i < 40; ++i) m.beats.push_back(0.5 * i);
    for (int i = 2; i < 40; ++i) m.hits[SongMap::kick].push_back(0.5 * i);
    m.hits[SongMap::snare] = {1.25};
    const Features live;  // silent: everything below comes from the map
    Drive d;
    int kicks = 0, snares = 0;
    double t = 0.9, hit_at = -1;
    for (int i = 0; i < 30; ++i) {
        const double next = t + 1 / 60.0;
        d = drive(live, &m, t, next, d, 1 / 60.f);
        if (d.kick_hit) ++kicks, hit_at = next;
        snares += d.snare_hit;
        t = next;
    }
    CHECK(kicks == 1 && hit_at >= 1.0 && hit_at < 1.0 + 1 / 60.0 + 1e-9, "one kick, on the frame after 1.0 s (%d, at %.3f)", kicks, hit_at);
    CHECK(snares == 1, "the snare at 1.25 s: %d", snares);
    CHECK(std::fabs(d.since_kick - float(t - 1.0)) < 1e-3f, "time since the kick: %.3f", d.since_kick);
    d = drive(live, &m, 3.1, 9.3, d, 1 / 60.f);  // a seek over many kicks
    CHECK(!d.kick_hit, "a seek doesn't fire the kicks it jumped over");
    Features f;  // without a map: the live analysis, counting its own time since a kick
    f.kick = 0.7f, f.kick_hit = true;
    Drive l = drive(f, nullptr, 0, 0, d, 1 / 60.f);
    CHECK(l.kick == 0.7f && l.kick_hit && l.since_kick == 0, "live");
    f.kick_hit = false;
    l = drive(f, nullptr, 0, 0, l, 0.5f);
    CHECK(std::fabs(l.since_kick - 0.5f) < 1e-6f, "live time since the kick: %.2f", l.since_kick);
}

// MARK: GPU

struct Stats {
    double mean = 0;
};

Stats read(ID3D11Device* dev, ID3D11Texture2D* tex, std::vector<uint8_t>* keep = nullptr) {
    ComPtr<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(&ctx);
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING, d.BindFlags = 0, d.CPUAccessFlags = D3D11_CPU_ACCESS_READ, d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> st;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &st))) return {};
    ctx->CopyResource(st.Get(), tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return {};
    double sum = 0;
    if (keep) keep->clear();
    for (UINT y = 0; y < d.Height; ++y)
        for (UINT x = 0; x < d.Width; ++x) {
            const uint8_t* p = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch + size_t(x) * 4;
            sum += (p[0] + p[1] + p[2]) / (3 * 255.0);
            if (keep) keep->insert(keep->end(), p, p + 3);
        }
    ctx->Unmap(st.Get(), 0);
    return {sum / (double(d.Width) * d.Height)};
}

double diff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    double s = 0;
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) s += std::abs(int(a[i]) - int(b[i]));
    return a.empty() ? 0 : s / double(a.size()) / 255;
}

void gpu(bool fl10) {
    const char* label = fl10 ? "feature level 10_0" : "Direct3D 11";
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl10 ? &levels[2] : levels,
                                         fl10 ? 1 : 3, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    CHECK(SUCCEEDED(hr), "WARP device (%s)", label);
    if (FAILED(hr)) return;

    // A user's scenes folder: one good scene, one broken one.
    const fs::path dir = fs::temp_directory_path() / L"wb-dasvis-test-scenes";
    fs::remove_all(dir);
    prepare_user_dir(dir);  // writes the example scene too
    std::ofstream(dir / L"broken.hlsl") << "#define DASVIS_SCENE\n#include \"dasvis.hlsli\"\nfloat3 scene(float2 uv, float2 p) { return nope; }\n";

    Config cfg;
    // Feature level 10_0 at Low (the smallest simulations), the other at Medium.
    cfg.start = fl10 ? Tier::low : Tier::medium, cfg.auto_tier = false, cfg.auto_advance = 0, cfg.user_dir = dir;
    DasVis dv(dev.Get(), cfg);
    CHECK(dv.ok(), "DasVis on %s: %s", label, dv.error().c_str());
    if (!dv.ok()) return;
    CHECK(dv.scene_count() == 14, "scenes: %zu (13 built in + the example)", dv.scene_count());
    CHECK(dv.user_errors().find("broken.hlsl") != std::string::npos && dv.user_errors().find("nope") != std::string::npos,
          "the broken scene's error names it: %s", dv.user_errors().c_str());

    std::vector<uint8_t> cover(64 * 64 * 4);
    for (int i = 0; i < 64 * 64; ++i) cover[size_t(i) * 4] = uint8_t(i % 64 * 4), cover[size_t(i) * 4 + 1] = uint8_t(i / 64 * 4), cover[size_t(i) * 4 + 3] = 255;
    dv.set_cover(cover.data(), 64, 64);

    D3D11_TEXTURE2D_DESC td{320, 180, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE};
    ComPtr<ID3D11Texture2D> screen;  // stands in for the window's back buffer
    CHECK(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &screen)), "target texture");
    Features f;
    const auto music = drums(60, 124, true, true);
    std::vector<float> win(Features::kFft);
    size_t at = kHop;
    Inputs in;
    in.f = &f, in.song = "test-song", in.camelot = "8A";
    auto frame = [&] {
        for (size_t i = 0; i < Features::kFft; ++i) win[i] = at >= Features::kFft - i ? music[(at - Features::kFft + i) % music.size()] : 0.f;
        at += kHop;
        f.update(win.data(), 1 / 60.f, 124);
        dv.render(in, screen.Get());
    };
    // Each scene in turn (N reaches them all, the heavy ones too, even on Low).
    std::set<std::string> seen;
    for (size_t s = 0; s < dv.scene_count(); ++s) {
        if (s) dv.next();
        for (int i = 0; i < 60; ++i) frame();  // the transition is over
        std::vector<uint8_t> a, b;
        const Stats one = read(dev.Get(), screen.Get(), &a);
        for (int i = 0; i < 20; ++i) frame();
        const Stats two = read(dev.Get(), screen.Get(), &b);
        const std::string name = dv.scene_name();
        if (!seen.insert(name).second) break;  // round again: every scene that fits was shown
        CHECK(one.mean > 0.01 && one.mean < 0.95, "%s on %s: mean %.3f (black or white)", name.c_str(), label, one.mean);
        CHECK(diff(a, b) > 0.002, "%s on %s: the picture doesn't move (%.4f)", name.c_str(), label, diff(a, b));
        std::printf("  %s on %s: mean %.3f, moving %.3f\n", name.c_str(), label, two.mean, diff(a, b));
    }
    CHECK(seen.size() == dv.scene_count(), "N reaches every scene on %s (%zu seen)", label, seen.size());
    // The phone deck: pick a scene, call a drop (a hero comes on), turn the camera (the picture moves with it).
    const auto names = dv.scene_names();
    const size_t kaleido = size_t(std::find(names.begin(), names.end(), "Kaleido Cover") - names.begin());
    dv.select(kaleido);
    CHECK(dv.scene_name() == "Kaleido Cover", "the deck picks a scene: %s", dv.scene_name().c_str());
    dv.drop_now();
    for (int i = 0; i < 30; ++i) frame();
    bool hero = false;
    for (const auto& t : builtin_scenes()) hero |= t.hero && t.name == dv.scene_name();
    CHECK(hero, "drop now brings a hero scene on %s: %s", label, dv.scene_name().c_str());
    dv.lock(true);
    for (int i = 0; i < 60; ++i) frame();
    std::vector<uint8_t> still, turned;
    read(dev.Get(), screen.Get(), &still);
    dv.set_camera(1, 0.5f);
    for (int i = 0; i < 30; ++i) frame();
    read(dev.Get(), screen.Get(), &turned);
    CHECK(diff(still, turned) > 0.02, "the deck's camera moves the picture on %s (%.3f)", label, diff(still, turned));
    dv.set_camera(0, 0);
    dv.lock(false);
    // The feedback stays bounded: Feedback Classic with loud music for 10 s doesn't white out.
    while (dv.scene_name() != "Feedback Classic") dv.next();
    for (int i = 0; i < 600; ++i) frame();
    std::vector<uint8_t> last, again;
    const Stats fb = read(dev.Get(), screen.Get(), &last);
    CHECK(fb.mean < 0.8, "Feedback Classic after 10 s on %s: mean %.3f", label, fb.mean);
    // Paused: redraw() puts the same picture back (apart from the film grain).
    const float clear[4] = {};
    ComPtr<ID3D11RenderTargetView> rtv;
    dev->CreateRenderTargetView(screen.Get(), nullptr, &rtv);
    ctx->ClearRenderTargetView(rtv.Get(), clear);
    dv.redraw(screen.Get());
    read(dev.Get(), screen.Get(), &again);
    CHECK(diff(last, again) < 0.02, "redraw on %s differs by %.3f", label, diff(last, again));
    fs::remove_all(dir);
}

// Room lights: the strip's colours from the palette, brighter on the kick, held back when the flash budget is spent.
void lights() {
    LightState s;
    for (size_t i = 0; i < 5; ++i) s.pal[i] = {0.1f * float(i + 1), 0.05f * float(i + 1), 0.2f};
    const auto mean = [](const std::vector<uint8_t>& v) {
        double sum = 0;
        for (const uint8_t b : v) sum += b;
        return sum / double(std::max<size_t>(1, v.size()));
    };
    const auto quiet = led_frame(s, 60);
    CHECK(quiet.size() == 180 && mean(quiet) > 10, "60 LEDs, lit at rest (%.1f)", mean(quiet));
    s.kick = 1;
    const double kicked = mean(led_frame(s, 60));
    CHECK(kicked > mean(quiet) * 1.3, "the kick brightens them (sRGB bytes): %.1f → %.1f", mean(quiet), kicked);
    s.flash = 0;
    CHECK(std::fabs(mean(led_frame(s, 60)) - mean(quiet)) < 0.5, "no flash budget left: no swing (%.1f)", mean(led_frame(s, 60)));
    s.flash = 1, s.reduce_flash = true;
    const double reduced = mean(led_frame(s, 60));
    CHECK(reduced < kicked && reduced > mean(quiet), "reduce flashing: a smaller swing (%.1f)", reduced);
    s.reduce_flash = false, s.kick = 0;
    const auto now = led_frame(s, 60);
    s.beats += 1;
    CHECK(led_frame(s, 60) != now, "the colours move on with the beat");
    CHECK(led_frame(s, 0).empty(), "no LEDs: nothing");
}

// The director on its own: a song's sections in, scenes out.
void director() {
    std::vector<SceneTags> tags = builtin_scenes();
    tags.push_back({"Heavy", kCalm | kMid | kPeak, 'H'});  // a heavy scene, never measured
    const size_t heavy = tags.size() - 1;
    const Section song[] = {Section::intro, Section::verse, Section::build, Section::drop, Section::breakdown, Section::verse,
                            Section::build, Section::drop, Section::chorus, Section::outro};
    // The scenes for `rounds` passes through the song: index, and whether it was a drop's.
    const auto run = [&](uint32_t seed, Tier tier, const std::map<std::string, float>& costs, const std::map<std::string, float>* taste,
                         int rounds) {
        Director d(tags);
        d.set_costs(costs);
        d.set_taste(taste);
        std::vector<std::pair<size_t, bool>> out;
        size_t current = 0;
        uint32_t index = 0;
        for (int r = 0; r < rounds; ++r)
            for (size_t i = 0; i < std::size(song); ++i) {
                Moment m;
                m.seed = seed, m.index = index++, m.section = song[i], m.from = i ? song[i - 1] : song[i];
                m.drop = song[i] == Section::drop, m.energy = Director::energy_of(song[i], 0.6f), m.tier = tier, m.budget_ms = 13;
                current = d.pick(m, current).scene;
                out.emplace_back(current, m.drop);
            }
        return out;
    };
    const auto a = run(42, Tier::medium, {}, nullptr, 20), b = run(42, Tier::medium, {}, nullptr, 20), c = run(43, Tier::medium, {}, nullptr, 20);
    CHECK(a == b, "the same seed gives the same scenes");
    CHECK(a != c, "another seed gives other scenes");
    int repeats = 0, unheroic = 0, heavy_on_low = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].second) unheroic += !tags[a[i].first].hero;
        else
            for (size_t k = i >= 4 ? i - 4 : 0; k < i; ++k) repeats += a[k].first == a[i].first;
    }
    CHECK(repeats == 0, "no scene repeats within 4 picks (%d did)", repeats);
    CHECK(unheroic == 0, "every drop gets a hero scene (%d didn't)", unheroic);
    // Low tier: a scene measured over the budget, and a heavy one not measured yet, are never picked.
    for (const auto& [i, drop] : run(7, Tier::low, {{"Coral|Low", 40.f}}, nullptr, 40)) heavy_on_low += i == heavy || tags[i].name == "Coral";
    CHECK(heavy_on_low == 0, "Low tier never picks a scene that doesn't fit (%d times)", heavy_on_low);
    int heavy_on_high = 0;
    for (const auto& [i, drop] : run(7, Tier::high, {}, nullptr, 40)) heavy_on_high += i == heavy;
    CHECK(heavy_on_high > 0, "High tier picks the heavy scene");
    // Likes and skips: a liked scene comes up more often than a skipped one.
    const std::map<std::string, float> taste{{"Kaleido Cover", 4.f}, {"Ridge Lines", 0.15f}};
    int liked = 0, skipped = 0;
    for (const auto& [i, drop] : run(9, Tier::medium, {}, &taste, 40)) liked += tags[i].name == "Kaleido Cover", skipped += tags[i].name == "Ridge Lines";
    CHECK(liked > 2 * skipped, "liked %d times, skipped %d times", liked, skipped);
    // Transitions and events.
    Director d(tags);
    Moment m;
    m.section = Section::drop, m.from = Section::build, m.drop = true;
    CHECK(d.dress(1, m).transition == Transition::zoom, "build-up to drop: zoom through");
    m.from = Section::verse;
    CHECK(d.dress(1, m).transition == Transition::shatter, "into a drop: shatter");
    m.drop = false, m.glitchy = true;
    CHECK(d.dress(1, m).transition == Transition::glitch, "dubstep: glitch");
    int whiteouts = 0;
    for (size_t k = 0; k < 50; ++k) whiteouts += (Director::events(5, k, 0) & kWhiteout) != 0;
    CHECK(whiteouts == 0, "chill: no white-outs");
    std::printf("  director: %zu picks, no repeats within 4, heroes on drops; liked %d, skipped %d\n", a.size(), liked, skipped);
}

// The director in the renderer: scenes change only where a part of the song starts, or on a 16-bar phrase after a
// scene has been on long enough; a drop gets a hero; the same song plays out the same way twice.
void director_timing() {
    ComPtr<ID3D11Device> dev;
    const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_10_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, nullptr)))
        return std::puts("FAIL: no WARP device"), void(++failures);
    SongMap map;  // 120 BPM: a bar is 2 s, a 16-bar phrase 32 s
    map.bpm = 120, map.duration = 176;
    for (double t = 0; t < 176; t += 0.5) map.beats.push_back(t);
    const std::pair<double, Section> parts[] = {{0, Section::intro},  {16, Section::verse},     {80, Section::build},
                                                {96, Section::drop},  {128, Section::breakdown}, {160, Section::outro}};
    for (size_t i = 0; i < std::size(parts); ++i)
        map.parts.push_back({parts[i].first, i + 1 < std::size(parts) ? parts[i + 1].first : 176, parts[i].second, parts[i].second == Section::drop ? 1.f : 0.5f});
    map.drops = {96}, map.gaps = {-1};
    D3D11_TEXTURE2D_DESC td{96, 54, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
    ComPtr<ID3D11Texture2D> screen;
    dev->CreateTexture2D(&td, nullptr, &screen);
    const auto play = [&] {
        Config cfg;
        cfg.start = Tier::low, cfg.auto_tier = false;
        DasVis dv(dev.Get(), cfg);
        Features f;
        Inputs in;
        in.f = &f, in.song = "director-song", in.map = &map, in.intensity = 2;  // insane: a new scene every 16 bars at most
        std::vector<std::pair<double, std::string>> changes;
        for (int i = 0; i < 176 * 30; ++i) {
            in.song_time = i / 30.0, in.dt = 1 / 30.f;
            dv.render(in, screen.Get());
            if (changes.empty() || changes.back().second != dv.scene_name()) changes.emplace_back(in.song_time, dv.scene_name());
        }
        return changes;
    };
    const auto one = play(), two = play();
    CHECK(one == two, "the same song plays out the same way twice");
    // Expected: the start, each part, and the verse's 16-bar phrase at 64 s (on 32 bars of verse).
    const double expect[] = {0, 16, 64, 80, 96, 128, 160};
    bool on_time = one.size() == std::size(expect);
    for (size_t i = 0; on_time && i < one.size(); ++i) on_time = one[i].first >= expect[i] && one[i].first < expect[i] + 0.05;
    std::string got;
    for (const auto& [t, name] : one) got += std::format("{:.2f} {}; ", t, name);
    CHECK(on_time, "scene changes at the starts of parts and phrases: %s", got.c_str());
    const auto at_drop = std::find_if(one.begin(), one.end(), [](const auto& c) { return c.first >= 96; });
    CHECK(at_drop != one.end() && (at_drop->second == "Neon Tunnel" || at_drop->second == "Galaxy"), "the drop gets a hero: %s",
          at_drop != one.end() ? at_drop->second.c_str() : "none");
    std::printf("  director in the renderer: %s\n", got.c_str());
}

// The cover shatter on a drop (insane: every event): the cover's shards are on screen after the drop. Two runs with grey
// covers (no palette from them, so everything else is the same), dark and light: only the shards can differ.
void cover_shatter() {
    ComPtr<ID3D11Device> dev;
    const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_10_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, nullptr)))
        return std::puts("FAIL: no WARP device"), void(++failures);
    SongMap map;
    map.bpm = 120, map.duration = 40;
    for (double t = 0; t < 40; t += 0.5) map.beats.push_back(t);
    map.parts = {{0, 16, Section::build, 0.5f}, {16, 40, Section::drop, 1.f}};
    map.drops = {16}, map.gaps = {-1};
    D3D11_TEXTURE2D_DESC td{160, 90, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE};
    ComPtr<ID3D11Texture2D> screen;
    dev->CreateTexture2D(&td, nullptr, &screen);
    const auto run = [&](uint8_t grey, double until) {
        Config cfg;
        cfg.start = Tier::low, cfg.auto_tier = false;
        DasVis dv(dev.Get(), cfg);
        std::vector<uint8_t> cover(32 * 32 * 4, grey);
        dv.set_cover(cover.data(), 32, 32);
        Features f;
        Inputs in;
        in.f = &f, in.song = "shatter-song", in.map = &map, in.intensity = 2;
        for (int i = 0; i <= int(until * 30); ++i) in.song_time = i / 30.0, in.dt = 1 / 30.f, dv.render(in, screen.Get());
        std::vector<uint8_t> px;
        read(dev.Get(), screen.Get(), &px);
        return px;
    };
    // Only outside the middle, where the spectrum ring (which shows the cover too, on a drop) never reaches here: the
    // test plays silence, so its bars stay short.
    const auto outside = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
        double sum = 0;
        int n = 0;
        for (int y = 0; y < 90; ++y)
            for (int x = 0; x < 160; ++x) {
                const double px = (x + 0.5) / 160 - 0.5, py = (y + 0.5) / 90 - 0.5;
                if (std::hypot(px * 160 / 90, py) < 0.24) continue;
                for (int c = 0; c < 3; ++c) sum += std::abs(int(a[(size_t(y) * 160 + x) * 3 + c]) - int(b[(size_t(y) * 160 + x) * 3 + c])) / 255.0;
                n += 3;
            }
        return sum / n;
    };
    const double before = outside(run(40, 15), run(220, 15)), after = outside(run(40, 16.4), run(220, 16.4));
    CHECK(after > before + 0.02, "the cover's shards show after the drop: %.3f before, %.3f after", before, after);
    std::printf("  cover shatter: dark vs light cover differ %.3f before the drop, %.3f after\n", before, after);
}

// dasvis_tests --map <audio file> [bpm]: prints a real song's map instead of testing (for tuning the analysis).
int print_map(const fs::path& file, double bpm) {
    const auto t0 = std::chrono::steady_clock::now();
    std::string why;
    const auto m = analyse_file(file, bpm, &why);
    if (!m) return std::printf("no map: %s\n", why.c_str()), 1;
    std::printf("%.1f s, %.2f BPM, %zu beats, downbeat %d, analysed in %.2f s\n", m->duration, m->bpm, m->beats.size(), m->downbeat,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    for (const auto& p : m->parts) std::printf("  %6.1f - %6.1f s  %-10s energy %.2f\n", p.start, p.end, section_name(p.kind), p.energy);
    for (size_t k = 0; k < m->drops.size(); ++k) std::printf("  drop at %.1f s (breath from %.1f)\n", m->drops[k], m->gaps[k]);
    std::printf("  hits: %zu kicks, %zu snares, %zu hats\n", m->hits[0].size(), m->hits[1].size(), m->hits[2].size());
    return 0;
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc >= 3 && std::string(argv[1]) == "--map") return print_map(fs::path(argv[2]), argc >= 4 ? std::atof(argv[3]) : 0);
    songmap_cost();  // first: it reads the process's peak memory
    songmap();
    songmap_off_grid();
    colours();
    drives();
    features();
    limiter();
    quality();
    director();
    lights();
    director_timing();
    cover_shatter();
    gpu(false);
    gpu(true);
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all DasVis tests passed");
    return 0;
}
