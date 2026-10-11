#include "dasvis/songmap.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <numbers>

#include "engine/decode.h"
#include "engine/fft.h"

namespace wb::dasvis {
namespace {

constexpr size_t kN = 2048, kBins = kN / 2, kHop = 512, kMed = 17, kHalf = kMed / 2;
constexpr double kBinHz = SongMap::kRate / double(kN);  // ~10.8 Hz
constexpr size_t kMel = 32, kChroma = 12;
// When an onset seen in frame t happened: a little after the frame's window centre. The flux (the change from the frame
// before) peaks while the hit is still in the window's later half. Measured on synthetic kicks in the tests: 7.3 ms.
constexpr double kOnsetLag = -0.0073;

size_t bin_of(double hz) { return std::clamp<size_t>(size_t(hz / kBinHz + 0.5), 1, kBins - 1); }
double frame_time(double f) { return (f * double(kHop) + kN / 2.0) / SongMap::kRate - kOnsetLag; }

float percentile(std::vector<float> v, double p) {
    if (v.empty()) return 0;
    const size_t k = std::min(v.size() - 1, size_t(p * double(v.size() - 1)));
    std::nth_element(v.begin(), v.begin() + long(k), v.end());
    return v[k];
}

double fold_bpm(double b) {
    while (b > 0 && b < 70) b *= 2;
    while (b > 180) b /= 2;
    return b;
}

uint64_t fnv1a64(const std::string& s) {
    uint64_t h = 14695981039346656037ull;
    for (const unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}

}  // namespace

const char* section_name(Section s) {
    static const char* names[] = {"Intro", "Verse", "Build-up", "Drop", "Breakdown", "Chorus", "Outro"};
    return names[int(s)];
}

// MARK: Building

struct MapBuilder::Impl {
    double known_bpm;
    RealFft fft{kN};
    std::vector<float> window = std::vector<float>(kN), frame = std::vector<float>(kN), tmp = std::vector<float>(kMed);
    std::vector<float> buf;  // samples not yet framed, from buf_at
    size_t buf_at = 0, samples = 0;
    std::vector<std::vector<float>> ring = std::vector<std::vector<float>>(kMed, std::vector<float>(kBins));  // the last 17 spectra
    size_t frames_in = 0;
    std::vector<float> prev_log = std::vector<float>(kBins);
    std::array<std::pair<size_t, size_t>, kMel> mel_bands{};
    std::vector<int> pitch_class = std::vector<int>(kBins, -1);
    // Per frame:
    std::vector<float> onset, rms;
    std::vector<std::array<float, kMel>> mel;
    std::vector<std::array<float, kChroma>> chroma;
    std::array<std::vector<float>, 5> level;  // the pseudo-stems' band levels

    explicit Impl(double bpm) : known_bpm(bpm) {
        for (size_t i = 0; i < kN; ++i) window[i] = 0.5f - 0.5f * std::cos(2 * std::numbers::pi_v<float> * float(i) / float(kN - 1));
        for (size_t b = 0; b < kMel; ++b) {  // log-spaced 40 Hz – 10 kHz
            const double lo = 40 * std::pow(250.0, double(b) / kMel), hi = 40 * std::pow(250.0, double(b + 1) / kMel);
            mel_bands[b] = {bin_of(lo), std::max(bin_of(lo), bin_of(hi) - 1)};
        }
        for (size_t b = bin_of(55); b <= bin_of(5000); ++b)
            pitch_class[b] = ((int(std::lround(12 * std::log2(double(b) * kBinHz / 440.0))) + 69) % 12 + 12) % 12;
    }

    void push(const float* s, size_t n) {
        buf.insert(buf.end(), s, s + n);
        samples += n;
        while (buf.size() - buf_at >= kN) {
            analyse_frame(&buf[buf_at]);
            buf_at += kHop;
        }
        if (buf_at > (1u << 16)) buf.erase(buf.begin(), buf.begin() + long(buf_at)), buf_at = 0;
    }

