#pragma once

#include <complex>
#include <cstdint>
#include <vector>

// The spectral layer: everything that looks at audio in the frequency domain. It
// sits beside io/ (which draws the time domain) and above session/, and nothing
// below it includes it. Plain C++20, no Qt (BRIEF.md amendment A3).
namespace omawave::spectral {

// A radix-2 complex FFT, in place. Hand-rolled rather than FFTW or KissFFT because
// the licence table in BRIEF.md section 2 has no FFT library in it and adding one is a
// decision; at the sizes a spectral display uses (512..8192) the iterative
// Cooley-Tukey below is a few hundred microseconds per frame, which is not the cost
// that matters -- writing the tiles is.
class Fft {
public:
    // `size` must be a power of two.
    explicit Fft(std::uint32_t size);

    [[nodiscard]] std::uint32_t size() const noexcept { return size_; }

    // Forward transform, unscaled. `data` holds size() values.
    void forward(std::complex<float>* data) const noexcept;
    // Inverse transform, scaled by 1/N, so inverse(forward(x)) == x.
    void inverse(std::complex<float>* data) const noexcept;

    [[nodiscard]] static constexpr bool isPowerOfTwo(std::uint32_t n) noexcept
    {
        return n != 0 && (n & (n - 1)) == 0;
    }

private:
    void transform(std::complex<float>* data, bool inverse) const noexcept;

    std::uint32_t size_;
    std::vector<std::uint32_t> bitReversed_;
    std::vector<std::complex<float>> twiddles_;   // e^(-2*pi*i*k/N), k < N/2
};

} // namespace omawave::spectral
