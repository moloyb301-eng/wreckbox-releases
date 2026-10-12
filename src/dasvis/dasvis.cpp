#include <windows.h>

#include "dasvis/dasvis.h"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

#include "dasvis/director.h"
#include "dasvis_gen/library.h"
#include "dasvis_gen/ps_aurora.h"
#include "dasvis_gen/ps_bloom_down.h"
#include "dasvis_gen/ps_bloom_up.h"
#include "dasvis_gen/ps_chrome.h"
#include "dasvis_gen/ps_copy.h"
#include "dasvis_gen/ps_coral.h"
#include "dasvis_gen/ps_coral_step.h"
#include "dasvis_gen/ps_feedback.h"
#include "dasvis_gen/ps_final.h"
#include "dasvis_gen/ps_galaxy.h"
#include "dasvis_gen/ps_hyperspace.h"
#include "dasvis_gen/ps_ink.h"
#include "dasvis_gen/ps_ink_divergence.h"
#include "dasvis_gen/ps_ink_gradient.h"
#include "dasvis_gen/ps_ink_jacobi.h"
#include "dasvis_gen/ps_ink_velocity.h"
#include "dasvis_gen/ps_kaleido.h"
#include "dasvis_gen/ps_lum.h"
#include "dasvis_gen/ps_mandelbox.h"
#include "dasvis_gen/ps_particles.h"
#include "dasvis_gen/ps_particles_update.h"
#include "dasvis_gen/ps_ribbon.h"
#include "dasvis_gen/ps_ridge.h"
#include "dasvis_gen/ps_ridges.h"
#include "dasvis_gen/ps_star.h"
#include "dasvis_gen/ps_storm.h"
#include "dasvis_gen/ps_milkdrop.h"
#include "dasvis_gen/ps_synthwave.h"
#include "dasvis_gen/ps_tunnel.h"
#include "dasvis_gen/vs_fullscreen.h"
#include "dasvis_gen/vs_particles.h"
#include "dasvis_gen/vs_ribbon.h"
#include "dasvis_gen/vs_ridge.h"
#include "dasvis_gen/vs_star.h"

namespace wb::dasvis {

using Microsoft::WRL::ComPtr;

namespace {

// Mirrors the cbuffer in dasvis.hlsli: float4s only, so the C++ and HLSL layouts can't drift apart.
struct Uniforms {
    float time[4], res[4], out[4], beat[4], drums[4], tone[4], look[4], post[4], key[4], pal[5][4], pass[4], song[4], drop[4], hits[4], sim[4], dir[4], drift[4], ext[4], title[4], deck[4], lyric[4];
};
static_assert(sizeof(Uniforms) % 16 == 0);

struct Target {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    int w = 0, h = 0;
};

// A scene's simulation, run by pixel shaders on textures (feature level 10_0 has no compute shaders), sized by the tier.
enum class Sim {
    none,
    particles,  // the particle layer (Feedback Classic): position and velocity in a texture, drawn as points
    fluid,      // Ink Ocean: stable fluids
    coral,      // Coral: reaction-diffusion
    galaxy,     // Galaxy: stars drawn as points, worked out from the time (nothing stored)
    ridges,     // Ridge Lines: the spectrum's history, drawn as lines with curtains
    milkdrop,   // MilkDrop: nothing to run here; the app's MilkDrop picture is the scene's texture
};

struct Scene {
    std::string name;
    ComPtr<ID3D11PixelShader> ps;
    Sim sim = Sim::none;
};

struct BuiltIn {
    const BYTE* code;
    size_t size;
    Sim sim;
    SceneTags tags;  // name, energy, cost class, hero, takes the particle layer, suits the ring
};
const BuiltIn kScenes[] = {
    {k_ps_feedback, sizeof k_ps_feedback, Sim::none, {"Feedback Classic", kMid | kPeak, 'L', false, true, false}},
    {k_ps_tunnel, sizeof k_ps_tunnel, Sim::none, {"Neon Tunnel", kMid | kPeak, 'L', true, true, true}},
    {k_ps_kaleido, sizeof k_ps_kaleido, Sim::none, {"Kaleido Cover", kCalm | kMid, 'L', false, true, false}},
    {k_ps_ink, sizeof k_ps_ink, Sim::fluid, {"Ink Ocean", kCalm | kMid, 'M', false, false, false}},
    {k_ps_ridges, sizeof k_ps_ridges, Sim::ridges, {"Ridge Lines", kCalm | kMid, 'L', false, false, false}},
    {k_ps_coral, sizeof k_ps_coral, Sim::coral, {"Coral", kCalm | kMid, 'M', false, false, false}},
    {k_ps_galaxy, sizeof k_ps_galaxy, Sim::galaxy, {"Galaxy", kMid | kPeak, 'M', true, false, false}},
    {k_ps_synthwave, sizeof k_ps_synthwave, Sim::none, {"Synthwave", kMid | kPeak, 'L', false, true, false}},
    {k_ps_hyperspace, sizeof k_ps_hyperspace, Sim::none, {"Hyperspace", kMid | kPeak, 'L', true, false, true}},
    {k_ps_aurora, sizeof k_ps_aurora, Sim::none, {"Aurora", kCalm, 'M', false, false, false}},
    {k_ps_mandelbox, sizeof k_ps_mandelbox, Sim::none, {"Mandelbox Flight", kMid | kPeak, 'H', true, false, false}},
    {k_ps_chrome, sizeof k_ps_chrome, Sim::none, {"Liquid Chrome", kMid | kPeak, 'H', false, false, false}},
    {k_ps_storm, sizeof k_ps_storm, Sim::none, {"Storm", kMid | kPeak, 'H', false, false, false}},
};
// Only with Config::milkdrop, after the others (the app runs projectM for it).
const BuiltIn kMilkDrop{k_ps_milkdrop, sizeof k_ps_milkdrop, Sim::milkdrop, {"MilkDrop", kMid | kPeak, 'M', false, true, true}};
const BuiltIn& builtin_at(size_t i) { return i < std::size(kScenes) ? kScenes[i] : kMilkDrop; }

// The simulations' grids by tier, in cells tall: no bigger, as an integrated GPU's textures come out of the PC's RAM
// (512 would cost ~10 MB more); the detail on screen comes from the picture drawn at full size over them.
constexpr int kGrid[] = {128, 192, 256};
constexpr int kHistory = 128;     // Ridge Lines: rows of spectrum history (128 KB)
constexpr double kRowRate = 30;   // rows a second: 4.3 s of history

constexpr float kIntensity[] = {0.5f, 1.f, 1.6f};

uint32_t fnv1a(const std::string& s) {
    uint32_t h = 2166136261u;
    for (const unsigned char c : s) h = (h ^ c) * 16777619u;
    return h;
}

}  // namespace

Drive drive(const Features& live, const SongMap* map, double t0, double t1, const Drive& last, float dt) {
    Drive d;
    if (map && map->beats.size() >= 16) {
        const SongMap::State s = map->at(t1);
        d.kick = std::max(s.stems[SongMap::kick], 0.6f * live.kick);
        d.snare = std::max(s.stems[SongMap::snare], 0.6f * live.snare);
        d.hats = std::max(s.stems[SongMap::hats], 0.6f * live.hats);
        d.bass = s.stems[SongMap::bass];
        d.lead = s.stems[SongMap::lead];
        const bool on = t1 > t0 && t1 - t0 < 0.25;  // playing on, not a seek (which would fire every hit it jumped over)
        d.kick_hit = on && map->hits_between(SongMap::kick, t0, t1) > 0;
        d.snare_hit = on && map->hits_between(SongMap::snare, t0, t1) > 0;
        d.hats_hit = on && map->hits_between(SongMap::hats, t0, t1) > 0;
        const auto& kh = map->hits[SongMap::kick];
        const auto it = std::upper_bound(kh.begin(), kh.end(), t1);
        d.since_kick = it == kh.begin() ? 10.f : float(t1 - *(it - 1));
    } else {
        d.kick = live.kick, d.snare = live.snare, d.hats = live.hats, d.bass = live.bass, d.lead = live.lead;
        d.kick_hit = live.kick_hit, d.snare_hit = live.snare_hit, d.hats_hit = live.hats_hit;
        d.since_kick = live.kick_hit ? 0.f : last.since_kick + dt;
    }
    d.since_kick = std::min(d.since_kick, 10.f);
    return d;
}

struct DasVis::Impl {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    Config cfg;
    std::string error, user_errors;
    DXGI_FORMAT hdr = DXGI_FORMAT_R11G11B10_FLOAT;

    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps_down, ps_up, ps_lum, ps_final, ps_copy;
    ComPtr<ID3D11PixelShader> ps_velocity, ps_divergence, ps_jacobi, ps_gradient, ps_coral_step, ps_update, ps_point, ps_star, ps_ridge;
    ComPtr<ID3D11VertexShader> vs_point, vs_star, vs_ridge, vs_ribbon;
    ComPtr<ID3D11PixelShader> ps_ribbon;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11SamplerState> clamp, wrap;
    ComPtr<ID3D11BlendState> add;
    ComPtr<ID3D11RasterizerState> raster;

