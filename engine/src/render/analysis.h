#pragma once

#include "session/audio_buffer.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// Measurements of a stretch of audio, as the Amplitude Statistics, Frequency
// Analysis and Phase panels show them. Worker or main thread; never realtime.
namespace omawave::render {

struct ChannelStatistics {
    double peakDb = -200, minimum = 0, maximum = 0;   // sample values, full scale = 1
    double rmsDb = -200, dcOffset = 0;                // DC offset as a fraction of full scale
    std::uint64_t clipped = 0;                        // samples at or beyond full scale
    std::uint64_t clippedRuns = 0;                    // runs of consecutive clipped samples
};

struct AmplitudeStatistics {
    bool ok = false, cancelled = false;
    std::string error;
    std::uint64_t frames = 0;
    std::vector<ChannelStatistics> channels;
    double peakDb = -200, rmsDb = -200;               // across all channels
    double dynamicRangeDb = 0;                        // loudest minus quietest 50 ms RMS window
    double minimumRmsDb = -200, maximumRmsDb = -200;  // 50 ms windows; silence below -120 dB excluded
};

// `channels` planes of `frames` samples starting at `first` within each plane.
[[nodiscard]] AmplitudeStatistics amplitudeStatistics(const session::AudioBuffer& audio,
    std::size_t first, std::size_t frames, std::uint32_t sampleRate, const std::atomic<bool>* cancel = nullptr);

enum class SpectrumWindow { Hann, Blackman, Rectangular };

struct Spectrum {
    bool ok = false;
    std::string error;
    std::uint32_t fftSize = 0, sampleRate = 0;
    // Per channel, fftSize/2 + 1 magnitudes in dBFS for a full-scale sine, averaged
    // over the analysed frames (Welch, 50% overlap).
    std::vector<std::vector<float>> magnitudeDb;
    [[nodiscard]] double binFrequency(std::size_t bin) const noexcept {
        return fftSize ? double(bin) * sampleRate / fftSize : 0;
    }
};

[[nodiscard]] Spectrum frequencySpectrum(const session::AudioBuffer& audio, std::size_t first,
    std::size_t frames, std::uint32_t sampleRate, std::uint32_t fftSize = 4096,
    SpectrumWindow window = SpectrumWindow::Hann);

struct PhaseAnalysis {
    bool ok = false;
    std::string error;
    double correlation = 0;     // -1 (opposite) .. +1 (identical); 0 when either side is silent
    double balanceDb = 0;       // left RMS minus right RMS
    double sideToMidDb = -200;  // stereo width: side RMS relative to mid RMS
};

// Needs at least two channels; uses the first two.
[[nodiscard]] PhaseAnalysis phaseAnalysis(const session::AudioBuffer& audio, std::size_t first, std::size_t frames);

} // namespace omawave::render