    void analyse_frame(const float* x) {
        double sq = 0;
        for (size_t i = 0; i < kN; ++i) frame[i] = x[i] * window[i], sq += double(x[i]) * x[i];
        rms.push_back(float(std::sqrt(sq / kN)));
        std::vector<float>& mags = ring[frames_in % kMed];
        fft.magnitudes(frame.data(), mags.data());
        float flux = 0;
        for (size_t b = 1; b < kBins; ++b) {
            const float lg = std::log1p(mags[b]);
            flux += std::max(0.f, lg - prev_log[b]);
            prev_log[b] = lg;
        }
        onset.push_back(flux);
        std::array<float, kMel> m{};
        for (size_t b = 0; b < kMel; ++b) {
            double e = 0;
            for (size_t k = mel_bands[b].first; k <= mel_bands[b].second; ++k) e += double(mags[k]) * mags[k];
            m[b] = float(std::log1p(e / double(mel_bands[b].second - mel_bands[b].first + 1)));
        }
        mel.push_back(m);
        std::array<float, kChroma> c{};
        for (size_t b = 0; b < kBins; ++b)
            if (pitch_class[b] >= 0) c[size_t(pitch_class[b])] += mags[b] * mags[b];
        chroma.push_back(c);
        ++frames_in;
        if (frames_in > kHalf) separate();
    }

    // Harmonic–percussive separation of the frame in the middle of the 17 held (Fitzgerald 2010): a median along time
    // keeps what's held (harmonic), a median along frequency keeps what's struck (percussive); soft masks split the
    // frame, and the stems are band levels of the two parts.
    void separate() {
        const std::vector<float>& c = ring[(frames_in - 1 - kHalf) % kMed];
        std::array<double, 5> sum{};
        std::array<int, 5> count{};
        const auto add = [&](int stem, float v) { sum[size_t(stem)] += double(v) * v, ++count[size_t(stem)]; };
        static const size_t kick_lo = bin_of(40), kick_hi = bin_of(120), sn_lo = bin_of(150), sn_hi = bin_of(400), sn2_lo = bin_of(1500),
                            sn2_hi = bin_of(4000), hat_lo = bin_of(6000), hat_hi = bin_of(10500), bass_lo = bin_of(40), bass_hi = bin_of(250),
                            lead_lo = bin_of(300), lead_hi = bin_of(4000);
        for (size_t b = 1; b < kBins; ++b) {
            for (size_t k = 0; k < kMed; ++k) tmp[k] = ring[k][b];
            std::nth_element(tmp.begin(), tmp.begin() + kHalf, tmp.end());
            const float h = tmp[kHalf];
            const size_t lo = b > kHalf ? b - kHalf : 1, hi = std::min(kBins - 1, b + kHalf);
            std::copy(c.begin() + long(lo), c.begin() + long(hi) + 1, tmp.begin());
            const size_t n = hi - lo + 1, mid = n / 2;
            std::nth_element(tmp.begin(), tmp.begin() + long(mid), tmp.begin() + long(n));
            const float p = tmp[mid];
            const float mh = h * h / (h * h + p * p + 1e-12f);
            const float harm = c[b] * mh, perc = c[b] * (1 - mh);
            if (b >= kick_lo && b <= kick_hi) add(SongMap::kick, perc);
            if ((b >= sn_lo && b <= sn_hi) || (b >= sn2_lo && b <= sn2_hi)) add(SongMap::snare, perc);
            if (b >= hat_lo && b <= hat_hi) add(SongMap::hats, perc);
            if (b >= bass_lo && b <= bass_hi) add(SongMap::bass, harm);
            if (b >= lead_lo && b <= lead_hi) add(SongMap::lead, harm);
        }
        for (size_t s = 0; s < 5; ++s) level[s].push_back(float(std::sqrt(sum[s] / std::max(1, count[s]))));
    }

