#include "stft.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <numbers>

namespace omawave::spectral {

std::vector<float> makeWindow(Window window, std::uint32_t size)
{
    std::vector<float> w(size, 0.0f);
    if (size == 0)
        return w;
    const double n = static_cast<double>(size);
    for (std::uint32_t i = 0; i < size; ++i) {
        // Periodic forms, so the overlap-add of hop N/2 and N/4 is exactly flat for
        // Hann and Hamming (the symmetric form is off by one sample and is not).
        const double x = 2.0 * std::numbers::pi * static_cast<double>(i) / n;
        double v = 0.0;
        switch (window) {
        case Window::Hann:
            v = 0.5 - 0.5 * std::cos(x);
            break;
        case Window::Hamming:
            v = 0.54 - 0.46 * std::cos(x);
            break;
        case Window::BlackmanHarris:
            v = 0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2.0 * x)
              - 0.01168 * std::cos(3.0 * x);
            break;
        }
        w[i] = static_cast<float>(v);
    }
    return w;
}

Stft::Stft(StftConfig config)
    : config_(config), fft_(config.fftSize), window_(makeWindow(config.window, config.fftSize)),
      scratch_(config.fftSize)
{
    assert(config.valid());
    double sum = 0.0;
    for (const float v : window_)
        sum += v;
    magnitudeScale_ = static_cast<float>(sum > 0.0 ? 2.0 / sum : 1.0);
}

void Stft::analyse(std::span<const float> signal, std::size_t column, std::complex<float>* out)
{
    const std::uint32_t n = config_.fftSize;
    const auto centre = static_cast<std::ptrdiff_t>(column * config_.hop);
    const std::ptrdiff_t first = centre - static_cast<std::ptrdiff_t>(n / 2);
    const auto frames = static_cast<std::ptrdiff_t>(signal.size());
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::ptrdiff_t at = first + static_cast<std::ptrdiff_t>(i);
        const float v = (at >= 0 && at < frames) ? signal[static_cast<std::size_t>(at)] : 0.0f;
        scratch_[i] = {v * window_[i], 0.0f};
    }
    fft_.forward(scratch_.data());
    std::copy_n(scratch_.data(), bins(), out);
}

Spectrogram Stft::forward(std::span<const float> signal, const std::atomic<bool>* cancel)
{
    if (cancel && cancel->load(std::memory_order_relaxed)) return {};
    Spectrogram spec;
    spec.bins = bins();
    spec.columns = columnCount(signal.size(), config_.hop);
    spec.data.resize(spec.columns * spec.bins);
    for (std::size_t c = 0; c < spec.columns; ++c) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return {};
        analyse(signal, c, spec.column(c));
    }
    return spec;
}

std::vector<float> Stft::inverse(const Spectrogram& spec, std::size_t frames, const std::atomic<bool>* cancel)
{
    if (cancel && cancel->load(std::memory_order_relaxed)) return {};
    std::vector<float> out(frames, 0.0f);
    std::vector<float> norm(frames, 0.0f);
    const std::uint32_t n = config_.fftSize;
    const std::uint32_t half = n / 2;
    const auto total = static_cast<std::ptrdiff_t>(frames);

    for (std::size_t c = 0; c < spec.columns; ++c) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return {};
        // Rebuild the full conjugate-symmetric frame from the half kept.
        const std::complex<float>* col = spec.column(c);
        for (std::uint32_t k = 0; k < spec.bins && k < n; ++k)
            scratch_[k] = col[k];
        for (std::uint32_t k = spec.bins; k < n; ++k)
            scratch_[k] = std::conj(col[n - k]);
        fft_.inverse(scratch_.data());

        const std::ptrdiff_t first = static_cast<std::ptrdiff_t>(c * config_.hop)
                                   - static_cast<std::ptrdiff_t>(half);
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::ptrdiff_t at = first + static_cast<std::ptrdiff_t>(i);
            if (at < 0 || at >= total)
                continue;
            const auto idx = static_cast<std::size_t>(at);
            out[idx] += scratch_[i].real() * window_[i];
            norm[idx] += window_[i] * window_[i];
        }
    }
    for (std::size_t i = 0; i < frames; ++i) {
        if (norm[i] > 1e-6f)
            out[i] /= norm[i];
    }
    return out;
}

bool Stft::inverseBlocks(std::size_t columns, std::size_t frames,
    const std::function<void(std::size_t, std::complex<float>*)>& readColumn,
    const std::function<void(std::size_t, std::span<const float>)>& writeBlock,
    const std::atomic<bool>* cancel)
{
    const auto stopped = [&] { return cancel && cancel->load(std::memory_order_relaxed); };
    if (stopped()) return false;
    const std::size_t n = config_.fftSize;
    std::vector<float> out(n), norm(n), block;
    block.reserve(8192);
    std::vector<std::complex<float>> col(bins());
    std::size_t emitted = 0, blockStart = 0;
    const auto flush = [&](std::size_t until) {
        for (; emitted < until; ++emitted) {
            const auto i = emitted % n;
            block.push_back(norm[i] > 1e-6f ? out[i] / norm[i] : out[i]);
            out[i] = norm[i] = 0;
            if (block.size() == 8192) {
                writeBlock(blockStart, block);
                if (stopped()) { block.clear(); return; }
                blockStart += block.size();
                block.clear();
            }
        }
    };
    for (std::size_t c = 0; c < columns; ++c) {
        if (stopped()) return false;
        readColumn(c, col.data());
        if (stopped()) return false;
        std::copy(col.begin(), col.end(), scratch_.begin());
        for (std::size_t k = bins(); k < n; ++k) scratch_[k] = std::conj(col[n - k]);
        fft_.inverse(scratch_.data());
        const auto first = static_cast<std::ptrdiff_t>(c * config_.hop) - std::ptrdiff_t(n / 2);
        for (std::size_t i = 0; i < n; ++i) {
            const auto at = first + std::ptrdiff_t(i);
            if (at < 0 || std::size_t(at) >= frames) continue;
            const auto slot = std::size_t(at) % n;
            out[slot] += scratch_[i].real() * window_[i];
            norm[slot] += window_[i] * window_[i];
        }
        const auto next = (c + 1) * config_.hop;
        flush(std::min(frames, next > n / 2 ? next - n / 2 : 0));
    }
    flush(frames);
    if (stopped()) return false;
    if (!block.empty()) writeBlock(blockStart, block);
    return !stopped();
}

} // namespace omawave::spectral