    std::vector<Scene> scenes;
    size_t builtin = 0, current = 0, previous = 0;
    bool locked = false, advance_due = false;
    double since_switch = 0, last_bar = 0;
    float transition = 1;
    // The director's choice on screen: the scene, its transition, its layers.
    Director dir;
    Cue cue;
    Moment now;            // what the director knows this frame (for N / P and skips)
    long last_part = -1;   // the song map's part on screen (-1: none yet, adopted without a change)
    int bars_on = 0;       // bars since the scene changed
    uint32_t picks = 0;    // picks this song
    uint32_t song_seed = 0;
    float ring_from = 0;   // the ring's mix when the last change began
    float drift_seed[4][2] = {};  // the song's drift waves (periods and phases, 0–1)

    Target comp[2], lum, spec, wave, cover, title;
    Target milk;  // the MilkDrop scene's picture, while it's on screen
    double travel = 0;  // TRAVEL: how far the flights have gone
    // The phone deck: the camera (target, eased), a shockwave's clock, a drop called by hand (seconds since, its events).
    float cam_x = 0, cam_y = 0, cam_tx = 0, cam_ty = 0, shock = 0;
    double manual_drop = -1;
    uint8_t manual_events = 0;
    float kicks = 0;    // kicks so far (the sigils' random numbers)
    // The current scene's simulation: made when the scene comes on screen, at its tier's size, and dropped when it goes,
    // so a PC only ever holds the one on screen.
    struct SimState {
        Sim kind = Sim::none;
        Tier tier = Tier::low;
        int w = 0, h = 0;   // the grid, in cells
        Target a[2], b[2];  // the state, back and forth (Ridge Lines: a[0], the history); the fluid's pressure
        int ai = 0, bi = 0;
        uint32_t frames = 0;
        int row = 0;        // Ridge Lines: the newest row
        double row_t = 0;   // and how long ago it was written
    } sim, layer;  // the scene's own, and the particle layer over it
    DXGI_FORMAT fine = DXGI_FORMAT_R32G32_FLOAT;  // the reaction-diffusion (16-bit floats are too coarse for it, if the
                                                  // card can't filter 32-bit ones)
    int out_w = 0, out_h = 0;
    std::vector<Target> bloom;
    int cur = 0;
    bool has_cover = false;
    ComPtr<ID3D11Texture2D> lum_stage[3];
    struct Timer {
        ComPtr<ID3D11Query> disjoint, t0, t1;
        bool issued = false;
    } timers[4];
    uint64_t frame = 0;

    QualityControl qc;
    FlashLimiter limiter;
    // The song map's clock: an offset learnt from the live kicks, and a whole number of beats added so the beat count
    // carries on from the live clock without jumping when the map arrives.
    double map_offset = 0, kick_drift = 0, beats_shift = 0;
    bool had_map = false;
    float gpu_ms = 0;
    double time = 0;
    std::string song, camelot;
    float seed = 0;
    Uniforms u{}, sent{};  // `sent`: what the GPU has
    bool uploaded = false;
    Drive drv;
    double last_t = 0;
    // The palette: the cover's when it has colours, else the key's; the one on screen fades towards it.
    std::optional<Palette> cover_pal;
    Palette pal_target{}, pal{};
    bool pal_set = false;

    explicit Impl(const Config& c) : cfg(c), qc(c.start, c.auto_tier, c.max_tier), limiter(c.reduce_flash ? 1 : 3) { dir.set_costs(c.costs); }