    SongMap finish();
};

MapBuilder::MapBuilder(double known_bpm) : p_(new Impl(known_bpm)) {}
MapBuilder::~MapBuilder() { delete p_; }
void MapBuilder::push(const float* samples, size_t n) { p_->push(samples, n); }
SongMap MapBuilder::finish() { return p_->finish(); }

SongMap MapBuilder::Impl::finish() {
    SongMap m;
    m.duration = double(samples) / SongMap::kRate;
    const size_t F = onset.size();
    // The last frames' separation: empty spectra after the end.
    for (size_t k = 0; k < kHalf && F; ++k) std::fill(ring[frames_in % kMed].begin(), ring[frames_in % kMed].end(), 0.f), ++frames_in, separate();
    if (F < 200) return m;  // under ~5 s: nothing to map
    const double fps = SongMap::kFps;

    // Stem onsets (rise of the log level, normalised by its loud end) → envelopes and hits.
    std::array<std::vector<float>, 5> on;
    for (size_t s = 0; s < 5; ++s) {
        const float ref = std::max(1e-6f, percentile(level[s], 0.99));
        on[s].assign(F, 0.f);
        float prev = 0;
        for (size_t t = 0; t < F; ++t) {
            const float lg = std::log1p(10 * level[s][t] / ref);
            on[s][t] = std::max(0.f, lg - prev);
            prev = lg;
        }
    }
    const float release[] = {0.12f, 0.15f, 0.06f};
    const double refractory[] = {0.15, 0.1, 0.06};
    for (size_t s = 0; s < 5; ++s) {
        m.env[s].resize(F);
        if (s < 3) {  // drums: a hit's envelope jumps up and falls away
            const float ref = std::max(1e-6f, percentile(on[s], 0.995));
            float e = 0;
            const float decay = std::exp(-float(1 / fps) / release[s]);
            double last = -1;
            for (size_t t = 0; t < F; ++t) {
                e = std::max(e * decay, std::min(1.f, on[s][t] / ref));
                m.env[s][t] = uint8_t(std::lround(e * 255));
                // A hit: a local peak of the onset, well above its neighbourhood and the song's floor, not too soon.
                const size_t a = t > 10 ? t - 10 : 0, b = std::min(F, t + 11);
                float local = 0;
                for (size_t k = a; k < b; ++k) local += on[s][k];
                local /= float(b - a);
                const bool peak = (t == 0 || on[s][t] >= on[s][t - 1]) && (t + 1 >= F || on[s][t] > on[s][t + 1]);
                const double when = frame_time(double(t));
                if (peak && on[s][t] > 0.12f * ref && on[s][t] > 1.6f * local && when - last >= refractory[s])
                    m.hits[s].push_back(when), last = when;
            }
        } else {  // bass, lead: a smoothed level
            const float ref = std::max(1e-6f, percentile(level[s], 0.98));
            float e = 0;
            for (size_t t = 0; t < F; ++t) {
                const float n = std::min(1.f, level[s][t] / ref);
                e += (n - e) * (1 - std::exp(-float(1 / fps) / (n > e ? 0.03f : 0.3f)));
                m.env[s][t] = uint8_t(std::lround(e * 255));
            }
        }
    }

    // What the beat is felt in: the kick, the snare and the bass first, with a little of the whole spectrum's flux for
    // music without drums. (The whole-spectrum flux alone is ruled by bright hats, which are often off the beat.)
    std::vector<float> o(F, 0.f);
    {
        const auto add = [&](const std::vector<float>& v, float w) {
            double mean = 0, var = 0;
            for (const float x : v) mean += x;
            mean /= double(F);
            for (const float x : v) var += (x - mean) * (x - mean);
            const float sd = float(std::sqrt(var / double(F))) + 1e-6f;
            for (size_t t = 0; t < F; ++t) o[t] += w * v[t] / sd;
        };
        add(on[SongMap::kick], 1.f), add(on[SongMap::snare], 0.6f), add(on[SongMap::bass], 0.4f), add(onset, 0.3f);
    }
    // The tempo: the library's when known; otherwise the lag at which the onsets best repeat, counting its multiples
    // too (a beat's lag repeats at 2, 3, 4 beats; an off-beat pattern's lag doesn't), 70–180 BPM, favouring ~120.
    double bpm = fold_bpm(known_bpm);
    if (bpm <= 0) {
        std::vector<float> d(F);  // onsets above their local mean
        for (size_t t = 0; t < F; ++t) {
            const size_t a = t > 8 ? t - 8 : 0, b = std::min(F, t + 9);
            float mean = 0;
            for (size_t k = a; k < b; ++k) mean += o[k];
            d[t] = std::max(0.f, o[t] - mean / float(b - a));
        }
        const size_t lo = size_t(60 * fps / 180), hi = size_t(60 * fps / 70) + 1;
        std::vector<double> ac(4 * (hi + 1) + 1, 0);
        for (size_t lag = lo; lag < ac.size() && lag < F; ++lag) {
            double s = 0;
            for (size_t t = 0; t + lag < F; ++t) s += double(d[t]) * d[t + lag];
            ac[lag] = s / double(F - lag);
        }
        std::vector<double> score(hi + 2, 0);
        for (size_t lag = lo; lag <= hi + 1; ++lag) {
            double s = 0;
            for (size_t k = 1; k <= 4; ++k) s += ac[std::min(ac.size() - 1, k * lag)] / double(k);
            const double b = 60 * fps / double(lag), w = std::log2(b / 120) / 0.9;
            score[lag] = s * std::exp(-0.5 * w * w);
        }
        size_t best = lo;
        for (size_t lag = lo; lag <= hi; ++lag)
            if (score[lag] > score[best]) best = lag;
        const double a = score[best - 1], b = score[best], c = score[best + 1], den = a - 2 * b + c;
        const double lag = double(best) + (std::fabs(den) > 1e-12 ? std::clamp(0.5 * (a - c) / den, -0.5, 0.5) : 0.0);
        bpm = fold_bpm(60 * fps / lag);
    }
    m.bpm = bpm;

    // The beats: dynamic programming (Ellis 2007). Each frame's score is its onset plus the best earlier beat's score,
    // less a penalty for a gap that isn't one beat long; the best path, traced back from the end, is the beat grid.
    const double period = 60 * fps / bpm;
    std::vector<double> cum(F);
    std::vector<int> back(F, -1);
    const double tight = 200;
    for (size_t t = 0; t < F; ++t) {
        double best = -1e300;
        int arg = -1;
        const long from = long(t) - long(std::lround(2 * period)), to = long(t) - long(std::lround(period / 2));
        for (long u = std::max(0L, from); u <= to; ++u) {
            const double r = std::log(double(long(t) - u) / period), v = cum[size_t(u)] - tight * r * r;
            if (v > best) best = v, arg = int(u);
        }
        cum[t] = o[t] + (arg >= 0 && best > 0 ? best : 0);
        back[t] = arg >= 0 && best > 0 ? arg : -1;
    }
    size_t end = F - 1;
    for (size_t t = F > size_t(period) ? F - size_t(period) : 0; t < F; ++t)
        if (cum[t] > cum[end]) end = t;
    std::vector<size_t> beat_frames;
    for (long t = long(end); t >= 0; t = back[size_t(t)]) beat_frames.push_back(size_t(t));
    std::reverse(beat_frames.begin(), beat_frames.end());
    for (const size_t t : beat_frames) {  // to a fraction of a frame: the parabola through the onset peak
        double d = 0;
        if (t > 0 && t + 1 < F) {
            const double a = o[t - 1], b = o[t], c = o[t + 1], den = a - 2 * b + c;
            if (den < 0) d = std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
        }
        m.beats.push_back(frame_time(double(t) + d));
    }
    const size_t B = m.beats.size();
    if (B < 16) return m;
    {  // The tempo the beats really keep: the slope of a straight line through them (finer than one frame's lag).
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (size_t i = 0; i < B; ++i) sx += double(i), sy += m.beats[i], sxx += double(i) * double(i), sxy += double(i) * m.beats[i];
        const double slope = (double(B) * sxy - sx * sy) / (double(B) * sxx - sx * sx);
        if (slope > 0) m.bpm = 60 / slope;
    }

    // Per beat: frame span, energy, and timbre / harmony (for sections), from the frames inside it.
    std::vector<size_t> bf(B + 1);
    for (size_t i = 0; i < B; ++i) bf[i] = std::min(F - 1, beat_frames[i]);
    bf[B] = F;
    std::vector<float> energy(B);
    std::vector<std::array<float, kMel + kChroma>> feat(B);
    std::vector<std::array<float, kChroma>> chroma_b(B);
    for (size_t i = 0; i < B; ++i) {
        const size_t a = bf[i], b = std::max(a + 1, bf[i + 1]);
        double e = 0;
        std::array<double, kMel + kChroma> f{};
        for (size_t t = a; t < b && t < F; ++t) {
            e += rms[t];
            for (size_t k = 0; k < kMel; ++k) f[k] += mel[t][k];
            double cs = 0;
            for (size_t k = 0; k < kChroma; ++k) cs += chroma[t][k];
            for (size_t k = 0; k < kChroma; ++k) f[kMel + k] += cs > 0 ? chroma[t][k] / cs : 0;
        }
        const double n = double(b - a);
        energy[i] = float(e / n);
        for (size_t k = 0; k < f.size(); ++k) feat[i][k] = float(f[k] / n);
        for (size_t k = 0; k < kChroma; ++k) chroma_b[i][k] = feat[i][kMel + k];
    }

    // Downbeats: the beat offset (0–3) where the bass and the harmony change, and the kick is strongest.
    {
        const float ref_b = std::max(1e-6f, percentile(on[SongMap::bass], 0.99)), ref_k = std::max(1e-6f, percentile(on[SongMap::kick], 0.99));
        std::array<double, 4> score{};
        std::array<int, 4> n{};
        for (size_t i = 1; i < B; ++i) {
            float bass = 0, kick = 0;
            for (size_t t = bf[i] > 2 ? bf[i] - 2 : 0; t <= std::min(F - 1, bf[i] + 2); ++t)
                bass = std::max(bass, on[SongMap::bass][t]), kick = std::max(kick, on[SongMap::kick][t]);
            double dot = 0, na = 0, nb = 0;
            for (size_t k = 0; k < kChroma; ++k)
                dot += chroma_b[i][k] * chroma_b[i - 1][k], na += chroma_b[i][k] * chroma_b[i][k], nb += chroma_b[i - 1][k] * chroma_b[i - 1][k];
            const double change = 1 - dot / (std::sqrt(na * nb) + 1e-12);
            score[i % 4] += bass / ref_b + change + 0.3 * kick / ref_k;
            ++n[i % 4];
        }
        int best = 0;
        for (int k = 1; k < 4; ++k)
            if (score[size_t(k)] / std::max(1, n[size_t(k)]) > score[size_t(best)] / std::max(1, n[size_t(best)])) best = k;
        m.downbeat = best;
    }

    // Sections: novelty of the beat-synchronous features (Foote's checkerboard kernel over their self-similarity,
    // worked out band by band, never as a whole matrix) plus energy changes; peaks snapped to 4-bar boundaries.
    for (size_t k = 0; k < kMel + kChroma; ++k) {  // z-score each feature over the song, then unit length per beat
        double mean = 0, var = 0;
        for (size_t i = 0; i < B; ++i) mean += feat[i][k];
        mean /= double(B);
        for (size_t i = 0; i < B; ++i) var += (feat[i][k] - mean) * (feat[i][k] - mean);
        const double sd = std::sqrt(var / double(B)) + 1e-9;
        for (size_t i = 0; i < B; ++i) feat[i][k] = float((feat[i][k] - mean) / sd);
    }
    for (auto& f : feat) {
        double n = 0;
        for (const float v : f) n += double(v) * v;
        n = std::sqrt(n) + 1e-9;
        for (float& v : f) v = float(v / n);
    }
    const auto sim = [&](size_t a, size_t b) {
        double s = 0;
        for (size_t k = 0; k < feat[a].size(); ++k) s += double(feat[a][k]) * feat[b][k];
        return s;
    };
    constexpr long L = 16;  // a 32-beat (8-bar) kernel: wide enough not to cut on a 4-bar chord cycle
    std::vector<double> nov(B, 0), enov(B, 0);
    for (size_t i = 0; i < B; ++i) {
        double s = 0;
        for (long a = -L; a < L; ++a)
            for (long b = -L; b < L; ++b) {
                const long x = long(i) + a, y = long(i) + b;
                if (x < 0 || y < 0 || x >= long(B) || y >= long(B)) continue;
                const double ga = (double(a) + 0.5) / (L / 2.0), gb = (double(b) + 0.5) / (L / 2.0);
                s += ((a < 0) == (b < 0) ? 1 : -1) * std::exp(-0.5 * (ga * ga + gb * gb)) * sim(size_t(x), size_t(y));
            }
        nov[i] = std::max(0.0, s);
        double before = 0, after = 0;
        int nb = 0, na = 0;
        for (long k = 1; k <= L; ++k) {
            if (long(i) - k >= 0) before += energy[i - size_t(k)], ++nb;
            if (i + size_t(k) - 1 < B) after += energy[i + size_t(k) - 1], ++na;
        }
        if (nb && na) enov[i] = std::fabs(after / na - before / nb);
    }
    const double nmax = *std::max_element(nov.begin(), nov.end()) + 1e-12, emax = *std::max_element(enov.begin(), enov.end()) + 1e-12;
    std::vector<double> score(B);
    double mean = 0, var = 0;
    for (size_t i = 0; i < B; ++i) score[i] = nov[i] / nmax + enov[i] / emax, mean += score[i];
    mean /= double(B);
    for (const double v : score) var += (v - mean) * (v - mean);
    const double thresh = mean + 0.75 * std::sqrt(var / double(B));
    std::vector<size_t> cuts{0};
    for (size_t i = size_t(L); i + size_t(L) < B; ++i) {
        bool top = score[i] > thresh;
        for (long k = -L; k <= L && top; ++k) top = k == 0 || score[size_t(long(i) + k)] <= score[i];
        if (!top) continue;
        const long rel = long(i) - m.downbeat;  // snap to the nearest 4-bar boundary
        long snapped = m.downbeat + long(std::lround(double(rel) / 16)) * 16;
        // …unless a clear step in loudness (×2 or more, bar against bar) lies within a bar of it: then the part starts
        // there. A downbeat guessed a beat or two off would otherwise start a drop with a bar of the quiet before it.
        double best = 2;
        for (long k = snapped - 4; k <= snapped + 4; ++k) {
            if (k < 4 || k + 4 > long(B)) continue;
            double before = 0, after = 0;
            for (long j = 0; j < 4; ++j) before += energy[size_t(k - 4 + j)], after += energy[size_t(k + j)];
            if (after > best * (before + 1e-9)) best = after / (before + 1e-9), snapped = k;
        }
        if (snapped > long(cuts.back()) + 8 && snapped + 8 < long(B)) cuts.push_back(size_t(snapped));
    }
    cuts.push_back(B);

    // Each part: its energy, kick density, how its energy and snare / hat density change from start to end.
    struct Stats {
        float energy, kicks, rise, busier, late_rate;  // late_rate: snare + hat hits a beat in its second half
    };
    std::vector<Stats> st;
    const auto count_hits = [&](int s, double a, double b) {
        return float(std::count_if(m.hits[size_t(s)].begin(), m.hits[size_t(s)].end(), [&](double t) { return t >= a && t < b; }));
    };
    const auto time_of = [&](size_t beat) { return beat < B ? m.beats[beat] : m.duration; };
    float emax_part = 1e-9f;
    for (size_t p = 0; p + 1 < cuts.size(); ++p) {
        const size_t a = cuts[p], b = cuts[p + 1], q = std::max<size_t>(1, (b - a) / 4);
        double e = 0, first = 0, last = 0;
        for (size_t i = a; i < b; ++i) e += energy[i];
        for (size_t i = a; i < a + q; ++i) first += energy[i];
        for (size_t i = b - q; i < b; ++i) last += energy[i];
        const double t0 = time_of(a), t1 = time_of(b), mid = (t0 + t1) / 2;
        const float early = count_hits(SongMap::snare, t0, mid) + count_hits(SongMap::hats, t0, mid);
        const float late = count_hits(SongMap::snare, mid, t1) + count_hits(SongMap::hats, mid, t1);
        st.push_back({float(e / double(b - a)), count_hits(SongMap::kick, t0, t1) / float(b - a), float(last / (first + 1e-9)),
                      (late + 1) / (early + 1), late / std::max(1.f, float(b - a) / 2)});
        emax_part = std::max(emax_part, st.back().energy);
    }
    const size_t P = st.size();
    std::vector<Section> kind(P, Section::verse);
    for (size_t p = 0; p < P; ++p) {  // drops first: loud, kicking, and a big step up from the part before
        const float rel = st[p].energy / emax_part;
        if (p > 0 && rel >= 0.75f && st[p].kicks >= 0.5f && st[p].energy >= 1.35f * st[p - 1].energy) kind[p] = Section::drop;
    }
    for (size_t p = 0; p < P; ++p) {
        if (kind[p] == Section::drop) continue;
        const float rel = st[p].energy / emax_part;
        const bool before_drop = p + 1 < P && kind[p + 1] == Section::drop;
        // A build-up rises into the drop: louder, or its snare / hats speeding up to more than one and a half a beat.
        if (before_drop && (st[p].rise > 1.15f || (st[p].busier > 1.3f && st[p].late_rate >= 1.5f))) kind[p] = Section::build;
        else if (p == 0 && rel < 0.8f) kind[p] = Section::intro;
        else if (p + 1 == P && rel < 0.8f) kind[p] = Section::outro;
        else if (rel < 0.6f && st[p].kicks < 0.3f) kind[p] = Section::breakdown;
        else kind[p] = rel >= 0.8f ? Section::chorus : Section::verse;
    }
    for (size_t p = 0; p < P; ++p) {
        const double t0 = p == 0 ? 0.0 : time_of(cuts[p]), t1 = p + 1 == P ? m.duration : time_of(cuts[p + 1]);
        if (!m.parts.empty() && m.parts.back().kind == kind[p] && kind[p] != Section::drop) {  // the same again: one part
            auto& last = m.parts.back();
            const double w0 = last.end - last.start, w1 = t1 - t0;
            last.energy = float((last.energy * w0 + st[p].energy / emax_part * w1) / std::max(1e-9, w0 + w1));
            last.end = t1;
            continue;
        }
        m.parts.push_back({t0, t1, kind[p], st[p].energy / emax_part});
        if (kind[p] != Section::drop) continue;
        m.drops.push_back(t0);
        // The breath before the drop: the last one or two bars much quieter than the bars before them.
        const size_t k = cuts[p];
        const auto avg = [&](size_t a, size_t b) {
            double s = 0;
            for (size_t i = a; i < b; ++i) s += energy[i];
            return b > a ? s / double(b - a) : 0.0;
        };
        double gap = -1;
        if (k >= 16 && avg(k - 8, k) < 0.45 * avg(k - 16, k - 8)) gap = m.beats[k - 8];
        else if (k >= 8 && avg(k - 4, k) < 0.45 * avg(k - 8, k - 4)) gap = m.beats[k - 4];
        m.gaps.push_back(gap);
    }
    return m;
}

// MARK: Reading

SongMap::State SongMap::at(double t) const {
    State s;
    const size_t n = beats.size();
    if (n < 2) {
        s.beats = t * (bpm > 0 ? bpm : 120) / 60;
    } else {
        const size_t i = size_t(std::upper_bound(beats.begin(), beats.end(), t) - beats.begin());
        if (i == 0) s.beats = (t - beats[0]) / (beats[1] - beats[0]);
        else if (i >= n) s.beats = double(n - 1) + (t - beats[n - 1]) / (beats[n - 1] - beats[n - 2]);
        else s.beats = double(i - 1) + (t - beats[i - 1]) / (beats[i] - beats[i - 1]);
    }
    s.beat_phase = s.beats - std::floor(s.beats);
    const double rel = s.beats - downbeat;
    s.bar = long(std::floor(rel / 4));
    s.bar_phase = rel / 4 - std::floor(rel / 4);
    s.phrase_phase = rel / 64 - std::floor(rel / 64);
    const auto part = std::upper_bound(parts.begin(), parts.end(), t, [](double v, const Part& p) { return v < p.start; });
    if (part != parts.begin()) {
        const Part& p = *(part - 1);
        s.section = p.kind;
        s.section_t = float(std::clamp((t - p.start) / std::max(1e-6, p.end - p.start), 0.0, 1.0));
        s.energy = p.energy;
    }
    const double bar_s = bar_seconds();
    for (size_t k = 0; k < drops.size(); ++k) {
        if (drops[k] <= t && t - drops[k] < bar_s) s.drop = float(1 - (t - drops[k]) / bar_s);
        if (drops[k] > t) {
            s.to_drop = float((drops[k] - t) / bar_s);
            s.gap = gaps[k] >= 0 && t >= gaps[k];
            if (part != parts.begin() && (part - 1)->kind == Section::build)
                s.build = float(std::clamp((t - (part - 1)->start) / std::max(1e-6, drops[k] - (part - 1)->start), 0.0, 1.0));
            if (s.gap) s.build = 1;
            break;
        }
    }
    const double f = (t * kRate - kN / 2.0) / kHop + kOnsetLag * kFps;  // frame centres, as the hits are timed
    for (size_t k = 0; k < 5; ++k) {
        const auto& e = env[k];
        if (e.empty()) continue;
        const double c = std::clamp(f, 0.0, double(e.size() - 1));
        const size_t a = size_t(c), b = std::min(e.size() - 1, a + 1);
        s.stems[k] = float((e[a] + (e[b] - e[a]) * (c - double(a))) / 255.0);
    }
    return s;
}

int SongMap::hits_between(Stem stem, double t0, double t1) const {
    if (stem > hats) return 0;
    const auto& h = hits[size_t(stem)];
    return int(std::upper_bound(h.begin(), h.end(), t1) - std::upper_bound(h.begin(), h.end(), t0));
}

// MARK: The cache file

namespace {

constexpr char kMagic[4] = {'D', 'V', 'M', 'P'};

template <class T>
void put(std::ostream& o, const T& v) {
    o.write(reinterpret_cast<const char*>(&v), sizeof v);
}
template <class T>
void put_vec(std::ostream& o, const std::vector<T>& v) {
    put(o, uint32_t(v.size()));
    if (!v.empty()) o.write(reinterpret_cast<const char*>(v.data()), std::streamsize(v.size() * sizeof(T)));
}
template <class T>
bool get(std::istream& i, T& v) {
    return bool(i.read(reinterpret_cast<char*>(&v), sizeof v));
}
template <class T>
bool get_vec(std::istream& i, std::vector<T>& v) {
    uint32_t n = 0;
    if (!get(i, n) || n > (1u << 24)) return false;
    v.resize(n);
    return n == 0 || bool(i.read(reinterpret_cast<char*>(v.data()), std::streamsize(n * sizeof(T))));
}

}  // namespace

bool SongMap::save(const std::filesystem::path& file) const {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const auto tmp = std::filesystem::path(file).concat(L".tmp");
    {
        std::ofstream o(tmp, std::ios::binary);
        o.write(kMagic, 4);
        put(o, kVersion);
        put(o, duration), put(o, bpm), put(o, int32_t(downbeat));
        put_vec(o, beats);
        put(o, uint32_t(parts.size()));
        for (const Part& p : parts) put(o, p.start), put(o, p.end), put(o, uint8_t(p.kind)), put(o, p.energy);
        put_vec(o, drops), put_vec(o, gaps);
        for (const auto& e : env) put_vec(o, e);
        for (const auto& h : hits) put_vec(o, h);
        if (!o) return false;
    }
    std::filesystem::rename(tmp, file, ec);  // whole or not at all
    return !ec;
}

std::optional<SongMap> SongMap::load(const std::filesystem::path& file) {
    std::ifstream i(file, std::ios::binary);
    char magic[4]{};
    uint32_t version = 0, n = 0;
    int32_t down = 0;
    SongMap m;
    if (!i.read(magic, 4) || std::memcmp(magic, kMagic, 4) != 0 || !get(i, version) || version != kVersion) return std::nullopt;
    if (!get(i, m.duration) || !get(i, m.bpm) || !get(i, down) || !get_vec(i, m.beats) || !get(i, n) || n > 10000) return std::nullopt;
    m.downbeat = down;
    for (uint32_t k = 0; k < n; ++k) {
        Part p;
        uint8_t kind = 0;
        if (!get(i, p.start) || !get(i, p.end) || !get(i, kind) || !get(i, p.energy) || kind > uint8_t(Section::outro)) return std::nullopt;
        p.kind = Section(kind);
        m.parts.push_back(p);
    }
    if (!get_vec(i, m.drops) || !get_vec(i, m.gaps) || m.gaps.size() != m.drops.size()) return std::nullopt;
    for (auto& e : m.env)
        if (!get_vec(i, e)) return std::nullopt;
    for (auto& h : m.hits)
        if (!get_vec(i, h)) return std::nullopt;
    return m;
}

std::optional<SongMap> analyse_file(const std::filesystem::path& audio, double known_bpm, std::string* error) {
    try {
        MapBuilder b(known_bpm);
        decode_mono_stream(audio, [&](const float* s, size_t n) { b.push(s, n); });
        SongMap m = b.finish();
        if (m.beats.size() < 16) {
            if (error) *error = "too short, or no beat found";
            return std::nullopt;
        }
        return m;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return std::nullopt;
    }
}

std::filesystem::path map_cache_file(const std::filesystem::path& dir, const std::string& track_id, const std::filesystem::path& audio) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(audio, ec);
    const auto when = std::filesystem::last_write_time(audio, ec).time_since_epoch().count();
    char name[32];
    snprintf(name, sizeof name, "%016llx.map",
             static_cast<unsigned long long>(fnv1a64(track_id + "|" + std::to_string(size) + "|" + std::to_string(when))));
    return dir / name;
}

}  // namespace wb::dasvis
