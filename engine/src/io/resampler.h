#pragma once

#include "session/audio_buffer.h"

#include <cstdint>
#include <atomic>
#include <vector>

namespace omawave::io {

// Windowed-sinc sample rate conversion, used on import only.
//
// BRIEF.md section 2: "Resample on import, never mid-graph." Nothing in the audio
// path calls this -- it runs on a worker while a file is being read.
//
// Written rather than pulled in: the licence table in section 2 lists no resampling
// library, and adding libsamplerate would need an explicit decision. A Kaiser-windowed
// sinc is about a hundred lines and keeps the dependency list where the brief put it.
class Resampler {
public:
    // taps is per side; the kernel is symmetric. 32 gives roughly -90 dB stopband
    // with the beta chosen below, which is well past transparent for voice.
    Resampler(std::uint32_t sourceRate, std::uint32_t targetRate,
              std::uint32_t tapsPerSide = 32, std::uint32_t phases = 256);

    [[nodiscard]] bool passthrough() const noexcept { return sourceRate_ == targetRate_; }

    [[nodiscard]] std::uint32_t sourceRate() const noexcept { return sourceRate_; }
    [[nodiscard]] std::uint32_t targetRate() const noexcept { return targetRate_; }

    // Output frame count for a given input length.
    [[nodiscard]] std::size_t outputFrames(std::size_t inputFrames) const noexcept;

    // Whole-buffer conversion. Returns the input untouched when rates match.
    [[nodiscard]] session::AudioBuffer process(const session::AudioBuffer& in, const std::atomic<bool>* cancel = nullptr) const;

private:
    [[nodiscard]] float tap(std::uint32_t phase, std::uint32_t index) const noexcept
    {
        return kernel_[static_cast<std::size_t>(phase) * kernelWidth_ + index];
    }

    std::uint32_t sourceRate_;
    std::uint32_t targetRate_;
    std::uint32_t tapsPerSide_;
    std::uint32_t phases_;
    std::uint32_t kernelWidth_;      // tapsPerSide_ * 2
    double ratio_;                   // target / source
    std::vector<float> kernel_;      // phases_ * kernelWidth_
};

} // namespace omawave::io
