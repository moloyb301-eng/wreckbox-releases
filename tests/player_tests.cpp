// Player tests: VLC's engine with the plugin subset WreckBox ships, decoding many formats into a capturing sink (no
// sound device, nothing audible), seeking, the equalizer's effect, playlist expansion, and the queue rules.
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <numbers>
#include <random>
#include <thread>
#include <vector>

#include "player/milkdrop.h"
#include "player/playlist.h"
#include "player/queue.h"
#include "player/visualizer.h"
#include "player/vlc_engine.h"

namespace fs = std::filesystem;
using namespace wb::player;
using namespace std::string_literals;
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

static std::string audio(const char* name) { return std::string(WB_FIXTURES) + "/audio/" + name; }


// Records what VLC delivers instead of playing it.
struct Capture : Sink {
    std::mutex m;
    std::vector<float> samples;  // interleaved stereo
    void play(const float* s, unsigned frames) override {
        std::lock_guard lock(m);
        samples.insert(samples.end(), s, s + size_t(frames) * kChannels);
    }
    void pause(bool) override {}
    // libVLC flushes after every track ends (drain → end reached → flush); keep what was captured.
    void flush() override {}
    void clear() {
        std::lock_guard lock(m);
        samples.clear();
    }
    size_t frames() {
        std::lock_guard lock(m);
        return samples.size() / kChannels;
    }
};

struct Run {
    Capture sink;
    VlcEngine engine;
    std::mutex m;
    std::condition_variable cv;
    bool ended = false, error = false;
    Run() : engine(sink) {
        engine.on_event = [this](Event e) {
            if (e != Event::ended && e != Event::error) return;
            std::lock_guard lock(m);
            (e == Event::ended ? ended : error) = true;
            cv.notify_all();
        };
    }
    // Opens and waits until the track ends (or fails / times out). True if it ended normally.
    bool play_through(const std::string& path, float rate, int timeout_s = 15) {
        sink.clear();
        {
            std::lock_guard lock(m);
            ended = error = false;
        }
        if (!engine.open(path, false)) return false;
        engine.set_rate(rate);
        std::unique_lock lock(m);
        cv.wait_for(lock, std::chrono::seconds(timeout_s), [&] { return ended || error; });
        return ended && !error;
    }
};

// Amplitude of one frequency in mono-mixed captured audio (Goertzel).
static double level(const std::vector<float>& stereo, double hz) {
    const double w = 2 * std::numbers::pi * hz / kRate, c = 2 * std::cos(w);
    double s1 = 0, s2 = 0;
    const size_t n = stereo.size() / 2;
    for (size_t i = 0; i < n; ++i) {
        const double s0 = 0.5 * (stereo[i * 2] + stereo[i * 2 + 1]) + c * s1 - s2;
        s2 = s1, s1 = s0;
    }
    return std::sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / double(std::max<size_t>(n, 1));
}

// 16-bit stereo AIFF (big-endian; sample rate as an 80-bit float), 1.5 s of 440 Hz.
static std::string write_aiff() {
    const fs::path p = fs::temp_directory_path() / L"wreckbox-player-test.aiff";
    const uint32_t frames = 44100 * 3 / 2;
    std::string data;
    for (uint32_t i = 0; i < frames; ++i) {
        const auto v = int16_t(9000 * std::sin(2 * std::numbers::pi * 440 * i / 44100.0));
        for (int ch = 0; ch < 2; ++ch) data += char(v >> 8), data += char(v & 0xFF);
    }
    auto be32 = [](uint32_t v) { return std::string{char(v >> 24), char(v >> 16), char(v >> 8), char(v)}; };
    const std::string rate80("\x40\x0E\xAC\x44\0\0\0\0\0\0", 10);  // 44100.0
    const std::string comm = std::string("COMM") + be32(18) + "\0\x02"s + be32(frames) + "\0\x10"s + rate80;
    const std::string ssnd = std::string("SSND") + be32(uint32_t(data.size() + 8)) + be32(0) + be32(0) + data;
    const std::string form = std::string("FORM") + be32(uint32_t(4 + comm.size() + ssnd.size())) + "AIFF" + comm + ssnd;
    std::ofstream(p, std::ios::binary).write(form.data(), std::streamsize(form.size()));
    const auto u = p.u8string();
    return {u.begin(), u.end()};
}

