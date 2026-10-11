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

#include "dasvis_gen/library.h"
#include "dasvis_gen/ps_bloom_down.h"
#include "dasvis_gen/ps_bloom_up.h"
#include "dasvis_gen/ps_copy.h"
#include "dasvis_gen/ps_feedback.h"
#include "dasvis_gen/ps_final.h"
#include "dasvis_gen/ps_kaleido.h"
#include "dasvis_gen/ps_lum.h"
#include "dasvis_gen/ps_tunnel.h"
#include "dasvis_gen/vs_fullscreen.h"

namespace wb::dasvis {

using Microsoft::WRL::ComPtr;

namespace {

// Mirrors the cbuffer in dasvis.hlsli: float4s only, so the C++ and HLSL layouts can't drift apart.
struct Uniforms {
    float time[4], res[4], out[4], beat[4], drums[4], tone[4], look[4], post[4], key[4], pal[5][4], pass[4], song[4], drop[4];
};
static_assert(sizeof(Uniforms) % 16 == 0);

struct Target {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    int w = 0, h = 0;
};

struct Scene {
    std::string name;
    ComPtr<ID3D11PixelShader> ps;
    bool ring = false;  // the spectrum ring over it
};

struct BuiltIn {
    const char* name;
    const BYTE* code;
    size_t size;
    bool ring;
};
const BuiltIn kScenes[] = {
    {"Feedback Classic", k_ps_feedback, sizeof k_ps_feedback, false},
    {"Neon Tunnel", k_ps_tunnel, sizeof k_ps_tunnel, true},
    {"Kaleido Cover", k_ps_kaleido, sizeof k_ps_kaleido, false},
};

constexpr float kIntensity[] = {0.5f, 1.f, 1.6f};

uint32_t fnv1a(const std::string& s) {
    uint32_t h = 2166136261u;
    for (const unsigned char c : s) h = (h ^ c) * 16777619u;
    return h;
}

void hsv(float h, float s, float v, float* out) {
    h = h - std::floor(h);
    const auto f = [&](float n) {
        const float k = std::fmod(n + h * 6, 6.f);
        return v - v * s * std::max(0.f, std::min({k, 4 - k, 1.f}));
    };
    for (int i = 0; i < 3; ++i) {
        const float c = f(i == 0 ? 5.f : i == 1 ? 3.f : 1.f);
        out[i] = c * c;  // to linear (≈ gamma 2)
    }
    out[3] = 1;
}

}  // namespace

struct DasVis::Impl {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    Config cfg;
    std::string error, user_errors;
    DXGI_FORMAT hdr = DXGI_FORMAT_R11G11B10_FLOAT;

    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps_down, ps_up, ps_lum, ps_final, ps_copy;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11SamplerState> clamp, wrap;
    ComPtr<ID3D11BlendState> add;
    ComPtr<ID3D11RasterizerState> raster;

    std::vector<Scene> scenes;
    size_t builtin = 0, current = 0, previous = 0;
    bool locked = false, advance_due = false;
    double since_switch = 0, last_bar = 0;
    float transition = 1;

    Target comp[2], lum, spec, wave, cover;
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
    float last_drop = 0;
    float gpu_ms = 0;
    double time = 0;
    std::string song, camelot;
    float seed = 0;
    Uniforms u{};

    explicit Impl(const Config& c) : cfg(c), qc(c.start, c.auto_tier, c.max_tier), limiter(c.reduce_flash ? 1 : 3) {}

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
        for (const auto& s : kScenes) {
            Scene sc{s.name, nullptr, s.ring};
            if (FAILED(hr = dev->CreatePixelShader(s.code, s.size, nullptr, &sc.ps))) return fail("scene shader", hr);
            scenes.push_back(std::move(sc));
        }
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

    std::string reload_user() {
        scenes.resize(builtin);
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
            Scene sc{f.stem().string(), nullptr, false};
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
        return user_errors;
    }

    void switch_to(size_t i) {
        if (scenes.empty()) return;
        i %= scenes.size();
        if (i == current) return;
        previous = current, current = i;
        transition = 0;
        since_switch = 0;
        advance_due = false;
    }

