#pragma once

#include "fft.h"

#include <atomic>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace omawave::spectral {

// The analysis window. Hann is the default for a display and for resynthesis (its
// squared overlap-add is smooth at every hop the UI offers); Blackman-Harris trades
// frequency resolution for a much lower leakage floor, which is what you want when
// hunting a quiet whistle under a loud voice.
enum class Window : std::uint8_t { Hann, Hamming, BlackmanHarris };

[[nodiscard]] constexpr std::string_view windowName(Window w) noexcept
{
    switch (w) {
    case Window::Hann: return "hann";
    case Window::Hamming: return "hamming";
    case Window::BlackmanHarris: return "blackman-harris";
    }
    return "hann";
}

[[nodiscard]] constexpr std::optional<Window> windowFromName(std::string_view name) noexcept
{
    for (const Window w : {Window::Hann, Window::Hamming, Window::BlackmanHarris}) {
        if (windowName(w) == name)
            return w;
    }
    return std::nullopt;
}

[[nodiscard]] std::vector<float> makeWindow(Window window, std::uint32_t size);

struct StftConfig {
    std::uint32_t fftSize = 2048;
    std::uint32_t hop = 512;
    Window window = Window::Hann;

    [[nodiscard]] constexpr std::uint32_t bins() const noexcept { return fftSize / 2 + 1; }
    // A power-of-two frame of at least 64, and a hop that overlaps by at least half:
    // anything less and the resynthesis has holes.
    [[nodiscard]] constexpr bool valid() const noexcept
    {
        return Fft::isPowerOfTwo(fftSize) && fftSize >= 64 && hop > 0 && hop <= fftSize / 2;
    }
};

// Column c is the frame centred on sample c * hop, so column 0 sits on the first
// sample and the last column on or before the last. The signal is zero outside.
[[nodiscard]] constexpr std::size_t columnCount(std::size_t frames, std::uint32_t hop) noexcept
{
    return frames == 0 ? 0 : frames / hop + 1;
}

// A whole signal's short-time spectrum, one channel: `columns` frames of `bins`
// complex values each, column-major (a column is contiguous).
struct Spectrogram {
    std::uint32_t bins = 0;
    std::size_t columns = 0;
    std::vector<std::complex<float>> data;

    [[nodiscard]] std::complex<float>* column(std::size_t c) noexcept
    {
        return data.data() + c * bins;
    }
    [[nodiscard]] const std::complex<float>* column(std::size_t c) const noexcept
    {
        return data.data() + c * bins;
    }
};

class Stft {
public:
    explicit Stft(StftConfig config);

    [[nodiscard]] const StftConfig& config() const noexcept { return config_; }
    [[nodiscard]] std::uint32_t bins() const noexcept { return config_.bins(); }
    // What a bin's magnitude is divided by so a full-scale sine reads 1.0 (0 dBFS)
    // at its bin: half the window's sum.
    [[nodiscard]] float magnitudeScale() const noexcept { return magnitudeScale_; }

    // One column of `signal`: the windowed frame centred on column * hop, transformed.
    // `out` receives bins() values (DC through Nyquist). Zero outside the signal.
    void analyse(std::span<const float> signal, std::size_t column,
                 std::complex<float>* out);

    [[nodiscard]] Spectrogram forward(std::span<const float> signal, const std::atomic<bool>* cancel = nullptr);

    // Weighted overlap-add resynthesis to `frames` samples, normalised by the summed
    // squared window, so inverse(forward(x)) is x to float precision for any valid
    // hop -- the property the repairs depend on to leave untouched bins untouched.
    [[nodiscard]] std::vector<float> inverse(const Spectrogram& spec, std::size_t frames,
                                              const std::atomic<bool>* cancel = nullptr);

    // Worker-only streamed inverse. Columns and output blocks are visited in order;
    // the overlap-add ring has fftSize samples regardless of the signal length.
    // Returns false on cancellation. Publish the output only after success.
    bool inverseBlocks(std::size_t columns, std::size_t frames,
        const std::function<void(std::size_t, std::complex<float>*)>& readColumn,
        const std::function<void(std::size_t, std::span<const float>)>& writeBlock,
        const std::atomic<bool>* cancel = nullptr);

private:
    StftConfig config_;
    Fft fft_;
    std::vector<float> window_;
    float magnitudeScale_ = 1.0f;
    std::vector<std::complex<float>> scratch_;
};

} // namespace omawave::spectral