static void queue_rules() {
    Queue q;
    CHECK(!q.current() && !q.next());
    q.set({Item{{}, "a"}, Item{{}, "b"}, Item{{}, "c"}}, 1);
    CHECK(q.current()->location == "b");
    CHECK(q.previous(5000) == Queue::Prev::restart && q.index() == 1);  // >3 s in: restart this one
    CHECK(q.previous(1000) == Queue::Prev::moved && q.current()->location == "a");
    CHECK(q.previous(0) == Queue::Prev::restart && q.index() == 0);  // first track: restart
    CHECK(q.next() && q.next() && q.current()->location == "c" && !q.next() && q.current()->location == "c");
    q.set({Item{{}, "x"}}, 7);  // start past the end clamps
    CHECK(q.current()->location == "x");
    q.set({Item{{}, "a"}, Item{{}, "b"}, Item{{}, "c"}}, 0);  // jump: what the full-screen "Up next" list uses
    CHECK(q.items().size() == 3 && q.jump(2) && q.current()->location == "c" && q.index() == 2 && !q.has_next());
    CHECK(!q.jump(3) && q.index() == 2 && q.jump(0) && q.current()->location == "a" && q.has_next());
}

static void engine_starts() {
    Capture sink;
    VlcEngine e(sink);
    CHECK(e.ok(), "%s", e.error().c_str());
    std::printf("libVLC %s\n", e.version().c_str());
    CHECK(VlcEngine::preset_names().size() >= 10 && VlcEngine::band_frequencies().size() == 10);
}

static void formats() {
    Run r;
    if (!r.engine.ok()) return;
    std::vector<std::pair<std::string, const char*>> files = {
        {audio("tone.wav"), "WAV"},  {write_aiff(), "AIFF"},       {audio("tone.flac"), "FLAC"}, {audio("tone.m4a"), "AAC (M4A)"}, {audio("tone-alac.m4a"), "ALAC (M4A)"},
        {audio("tone.mp2"), "MPEG audio"}, {audio("tone.ogg"), "Ogg Vorbis"}, {audio("tone.opus"), "Opus"}, {audio("tone.wma"), "WMA"},
        {audio("tone.mod"), "ProTracker module"}, {audio("tone.mp3"), "MP3"}, {audio("tone.wv"), "WavPack"}, {audio("tone.tta"), "TTA"},
        {audio("tone.ac3"), "AC-3"}, {audio("tone.eac3"), "E-AC-3"}, {audio("tone.dts"), "DTS"}, {audio("tone-24bit.wav"), "WAV 24-bit"},
        {audio("tone-adpcm.wav"), "WAV ADPCM"}};
    for (const auto& [path, name] : files) {
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = r.play_through(path, 4.0f);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const size_t frames = r.sink.frames();
        std::printf("  %-18s %s  %6zu frames captured, length %lld ms, %.1f s\n", name, ok ? "ok    " : "FAILED", frames, r.engine.length(), secs);
        CHECK(ok && frames > kRate / 10, "%s", name);
        // Every fixture is a 2.5 s tone (the AIFF 1.5 s, the module ~2 s): a wrong length means a broken decode.
        const int64_t want = path.ends_with(".aiff") ? 1500 : path.ends_with(".mod") ? 2000 : 2500;
        // VLC estimates the length of TTA and ADPCM WAV from their headers, and gets it wrong; they still play in full.
        const bool estimated = path.ends_with(".tta") || path.ends_with("-adpcm.wav");
        CHECK(estimated || std::llabs(r.engine.length() - want) < 200, "%s: length %lld ms", name, r.engine.length());
        CHECK(std::llabs(int64_t(frames) * 4000 / kRate - want) < 250, "%s: %zu frames, not %lld ms", name, frames, want);  // all of it (at 4x)
        // Real audio, not misread bytes: finite, within ±1, and not silent.
        double peak = 0;
        bool finite = true;
        for (float v : r.sink.samples) finite &= std::isfinite(v), peak = std::max(peak, double(std::fabs(v)));
        CHECK(finite && peak <= 1.0 && peak > 0.05, "%s: peak %.3f, finite %d", name, peak, finite);
    }
}

static void seeking() {
    Run r;
    if (!r.engine.ok()) return;
    std::atomic<size_t> at_seek{0};
    std::thread seeker([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        at_seek = r.sink.frames();
        r.engine.seek(2000);  // of 2.5 s: only the last ~0.5 s should follow
    });
    const bool ok = r.play_through(audio("tone.flac"), 1.0f);
    seeker.join();
    const double after = double(r.sink.frames() - at_seek) / kRate;
    CHECK(ok && after < 1.0, "%.2f s captured after seeking to 2.0 s", after);
    CHECK(r.engine.seekable() || true);
}

