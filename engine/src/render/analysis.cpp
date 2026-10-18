#include "analysis.h"

#include "spectral/fft.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>

namespace omawave::render {
namespace {
constexpr double kFloorDb = -200;
double toDb(double value) { return value > 0 ? std::max(kFloorDb, 20 * std::log10(value)) : kFloorDb; }
}

AmplitudeStatistics amplitudeStatistics(const session::AudioBuffer& audio, std::size_t first,
    std::size_t frames, std::uint32_t sampleRate, const std::atomic<bool>* cancel)
{
    AmplitudeStatistics result;
    if (audio.empty() || first > audio.frames() || frames > audio.frames() - first || !frames || !sampleRate) {
        result.error = "Choose a range of audio to measure.";
        return result;
    }
    const auto channels = audio.channels();
    result.frames = frames;
    result.channels.resize(channels);
    std::vector<long double> sums(channels, 0), squares(channels, 0);
    double totalSquares = 0;
    const std::size_t window = std::max<std::size_t>(1, sampleRate / 20); // 50 ms
    double windowSquares = 0;
    std::size_t windowCount = 0;
    double loudest = -1, quietest = std::numeric_limits<double>::max();
    for (std::uint32_t c = 0; c < channels; ++c) {
        auto& stats = result.channels[c];
        stats.minimum = std::numeric_limits<double>::max();
        stats.maximum = std::numeric_limits<double>::lowest();
    }
    std::vector<bool> inRun(channels, false);
    for (std::size_t f = 0; f < frames; ++f) {
        if ((f & 0xFFFF) == 0 && cancel && cancel->load(std::memory_order_relaxed)) {
            result.cancelled = true; result.error = "Measurement cancelled.";
            return result;
        }
        double frameSquares = 0;
        for (std::uint32_t c = 0; c < channels; ++c) {
            const double v = audio.plane(c)[first + f];
            auto& stats = result.channels[c];
            stats.minimum = std::min(stats.minimum, v);
            stats.maximum = std::max(stats.maximum, v);
            sums[c] += v;
            squares[c] += v * v;
            frameSquares += v * v;
            if (std::abs(v) >= 1.0) {
                ++stats.clipped;
                if (!inRun[c]) { ++stats.clippedRuns; inRun[c] = true; }
            } else inRun[c] = false;
        }
        totalSquares += frameSquares;
        windowSquares += frameSquares / channels;
        if (++windowCount == window || f + 1 == frames) {
            const double rms = std::sqrt(windowSquares / windowCount);
            if (rms > 1e-6) { loudest = std::max(loudest, rms); quietest = std::min(quietest, rms); }
            windowSquares = 0; windowCount = 0;
        }
    }
    double peak = 0;
    for (std::uint32_t c = 0; c < channels; ++c) {
        auto& stats = result.channels[c];
        const double absolute = std::max(std::abs(stats.minimum), std::abs(stats.maximum));
        stats.peakDb = toDb(absolute);
        stats.rmsDb = toDb(std::sqrt(double(squares[c] / frames)));
        stats.dcOffset = double(sums[c] / frames);
        peak = std::max(peak, absolute);
    }
    result.peakDb = toDb(peak);
    result.rmsDb = toDb(std::sqrt(totalSquares / (double(frames) * channels)));
    if (loudest > 0) {
        result.maximumRmsDb = toDb(loudest);
        result.minimumRmsDb = toDb(quietest);
        result.dynamicRangeDb = result.maximumRmsDb - result.minimumRmsDb;
    }
    result.ok = true;
    return result;
}

Spectrum frequencySpectrum(const session::AudioBuffer& audio, std::size_t first, std::size_t frames,
    std::uint32_t sampleRate, std::uint32_t fftSize, SpectrumWindow window)
{
    Spectrum result;
    if (!spectral::Fft::isPowerOfTwo(fftSize) || fftSize < 64 || fftSize > 65536) {
        result.error = "Choose an analysis size between 64 and 65536 that is a power of two.";
        return result;
    }
    if (audio.empty() || first > audio.frames() || frames > audio.frames() - first || !frames || !sampleRate) {
        result.error = "Choose a range of audio to analyse.";
        return result;
    }
    result.fftSize = fftSize; result.sampleRate = sampleRate;
    std::vector<float> shape(fftSize);
    double coherent = 0;
    for (std::uint32_t i = 0; i < fftSize; ++i) {
        const double x = 2 * M_PI * i / (fftSize - 1);
        shape[i] = window == SpectrumWindow::Rectangular ? 1.0f
            : window == SpectrumWindow::Hann ? float(0.5 - 0.5 * std::cos(x))
            : float(0.42 - 0.5 * std::cos(x) + 0.08 * std::cos(2 * x));
        coherent += shape[i];
    }
    const spectral::Fft fft(fftSize);
    const std::size_t hop = fftSize / 2;
    // At least one frame: shorter ranges are zero padded.
    const std::size_t count = frames <= fftSize ? 1 : 1 + (frames - fftSize + hop - 1) / hop;
    const std::size_t limitedCount = std::min<std::size_t>(count, 4096);
    const std::size_t stride = count > limitedCount ? count / limitedCount : 1;
    std::vector<std::complex<float>> data(fftSize);
    const double scale = 2.0 / coherent; // a full-scale sine reads 0 dB
    for (std::uint32_t c = 0; c < audio.channels(); ++c) {
        std::vector<double> power(fftSize / 2 + 1, 0);
        std::size_t used = 0;
        for (std::size_t k = 0; k < count; k += stride) {
            const std::size_t at = first + k * hop;
            for (std::uint32_t i = 0; i < fftSize; ++i) {
                const std::size_t index = at + i;
                const float v = index < first + frames ? audio.plane(c)[index] : 0.0f;
                data[i] = {v * shape[i], 0};
            }
            fft.forward(data.data());
            for (std::size_t bin = 0; bin <= fftSize / 2; ++bin) power[bin] += std::norm(data[bin]);
            ++used;
        }
        std::vector<float> db(fftSize / 2 + 1);
        for (std::size_t bin = 0; bin <= fftSize / 2; ++bin)
            db[bin] = float(toDb(std::sqrt(power[bin] / used) * scale));
        result.magnitudeDb.push_back(std::move(db));
    }
    result.ok = true;
    return result;
}

PhaseAnalysis phaseAnalysis(const session::AudioBuffer& audio, std::size_t first, std::size_t frames)
{
    PhaseAnalysis result;
    if (audio.channels() < 2) { result.error = "Phase needs stereo audio."; return result; }
    if (first > audio.frames() || frames > audio.frames() - first || !frames) {
        result.error = "Choose a range of audio to analyse."; return result;
    }
    long double lr = 0, ll = 0, rr = 0, mid = 0, side = 0;
    for (std::size_t f = 0; f < frames; ++f) {
        const double l = audio.plane(0)[first + f], r = audio.plane(1)[first + f];
        lr += l * r; ll += l * l; rr += r * r;
        mid += (l + r) * (l + r) / 4; side += (l - r) * (l - r) / 4;
    }
    result.correlation = ll > 0 && rr > 0 ? double(lr / std::sqrt(ll * rr)) : 0;
    result.balanceDb = toDb(std::sqrt(double(ll / frames))) - toDb(std::sqrt(double(rr / frames)));
    result.sideToMidDb = mid > 0 ? toDb(std::sqrt(double(side / mid))) : (side > 0 ? 200 : kFloorDb);
    result.ok = true;
    return result;
}

} // namespace omawave::render
