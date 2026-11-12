// Magnitude spectrum of a real frame, for the two power-of-two sizes the analysis uses (1024, 8192).
//
// A real input of size n is packed into a complex FFT of size n/2 (even samples real, odd samples imaginary) and
// unpacked afterwards — half the work of a complex FFT of size n. Twiddles are computed in double once per size and
// stored per stage so the inner loop reads them sequentially.
//
// Four butterflies (and four output bins) at a time with SSE2, which every 64-bit PC has, so it still runs on old
// CPUs without AVX. Real and imaginary parts are kept in separate arrays for that. Each value goes through exactly
// the multiplies, adds and square root the one-at-a-time code did, in the same order, so the results are the same
// to the last bit — only the callers' choice of bins (`magnitudes(in, out, first, last)`) skips work.
#pragma once
#include <emmintrin.h>

#include <cmath>
#include <complex>
#include <numbers>
#include <utility>
#include <vector>

namespace wb {

class RealFft {
public:
    explicit RealFft(size_t n) : n_(n), h_(n / 2), rev_(h_), re_(h_ + 4), im_(h_ + 4), post_re_(h_ + 4), post_im_(h_ + 4) {
        for (size_t i = 1, j = 0; i < h_; ++i) {
            size_t bit = h_ >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            rev_[i] = j;
        }
        for (size_t len = 2; len <= h_; len <<= 1)
            for (size_t k = 0; k < len / 2; ++k) {
                const auto t = twiddle(k, len);
                tw_re_.push_back(t.real());
                tw_im_.push_back(t.imag());
            }
        for (size_t k = 0; k < h_; ++k) {
            const auto t = twiddle(k, n_);
            post_re_[k] = t.real();
            post_im_[k] = t.imag();
        }
    }

    size_t size() const { return n_; }

    // in: n real samples; out: n/2 magnitudes |X[0..n/2)|.
    void magnitudes(const float* in, float* out) { magnitudes(in, out, 0, h_); }

    // The same, filling only out[first..last) — for callers that read a band of the spectrum.
    void magnitudes(const float* in, float* out, size_t first, size_t last) {
        float* re = re_.data();
        float* im = im_.data();
        for (size_t i = 0; i < h_; ++i) {
            re[rev_[i]] = in[2 * i];
            im[rev_[i]] = in[2 * i + 1];
        }
        const float* twr = tw_re_.data();
        const float* twi = tw_im_.data();
        for (size_t len = 2; len <= h_; len <<= 1) {
            const size_t half = len / 2;
            if (half < 4) {
                for (size_t i = 0; i < h_; i += len)
                    for (size_t k = 0; k < half; ++k) {
                        const size_t a = i + k, b = a + half;
                        const float tr = twr[k] * re[b] - twi[k] * im[b];
                        const float ti = twr[k] * im[b] + twi[k] * re[b];
                        re[b] = re[a] - tr;
                        im[b] = im[a] - ti;
                        re[a] = re[a] + tr;
                        im[a] = im[a] + ti;
                    }
            } else {
                for (size_t i = 0; i < h_; i += len)
                    for (size_t k = 0; k < half; k += 4) {
                        const size_t a = i + k, b = a + half;
                        const __m128 wr = _mm_loadu_ps(twr + k), wi = _mm_loadu_ps(twi + k);
                        const __m128 br = _mm_loadu_ps(re + b), bi = _mm_loadu_ps(im + b);
                        const __m128 ar = _mm_loadu_ps(re + a), ai = _mm_loadu_ps(im + a);
                        const __m128 tr = _mm_sub_ps(_mm_mul_ps(wr, br), _mm_mul_ps(wi, bi));
                        const __m128 ti = _mm_add_ps(_mm_mul_ps(wr, bi), _mm_mul_ps(wi, br));
                        _mm_storeu_ps(re + b, _mm_sub_ps(ar, tr));
                        _mm_storeu_ps(im + b, _mm_sub_ps(ai, ti));
                        _mm_storeu_ps(re + a, _mm_add_ps(ar, tr));
                        _mm_storeu_ps(im + a, _mm_add_ps(ai, ti));
                    }
            }
            twr += half;
            twi += half;
        }
        // Unpack: X[k] = E[k] + W^k O[k], with E = (Z[k] + conj Z[h-k]) / 2 and O = (Z[k] - conj Z[h-k]) / 2i.
        last = last < h_ ? last : h_;
        size_t k = first;
        // One at a time up to a multiple of four (and bin 0, whose mirror is itself).
        for (; k < last && (k == 0 || (k & 3)); ++k) out[k] = unpack(k);
        const __m128 half = _mm_set1_ps(0.5f), minus_half = _mm_set1_ps(-0.5f);
        for (; k + 4 <= last; k += 4) {
            // Z[h-k], Z[h-k-1], Z[h-k-2], Z[h-k-3]: load the four ending at h-k and reverse them.
            const __m128 zr = _mm_loadu_ps(re + k), zi = _mm_loadu_ps(im + k);
            const __m128 mr = reversed(_mm_loadu_ps(re + h_ - k - 3));
            const __m128 mi = reversed(_mm_loadu_ps(im + h_ - k - 3));
            // zc = conj(mirror) = (mr, -mi).  e = 0.5 (z + zc);  d = z - zc;  o = (0.5 d.imag, -0.5 d.real)
            const __m128 er = _mm_mul_ps(half, _mm_add_ps(zr, mr));
            const __m128 ei = _mm_mul_ps(half, _mm_sub_ps(zi, mi));
            const __m128 dr = _mm_sub_ps(zr, mr);
            const __m128 di = _mm_add_ps(zi, mi);
            const __m128 or_ = _mm_mul_ps(half, di);
            const __m128 oi = _mm_mul_ps(minus_half, dr);
            const __m128 pr = _mm_loadu_ps(post_re_.data() + k), pi = _mm_loadu_ps(post_im_.data() + k);
            const __m128 xr = _mm_add_ps(er, _mm_sub_ps(_mm_mul_ps(pr, or_), _mm_mul_ps(pi, oi)));
            const __m128 xi = _mm_add_ps(ei, _mm_add_ps(_mm_mul_ps(pr, oi), _mm_mul_ps(pi, or_)));
            _mm_storeu_ps(out + k, _mm_sqrt_ps(_mm_add_ps(_mm_mul_ps(xr, xr), _mm_mul_ps(xi, xi))));
        }
        for (; k < last; ++k) out[k] = unpack(k);
    }

private:
    static std::complex<float> twiddle(size_t k, size_t len) {
        const double a = -2.0 * std::numbers::pi * double(k) / double(len);
        return {float(std::cos(a)), float(std::sin(a))};
    }

    static __m128 reversed(__m128 v) { return _mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 1, 2, 3)); }

    // One output bin, written out the way std::complex's operators compute it.
    float unpack(size_t k) const {
        const size_t m = k == 0 ? 0 : h_ - k;
        const float zr = re_[k], zi = im_[k], mr = re_[m], mi = im_[m];
        const float zci = -mi;
        const float er = 0.5f * (zr + mr), ei = 0.5f * (zi + zci);
        const float dr = zr - mr, di = zi - zci;
        const float or_ = 0.5f * di, oi = -0.5f * dr;
        const float xr = er + (post_re_[k] * or_ - post_im_[k] * oi);
        const float xi = ei + (post_re_[k] * oi + post_im_[k] * or_);
        return std::sqrt(xr * xr + xi * xi);
    }

    size_t n_, h_;
    std::vector<size_t> rev_;
    std::vector<float> re_, im_, post_re_, post_im_, tw_re_, tw_im_;
};

}  // namespace wb