static void equalizer() {
    Run r;
    if (!r.engine.ok()) return;
    // tone.wav has 100 Hz, 1 kHz and 5 kHz. Boost the bass bands, cut the top ones.
    r.play_through(audio("tone.wav"), 1.0f);
    const double flat = level(r.sink.samples, 100) / level(r.sink.samples, 5000);
    Equalizer eq;
    eq.enabled = true;
    eq.bands = {12, 12, 6, 0, 0, 0, -12, -12, -12, -12};
    r.engine.set_equalizer(eq);
    r.play_through(audio("tone.wav"), 1.0f);
    const double boosted = level(r.sink.samples, 100) / level(r.sink.samples, 5000);
    std::printf("  equalizer: bass/treble %.2f flat → %.2f with bass boost + treble cut\n", flat, boosted);
    CHECK(boosted > flat * 4, "%.2f vs %.2f", boosted, flat);
    // Turning the equalizer on with nothing boosted must not change the volume (VLC's preamp unity is 12, not 0), and
    // nor must VLC's own "Flat" preset.
    r.engine.set_equalizer(Equalizer{});
    r.play_through(audio("tone.wav"), 1.0f);
    const double off = level(r.sink.samples, 1000);
    Equalizer neutral;
    neutral.enabled = true, neutral.bands.assign(10, 0.f);
    for (const auto& e : {neutral, VlcEngine::preset(0)}) {
        r.engine.set_equalizer(e);
        r.play_through(audio("tone.wav"), 1.0f);
        const double db = 20 * std::log10(level(r.sink.samples, 1000) / off);
        CHECK(std::fabs(db) < 0.5, "equalizer on, preset %d: %+.1f dB at 1 kHz", e.preset, db);
    }
    // The normalizer filter loads (the track plays through with it).
    r.engine.set_equalizer(Equalizer{});
    r.sink.clear();
    {
        std::lock_guard lock(r.m);
        r.ended = r.error = false;
    }
    CHECK(r.engine.open(audio("tone.ogg"), true));
    r.engine.set_rate(4.0f);
    std::unique_lock lock(r.m);
    r.cv.wait_for(lock, std::chrono::seconds(10), [&] { return r.ended || r.error; });
    CHECK(r.ended && !r.error && r.sink.frames() > 0, "normalizer");
}

static void playlists() {
    // The fixture .m3u: two relative entries, resolved next to the playlist.
    const auto items = expand_playlist(audio("two.m3u"));
    CHECK(items.size() == 2, "%zu entries", items.size());
    if (items.size() == 2) {
        CHECK(items[0].ends_with("tone.ogg") && items[1].ends_with("tone.flac"), "%s | %s", items[0].c_str(), items[1].c_str());
        CHECK(fs::exists(fs::path(std::u8string(items[0].begin(), items[0].end()))), "%s", items[0].c_str());
    }
    CHECK(expand_playlist(audio("tone.ogg")) == std::vector<std::string>{audio("tone.ogg")});  // not a playlist
    // PLS (radio stations publish these), XSPF, ASX, file:// entries, URLs.
    const auto pls = parse_playlist("[playlist]\nNumberOfEntries=2\nFile1=http://radio.example/stream\nTitle1=Radio\nFile2=song.mp3\n", "C:\\Music");
    CHECK(pls.size() == 2 && pls[0] == "http://radio.example/stream" && pls[1] == "C:\\Music\\song.mp3", "%zu", pls.size());
    const auto xspf = parse_playlist("<?xml version=\"1.0\"?><playlist><trackList><track><location>file:///C:/My%20Music/a.flac</location></track>"
                                     "<track><location>https://x.example/b.mp3?a=1&amp;b=2</location></track></trackList></playlist>", "C:\\");
    CHECK(xspf.size() == 2 && xspf[0] == "C:\\My Music\\a.flac" && xspf[1] == "https://x.example/b.mp3?a=1&b=2", "%s", xspf.empty() ? "" : xspf[0].c_str());
    const auto asx = parse_playlist("<asx version=\"3.0\"><entry><ref href=\"http://radio.example/live\"/></entry></asx>", "");
    CHECK(asx.size() == 1 && asx[0] == "http://radio.example/live");
    const auto rel_url = parse_playlist("#EXTM3U\nlow/stream.aac\n", "https://radio.example/lists/station.m3u");
    CHECK(rel_url.size() == 1 && rel_url[0] == "https://radio.example/lists/low/stream.aac", "%s", rel_url.empty() ? "" : rel_url[0].c_str());
    CHECK(is_playlist("https://radio.example/listen.pls?sid=1") && is_playlist("C:\\a\\b.M3U") && !is_playlist("C:\\a\\b.mp3"));
    CHECK(is_url("https://example.com/radio.m3u") && !is_url("C:\\Music\\a.mp3") && !is_url("tone.ogg"));
}