    bool fail(const char* what, HRESULT hr) {
        char buf[160];
        snprintf(buf, sizeof buf, "%s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
        error = buf;
        return false;
    }

    Target make(int w, int h, DXGI_FORMAT f, bool render = true, UINT mips = 1) {
        Target t;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = UINT(std::max(1, w)), d.Height = UINT(std::max(1, h));
        d.MipLevels = mips, d.ArraySize = 1, d.Format = f, d.SampleDesc.Count = 1, d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (render ? D3D11_BIND_RENDER_TARGET : 0);
        d.MiscFlags = mips != 1 ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &t.tex)) || FAILED(dev->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv)) ||
            (render && FAILED(dev->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv))))
            return {};
        t.w = int(d.Width), t.h = int(d.Height);
        return t;
    }

    bool init(ID3D11Device* device) {
        dev = device;
        dev->GetImmediateContext(&ctx);
        UINT support = 0;
        const UINT need = D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE | D3D11_FORMAT_SUPPORT_BLENDABLE;
        if (FAILED(dev->CheckFormatSupport(hdr, &support)) || (support & need) != need) hdr = DXGI_FORMAT_R16G16B16A16_FLOAT;
        HRESULT hr;
        if (FAILED(hr = dev->CreateVertexShader(k_vs_fullscreen, sizeof k_vs_fullscreen, nullptr, &vs))) return fail("vertex shader", hr);
        const std::pair<const BYTE*, size_t> post[] = {{k_ps_bloom_down, sizeof k_ps_bloom_down}, {k_ps_bloom_up, sizeof k_ps_bloom_up},
                                                       {k_ps_lum, sizeof k_ps_lum}, {k_ps_final, sizeof k_ps_final}, {k_ps_copy, sizeof k_ps_copy}};
        ComPtr<ID3D11PixelShader>* slots[] = {&ps_down, &ps_up, &ps_lum, &ps_final, &ps_copy};
        for (size_t i = 0; i < std::size(post); ++i)
            if (FAILED(hr = dev->CreatePixelShader(post[i].first, post[i].second, nullptr, &*slots[i]))) return fail("post shader", hr);
        const std::pair<const BYTE*, size_t> sims[] = {
            {k_ps_ink_velocity, sizeof k_ps_ink_velocity}, {k_ps_ink_divergence, sizeof k_ps_ink_divergence}, {k_ps_ink_jacobi, sizeof k_ps_ink_jacobi},
            {k_ps_ink_gradient, sizeof k_ps_ink_gradient}, {k_ps_coral_step, sizeof k_ps_coral_step}, {k_ps_particles_update, sizeof k_ps_particles_update},
            {k_ps_particles, sizeof k_ps_particles},       {k_ps_star, sizeof k_ps_star},               {k_ps_ridge, sizeof k_ps_ridge}};
        ComPtr<ID3D11PixelShader>* sim_slots[] = {&ps_velocity, &ps_divergence, &ps_jacobi, &ps_gradient, &ps_coral_step, &ps_update, &ps_point, &ps_star, &ps_ridge};
        for (size_t i = 0; i < std::size(sims); ++i)
            if (FAILED(hr = dev->CreatePixelShader(sims[i].first, sims[i].second, nullptr, &*sim_slots[i]))) return fail("simulation shader", hr);
        if (FAILED(hr = dev->CreateVertexShader(k_vs_particles, sizeof k_vs_particles, nullptr, &vs_point)) ||
            FAILED(hr = dev->CreateVertexShader(k_vs_star, sizeof k_vs_star, nullptr, &vs_star)) ||
            FAILED(hr = dev->CreateVertexShader(k_vs_ridge, sizeof k_vs_ridge, nullptr, &vs_ridge)) ||
            FAILED(hr = dev->CreateVertexShader(k_vs_ribbon, sizeof k_vs_ribbon, nullptr, &vs_ribbon)) ||
            FAILED(hr = dev->CreatePixelShader(k_ps_ribbon, sizeof k_ps_ribbon, nullptr, &ps_ribbon)))
            return fail("vertex shader", hr);
        if (FAILED(dev->CheckFormatSupport(fine, &support)) || !(support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE)) fine = DXGI_FORMAT_R16G16_FLOAT;
        // The heavy scenes are compiled by the driver only when they're first shown on Low tier, where the director
        // doesn't pick them (they measure over the budget there): ~1 MB less on the PCs that need it most. Elsewhere,
        // up front, so a pick never waits for the driver.
        for (const auto& s : kScenes) {
            Scene sc{s.tags.name, nullptr, s.sim};
            if ((s.tags.cost != 'H' || cfg.start != Tier::low) && FAILED(hr = dev->CreatePixelShader(s.code, s.size, nullptr, &sc.ps)))
                return fail("scene shader", hr);
            scenes.push_back(std::move(sc));
        }
        if (cfg.milkdrop) scenes.push_back({kMilkDrop.tags.name, nullptr, Sim::milkdrop});  // compiled when first shown
        builtin = scenes.size();

        D3D11_BUFFER_DESC bd{sizeof(Uniforms), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE};
        if (FAILED(hr = dev->CreateBuffer(&bd, nullptr, &cb))) return fail("constant buffer", hr);
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(hr = dev->CreateSamplerState(&sd, &clamp))) return fail("sampler", hr);
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_MIRROR;
        if (FAILED(hr = dev->CreateSamplerState(&sd, &wrap))) return fail("sampler", hr);
        D3D11_BLEND_DESC blend{};
        auto& rt = blend.RenderTarget[0];
        rt.BlendEnable = TRUE, rt.SrcBlend = rt.DestBlend = rt.SrcBlendAlpha = rt.DestBlendAlpha = D3D11_BLEND_ONE;
        rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD, rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(hr = dev->CreateBlendState(&blend, &add))) return fail("blend state", hr);
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID, rd.CullMode = D3D11_CULL_NONE, rd.DepthClipEnable = TRUE;
        if (FAILED(hr = dev->CreateRasterizerState(&rd, &raster))) return fail("rasterizer state", hr);

        spec = make(int(Features::kBins), 1, DXGI_FORMAT_R32G32_FLOAT, false);
        wave = make(int(Features::kWave), 1, DXGI_FORMAT_R32_FLOAT, false);
        lum = make(16, 16, DXGI_FORMAT_R8_UNORM);
        if (!spec.tex || !wave.tex || !lum.tex) return fail("textures", E_FAIL);
        D3D11_TEXTURE2D_DESC sdesc{};
        lum.tex->GetDesc(&sdesc);
        sdesc.Usage = D3D11_USAGE_STAGING, sdesc.BindFlags = 0, sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto& s : lum_stage)
            if (FAILED(hr = dev->CreateTexture2D(&sdesc, nullptr, &s))) return fail("staging texture", hr);
        D3D11_QUERY_DESC dq{D3D11_QUERY_TIMESTAMP_DISJOINT}, tq{D3D11_QUERY_TIMESTAMP};
        for (auto& t : timers)
            if (FAILED(dev->CreateQuery(&dq, &t.disjoint)) || FAILED(dev->CreateQuery(&tq, &t.t0)) || FAILED(dev->CreateQuery(&tq, &t.t1)))
                return fail("timer queries", E_FAIL);
        reload_user();
        return true;
    }

    // MARK: Scenes

    // The director's list: the built-in scenes' tags, and the user's scenes as light ones that suit anything.
    void tag_scenes() {
        std::vector<SceneTags> tags;
        for (size_t i = 0; i < scenes.size(); ++i) tags.push_back(i < builtin ? builtin_at(i).tags : SceneTags{scenes[i].name});
        dir.set_scenes(std::move(tags));
    }

    std::string reload_user() {
        scenes.resize(builtin);
        tag_scenes();
        if (current >= scenes.size()) current = 0;
        if (previous >= scenes.size()) previous = current;
        user_errors.clear();
        if (cfg.user_dir.empty()) return user_errors;
        std::vector<std::filesystem::path> files;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(cfg.user_dir, ec))
            if (e.is_regular_file() && e.path().extension() == L".hlsl") files.push_back(e.path());
        if (files.empty()) return user_errors;
        std::sort(files.begin(), files.end());
        prepare_user_dir(cfg.user_dir);  // dasvis.hlsli matching this version
        // The compiler is loaded only now, and only when there are scenes to compile.
        HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
        const auto compile = dll ? reinterpret_cast<pD3DCompile>(GetProcAddress(dll, "D3DCompile")) : nullptr;
        if (!compile) {
            user_errors = "Your scenes need d3dcompiler_47.dll, which wasn't found";
            if (dll) FreeLibrary(dll);
            return user_errors;
        }
        for (const auto& f : files) {
            std::ifstream in(f, std::ios::binary);
            const std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            ComPtr<ID3DBlob> code, errors;
            const std::string name = f.string();
            const HRESULT hr = compile(src.data(), src.size(), name.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "ps_4_0",
                                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
            Scene sc{f.stem().string(), nullptr, Sim::none};
            if (SUCCEEDED(hr) && SUCCEEDED(dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &sc.ps))) {
                scenes.push_back(std::move(sc));
            } else {
                std::string msg = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "couldn't compile";
                if (const size_t nl = msg.find('\n'); nl != std::string::npos) msg.resize(nl);  // the first error is enough
                if (const size_t at = msg.find(cfg.user_dir.string()); at != std::string::npos) msg.erase(at, cfg.user_dir.string().size() + 1);
                user_errors += (user_errors.empty() ? "" : "\n") + msg;
            }
        }
        FreeLibrary(dll);
        tag_scenes();
        return user_errors;
    }

    // Puts the director's cue on screen: a new scene starts its transition; the same one only changes its layers.
    void apply(const Cue& c) {
        if (scenes.empty()) return;
        ring_from = u.post[3];
        cue = c;
        cue.scene %= scenes.size();
        if (cue.scene == current) return;
        if (!scenes[cue.scene].ps && cue.scene < builtin)  // a heavy scene not compiled yet (Low tier), or MilkDrop: now
            dev->CreatePixelShader(builtin_at(cue.scene).code, builtin_at(cue.scene).size, nullptr, &scenes[cue.scene].ps);
        if (!scenes[cue.scene].ps) return;
        previous = current, current = cue.scene;
        transition = 0, since_switch = 0, bars_on = 0, advance_due = false;
    }

    // What the director knows now, for a pick at the start of a `kind` part (from `from`) with this energy.
    Moment moment(const Inputs& in, Section kind, Section from, float part_energy) {
        Moment m;
        m.seed = song_seed, m.index = picks;
        m.section = kind, m.from = from, m.drop = kind == Section::drop;
        m.energy = Director::energy_of(kind, part_energy);
        m.tier = qc.tier(), m.budget_ms = in.budget_ms, m.intensity = std::clamp(in.intensity, 0, 2), m.glitchy = in.glitchy;
        return m;
    }

    // The director picks for `m` (never the scene on screen), and it goes on screen.
    void choose(const Moment& m) {
        apply(dir.pick(m, current));
        ++picks;
    }

    // N / P: the next or previous scene, dressed by the director. Any scene: it's your choice (the heavy ones too, at a
    // smaller render size if they must); only the director's own picks keep to what fits.
    void step_scene(int by) {
        if (scenes.empty()) return;
        const size_t i = by > 0 ? (current + 1) % scenes.size() : (current + scenes.size() - 1) % scenes.size();
        Moment m = now;
        m.drop = false;
        apply(dir.dress(i, m));
    }

    // A new song: its own seed (camera, turn direction, tunnel sides …), palette and starting scene.
    void new_song(const Inputs& in) {
        song = in.song;
        song_seed = fnv1a(song);
        seed = float(song_seed % 100000) / 100000.f;
        for (int i = 0; i < 4; ++i)
            for (int k = 0; k < 2; ++k) drift_seed[i][k] = float(fnv1a(song + char('a' + 4 * k + i)) % 1000) / 1000.f;
        dir.reset();
        picks = 0, last_part = -1;
        dir.set_taste(in.taste);
        if (!locked) choose(moment(in, Section::intro, Section::intro, 0.5f));
    }

    // The palette the picture fades towards: the cover's, or five colours around the song's key.
    void palette(const std::optional<std::string>& key) {
        camelot = key.value_or("");
        pal_target = cover_pal ? *cover_pal : key_palette(key, seed);
        const Rgb tint = key_tint(key, seed);
        for (size_t i = 0; i < 3; ++i) u.key[i] = tint[i];
    }

    void fade_palette(float dt) {
        const float k = pal_set ? 1 - std::exp(-dt / 0.4f) : 1.f;  // ~1 s to settle; the very first one at once
        for (size_t i = 0; i < 5; ++i) {
            for (size_t c = 0; c < 3; ++c) pal[i][c] += (pal_target[i][c] - pal[i][c]) * k, u.pal[i][c] = pal[i][c];
            u.pal[i][3] = 1;
        }
        pal_set = true;
    }

    // MARK: Frame

    void resize(int w, int h) {
        const float s = render_scale();
        const int cw = std::max(16, int(std::lround(w * s))) & ~1, ch = std::max(16, int(std::lround(h * s))) & ~1;
        out_w = w, out_h = h;
        if (comp[0].w != cw || comp[0].h != ch) {
            Target old = comp[cur];
            comp[0] = make(cw, ch, hdr), comp[1] = make(cw, ch, hdr);
            const float black[4] = {};
            for (auto& c : comp)
                if (c.rtv) ctx->ClearRenderTargetView(c.rtv.Get(), black);
            if (old.srv) pass(ps_copy.Get(), comp[cur], old, nullptr, false);  // carry the picture over
        }
        const int levels = tier_spec(qc.tier()).bloom_levels;
        if (int(bloom.size()) != levels || (levels && bloom[0].w != std::max(1, cw / 2))) {
            bloom.clear();
            int bw = cw, bh = ch;
            for (int i = 0; i < levels && bw > 8 && bh > 8; ++i) bloom.push_back(make(bw /= 2, bh /= 2, hdr));
        }
    }

    void upload_uniforms(const Target& dst, const Target* src) {
        u.res[0] = float(dst.w), u.res[1] = float(dst.h), u.res[2] = 1.f / float(dst.w), u.res[3] = 1.f / float(dst.h);
        u.pass[0] = src ? 1.f / float(src->w) : 0, u.pass[1] = src ? 1.f / float(src->h) : 0;
        if (uploaded && std::memcmp(&u, &sent, sizeof u) == 0) return;  // the simulations' passes: mostly the same
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            std::memcpy(m.pData, &u, sizeof u);
            ctx->Unmap(cb.Get(), 0);
            sent = u, uploaded = true;
        }
    }

    void set_target(const Target& dst, const Target* src, bool additive) {
        ID3D11ShaderResourceView* none[6] = {};
        ctx->PSSetShaderResources(0, 6, none);  // nothing bound as both input and output
        ctx->VSSetShaderResources(5, 1, none);
        ctx->OMSetRenderTargets(1, dst.rtv.GetAddressOf(), nullptr);
        const D3D11_VIEWPORT vp{0, 0, float(dst.w), float(dst.h), 0, 1};
        ctx->RSSetViewports(1, &vp);
        upload_uniforms(dst, src);
        ctx->OMSetBlendState(additive ? add.Get() : nullptr, nullptr, 0xFFFFFFFF);
    }

    // A full-screen pass. `sim_srv`: t5 (a simulation pass's second input, or what a scene shows of its simulation).
    void pass(ID3D11PixelShader* ps, const Target& dst, const Target& src, const Target* bloom_src, bool additive,
              ID3D11ShaderResourceView* sim_srv = nullptr) {
        if (!dst.rtv) return;
        set_target(dst, src.tex ? &src : nullptr, additive);
        ID3D11ShaderResourceView* srvs[6] = {src.srv.Get(), has_cover ? cover.srv.Get() : nullptr, spec.srv.Get(), wave.srv.Get(),
                                             bloom_src ? bloom_src->srv.Get() : nullptr, sim_srv};
        ctx->PSSetShaderResources(0, 6, srvs);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->Draw(3, 0);
    }

    // Points or triangles over the scene (particles, stars, ridges): `count` vertices that the vertex shader works out
    // from their number, reading `state` (t5) if it needs to. No vertex buffers.
    void draw(ID3D11VertexShader* v, ID3D11PixelShader* ps, const Target& dst, D3D11_PRIMITIVE_TOPOLOGY topology, UINT count,
              ID3D11ShaderResourceView* state, bool additive) {
        if (!dst.rtv) return;
        set_target(dst, nullptr, additive);
        ctx->VSSetShaderResources(5, 1, &state);
        ctx->IASetPrimitiveTopology(topology);
        ctx->VSSetShader(v, nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->Draw(count, 0);
        ID3D11ShaderResourceView* none = nullptr;  // back to the full-screen passes
        ctx->VSSetShaderResources(5, 1, &none);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
    }

    // MARK: Simulations

    void set_sim(float a, float b, float c, float d) { u.sim[0] = a, u.sim[1] = b, u.sim[2] = c, u.sim[3] = d; }

    // Makes the simulation `kind` needs at this tier and screen shape, unless it's there already (dropping any other).
    // Grids are kGrid cells tall; the particle layer is that number squared (16k, 37k, 65k).
    void ensure_sim(SimState& s, Sim kind, float aspect) {
        const int n = kGrid[int(qc.tier())];
        int w = 0, h = 0;
        if (kind == Sim::fluid || kind == Sim::coral) w = std::max(16, int(std::lround(float(n) * aspect))), h = n;
        else if (kind == Sim::particles) w = h = n;
        else if (kind == Sim::ridges) w = int(Features::kBins), h = kHistory;
        if (s.kind == kind && s.tier == qc.tier() && s.w == w && s.h == h) return;
        s = {};
        s.kind = kind, s.tier = qc.tier(), s.w = w, s.h = h;
        if (!w) return;
        const float zero[4] = {}, food[4] = {1, 0, 0, 0};
        const auto both = [&](Target* t, DXGI_FORMAT f, const float* clear) {
            for (int i = 0; i < 2; ++i) {
                t[i] = make(w, h, f);
                if (t[i].rtv) ctx->ClearRenderTargetView(t[i].rtv.Get(), clear);
            }
        };
        switch (kind) {
        case Sim::particles: both(s.a, DXGI_FORMAT_R32G32B32A32_FLOAT, zero); break;
        case Sim::fluid: both(s.a, DXGI_FORMAT_R16G16_FLOAT, zero), both(s.b, DXGI_FORMAT_R16G16_FLOAT, zero); break;
        case Sim::coral: both(s.a, fine, food); break;  // all food and nothing growing: the first frame seeds it
        case Sim::ridges:
            s.a[0] = make(w, h, DXGI_FORMAT_R32_FLOAT);
            if (s.a[0].rtv) ctx->ClearRenderTargetView(s.a[0].rtv.Get(), zero);
            break;
        default: break;
        }
    }

    // One frame of the simulation, before the scene is drawn.
    void step_sim(SimState& s, const Features& f, float dt) {
        set_sim(float(s.w), float(s.h), float(s.frames), 0);
        const int level = int(s.tier);
        switch (s.kind) {
        case Sim::particles:
            pass(ps_update.Get(), s.a[s.ai ^ 1], s.a[s.ai], nullptr, false);
            s.ai ^= 1;
            break;
        case Sim::fluid:  // the flow ends up back in a[0]
            pass(ps_velocity.Get(), s.a[1], s.a[0], nullptr, false);
            pass(ps_divergence.Get(), s.b[s.bi ^ 1], s.a[1], nullptr, false, s.b[s.bi].srv.Get());
            s.bi ^= 1;
            for (int i = 0; i < 8 + 4 * level; ++i) pass(ps_jacobi.Get(), s.b[s.bi ^ 1], s.b[s.bi], nullptr, false), s.bi ^= 1;
            pass(ps_gradient.Get(), s.a[0], s.a[1], nullptr, false, s.b[s.bi].srv.Get());
            break;
        case Sim::coral:
            set_sim(float(s.w), float(s.h), s.frames == 0 ? 1.f : 0.f, 0);
            for (int i = 0; i < 2 + level; ++i) pass(ps_coral_step.Get(), s.a[s.ai ^ 1], s.a[s.ai], nullptr, false), s.ai ^= 1;
            break;
        case Sim::ridges:  // a new row of history every 1/30 s, whatever the frame rate
            s.row_t += dt;
            if (s.row_t >= 1 / kRowRate && s.a[0].tex) {
                s.row_t = std::fmod(s.row_t, 1 / kRowRate);
                s.row = (s.row + 1) % kHistory;
                const D3D11_BOX box{0, UINT(s.row), 0, UINT(Features::kBins), UINT(s.row + 1), 1};
                ctx->UpdateSubresource(s.a[0].tex.Get(), 0, &box, f.spectrum().data(), UINT(Features::kBins * sizeof(float)), 0);
            }
            break;
        default: break;
        }
        ++s.frames;
    }

    // What the scene's own pass reads of its simulation (t5).
    static ID3D11ShaderResourceView* sim_view(const SimState& s) {
        if (s.kind == Sim::fluid) return s.a[0].srv.Get();
        if (s.kind == Sim::coral) return s.a[s.ai].srv.Get();
        return nullptr;
    }

    // The simulation's points or lines, over the scene.
    void draw_sim(const SimState& s, const Target& dst) {
        const int level = int(s.tier);
        switch (s.kind) {
        case Sim::particles:
            set_sim(float(s.w), float(s.h), float(s.frames), 0);
            if (s.a[s.ai].srv)
                draw(vs_point.Get(), ps_point.Get(), dst, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST, UINT(s.w * s.h), s.a[s.ai].srv.Get(), true);
            break;
        case Sim::galaxy: {
            const UINT stars = 65536u << (2 * level);  // 64k, 256k, 1M
            set_sim(float(stars), 0, 0, 0);
            draw(vs_star.Get(), ps_star.Get(), dst, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST, stars, nullptr, true);
            break;
        }
        case Sim::ridges:
            if (s.a[0].srv) {
                const int rows = 64 + 32 * level, segs = 96 + 64 * level;
                set_sim(float(rows), float(segs), float(s.row), float(s.row_t * kRowRate));
                draw(vs_ridge.Get(), ps_ridge.Get(), dst, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, UINT(rows * segs * 6), s.a[0].srv.Get(), false);
            }
            break;
        default: break;
        }
    }

    void read_timers() {
        Timer& t = timers[(frame + 1) % std::size(timers)];  // issued three frames ago
        if (!t.issued) return;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        UINT64 a = 0, b = 0;
        if (ctx->GetData(t.disjoint.Get(), &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ctx->GetData(t.t0.Get(), &a, sizeof a, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ctx->GetData(t.t1.Get(), &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            return;
        t.issued = false;
        if (dj.Disjoint || !dj.Frequency || b < a) return;
        const float ms = float(double(b - a) * 1000.0 / double(dj.Frequency));
        gpu_ms = gpu_ms ? gpu_ms + (ms - gpu_ms) * 0.1f : ms;
    }

    void read_lum(float dt) {
        if (frame < 2) return;
        ID3D11Texture2D* s = lum_stage[(frame + 1) % 3].Get();  // copied two frames ago
        D3D11_MAPPED_SUBRESOURCE m;
        if (ctx->Map(s, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) return;
        unsigned sum = 0;
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x) sum += static_cast<const uint8_t*>(m.pData)[size_t(y) * m.RowPitch + size_t(x)];
        ctx->Unmap(s, 0);
        limiter.update(float(sum) / (256.f * 255.f), dt);
    }

    // Everything this pipeline needs, set every frame: Direct2D shares the device and may have left its own state.
    float render_scale() const { return cfg.fixed_scale > 0 ? cfg.fixed_scale : qc.scale(); }

    void bind_pipeline() {
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->RSSetState(raster.Get());
        ctx->OMSetDepthStencilState(nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, cb.GetAddressOf());
        ctx->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
        ID3D11SamplerState* samplers[] = {clamp.Get(), wrap.Get()};
        ctx->PSSetSamplers(0, 2, samplers);
    }

    // The window's back buffer (or a test's texture), as a target for this frame only: nothing keeps it, so the
    // window can resize its buffers.
    static Target wrap_target(ID3D11Device* dev, ID3D11Texture2D* tex) {
        Target t;
        D3D11_TEXTURE2D_DESC d;
        tex->GetDesc(&d);
        if (FAILED(dev->CreateRenderTargetView(tex, nullptr, &t.rtv))) return {};
        t.w = int(d.Width), t.h = int(d.Height);
        return t;
    }

    void final_pass(const Target& out) {
        u.out[0] = float(out.w), u.out[1] = float(out.h), u.out[2] = float(out.w) / float(out.h);
        pass(ps_final.Get(), out, comp[cur], bloom.empty() ? &comp[cur] : &bloom[0], false, title.srv.Get());
        ID3D11ShaderResourceView* none[6] = {};
        ctx->PSSetShaderResources(0, 6, none);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);  // Direct2D draws over it next
        ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    }

    void redraw(ID3D11Texture2D* target) {
        const Target out = wrap_target(dev.Get(), target);
        if (!frame || !out.rtv || !comp[cur].srv) return;
        bind_pipeline();
        final_pass(out);
    }

    void render(const Inputs& in, ID3D11Texture2D* target) {
        const Target out = wrap_target(dev.Get(), target);
        if (!out.rtv) return;
        const int w = out.w, h = out.h;
        const float dt = std::clamp(in.dt, 0.f, 0.1f);
        time += dt;
        const Features& f = *in.f;
        if (in.song != song || frame == 0) cover_pal.reset(), new_song(in), palette(in.camelot);  // the new cover arrives later
        else if (in.camelot.value_or("") != camelot) palette(in.camelot);  // the analysis finished while it played

        // Where the song is: from the map when there is one, else the live clock.
        const bool has_map = in.map && in.map->beats.size() >= 16;
        SongMap::State ms;
        double beats = f.beats, beat_phase = f.beat_phase(), bar = f.bar_phase();
        if (has_map) {
            double t = in.song_time + map_offset;
            // Keep the map in step with what's heard: each live kick is compared with the map's nearest kick, and a
            // steady difference of more than 25 ms (the player's clock running early or late) is taken out slowly.
            const auto& kh = in.map->hits[SongMap::kick];
            if (f.kick_hit && !kh.empty()) {
                const auto it = std::lower_bound(kh.begin(), kh.end(), t);
                double d = 1;
                if (it != kh.end()) d = *it - t;
                if (it != kh.begin() && std::fabs(*(it - 1) - t) < std::fabs(d)) d = *(it - 1) - t;
                if (std::fabs(d) < 0.1) kick_drift += (d - kick_drift) * 0.1;
                if (std::fabs(kick_drift) > 0.025) map_offset = std::clamp(map_offset + (kick_drift - std::copysign(0.025, kick_drift)) * 0.2, -0.2, 0.2);
                t = in.song_time + map_offset;
            }
            ms = in.map->at(t);
            if (!had_map) beats_shift = std::round(f.beats - ms.beats);
            beats = ms.beats + beats_shift, beat_phase = ms.beat_phase, bar = ms.bar_phase;
        }
        had_map = has_map;
        const double now_t = in.song_time + map_offset;
        drv = drive(f, has_map ? in.map : nullptr, last_t, now_t, drv, dt);
        last_t = now_t;
        fade_palette(dt);

        // The director. With the map: a pick as each part of the song starts (they start on a 4-bar downbeat; a drop
        // gets a hero), and on a 16-bar phrase when a scene has been on for long enough. Without: when the scene's time
        // is up (shorter on insane, longer on chill), on a bar's downbeat.
        since_switch += dt;
        const bool new_bar = bar < last_bar;
        bars_on += new_bar;
        last_bar = bar;
        now = moment(in, ms.section, ms.section, has_map ? ms.energy : f.energy);
        dir.set_taste(in.taste);
        if (!locked && has_map) {
            const auto& parts = in.map->parts;
            const auto it = std::upper_bound(parts.begin(), parts.end(), now_t, [](double t, const SongMap::Part& p) { return t < p.start; });
            const long part = long(it - parts.begin()) - 1;
            if (last_part < 0 || part < 0) {
                last_part = part;  // the map just arrived, or before the first part: carry on
            } else if (part != last_part) {
                last_part = part;
                const auto& p = parts[size_t(part)];
                choose(moment(in, p.kind, part ? parts[size_t(part) - 1].kind : p.kind, p.energy));
            } else if (new_bar && ms.bar % 16 == 0 && bars_on >= Director::phrase_bars(in.intensity) && ms.section != Section::drop) {
                choose(moment(in, ms.section, ms.section, ms.energy));
            }
        } else if (!locked) {
            const double every = cfg.auto_advance * (in.intensity <= 0 ? 2 : in.intensity >= 2 ? 0.5 : 1);
            if (every > 0 && since_switch > every) advance_due = true;
            const float e = f.energy;
            if (advance_due && new_bar) choose(moment(in, e > 0.75f ? Section::chorus : Section::verse, Section::verse, e));
        }
        const float beat_s = float(60.0 / std::max(60.0, has_map ? in.map->bpm : f.bpm));
        transition = std::min(1.f, transition + dt / std::max(0.25f, cue.beats * beat_s));
        // The drop's events: the next drop's while it's coming, the last one's while it lands (and the cover shatter
        // plays out over the 8 bars after it).
        uint8_t events = 0;
        float since_drop = 99;
        if (has_map && !in.map->drops.empty()) {
            const auto& dr = in.map->drops;
            const size_t k = size_t(std::upper_bound(dr.begin(), dr.end(), now_t) - dr.begin());
            if (k) since_drop = float((now_t - dr[k - 1]) / in.map->bar_seconds());
            events = Director::events(song_seed, k && (ms.drop > 0 || since_drop < 9) ? k - 1 : k, std::clamp(in.intensity, 0, 2));
        }
        if (title.tex && u.lyric[2] == 0 && in.song_time > 10.5) title = {}, u.title[3] = 0;  // the card has played: its texture goes
        // A drop called from the phone: it lands now, for a bar (and the cover shatter plays out over 8).
        if (manual_drop >= 0) {
            manual_drop += dt;
            const double bar_s = has_map ? in.map->bar_seconds() : 240.0 / std::max(60.0, f.bpm);
            const float b = float(manual_drop / bar_s);
            if (b > 9) manual_drop = -1;
            else {
                ms.drop = std::max(ms.drop, std::max(0.f, 1 - b));
                if (b < since_drop) since_drop = b, events = manual_events;
            }
        }
        const float ease = 1 - std::exp(-dt / 0.12f);
        cam_x += (cam_tx - cam_x) * ease, cam_y += (cam_ty - cam_y) * ease;
        shock = std::max(0.f, shock - dt / 0.6f);
        travel += dt * (0.3 + 0.6 * f.energy + 0.6 * drv.kick + 3.0 * ms.drop);
        kicks += drv.kick_hit;

        read_timers();
        read_lum(dt);
        // What this scene costs here, for the director: measured once it's settled, scaled to the tier's smallest size
        // (cost goes roughly with the pixels).
        if (transition >= 1 && since_switch > 3 && gpu_ms > 0 && frame % 30 == 0) {
            const float smin = tier_spec(qc.tier()).scale_min, sc = render_scale();
            dir.set_cost(scenes[current].name, qc.tier(), gpu_ms * (smin / sc) * (smin / sc));
        }
        bind_pipeline();
        const bool changed = gpu_ms > 0 && cfg.fixed_scale <= 0 && qc.update(gpu_ms, in.budget_ms, dt);  // real measurements only
        if (changed || out_w != w || out_h != h || !comp[0].tex) resize(w, h);
        if (!comp[0].tex || !lum.tex) return;

        const TierSpec& ts = tier_spec(qc.tier());
        const float intensity = kIntensity[std::clamp(in.intensity, 0, 2)];
        const float flash = limiter.budget() * (cfg.reduce_flash ? 0.5f : 1.f);
        const auto set = [](float* v, float a, float b, float c, float d) { v[0] = a, v[1] = b, v[2] = c, v[3] = d; };
        set(u.time, float(time), dt, seed, ts.quality);
        set(u.out, float(w), float(h), float(w) / float(h), transition);
        set(u.beat, float(beat_phase), float(bar), float(beats), float(has_map ? in.map->bpm : f.bpm));
        set(u.drums, drv.kick, drv.snare, drv.hats, drv.kick_hit ? 1.f : 0.f);
        set(u.tone, drv.bass, drv.lead, f.energy, has_map ? 0.f : 1.f);
        set(u.hits, drv.kick_hit ? 1.f : 0.f, drv.snare_hit ? 1.f : 0.f, drv.hats_hit ? 1.f : 0.f, drv.since_kick);
        set(u.song, float(ms.phrase_phase), float(int(ms.section)), ms.section_t, ms.build);
        set(u.drop, ms.drop, std::min(ms.to_drop, 99.f), ms.gap ? 1.f : 0.f, ms.energy);
        set(u.look, intensity, flash, limiter.gain(), has_cover ? 1.f : 0.f);
        const float ring = ring_from + ((cue.ring ? 1.f : 0.f) - ring_from) * transition;
        set(u.post, 0.08f + 0.04f * f.energy, float(std::max<size_t>(1, bloom.size())), 0.018f, ring);
        set(u.dir, float(int(cue.transition)), float(events), cue.sigils ? 1.f : 0.f, 0);
        set(u.ext, float(std::fmod(travel, 10000.0)), kicks, float(in.song_time), since_drop);
        set(u.deck, std::fabs(cam_x) < 1e-3f ? 0.f : cam_x, std::fabs(cam_y) < 1e-3f ? 0.f : cam_y, shock, 0);
        // Drift: four slow waves from the song's seed, so a long section keeps changing without looping.
        for (int i = 0; i < 4; ++i) {
            const float a = drift_seed[i][0], b = drift_seed[i][1];
            u.drift[i] = 0.5f + 0.3f * std::sin(float(time) * 6.2832f / (40 + 50 * a) + 6.2832f * b) +
                         0.2f * std::sin(float(time) * 6.2832f / (17 + 20 * b) + 6.2832f * a);
        }
        u.key[3] = ts.quality >= 0.5f ? 0.0015f + 0.005f * drv.kick * intensity + 0.008f * ms.drop * flash : 0.f;

        float sp[Features::kBins * 2];
        for (size_t i = 0; i < Features::kBins; ++i) sp[2 * i] = f.spectrum()[i], sp[2 * i + 1] = f.spectrum_peaks()[i];
        ctx->UpdateSubresource(spec.tex.Get(), 0, nullptr, sp, UINT(sizeof sp), 0);
        ctx->UpdateSubresource(wave.tex.Get(), 0, nullptr, f.wave().data(), UINT(Features::kWave * sizeof(float)), 0);

        Timer& tm = timers[frame % std::size(timers)];
        const bool timing = !tm.issued;  // a query still in flight from four frames ago is skipped, not overwritten
        if (timing) ctx->Begin(tm.disjoint.Get()), ctx->End(tm.t0.Get());

        ensure_sim(sim, scenes[current].sim, float(w) / float(h));
        ensure_sim(layer, cue.mid == Mid::particles ? Sim::particles : Sim::none, float(w) / float(h));
        step_sim(layer, f, dt);
        step_sim(sim, f, dt);  // last: the scene's pass reads its u_sim
        if (sim.kind == Sim::milkdrop) set_sim(milk.srv ? 1.f : 0.f, 0, 0, 0);  // a picture yet?
        else milk = {};  // off screen: its texture goes
        const int prev = cur;
        cur ^= 1;
        pass(scenes[current].ps.Get(), comp[cur], comp[prev], nullptr, false, sim.kind == Sim::milkdrop ? milk.srv.Get() : sim_view(sim));
        draw_sim(sim, comp[cur]);
        draw_sim(layer, comp[cur]);
        if (cue.mid == Mid::ribbon) draw(vs_ribbon.Get(), ps_ribbon.Get(), comp[cur], D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, 1024, wave.srv.Get(), true);
        const Target* src = &comp[cur];
        for (auto& b : bloom) pass(ps_down.Get(), b, *src, nullptr, false), src = &b;
        if (!bloom.empty()) pass(ps_lum.Get(), lum, bloom.back(), nullptr, false);
        for (size_t i = bloom.size(); i-- > 1;) pass(ps_up.Get(), bloom[i - 1], bloom[i], nullptr, true);
        ctx->CopyResource(lum_stage[frame % 3].Get(), lum.tex.Get());
        final_pass(out);
        if (timing) ctx->End(tm.t1.Get()), ctx->End(tm.disjoint.Get()), tm.issued = true;
        ++frame;
    }

    void set_milkdrop(const uint8_t* bgra, int w, int h) {
        if (scenes[current].sim != Sim::milkdrop || !bgra || w <= 0 || h <= 0) return;
        if (milk.w != w || milk.h != h) milk = make(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, false);
        if (milk.tex) ctx->UpdateSubresource(milk.tex.Get(), 0, nullptr, bgra, UINT(w) * 4, 0);
    }

    ID3D11Texture2D* title_target(int w, int h) {
        u.title[3] = u.lyric[2] = 0;
        if (title.w != w || title.h != h) title = make(w, h, DXGI_FORMAT_B8G8R8A8_UNORM);
        return title.tex.Get();
    }

    void show_title(int letters, float share) {
        if (!letters || !title.tex) title = {};  // nothing to show: the texture goes
        u.title[0] = title.h ? float(title.w) / float(title.h) : 1, u.title[1] = float(letters), u.title[2] = share;
        u.title[3] = title.tex ? 1.f : 0.f;
    }

    void show_lyric(int letters, double start, double end) {
        if (!letters || !title.tex) return void(title = {});
        u.title[0] = float(title.w) / float(title.h), u.title[1] = float(letters), u.title[2] = 1;
        u.lyric[0] = float(start), u.lyric[1] = float(end), u.lyric[2] = 1;
    }

    void set_cover(const uint8_t* bgra, int w, int h) {
        has_cover = false;
        cover = {};
        cover_pal = cover_palette(bgra, w, h);  // nullopt without a cover, or for a grey one: the key's colours then
        palette(camelot.empty() ? std::nullopt : std::optional<std::string>(camelot));
        if (!bgra || w <= 0 || h <= 0) return;
        UINT support = 0;
        dev->CheckFormatSupport(DXGI_FORMAT_B8G8R8A8_UNORM, &support);
        const bool mips = support & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN;
        cover = make(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, mips, mips ? 0 : 1);
        if (!cover.tex) return;
        ctx->UpdateSubresource(cover.tex.Get(), 0, nullptr, bgra, UINT(w) * 4, 0);
        if (mips) ctx->GenerateMips(cover.srv.Get());
        has_cover = true;
    }
};

DasVis::DasVis(ID3D11Device* device, const Config& cfg) : p_(std::make_unique<Impl>(cfg)) {
    if (!device) p_->error = "no Direct3D device";
    else p_->init(device);
}
DasVis::~DasVis() = default;

bool DasVis::ok() const { return p_->error.empty(); }
const std::string& DasVis::error() const { return p_->error; }
void DasVis::render(const Inputs& in, ID3D11Texture2D* target) {
    if (ok() && in.f && target) p_->render(in, target);
}
void DasVis::redraw(ID3D11Texture2D* target) {
    if (ok() && target) p_->redraw(target);
}
void DasVis::set_cover(const uint8_t* bgra, int w, int h) {
    if (ok()) p_->set_cover(bgra, w, h);
}
void DasVis::next() {
    if (ok()) p_->step_scene(1);
}
void DasVis::previous() {
    if (ok()) p_->step_scene(-1);
}
void DasVis::skip() {
    if (!ok()) return;
    Moment m = p_->now;
    m.drop = false;
    p_->choose(m);
}
std::map<std::string, float> DasVis::costs() const { return p_->dir.costs(); }
std::vector<std::string> DasVis::scene_names() const {
    std::vector<std::string> names;
    for (const auto& s : p_->scenes) names.push_back(s.name);
    return names;
}
void DasVis::select(size_t scene) {
    if (!ok() || scene >= p_->scenes.size()) return;
    Moment m = p_->now;
    m.drop = false;
    p_->apply(p_->dir.dress(scene, m));
}
void DasVis::drop_now() {
    if (!ok()) return;
    Moment m = p_->now;
    m.drop = true, m.section = Section::drop, m.from = Section::build, m.energy = kPeak;
    p_->choose(m);
    p_->manual_drop = 0;
    p_->manual_events = Director::events(p_->song_seed, 1000 + p_->picks, m.intensity);
}
void DasVis::shockwave() { p_->shock = 1; }
LightState DasVis::lights() const {
    LightState s;
    s.pal = p_->pal, s.kick = p_->drv.kick, s.snare = p_->drv.snare, s.bass = p_->drv.bass, s.drop = p_->u.drop[0];
    s.beats = p_->u.beat[2], s.flash = p_->limiter.budget(), s.reduce_flash = p_->cfg.reduce_flash;
    return s;
}
void DasVis::set_camera(float turn, float zoom) {
    p_->cam_tx = std::clamp(turn, -1.f, 1.f), p_->cam_ty = std::clamp(zoom, -1.f, 1.f);
}
bool DasVis::particles() const { return p_->cue.mid == Mid::particles; }
bool DasVis::wants_milkdrop() const { return ok() && !p_->scenes.empty() && p_->scenes[p_->current].sim == Sim::milkdrop; }
void DasVis::set_milkdrop(const uint8_t* bgra, int w, int h) {
    if (ok()) p_->set_milkdrop(bgra, w, h);
}
ID3D11Texture2D* DasVis::title_target(int w, int h) { return ok() ? p_->title_target(w, h) : nullptr; }
void DasVis::show_lyric(int letters, double start, double end) {
    if (ok()) p_->show_lyric(letters, start, end);
}
void DasVis::show_title(int letters, float title_share) {
    if (ok()) p_->show_title(letters, title_share);
}
void DasVis::lock(bool on) { p_->locked = on, p_->advance_due = false; }
bool DasVis::locked() const { return p_->locked; }
void DasVis::set_auto_advance(double seconds) { p_->cfg.auto_advance = seconds; }
void DasVis::set_reduce_flash(bool on) {
    p_->cfg.reduce_flash = on;
    p_->limiter.set_max(on ? 1 : 3);
}
std::string DasVis::scene_name() const { return p_->scenes.empty() ? "" : p_->scenes[p_->current].name; }
size_t DasVis::scene_count() const { return p_->scenes.size(); }
std::string DasVis::reload_user_scenes() { return ok() ? p_->reload_user() : p_->error; }
const std::string& DasVis::user_errors() const { return p_->user_errors; }
Tier DasVis::tier() const { return p_->qc.tier(); }
float DasVis::scale() const { return p_->render_scale(); }
float DasVis::gpu_ms() const { return p_->gpu_ms; }
bool DasVis::tier_settled() const { return p_->qc.settled(); }
float DasVis::flash_budget() const { return p_->limiter.budget(); }
double DasVis::map_offset() const { return p_->map_offset; }

const char* library_source() { return k_library; }

std::vector<uint8_t> led_frame(const LightState& s, int leds) {
    std::vector<uint8_t> out(size_t(std::max(0, leds)) * 3);
    const float flash = s.flash * (s.reduce_flash ? 0.5f : 1.f);
    const float base = 0.35f;  // the level the music moves the lights from; the swings above it are what's limited
    const float level = std::min(1.f, base + (0.45f * std::max(s.kick, 0.8f * s.bass) + 0.4f * s.drop) * flash);
    const auto srgb = [](float v) { return uint8_t(std::lround(255 * std::pow(std::clamp(v, 0.f, 1.f), 1 / 2.2f))); };
    for (int i = 0; i < leds; ++i) {
        // Along the strip, the palette's lighter colours (the dark one would just be off), a step further each beat.
        const double t = double(i) / std::max(1, leds) * 2 + s.beats * 0.125;
        const double f = (t - std::floor(t)) * 4;
        const size_t a = 1 + size_t(f), b = a == 4 ? 1 : a + 1;
        const float k = float(f - std::floor(f)), w = k * k * (3 - 2 * k);
        const uint32_t h = uint32_t(i) * 2654435761u ^ uint32_t(std::floor(s.beats * 4)) * 40503u;
        const float glint = (h % 7 == 0) ? s.snare * flash : 0.f;  // a few LEDs at a time catch the snare
        for (size_t c = 0; c < 3; ++c) {
            const float v = s.pal[a][c] + (s.pal[b][c] - s.pal[a][c]) * w;
            out[size_t(i) * 3 + c] = srgb(v * level + glint);
        }
    }
    return out;
}

std::vector<SceneTags> builtin_scenes() {
    std::vector<SceneTags> t;
    for (const auto& s : kScenes) t.push_back(s.tags);
    return t;
}

void prepare_user_dir(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream(dir / L"dasvis.hlsli", std::ios::binary) << k_library;
    bool any = false;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) any |= e.path().extension() == L".hlsl";
    if (any) return;
    std::ofstream(dir / L"my_first_scene.hlsl", std::ios::binary)
        << "// Your own DasVis scene. Change it, save, press F5 in the visualizer. dasvis.hlsli lists what you can use.\n"
           "#define DASVIS_SCENE\n"
           "#include \"dasvis.hlsli\"\n\n"
           "float3 scene(float2 uv, float2 p) {\n"
           "    float r = length(p);\n"
           "    float rings = sin(r * 40 - BEATS * TAU) * 0.5 + 0.5;  // rings moving out, one per beat\n"
           "    float3 c = PAL(r - BEATS * 0.1) * pow(rings, 4) * (0.4 + 1.5 * KICK * FLASH);\n"
           "    return max(c, PREV(0.5 + (uv - 0.5) * 0.98) * 0.9);   // a little feedback\n"
           "}\n";
}

}  // namespace wb::dasvis