    // A new song: its own seed (camera, turn direction, tunnel sides …), palette and starting scene.
    void new_song(const Inputs& in) {
        song = in.song;
        seed = float(fnv1a(song) % 100000) / 100000.f;
        if (!locked) switch_to(size_t(seed * float(scenes.size())));
    }

    // The palette: five colours around the song's key on the Camelot wheel (minor keys cooler and softer), or around a
    // hue from the seed when the key isn't known. Phase 3 takes them from the cover instead.
    void palette(const std::optional<std::string>& key) {
        camelot = key.value_or("");
        float h = seed;
        bool minor = false;
        if (key && key->size() >= 2) {
            const int n = std::atoi(key->c_str());
            minor = key->back() == 'A';
            if (n >= 1 && n <= 12) h = float(n - 1) / 12.f;
        }
        // Neighbouring hues, dark → light, and one accent opposite (the brightest): colours that sit together.
        const float s = minor ? 0.85f : 1.f;
        const float offs[] = {0.0f, -0.05f, 0.06f, 0.12f, 0.5f}, sats[] = {0.85f, 0.8f, 0.7f, 0.55f, 0.6f}, vals[] = {0.4f, 0.62f, 0.8f, 0.92f, 1.f};
        for (int i = 0; i < 5; ++i) hsv(h + offs[i], sats[i] * s, vals[i], u.pal[i]);
        hsv(h, 0.45f, 1.f, u.key);
    }

    // MARK: Frame

    void resize(int w, int h) {
        const float s = qc.scale();
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
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            std::memcpy(m.pData, &u, sizeof u);
            ctx->Unmap(cb.Get(), 0);
        }
    }

