#include "dasvis/palette.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace wb::dasvis {
namespace {

struct Lab {
    float L = 0, a = 0, b = 0;
};

float to_linear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

// Oklab (Björn Ottosson, 2020), from and to linear sRGB.
Lab oklab(const Rgb& c) {
    const float l = std::cbrt(0.4122214708f * c[0] + 0.5363325363f * c[1] + 0.0514459929f * c[2]);
    const float m = std::cbrt(0.2119034982f * c[0] + 0.6806995451f * c[1] + 0.1073969566f * c[2]);
    const float s = std::cbrt(0.0883024619f * c[0] + 0.2817188376f * c[1] + 0.6299787005f * c[2]);
    return {0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s, 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
            0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s};
}

Rgb rgb(const Lab& c) {
    const float l = std::pow(c.L + 0.3963377774f * c.a + 0.2158037573f * c.b, 3.f);
    const float m = std::pow(c.L - 0.1055613458f * c.a - 0.0638541728f * c.b, 3.f);
    const float s = std::pow(c.L - 0.0894841775f * c.a - 1.2914855480f * c.b, 3.f);
    return {4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s, -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s,
            -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s};
}

// The colour at lightness L with the hue of `c` and as much of its chroma as fits in sRGB.
Rgb in_gamut(Lab c) {
    const float a = c.a, b = c.b;
    float lo = 0, hi = 1;
    for (int i = 0; i < 12; ++i) {
        const float k = (lo + hi) / 2;
        c.a = a * k, c.b = b * k;
        const Rgb r = rgb(c);
        const bool ok = std::all_of(r.begin(), r.end(), [](float v) { return v >= 0 && v <= 1; });
        (ok ? lo : hi) = k;
    }
    c.a = a * lo, c.b = b * lo;
    Rgb r = rgb(c);
    for (float& v : r) v = std::clamp(v, 0.f, 1.f);
    return r;
}

float chroma(const Lab& c) { return std::sqrt(c.a * c.a + c.b * c.b); }

void hsv(float h, float s, float v, Rgb& out) {
    h = h - std::floor(h);
    for (int i = 0; i < 3; ++i) {
        const float k = std::fmod((i == 0 ? 5.f : i == 1 ? 3.f : 1.f) + h * 6, 6.f);
        const float c = v - v * s * std::max(0.f, std::min({k, 4 - k, 1.f}));
        out[size_t(i)] = c * c;  // to linear (≈ gamma 2)
    }
}

// The key's hue on the Camelot wheel; minor keys are nudged towards blue (cooler).
float key_hue(const std::optional<std::string>& camelot, float seed, bool& minor) {
    minor = false;
    if (!camelot || camelot->size() < 2) return seed;
    const int n = std::atoi(camelot->c_str());
    if (n < 1 || n > 12) return seed;
    minor = camelot->back() == 'A';
    float h = float(n - 1) / 12.f;
    if (minor) {  // the shortest way round the wheel towards blue (0.62), a sixth of the way
        float d = 0.62f - h;
        d -= std::round(d);
        h += d / 6;
    }
    return h;
}

}  // namespace