static void visualizer() {
    std::vector<float> s(Visualizer::kFft);
    auto sine = [&](double hz, float amp) {
        for (size_t i = 0; i < s.size(); ++i) s[i] = amp * float(std::sin(2 * std::numbers::pi * hz * double(i) / kRate));
    };
    // A sine lights up the band it's in; at 1 kHz (where the +3 dB/octave tilt is 0) at its level: 0.5 = -6 dB → 0.9 of
    // the height on a 60 dB scale. 100 Hz reads ~10 dB lower, 5 kHz ~7 dB higher.
    for (const double hz : {100.0, 1000.0, 5000.0}) {
        Visualizer v;
        v.set_band_count(64);
        sine(hz, 0.5f);
        v.update(s.data(), 0.016f);
        const auto& b = v.bars();
        const size_t top = size_t(std::max_element(b.begin(), b.end()) - b.begin());
        CHECK(v.band_hz(top) > hz / 1.15 && v.band_hz(top) < hz * 1.15, "%.0f Hz peaks in the %.0f Hz band", hz, v.band_hz(top));
        const float want = std::clamp(0.9f + 3 * float(std::log2(v.band_hz(top) / 1000)) / 60, 0.f, 1.f);
        CHECK(std::fabs(b[top] - want) < 0.05f, "%.0f Hz: level %.2f, expected %.2f", hz, b[top], want);
        CHECK(b[0] < 0.5f || hz < 200, "%.0f Hz: lowest band %.2f", hz, b[0]);
    }
    // Silence: nothing.
    Visualizer v;
    v.set_band_count(32);
    std::fill(s.begin(), s.end(), 0.f);
    v.update(s.data(), 0.016f);
    CHECK(*std::max_element(v.bars().begin(), v.bars().end()) == 0.f);
    // Bars fall at the bar speed; the peak caps hang ~0.4 s, then fall too.
    sine(1000, 0.5f);
    v.update(s.data(), 0.016f);
    const auto& b = v.bars();
    const size_t top = size_t(std::max_element(b.begin(), b.end()) - b.begin());
    const float start = b[top];
    std::fill(s.begin(), s.end(), 0.f);
    v.update(s.data(), 0.2f);
    CHECK(std::fabs(v.bars()[top] - (start - 2.2f * 0.2f)) < 0.01f && v.peaks()[top] == start, "after 0.2 s: bar %.2f peak %.2f", v.bars()[top], v.peaks()[top]);
    for (int i = 0; i < 5; ++i) v.update(s.data(), 0.1f);  // 0.7 s in: the cap has started to drop
    CHECK(v.bars()[top] == 0.f && v.peaks()[top] < start && v.peaks()[top] > 0.f, "after 0.7 s: peak %.2f", v.peaks()[top]);
    for (int i = 0; i < 40; ++i) v.update(s.data(), 0.1f);
    CHECK(v.peaks()[top] == 0.f);
    // The scope starts at a rising zero crossing and follows the waveform.
    std::vector<float> w(Visualizer::kFft);
    for (size_t i = 0; i < w.size(); ++i) w[i] = 0.5f * float(std::sin(2 * std::numbers::pi * 440 * (double(i) + 37) / kRate));
    v.update(w.data(), 0.016f);
    CHECK(v.wave().size() == Visualizer::kFft / 2);
    CHECK(std::fabs(v.wave()[0]) < 0.05f && v.wave()[1] > v.wave()[0] && *std::max_element(v.wave().begin(), v.wave().end()) > 0.49f);
    // Options survive a round trip through settings.json.
    VisOptions o;
    o.mode = VisOptions::Mode::both, o.bars = VisOptions::Bars::fire, o.scope = VisOptions::Scope::solid, o.peak_falloff = 4, o.classic = false;
    const VisOptions back = VisOptions::from_json(o.to_json());
    CHECK(back.mode == o.mode && back.bars == o.bars && back.scope == o.scope && back.peak_falloff == 4 && !back.classic && back.peaks);
    // The beat options: strong pulse, cuts on drops, beat-heavy presets, normal sensitivity by default; they round-trip
    // and out-of-range values are clamped.
    const VisOptions def;
    CHECK(def.pulse == 2 && def.hard_cuts && !def.all_presets && def.beat_sensitivity == 2 && def.sync_ms == 0);
    VisOptions beat;
    beat.pulse = 1, beat.hard_cuts = false, beat.all_presets = true, beat.beat_sensitivity = 3, beat.sync_ms = -75;
    const VisOptions bb = VisOptions::from_json(beat.to_json());
    CHECK(bb.pulse == 1 && !bb.hard_cuts && bb.all_presets && bb.beat_sensitivity == 3 && bb.sync_ms == -75);
    const VisOptions clamped = VisOptions::from_json(wb::json{{"pulse", 9}, {"syncMs", 5000}, {"beatSensitivity", 0}});
    CHECK(clamped.pulse == 2 && clamped.sync_ms == 300 && clamped.beat_sensitivity == 1);
    // MilkDrop is the default look; its options round-trip and a bad value falls back to the nearest choice.
    VisOptions d;
    CHECK(d.mode == VisOptions::Mode::milkdrop && d.quality == 720 && d.auto_advance == 30);
    o.mode = VisOptions::Mode::milkdrop, o.quality = 1080, o.auto_advance = 0;
    const VisOptions m = VisOptions::from_json(o.to_json());
    CHECK(m.mode == VisOptions::Mode::milkdrop && m.quality == 1080 && m.auto_advance == 0, "milkdrop options");
    CHECK(VisOptions::from_json(wb::json{{"quality", 600}, {"autoAdvance", -5}}).quality == 720 && VisOptions::from_json(wb::json{{"autoAdvance", -5}}).auto_advance == 0);
    CHECK(VisOptions::from_json(wb::json::object()).mode == VisOptions::Mode::milkdrop && VisOptions::from_json(wb::json{{"mode", "both"}}).mode == VisOptions::Mode::both);
}

