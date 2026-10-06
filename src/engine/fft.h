// Magnitude spectrum of a real frame, for the two power-of-two sizes the analysis uses (1024, 8192).
//
// A real input of size n is packed into a complex FFT of size n/2 (even samples real, odd samples imaginary) and
// unpacked afterwards — half the work of a complex FFT of size n. Twiddles are computed in double once per size and
// stored per stage so the inner loop reads them sequentially. Plain scalar code: compiles to SSE2, so it runs on
// old CPUs without AVX.
#pragma once
#include <cmath>
#include <complex>
#include <numbers>
#include <utility>
#include <vector>

namespace wb {

class RealFft {
public:
    explicit RealFft(size_t n) : n_(n), h_(n / 2), rev_(h_), buf_(h_), post_(h_) {
        for (size_t i = 1, j = 0; i < h_; ++i) {
            size_t bit = h_ >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            rev_[i] = j;
        }
        for (size_t len = 2; len <= h_; len <<= 1)
            for (size_t k = 0; k < len / 2; ++k) stage_tw_.push_back(twiddle(k, len));
        for (size_t k = 0; k < h_; ++k) post_[k] = twiddle(k, n_);
    }

    size_t size() const { return n_; }

    // in: n real samples; out: n/2 magnitudes |X[0..n/2)|.
    void magnitudes(const float* in, float* out) {
        for (size_t i = 0; i < h_; ++i) buf_[rev_[i]] = {in[2 * i], in[2 * i + 1]};
        const std::complex<float>* tw = stage_tw_.data();
        for (size_t len = 2; len <= h_; len <<= 1) {
            const size_t half = len / 2;
            for (size_t i = 0; i < h_; i += len)
                for (size_t k = 0; k < half; ++k) {
                    std::complex<float>& a = buf_[i + k];
                    std::complex<float>& b = buf_[i + k + half];
                    const float tr = tw[k].real() * b.real() - tw[k].imag() * b.imag();
                    const float ti = tw[k].real() * b.imag() + tw[k].imag() * b.real();
                    b = {a.real() - tr, a.imag() - ti};
                    a = {a.real() + tr, a.imag() + ti};
                }
            tw += half;
        }
        // Unpack: X[k] = E[k] + W^k O[k], with E = (Z[k] + conj Z[h-k]) / 2 and O = (Z[k] - conj Z[h-k]) / 2i.
        for (size_t k = 0; k < h_; ++k) {
            const std::complex<float> z = buf_[k], zc = std::conj(buf_[k == 0 ? 0 : h_ - k]);
            const std::complex<float> e = 0.5f * (z + zc);
            const std::complex<float> d = z - zc;  // O = d / 2i = (d.imag - i d.real) / 2
            const std::complex<float> o{0.5f * d.imag(), -0.5f * d.real()};
            const std::complex<float> x = e + post_[k] * o;
            out[k] = std::sqrt(x.real() * x.real() + x.imag() * x.imag());
        }
    }

private:
    static std::complex<float> twiddle(size_t k, size_t len) {
        const double a = -2.0 * std::numbers::pi * double(k) / double(len);
        return {float(std::cos(a)), float(std::sin(a))};
    }

    size_t n_, h_;
    std::vector<size_t> rev_;
    std::vector<std::complex<float>> buf_, post_, stage_tw_;
};

}  // namespace wb
