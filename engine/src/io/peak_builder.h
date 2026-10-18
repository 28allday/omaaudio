#pragma once

#include "session/audio_buffer.h"

#include <cstdint>
#include <vector>

namespace omawave::io {

// One resolution of waveform peak data.
//
// Phase 1 builds a single level. The multi-resolution pyramid with zoom levels in
// powers of 8 is Phase 2 (BRIEF.md section 3) -- the layout here is chosen so that
// adding levels later is additive rather than a rewrite.
struct PeakLevel {
    std::uint32_t samplesPerPeak = 0;
    std::uint32_t channels = 0;
    std::size_t count = 0;              // peaks per channel

    // Channel-major, min/max interleaved within a channel:
    //   data[(c * count + i) * 2 + 0] = min of peak i on channel c
    //   data[(c * count + i) * 2 + 1] = max
    //
    // Channel-major because the renderer draws one channel at a time, so a channel's
    // peaks stay contiguous when uploaded.
    std::vector<float> data;

    [[nodiscard]] float minAt(std::uint32_t channel, std::size_t index) const noexcept
    {
        return data[((static_cast<std::size_t>(channel) * count) + index) * 2];
    }

    [[nodiscard]] float maxAt(std::uint32_t channel, std::size_t index) const noexcept
    {
        return data[((static_cast<std::size_t>(channel) * count) + index) * 2 + 1];
    }

    [[nodiscard]] bool empty() const noexcept { return count == 0 || channels == 0; }
};

// Peak count is ceil(frames / samplesPerPeak): a partial final block still gets a peak,
// so the waveform reaches the end of the audio instead of stopping short.
[[nodiscard]] PeakLevel buildPeaks(const session::AudioBuffer& audio,
                                   std::uint32_t samplesPerPeak);

} // namespace omawave::io