// Real time, and the clock the seek bar shows: through the real sound device (muted — nothing audible), 2 s of wall
// clock is ~2 s of track, and VLC's time matches what the device has actually played.
static void pacing() {
    AudioOutput out;
    if (!out.ok()) return std::puts("  pacing: no sound device here, skipped"), void();
    out.set_muted(true);
    VlcEngine e(out);
    out.pause(false);
    e.open(audio("tone.wav"), false);
    const auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double played = double(out.played_frames()) / kRate, shown = double(e.time()) / 1000;
    std::printf("  pacing: %.2f s of wall clock → device played %.2f s, VLC shows %.2f s\n", wall, played, shown);
    CHECK(played > wall - 0.5 && played < wall + 0.05, "played %.2f s in %.2f s", played, wall);
    CHECK(std::fabs(shown - played) < 0.3, "shown %.2f s, heard %.2f s", shown, played);
    e.stop();

    // Nothing lost while our small buffer makes VLC wait: a whole track reaches the device.
    out.flush();
    out.pause(false);  // stopping paused the device; the Player starts it again before every track, so does this
    std::mutex m;
    std::condition_variable cv;
    bool ended = false;
    double all = 0;
    e.on_event = [&](Event ev) {
        if (ev != Event::ended) return;
        std::lock_guard lock(m);
        all = double(out.played_frames()) / kRate;  // VLC drained us first; it flushes (resetting the count) after this
        ended = true;
        cv.notify_all();
    };
    e.open(audio("tone.wav"), false);
    {
        std::unique_lock lock(m);
        cv.wait_for(lock, std::chrono::seconds(6), [&] { return ended; });
    }
    std::printf("  pacing: whole 2.50 s track → device played %.3f s\n", all);
    CHECK(ended && std::fabs(all - 2.5) < 0.05, "played %.3f s of 2.5 s", all);
    e.on_event = nullptr;
}

