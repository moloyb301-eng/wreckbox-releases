// DasVis tests: the live audio features on synthetic drum tracks, the flash limiter against a strobe, the quality
// controller on fake slow and fast GPUs, and every scene rendered on WARP (Windows' software GPU, so it runs on any
// PC and in CI), at full Direct3D 11 and limited to feature level 10_0 (old graphics cards).
#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <random>
#include <vector>

#include "dasvis/control.h"
#include "dasvis/dasvis.h"
#include "dasvis/features.h"

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
    CHECK(strong.tier == Tier::high && strong.scale == 1, "strong GPU: %s at %.2f", tier_name(strong.tier), strong.scale);
    const Sim middling = simulate(25, Tier::medium);
    CHECK(middling.ms <= 13 * 0.9f, "middling GPU: %.1f ms", middling.ms);
    for (const Sim& s : {weak, strong, middling}) CHECK(s.tier_changes <= 2, "%d tier changes (see-sawing)", s.tier_changes);
    const Sim fixed = simulate(80, Tier::high, false);
    CHECK(fixed.tier == Tier::high && fixed.scale == tier_spec(Tier::high).scale_min, "a fixed tier only moves the scale");
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
    cfg.start = Tier::medium, cfg.auto_tier = false, cfg.auto_advance = 0, cfg.user_dir = dir;
    DasVis dv(dev.Get(), cfg);
    CHECK(dv.ok(), "DasVis on %s: %s", label, dv.error().c_str());
    if (!dv.ok()) return;
    CHECK(dv.scene_count() == 4, "scenes: %zu (3 built in + the example)", dv.scene_count());
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
    for (size_t s = 0; s < dv.scene_count(); ++s) {
        if (s) dv.next();
        for (int i = 0; i < 60; ++i) frame();  // the transition is over
        std::vector<uint8_t> a, b;
        const Stats one = read(dev.Get(), screen.Get(), &a);
        for (int i = 0; i < 20; ++i) frame();
        const Stats two = read(dev.Get(), screen.Get(), &b);
        const std::string name = dv.scene_name();
        CHECK(one.mean > 0.01 && one.mean < 0.95, "%s on %s: mean %.3f (black or white)", name.c_str(), label, one.mean);
        CHECK(diff(a, b) > 0.002, "%s on %s: the picture doesn't move (%.4f)", name.c_str(), label, diff(a, b));
        std::printf("  %s on %s: mean %.3f, moving %.3f\n", name.c_str(), label, two.mean, diff(a, b));
    }
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

int main() {
    SetConsoleOutputCP(CP_UTF8);
    features();
    limiter();
    quality();
    gpu(false);
    gpu(true);
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all DasVis tests passed");
    return 0;
}
