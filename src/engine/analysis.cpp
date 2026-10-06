// Port of core/src/analysis.rs, kept line-for-line so results match the Rust engine.
//
// Tempo: spectral-flux onset envelope → autocorrelation with harmonic reinforcement → parabolic peak,
// folded into a DJ range (70–180) with the half / double tempo reported when it's a judgement call.
// Key: harmonic pitch-class profile from spectral peaks, Pearson-correlated with the EDMA key profiles
// (Faraldo et al. 2016, as used by Essentia's KeyExtractor), cross-checked with two other profiles.
//
// One difference from the Rust code: the spectrogram is processed frame by frame instead of being kept whole,
// which saves ~55 MB per 5-minute track. The math is unchanged.
#include "engine/analysis.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <string>

#include "engine/fft.h"

namespace wb {
namespace {

// Same algorithm as LLVM's __powidf2, which Rust's powi compiles to, so results agree to the last bit.
double powi(double a, int b) {
    double r = 1;
    for (;;) {
        if (b & 1) r *= a;
        b /= 2;
        if (b == 0) break;
        a *= a;
    }
    return r;
}

double round_to(double v, double scale) { return std::round(v * scale) / scale; }

// The Rust engine's tuning knobs (WB_BAND, WB_CAND, WB_KEY_*), kept so experiments carry over.
double env_number(const char* name, double fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    char* end = nullptr;
    const double d = std::strtod(v, &end);
    return end && *end == 0 ? d : fallback;
}

std::string env_string(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

// Calls on_frame(index, magnitudes[n/2]) for each Hann-windowed frame — the Rust stft(), one frame at a time.
template <class F>
size_t stft(std::span<const float> x, size_t n, size_t hop, F&& on_frame) {
    std::vector<float> w(n);
    for (size_t i = 0; i < n; ++i)
        w[i] = 0.5f - 0.5f * std::cos(2.0f * std::numbers::pi_v<float> * float(i) / float(n));
    RealFft fft(n);
    std::vector<float> buf(n), mag(n / 2);
    size_t frames = 0;
    for (size_t start = 0; start + n <= x.size(); start += hop, ++frames) {
        for (size_t i = 0; i < n; ++i) buf[i] = x[start + i] * w[i];
        fft.magnitudes(buf.data(), mag.data());
        on_frame(frames, mag);
    }
    return frames;
}

size_t frame_count(size_t len, size_t n, size_t hop) { return len < n ? 0 : (len - n) / hop + 1; }

constexpr std::array<double, 12> EDMA_MAJOR{1.00, 0.29, 0.50, 0.40, 0.60, 0.56, 0.32, 0.80, 0.31, 0.45, 0.42, 0.39};
constexpr std::array<double, 12> EDMA_MINOR{1.00, 0.31, 0.44, 0.58, 0.33, 0.49, 0.29, 0.78, 0.43, 0.29, 0.53, 0.32};
constexpr std::array<double, 12> BGATE_MAJOR{1.00, 0.00, 0.42, 0.00, 0.53, 0.37, 0.00, 0.77, 0.00, 0.38, 0.21, 0.30};
constexpr std::array<double, 12> BGATE_MINOR{1.00, 0.00, 0.36, 0.39, 0.00, 0.38, 0.00, 0.74, 0.27, 0.00, 0.42, 0.23};
constexpr std::array<double, 12> TEMPERLEY_MAJOR{5.0, 2.0, 3.5, 2.0, 4.5, 4.0, 2.0, 4.5, 2.0, 3.5, 1.5, 4.0};
constexpr std::array<double, 12> TEMPERLEY_MINOR{5.0, 2.0, 3.5, 4.5, 2.0, 4.0, 2.0, 4.5, 3.5, 2.0, 1.5, 4.0};
constexpr std::array<const char*, 12> NOTE_NAMES{"A", "Bb", "B", "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab"};

using Profile = std::array<double, 12>;

// Pitch-class profile indexed from A (0 = A, 1 = Bb, …), from spectral peaks and their harmonics.
std::optional<Profile> pitch_profile(std::span<const float> x, uint32_t rate) {
    const size_t n = size_t(env_number("WB_KEY_N", 8192.0));
    const size_t hop = n / 2;
    const size_t max_peaks = size_t(env_number("WB_KEY_PEAKS", 1e9));
    const int power = int(env_number("WB_KEY_POW", 2.0));
    const double win = env_number("WB_KEY_WIN", 4.0 / 3.0);
    const double fmin = env_number("WB_KEY_FMIN", 60.0);
    if (frame_count(x.size(), n, hop) == 0) return std::nullopt;

    const double bin_hz = double(rate) / double(n);
    const size_t lo = std::max<size_t>(size_t(fmin / bin_hz), 1);
    const size_t hi = std::min<size_t>(size_t(3500.0 / bin_hz), n / 2 - 2);
    Profile pcp{};
    std::vector<size_t> peaks;
    stft(x, n, hop, [&](size_t, const std::vector<float>& frame) {
        Profile fp{};
        float peak_f = 0.0f;
        for (size_t b = lo; b < hi; ++b) peak_f = std::max(peak_f, frame[b]);
        const double peak = peak_f;
        if (peak <= 0.0) return;
        peaks.clear();
        for (size_t b = lo; b < hi; ++b)
            if (frame[b] >= frame[b - 1] && frame[b] >= frame[b + 1] && double(frame[b]) >= peak * 1e-3) peaks.push_back(b);
        if (peaks.size() > max_peaks) {
            std::stable_sort(peaks.begin(), peaks.end(), [&](size_t a, size_t b) { return frame[a] > frame[b]; });
            peaks.resize(max_peaks);
        }
        for (const size_t b : peaks) {
            const double m = frame[b];
            // Quadratic interpolation of the peak frequency.
            const double a = frame[b - 1], c = frame[b + 1];
            const double d = a - 2.0 * m + c;
            const double off = std::abs(d) > 1e-12 ? 0.5 * (a - c) / d : 0.0;
            const double f = (double(b) + off) * bin_hz;
            // The peak and its sub-harmonics (it may be the 2nd–4th harmonic of a lower note).
            for (int h = 1; h <= 4; ++h) {
                const double f0 = f / h;
                if (f0 < 50.0) break;
                const double semis = 12.0 * std::log2(f0 / 440.0);
                const double nearest = std::round(semis);
                const double dist = std::abs(semis - nearest);  // 0 … 0.5 semitones
                if (dist > 0.5) continue;
                if (dist > win / 2.0) continue;
                const double w = powi(std::cos(std::numbers::pi * dist / win), 2);  // HPCP-style window
                const long long pc = ((static_cast<long long>(nearest) % 12) + 12) % 12;
                fp[size_t(pc)] += powi(m, power) * w * powi(0.6, h - 1);
            }
        }
        double mx = 0.0;
        for (const double v : fp) mx = std::max(mx, v);
        if (mx > 0.0)
            for (size_t i = 0; i < 12; ++i) pcp[i] += fp[i] / mx;  // each frame votes equally
    });
    double sum = 0;
    for (const double v : pcp) sum += v;
    if (sum <= 0.0) return std::nullopt;
    return pcp;
}

double pearson(const Profile& x, const Profile& y) {
    double mx = 0, my = 0;
    for (size_t i = 0; i < 12; ++i) mx += x[i], my += y[i];
    mx /= 12.0, my /= 12.0;
    double num = 0, dx = 0, dy = 0;
    for (size_t i = 0; i < 12; ++i) {
        num += (x[i] - mx) * (y[i] - my);
        dx += powi(x[i] - mx, 2);
        dy += powi(y[i] - my, 2);
    }
    return num / std::max(std::sqrt(dx * dy), 1e-12);
}

struct BestKey {
    size_t tonic = 0;
    bool minor = false;
    double r = std::numeric_limits<double>::lowest();
};

// Best (tonic index from A, minor?, correlation) for one pair of profiles.
BestKey best_key(const Profile& pcp, const Profile& major, const Profile& minor) {
    BestKey best;
    for (size_t tonic = 0; tonic < 12; ++tonic) {
        Profile rotated;
        for (size_t i = 0; i < 12; ++i) rotated[i] = pcp[(i + tonic) % 12];
        for (const auto& [minor_mode, profile] : {std::pair{false, &major}, std::pair{true, &minor}}) {
            const double r = pearson(rotated, *profile);
            if (r > best.r) best = {tonic, minor_mode, r};
        }
    }
    return best;
}

}  // namespace

Analysis analyze(const Audio& audio) {
    Analysis a;
    a.duration_sec = round_to(audio.duration, 10.0);
    const auto& x = audio.samples;
    if (x.size() < size_t(audio.rate) * 5) return a;
    // Tempo from the body of the track (skip intro / outro), like the Mac app.
    const std::span<const float> all(x);
    const auto body = audio.duration > 60.0 ? all.subspan(x.size() / 10, x.size() * 9 / 10 - x.size() / 10) : all;
    if (auto t = tempo(body, audio.rate)) {
        for (const double c : t->others) a.bpm_candidates.push_back(round_to(fold_bpm(c).first, 10.0));
        const auto [bpm, alt] = fold_bpm(t->bpm);
        a.bpm = round_to(bpm, 10.0);
        a.bpm_confidence = round_to(t->confidence, 100.0);
        if (alt) a.bpm_alternate = round_to(*alt, 10.0);
    }
    if (auto k = key(all, audio.rate)) {
        a.key = k->name;
        a.camelot = k->camelot;
        a.key_strength = round_to(k->strength, 100.0);
        a.key_agreement = k->agreement;
    }
    double sq = 0;
    for (const float v : x) sq += double(v) * double(v);
    const double rms = std::sqrt(sq / double(x.size()));
    const double db = 20.0 * std::log10(rms + 1e-9);
    a.energy = round_to(std::clamp((db + 30.0) / 24.0, 0.0, 1.0), 100.0);
    a.loudness_lufs = round_to(db - 0.7, 10.0);  // RMS-based approximation of integrated loudness
    return a;
}

std::optional<Tempo> tempo(std::span<const float> x, uint32_t rate) {
    const size_t n = 1024, hop = 256;
    if (frame_count(x.size(), n, hop) < 64) return std::nullopt;

    // Onset envelope: log-compressed spectral flux (half-wave rectified), up to ~8 kHz.
    const float bin_hz = float(rate) / float(n);
    const size_t max_bin = size_t(8000.0f / bin_hz);
    const size_t low_lo = size_t(40.0f / bin_hz), low_hi = size_t(150.0f / bin_hz);
    const std::string band = env_string("WB_BAND");
    const double w_full = band == "low" ? 0.0 : 1.0, w_low = band == "full" ? 0.0 : 1.0;
    const size_t top = std::min(max_bin, n / 2);
    std::vector<double> full_env, low_env;
    std::vector<float> prev(n / 2), cur(n / 2);
    stft(x, n, hop, [&](size_t t, const std::vector<float>& frame) {
        for (size_t b = 1; b < top; ++b) cur[b] = std::log(1.0f + 100.0f * frame[b]);
        if (t == 0) {
            full_env.push_back(0.0);
            low_env.push_back(0.0);
        } else {
            double flux = 0.0, low = 0.0;
            for (size_t b = 1; b < top; ++b) {
                const float d = cur[b] - prev[b];
                if (d > 0.0f) {
                    flux += double(d);
                    if (b >= std::max<size_t>(low_lo, 1) && b <= low_hi) low += double(d);
                }
            }
            full_env.push_back(flux);
            low_env.push_back(low);
        }
        std::swap(prev, cur);
    });

    // Normalise each band so the kick band can count as much as the whole spectrum.
    auto norm = [](std::vector<double>& v) {
        double m = 0;
        for (const double e : v) m += e;
        m /= double(v.size());
        for (double& e : v) e /= std::max(m, 1e-9);
    };
    norm(full_env);
    norm(low_env);
    std::vector<double> env(full_env.size());
    for (size_t i = 0; i < env.size(); ++i) env[i] = w_full * full_env[i] + w_low * low_env[i];

    // Remove the slow trend (local mean over ~1 s) and rectify.
    const double fps = double(rate) / double(hop);
    const size_t win = size_t(fps);
    std::vector<double> prefix(env.size() + 1, 0.0);
    for (size_t i = 0; i < env.size(); ++i) prefix[i + 1] = prefix[i] + env[i];
    {
        std::vector<double> detrended(env.size());
        for (size_t i = 0; i < env.size(); ++i) {
            const size_t lo = i >= win / 2 ? i - win / 2 : 0, hi = std::min(i + win / 2, env.size());
            detrended[i] = std::max(env[i] - (prefix[hi] - prefix[lo]) / double(hi - lo), 0.0);
        }
        env = std::move(detrended);
    }
    const size_t len = env.size();

    // Autocorrelation over lags for 50–220 BPM, reinforced by multiples of the lag (beat structure).
    auto lag_of = [&](double bpm) { return fps * 60.0 / bpm; };
    const size_t min_lag = size_t(std::floor(lag_of(220.0))), max_lag = size_t(std::ceil(lag_of(50.0)));
    auto ac = [&](size_t lag) -> double {
        if (lag >= len) return 0.0;
        double s = 0;
        for (size_t i = 0; i < len - lag; ++i) s += env[lag + i] * env[i];
        return s / double(len - lag);
    };
    std::vector<double> raw(max_lag * 4 + 3);
    for (size_t l = 0; l < raw.size(); ++l) raw[l] = ac(l);
    auto score = [&](size_t lag) { return raw[lag] + 0.5 * raw[lag * 2] + 0.25 * raw[std::min(lag * 4, raw.size() - 1)]; };
    std::vector<std::pair<size_t, double>> scores;
    for (size_t l = min_lag; l <= max_lag; ++l) scores.emplace_back(l, score(l));
    double mean = 0;
    for (const auto& s : scores) mean += s.second;
    mean /= double(scores.size());

    // Candidates: the strongest autocorrelation peaks and their metrical relatives. Syncopated genres
    // (jersey, baile, afro house) make 2/3, 4/5 and half-tempo periodicities strong, so each candidate
    // is re-ranked by how well a beat grid at that tempo lands on onsets rather than between them.
    std::vector<std::pair<size_t, double>> peaks;
    for (size_t i = 1; i + 1 < scores.size(); ++i)
        if (scores[i].second >= scores[i - 1].second && scores[i].second >= scores[i + 1].second) peaks.push_back(scores[i]);
    std::stable_sort(peaks.begin(), peaks.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    auto refine = [&](size_t lag) {
        const double y0 = score(lag - 1), y1 = score(lag), y2 = score(lag + 1);
        const double d = y0 - 2.0 * y1 + y2;
        return double(lag) + (std::abs(d) > 1e-12 ? std::clamp(0.5 * (y0 - y2) / d, -0.5, 0.5) : 0.0);
    };
    static constexpr double kRelSome[] = {1.0, 2.0, 0.5};
    static constexpr double kRelAll[] = {1.0, 2.0, 0.5, 1.5, 2.0 / 3.0, 4.0 / 3.0, 0.75, 1.25, 0.8};
    const std::span<const double> rel = env_string("WB_CAND") == "all" ? std::span<const double>(kRelAll) : std::span<const double>(kRelSome);
    std::vector<double> candidates;
    for (size_t p = 0; p < std::min<size_t>(5, peaks.size()); ++p) {
        const double bpm = fps * 60.0 / refine(peaks[p].first);
        for (const double r : rel) {
            const double c = bpm * r;
            if (c >= 68.0 && c <= 185.0) candidates.push_back(c);
        }
    }
    auto grid = [&](double bpm) {
        // Best phase: mean onset strength on the beats minus halfway between them.
        const double period = fps * 60.0 / bpm;
        const int steps = 16;
        double best = std::numeric_limits<double>::lowest();
        auto at = [&](double t) {
            const size_t i = size_t(std::round(t));
            return std::max({env[i >= 1 ? i - 1 : 0], env[i], env[std::min(i + 1, len - 1)]});
        };
        for (int s = 0; s < steps; ++s) {
            const double phase = period * double(s) / double(steps);
            double on = 0.0, off = 0.0;
            size_t count = 0;
            for (double t = phase; t + period / 2.0 + 1.0 < double(len); t += period) {
                on += at(t);
                off += at(t + period / 2.0);
                ++count;
            }
            // Scale-free contrast, so slower grids (which only hit the biggest kicks) aren't favoured.
            if (count > 0 && on + off > 0.0) best = std::max(best, (on - off) / (on + off));
        }
        return best;
    };
    // Choose by periodicity strength (interpolated at the candidate's exact lag) with a gentle prior around
    // typical DJ tempos; the beat-grid contrast only settles half- vs double-time.
    auto ac_at = [&](double bpm) {
        const double lag = fps * 60.0 / bpm;
        const size_t i = size_t(std::floor(lag));
        const double f = lag - std::floor(lag);
        if (i + 1 >= raw.size() / 4) return 0.0;
        return score(i) * (1.0 - f) + score(i + 1) * f;
    };
    auto prior = [](double bpm) { return std::exp(-0.5 * powi(std::log2(bpm / 122.0) / 1.0, 2)); };
    auto strength = [&](double bpm) { return ac_at(bpm) * prior(bpm); };

    if (std::getenv("WB_DEBUG_TEMPO")) {
        auto sorted = candidates;
        std::sort(sorted.begin(), sorted.end());
        double last = -1e9;
        for (const double c : sorted) {
            if (std::abs(c - last) < 0.5) continue;
            last = c;
            std::fprintf(stderr, "  cand %6.1f  grid %6.3f  ac/mean %5.2f\n", c, grid(c), score(size_t(std::round(fps * 60.0 / c))) / mean);
        }
    }

    if (candidates.empty()) return std::nullopt;
    // Iterator::max_by keeps the last of equal maxima.
    double bpm = candidates[0];
    for (const double c : candidates)
        if (strength(c) >= strength(bpm)) bpm = c;
    for (const double alt : {bpm * 2.0, bpm / 2.0}) {
        if (alt >= 68.0 && alt <= 185.0 && ac_at(alt) >= 0.5 * ac_at(bpm) && grid(alt) > grid(bpm) + 0.05) {
            bpm = alt;
            break;
        }
    }
    const double first = peaks.empty() ? 0.0 : peaks.front().second;
    const double conf = std::clamp((first / std::max(mean, 1e-12) - 1.0) / 3.0, 0.0, 1.0);
    auto ranked = candidates;
    std::stable_sort(ranked.begin(), ranked.end(), [&](double a, double b) { return strength(a) > strength(b); });
    std::vector<double> others;
    for (const double c : ranked) {
        if (std::abs(c - bpm) / bpm > 0.03 &&
            std::all_of(others.begin(), others.end(), [&](double o) { return std::abs(o - c) / c > 0.03; }))
            others.push_back(c);
        if (others.size() == 3) break;
    }
    return Tempo{bpm, conf, std::move(others)};
}

std::pair<double, std::optional<double>> fold_bpm(double bpm) {
    if (bpm <= 0.0) return {bpm, std::nullopt};
    while (bpm < 70.0) bpm *= 2.0;
    while (bpm > 180.0) bpm /= 2.0;
    std::optional<double> alt;
    if (bpm < 95.0) alt = bpm * 2.0;
    else if (bpm > 150.0) alt = bpm / 2.0;
    return {bpm, alt};
}

std::optional<KeyResult> key(std::span<const float> x, uint32_t rate) {
    const auto pcp = pitch_profile(x, rate);
    if (!pcp) return std::nullopt;
    const BestKey main = best_key(*pcp, EDMA_MAJOR, EDMA_MINOR);
    int agreement = 1;
    for (const BestKey& o : {best_key(*pcp, BGATE_MAJOR, BGATE_MINOR), best_key(*pcp, TEMPERLEY_MAJOR, TEMPERLEY_MINOR)})
        if (o.tonic == main.tonic && o.minor == main.minor) ++agreement;
    return KeyResult{std::string(NOTE_NAMES[main.tonic]) + (main.minor ? " minor" : " major"), camelot(main.tonic, main.minor), main.r,
                     agreement};
}

std::string camelot(size_t tonic_from_a, bool minor) {
    const size_t from_c = (tonic_from_a + 9) % 12;  // A=0 → C-based index
    const size_t major_tonic = minor ? (from_c + 3) % 12 : from_c;
    const size_t n = ((7 * major_tonic) % 12 + 7) % 12 + 1;
    return std::to_string(n) + (minor ? "A" : "B");
}

}  // namespace wb