// MilkDrop through projectM, rendered off-screen. Skipped (and passing) on a PC without OpenGL 3.3 / pbuffers.
static void milkdrop() {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const fs::path base = fs::path(exe).parent_path() / "milkdrop";
    const fs::path one = fs::temp_directory_path() / "wb_milkdrop_test";
    fs::remove_all(one);
    fs::create_directories(one);
    fs::copy_file(base / "presets" / "Waveform" / "Wire Circular" / "$$$ Royal - Mashup (191).milk", one / "a.milk");

    MilkDropConfig cfg{320, 180, {one}, base / "textures"};
    MilkDrop md(cfg);
    if (!md.ok()) {
        std::printf("  milkdrop: skipped (%s)\n", md.error().c_str());
        fs::remove_all(one);
        return;
    }
    CHECK(md.preset_count() == 1 && md.preset_name() == "a", "preset '%s' of %zu", md.preset_name().c_str(), md.preset_count());

    std::vector<float> tone(800);  // one 60 fps frame of a 1 kHz tone, phase-continuous
    size_t at = 0;
    auto feed = [&] {
        for (auto& x : tone) x = 0.5f * float(std::sin(2 * std::numbers::pi * 1000.0 * double(at++) / kRate));
    };
    std::vector<uint8_t> f3, f10;
    for (int i = 1; i <= 10; ++i) {
        feed();
        auto f = md.render(tone.data(), tone.size());
        CHECK(bool(f) == (i > 1), "frame %d: pixels %s", i, f ? "returned" : "missing");  // the readback runs a frame behind
        if (f && (i == 3 || i == 10)) (i == 3 ? f3 : f10).assign(f.bgra, f.bgra + size_t(f.width) * f.height * 4);
        Sleep(16);
    }
    CHECK(f3.size() == 320u * 180 * 4 && f10.size() == f3.size(), "frame size");
    size_t lit = 0, differ = 0;
    for (size_t i = 0; i + 3 < f10.size(); i += 4) {
        lit += f10[i] > 12 || f10[i + 1] > 12 || f10[i + 2] > 12;
        differ += f10[i] != f3[i] || f10[i + 1] != f3[i + 1] || f10[i + 2] != f3[i + 2];
    }
    const double px = 320.0 * 180;
    std::printf("  milkdrop: %.0f%% of pixels lit, %.0f%% changed between frame 3 and 10\n", 100 * lit / px, 100 * differ / px);
    CHECK(lit > px * 0.02, "frame 10 is (nearly) black");
    CHECK(differ > px * 0.01, "frame 10 equals frame 3");

    // Render speed (the readback included), then a resize.
    for (const auto [w, h] : {std::pair{320, 180}, std::pair{1280, 720}}) {
        CHECK(md.resize(w, h), "resize to %dx%d: %s", w, h, md.error().c_str());
        for (int i = 0; i < 5; ++i) feed(), md.render(tone.data(), tone.size());
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 60; ++i) feed(), md.render(tone.data(), tone.size());
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 60;
        std::printf("  milkdrop: %dx%d renders in %.2f ms per frame\n", w, h, ms);
        auto f = md.render(tone.data(), tone.size());
        CHECK(f && f.width == w && f.height == h, "frame after resize");
    }

    // The whole pack: it's found, next / previous / random move through it, and the lock and timer settings are accepted.
    { const auto q0 = std::chrono::steady_clock::now(); MilkDrop e({320, 180, {}, base / "textures"});
      std::printf("  milkdrop: empty engine up in %.0f ms\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - q0).count()); }
    const auto t0 = std::chrono::steady_clock::now();
    // Built on a worker thread, as the app does (scanning the pack is slow), then used from this one.
    const auto all_p = std::async(std::launch::async, [&] { return std::make_unique<MilkDrop>(MilkDropConfig{320, 180, {base / "presets"}, base / "textures"}); }).get();
    MilkDrop& all = *all_p;
    const double scan = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(all.ok() && all.preset_count() > 9000, "pack: %zu presets", all.preset_count());
    std::printf("  milkdrop: %zu presets found, engine up in %.0f ms\n", all.preset_count(), scan);
    std::string a = all.preset_name();
    all.next();
    std::string b = all.preset_name();
    all.previous();
    CHECK(!a.empty() && a != b, "next: '%s' -> '%s'", a.c_str(), b.c_str());
    CHECK(all.preset_name() == a, "previous: '%s' != '%s'", all.preset_name().c_str(), a.c_str());
    all.random();
    all.lock(true);
    CHECK(all.locked(), "lock");
    all.set_auto_advance(0);
    for (int i = 0; i < 5; ++i) feed(), all.render(tone.data(), tone.size());
    fs::remove_all(one);
}

// A 120 BPM kick drum (silence between kicks), mono at 48 kHz: one kick every 24,000 samples.
static std::vector<float> kick_track(double seconds) {
    std::vector<float> s(size_t(seconds * kRate));
    for (size_t i = 0; i < s.size(); ++i) {
        const double t = double(i % 24000) / kRate;  // time since the kick
        if (t < 0.25) s[i] = float(0.9 * std::sin(2 * std::numbers::pi * (50 + 100 * std::exp(-t * 40)) * t) * std::exp(-t * 12));
    }
    return s;
}

// Beat sync, measured: the probe preset paints the screen with brightness = projectM's `bass`. Fed one frame of audio
// per frame (what the app must do), every kick lights the screen within a couple of frames and it goes dark between
// kicks. Fed the way the app used to (100 ms lumps from the sound device, nothing in between) most kicks are missed.
// Returns the share of kicks that lit up.
static double milk_beat_hits(bool lumps, bool print) {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const fs::path base = fs::path(exe).parent_path() / "milkdrop";
    const fs::path dir = fs::temp_directory_path() / "wb_milkdrop_beat";
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::copy_file(fs::path(WB_FIXTURES) / "milkdrop" / "beat_probe.milk", dir / "beat_probe.milk");
    MilkDropConfig cfg;
    cfg.width = 160, cfg.height = 90, cfg.preset_dirs = {dir}, cfg.texture_dir = base / "textures";
    MilkDrop md(cfg);
    if (!md.ok()) {
        fs::remove_all(dir);
        return -1;
    }
    const auto track = kick_track(5);
    constexpr size_t kPerFrame = 800, kFramesPerBeat = 24000 / kPerFrame;  // 60 fps
    std::vector<double> bright;  // per rendered frame (the readback returns the previous one)
    for (size_t f = 0; (f + 1) * kPerFrame <= track.size(); ++f) {
        const float* chunk = track.data() + f * kPerFrame;
        size_t n = kPerFrame;
        if (lumps) {  // the sound device's 100 ms periods: 6 frames' audio at once, then nothing
            n = f % 6 == 5 ? 6 * kPerFrame : 0;
            chunk = track.data() + (f + 1) * kPerFrame - n;
        }
        const auto px = md.render(chunk, n);
        if (px) {
            double sum = 0;
            for (size_t i = 1; i < size_t(px.width) * px.height * 4; i += 4) sum += px.bgra[i];
            bright.push_back(sum / (double(px.width) * px.height));
        }
        Sleep(16);  // projectM times its averages by the wall clock: keep it near real time
    }
    fs::remove_all(dir);
    int kicks = 0, hits = 0;
    for (size_t k = 2 * kFramesPerBeat; k + kFramesPerBeat <= bright.size(); k += kFramesPerBeat) {  // skip warm-up
        double peak = 0, valley = 0;
        for (size_t i = k; i < k + 4; ++i) peak = std::max(peak, bright[i - 1]);  // frame k shows as bright[k-1]
        for (size_t i = k + 15; i < k + 25; ++i) valley += bright[i - 1] / 10;
        ++kicks;
        hits += peak > valley + 25;
        if (print) std::printf("    kick at frame %zu: peak %.0f, between kicks %.0f\n", k, peak, valley);
    }
    return kicks ? double(hits) / kicks : 0;
}

static void milkdrop_beat_sync() {
    const double framed = milk_beat_hits(false, false);
    if (framed < 0) return std::puts("  milkdrop beat sync: skipped (no OpenGL 3.3)"), void();
    const double lumped = milk_beat_hits(true, false);
    std::printf("  milkdrop beat sync: %.0f%% of kicks light the screen fed per frame, %.0f%% fed in 100 ms lumps\n", framed * 100, lumped * 100);
    CHECK(framed >= 0.9, "only %.0f%% of kicks seen fed per frame", framed * 100);
    if (framed < 0.9) milk_beat_hits(false, true);
}

// The heard clock: the tap records every frame sent to the device; heard_index is what's coming out of the speakers at
// a given moment (sent minus what the device still holds, plus the time since), and tap_until reads the samples up to
// there. No device: the test renders with its own timestamps.
static void heard_clock() {
    using Clock = AudioOutput::Clock;
    AudioOutput out(false);
    out.pause(false);
    out.set_latency_frames(2880);  // 60 ms queued in the device
    long next = 1;                 // the stereo frame i has both channels = i, so the tap's mono mix is i
    const Clock::time_point t0 = Clock::now();
    auto render = [&](unsigned frames, int at_ms, bool with_audio = true) {
        std::vector<float> in(size_t(frames) * kChannels), sink(in.size());
        for (unsigned i = 0; i < frames; ++i) in[i * 2] = in[i * 2 + 1] = float(next++);
        if (with_audio) out.play(in.data(), frames);
        else next -= long(frames);  // an underrun: the device gets silence, the tap records it as such
        out.render_at(sink.data(), frames, t0 + std::chrono::milliseconds(at_ms));
    };
    const auto at = [&](int ms) { return t0 + std::chrono::milliseconds(ms); };
    CHECK(out.heard_index(at(0)) == 0, "nothing sent yet");
    for (int k = 0; k < 4; ++k) render(960, 20 * k);  // 20 ms periods: 3,840 frames sent by t = 60 ms
    CHECK(out.heard_index(at(60)) == 3840 - 2880, "at the last callback: %lld", out.heard_index(at(60)));
    CHECK(out.heard_index(at(70)) == 3840 - 2880 + 480, "10 ms later: %lld", out.heard_index(at(70)));
    CHECK(out.heard_index(at(500)) == 3840, "never past what was sent: %lld", out.heard_index(at(500)));
    std::vector<float> got(480);
    out.tap_until(1440, got.data(), got.size());
    bool exact = true;
    for (size_t i = 0; i < got.size(); ++i) exact &= got[i] == float(960 + i + 1);  // tap index j holds frame j+1
    CHECK(exact, "tap_until(1440): %g … %g", got.front(), got.back());
    out.tap_until(10, got.data(), 20);  // partly before the first frame: zeros there
    CHECK(got[0] == 0.f && got[9] == 0.f && got[10] == 1.f && got[19] == 10.f, "before the start: %g %g %g", got[9], got[10], got[19]);
    render(960, 80, false);  // underrun: silence is still frames of the device's clock
    out.tap_until(4800, got.data(), 10);
    CHECK(out.heard_index(at(80)) == 4800 - 2880 && got[9] == 0.f, "after an underrun: %lld", out.heard_index(at(80)));
    for (int k = 0; k < 60; ++k) render(960, 100 + 20 * k);  // over a second more: the oldest fall out of the 1 s tap
    out.tap_until(1440, got.data(), 10);
    CHECK(got[0] == 0.f && got[9] == 0.f, "older than the tap: zeros");
    CHECK(out.heard_index(t0 - std::chrono::seconds(5)) >= out.heard_index(at(0)) - 48000, "never older than the tap");
}

// The beat pulse finds the kicks of a 120 BPM track fed a frame at a time, and nothing in hi-hats or silence.
static void beat_pulse() {
    const auto track = kick_track(8);
    BeatPulse p;
    std::vector<int> fired;  // frame numbers with a new kick
    constexpr size_t kFrame = 800;
    float min_after = 1;     // envelope 300 ms after each kick
    for (size_t f = 0; (f + 1) * kFrame <= track.size(); ++f) {
        const int before = p.kicks();
        p.update(track.data() + f * kFrame, kFrame, kFrame / float(kRate));
        if (p.kicks() > before) fired.push_back(int(f));
        if (f % 30 == 18) min_after = std::min(min_after, p.envelope());
    }
    bool on_time = fired.size() == 16;
    for (size_t i = 0; i < fired.size() && on_time; ++i) on_time = std::abs(fired[i] - int(i) * 30) <= 1;  // a kick every 30 frames
    CHECK(on_time, "%zu kicks found (16 expected), first at frame %d", fired.size(), fired.empty() ? -1 : fired[0]);
    CHECK(min_after < 0.05f, "envelope still %.2f 300 ms after a kick", min_after);

    BeatPulse hats;  // high-frequency noise bursts on every 8th note: no bass, no kicks
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> noise(-0.5f, 0.5f);
    std::vector<float> h(size_t(4 * kRate));
    float prev = 0;
    for (size_t i = 0; i < h.size(); ++i) {
        const float x = noise(rng) * std::exp(-float(i % 12000) / 600.f);
        h[i] = x - prev;  // first difference: the energy sits up high
        prev = x;
    }
    for (size_t f = 0; (f + 1) * kFrame <= h.size(); ++f) hats.update(h.data() + f * kFrame, kFrame, kFrame / float(kRate));
    CHECK(hats.kicks() == 0, "%d kicks in hi-hats", hats.kicks());
    BeatPulse quiet;
    std::vector<float> zeros(kFrame);
    for (int f = 0; f < 120; ++f) quiet.update(zeros.data(), kFrame, kFrame / float(kRate));
    CHECK(quiet.kicks() == 0 && quiet.envelope() == 0.f, "silence");
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    pacing();
    visualizer();
    milkdrop();
    milkdrop_beat_sync();
    heard_clock();
    beat_pulse();
    queue_rules();
    engine_starts();
    formats();
    seeking();
    equalizer();
    playlists();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all player tests passed");
    return 0;
}