std::optional<Palette> cover_palette(const uint8_t* bgra, int w, int h) {
    if (!bgra || w <= 0 || h <= 0) return std::nullopt;
    // A thumbnail of at most 64 × 64: each pixel the average of its block, in linear light.
    const int tw = std::min(64, w), th = std::min(64, h);
    std::vector<Lab> px;
    px.reserve(size_t(tw) * size_t(th));
    for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; ++x) {
            const int x0 = x * w / tw, x1 = std::max(x0 + 1, (x + 1) * w / tw), y0 = y * h / th, y1 = std::max(y0 + 1, (y + 1) * h / th);
            Rgb sum{};
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx) {
                    const uint8_t* p = bgra + (size_t(yy) * size_t(w) + size_t(xx)) * 4;
                    sum[0] += to_linear(p[2] / 255.f), sum[1] += to_linear(p[1] / 255.f), sum[2] += to_linear(p[0] / 255.f);
                }
            const float n = float((x1 - x0) * (y1 - y0));
            px.push_back(oklab({sum[0] / n, sum[1] / n, sum[2] / n}));
        }

    // k-means, k = 5, started from the lightness quantiles (deterministic: the same cover always gives the same colours).
    constexpr int K = 5;
    std::vector<Lab> sorted = px;
    std::sort(sorted.begin(), sorted.end(), [](const Lab& a, const Lab& b) { return a.L < b.L; });
    std::array<Lab, K> c;
    for (int k = 0; k < K; ++k) c[size_t(k)] = sorted[size_t((2 * k + 1) * sorted.size() / (2 * K))];
    std::vector<int> owner(px.size(), 0);
    std::array<int, K> count{};
    for (int it = 0; it < 12; ++it) {
        for (size_t i = 0; i < px.size(); ++i) {
            float best = 1e9f;
            for (int k = 0; k < K; ++k) {
                const float dl = px[i].L - c[size_t(k)].L, da = px[i].a - c[size_t(k)].a, db = px[i].b - c[size_t(k)].b;
                const float d = dl * dl + 2 * (da * da + db * db);  // hue counts double: palettes are about colour
                if (d < best) best = d, owner[i] = k;
            }
        }
        std::array<Lab, K> sum{};
        count = {};
        for (size_t i = 0; i < px.size(); ++i) {
            Lab& s = sum[size_t(owner[i])];
            s.L += px[i].L, s.a += px[i].a, s.b += px[i].b;
            ++count[size_t(owner[i])];
        }
        for (int k = 0; k < K; ++k)
            if (count[size_t(k)]) c[size_t(k)] = {sum[size_t(k)].L / float(count[size_t(k)]), sum[size_t(k)].a / float(count[size_t(k)]), sum[size_t(k)].b / float(count[size_t(k)])};
    }

    // A grey cover (black and white, or nearly): its colours wouldn't carry a picture.
    float weighted = 0;
    for (int k = 0; k < K; ++k) weighted += chroma(c[size_t(k)]) * float(count[size_t(k)]);
    if (weighted / float(px.size()) < 0.035f && std::all_of(c.begin(), c.end(), [](const Lab& x) { return chroma(x) < 0.08f; })) return std::nullopt;

    // Dark → light, then onto a fixed lightness ladder with the chroma lifted a little (pictures are made of light: a
    // cover's muddy middle tones would look dull), each kept inside sRGB.
    std::array<int, K> order{0, 1, 2, 3, 4};
    std::sort(order.begin(), order.end(), [&](int a, int b) { return c[size_t(a)].L < c[size_t(b)].L; });
    const float ladder[K] = {0.32f, 0.5f, 0.64f, 0.77f, 0.9f};
    Palette out;
    for (int k = 0; k < K; ++k) {
        Lab x = c[size_t(order[size_t(k)])];
        const float cr = chroma(x), want = std::min(0.32f, std::max(cr * 1.3f, cr > 0.02f ? 0.06f : cr));
        if (cr > 1e-4f) x.a *= want / cr, x.b *= want / cr;
        x.L = ladder[k];
        out[size_t(k)] = in_gamut(x);
    }
    return out;
}

Palette key_palette(const std::optional<std::string>& camelot, float seed) {
    bool minor = false;
    const float h = key_hue(camelot, seed, minor);
    // Neighbouring hues, dark → light, and one accent opposite (the brightest): colours that sit together.
    const float s = minor ? 0.85f : 1.f;
    const float offs[] = {0.0f, -0.05f, 0.06f, 0.12f, 0.5f}, sats[] = {0.85f, 0.8f, 0.7f, 0.55f, 0.6f}, vals[] = {0.4f, 0.62f, 0.8f, 0.92f, 1.f};
    Palette p;
    for (size_t i = 0; i < 5; ++i) hsv(h + offs[i], sats[i] * s, vals[i], p[i]);
    return p;
}

Rgb key_tint(const std::optional<std::string>& camelot, float seed) {
    bool minor = false;
    Rgb t;
    hsv(key_hue(camelot, seed, minor), minor ? 0.35f : 0.45f, 1.f, t);
    return t;
}

}  // namespace wb::dasvis