    void pass(ID3D11PixelShader* ps, const Target& dst, const Target& src, const Target* bloom_src, bool additive) {
        if (!dst.rtv) return;
        ID3D11ShaderResourceView* none[5] = {};
        ctx->PSSetShaderResources(0, 5, none);  // nothing bound as both input and output
        ctx->OMSetRenderTargets(1, dst.rtv.GetAddressOf(), nullptr);
        const D3D11_VIEWPORT vp{0, 0, float(dst.w), float(dst.h), 0, 1};
        ctx->RSSetViewports(1, &vp);
        upload_uniforms(dst, src.tex ? &src : nullptr);
        ID3D11ShaderResourceView* srvs[5] = {src.srv.Get(), has_cover ? cover.srv.Get() : nullptr, spec.srv.Get(), wave.srv.Get(),
                                             bloom_src ? bloom_src->srv.Get() : nullptr};
        ctx->PSSetShaderResources(0, 5, srvs);
        ctx->OMSetBlendState(additive ? add.Get() : nullptr, nullptr, 0xFFFFFFFF);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->Draw(3, 0);
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
        pass(ps_final.Get(), out, comp[cur], bloom.empty() ? &comp[cur] : &bloom[0], false);
        ID3D11ShaderResourceView* none[5] = {};
        ctx->PSSetShaderResources(0, 5, none);
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
        if (in.song != song || frame == 0) new_song(in), palette(in.camelot);
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

        // Auto-advance: when it's time, on the next bar's downbeat (with the map: the next 4-bar boundary). And with
        // the map, a new scene on each drop (if the last change wasn't just now).
        since_switch += dt;
        if (!locked && cfg.auto_advance > 0 && since_switch > cfg.auto_advance) advance_due = true;
        const bool new_bar = bar < last_bar;
        if (advance_due && new_bar && (!has_map || ms.bar % 4 == 0)) switch_to((current + 1) % scenes.size());
        if (has_map && !locked && ms.drop > 0.9f && last_drop < 0.9f && since_switch > 8) switch_to((current + 1) % scenes.size());
        last_bar = bar;
        last_drop = ms.drop;
        const float beat_s = float(60.0 / std::max(60.0, f.bpm));
        transition = std::min(1.f, transition + dt / std::max(0.4f, 2 * beat_s));  // two beats

        read_timers();
        read_lum(dt);
        bind_pipeline();
        const bool changed = gpu_ms > 0 && qc.update(gpu_ms, in.budget_ms, dt);  // acts on real measurements only
        if (changed || out_w != w || out_h != h || !comp[0].tex) resize(w, h);
        if (!comp[0].tex || !lum.tex) return;

        const TierSpec& ts = tier_spec(qc.tier());
        const float intensity = kIntensity[std::clamp(in.intensity, 0, 2)];
        const float flash = limiter.budget() * (cfg.reduce_flash ? 0.5f : 1.f);
        const auto set = [](float* v, float a, float b, float c, float d) { v[0] = a, v[1] = b, v[2] = c, v[3] = d; };
        set(u.time, float(time), dt, seed, ts.quality);
        set(u.out, float(w), float(h), float(w) / float(h), transition);
        set(u.beat, float(beat_phase), float(bar), float(beats), float(has_map ? in.map->bpm : f.bpm));
        set(u.drums, f.kick, f.snare, f.hats, f.kick_hit ? 1.f : 0.f);
        set(u.tone, f.bass, f.lead, f.energy, has_map ? 0.f : 1.f);
        set(u.song, float(ms.phrase_phase), float(int(ms.section)), ms.section_t, ms.build);
        set(u.drop, ms.drop, std::min(ms.to_drop, 99.f), ms.gap ? 1.f : 0.f, ms.energy);
        set(u.look, intensity, flash, limiter.gain(), has_cover ? 1.f : 0.f);
        const float ring = scenes[previous].ring + (float(scenes[current].ring) - float(scenes[previous].ring)) * transition;
        set(u.post, 0.08f + 0.04f * f.energy, float(std::max<size_t>(1, bloom.size())), 0.018f, ring);
        u.key[3] = ts.quality >= 0.5f ? 0.0015f + 0.005f * f.kick * intensity + 0.008f * ms.drop * flash : 0.f;

        float sp[Features::kBins * 2];
        for (size_t i = 0; i < Features::kBins; ++i) sp[2 * i] = f.spectrum()[i], sp[2 * i + 1] = f.spectrum_peaks()[i];
        ctx->UpdateSubresource(spec.tex.Get(), 0, nullptr, sp, UINT(sizeof sp), 0);
        ctx->UpdateSubresource(wave.tex.Get(), 0, nullptr, f.wave().data(), UINT(Features::kWave * sizeof(float)), 0);

        Timer& tm = timers[frame % std::size(timers)];
        const bool timing = !tm.issued;  // a query still in flight from four frames ago is skipped, not overwritten
        if (timing) ctx->Begin(tm.disjoint.Get()), ctx->End(tm.t0.Get());

        const int prev = cur;
        cur ^= 1;
        pass(scenes[current].ps.Get(), comp[cur], comp[prev], nullptr, false);
        const Target* src = &comp[cur];
        for (auto& b : bloom) pass(ps_down.Get(), b, *src, nullptr, false), src = &b;
        if (!bloom.empty()) pass(ps_lum.Get(), lum, bloom.back(), nullptr, false);
        for (size_t i = bloom.size(); i-- > 1;) pass(ps_up.Get(), bloom[i - 1], bloom[i], nullptr, true);
        ctx->CopyResource(lum_stage[frame % 3].Get(), lum.tex.Get());
        final_pass(out);
        if (timing) ctx->End(tm.t1.Get()), ctx->End(tm.disjoint.Get()), tm.issued = true;
        ++frame;
    }

    void set_cover(const uint8_t* bgra, int w, int h) {
        has_cover = false;
        cover = {};
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
    if (ok()) p_->switch_to(p_->current + 1);
}
void DasVis::previous() {
    if (ok()) p_->switch_to(p_->current + p_->scenes.size() - 1);
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
float DasVis::scale() const { return p_->qc.scale(); }
float DasVis::gpu_ms() const { return p_->gpu_ms; }
bool DasVis::tier_settled() const { return p_->qc.settled(); }
float DasVis::flash_budget() const { return p_->limiter.budget(); }
double DasVis::map_offset() const { return p_->map_offset; }

const char* library_source() { return k_library; }

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
