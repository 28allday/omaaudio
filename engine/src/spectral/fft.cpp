#include "fft.h"

#include <cassert>
#include <cmath>
#include <numbers>
#include <utility>

namespace omawave::spectral {

Fft::Fft(std::uint32_t size) : size_(size)
{
    assert(isPowerOfTwo(size));
    bitReversed_.resize(size);
    std::uint32_t bits = 0;
    while ((1u << bits) < size)
        ++bits;
    for (std::uint32_t i = 0; i < size; ++i) {
        std::uint32_t r = 0;
        for (std::uint32_t b = 0; b < bits; ++b) {
            if (i & (1u << b))
                r |= 1u << (bits - 1 - b);
        }
        bitReversed_[i] = r;
    }
    twiddles_.resize(size / 2);
    for (std::uint32_t k = 0; k < size / 2; ++k) {
        const double angle = -2.0 * std::numbers::pi * static_cast<double>(k)
                           / static_cast<double>(size);
        twiddles_[k] = {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
    }
}

void Fft::forward(std::complex<float>* data) const noexcept { transform(data, false); }

void Fft::inverse(std::complex<float>* data) const noexcept
{
    transform(data, true);
    const float scale = 1.0f / static_cast<float>(size_);
    for (std::uint32_t i = 0; i < size_; ++i)
        data[i] *= scale;
}

void Fft::transform(std::complex<float>* data, bool inverse) const noexcept
{
    for (std::uint32_t i = 0; i < size_; ++i) {
        const std::uint32_t j = bitReversed_[i];
        if (j > i)
            std::swap(data[i], data[j]);
    }
    for (std::uint32_t len = 2; len <= size_; len <<= 1) {
        const std::uint32_t half = len / 2;
        const std::uint32_t stride = size_ / len;
        for (std::uint32_t start = 0; start < size_; start += len) {
            for (std::uint32_t k = 0; k < half; ++k) {
                std::complex<float> w = twiddles_[k * stride];
                if (inverse)
                    w = std::conj(w);
                const std::complex<float> t = w * data[start + k + half];
                const std::complex<float> u = data[start + k];
                data[start + k] = u + t;
                data[start + k + half] = u - t;
            }
        }
    }
}

} // namespace omawave::spectral
