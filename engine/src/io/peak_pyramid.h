#pragma once

#include "peak_builder.h"
#include "session/audio_buffer.h"

#include <cstdint>
#include <vector>

namespace omawave::io {

// Multi-resolution peaks.
//
// BRIEF.md section 3: "Zoom levels in powers of 8." Level 0 is built from the audio
// at kBasePeakSamples; every level above it is built from the level below by taking
// the min and max of each group of 8, which is exact and far cheaper than rescanning
// the audio.
//
// The 8-per-level ratio is what keeps the renderer's tap count bounded: at any zoom,
// some level has between 1 and 8 peaks per pixel, and the shader never needs more than
// 8 taps to draw a column correctly. Phase 1 had only one level and undersampled when
// zoomed out; this is the fix.
struct PeakPyramid {
    std::vector<PeakLevel> levels;

    [[nodiscard]] bool empty() const noexcept { return levels.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return levels.size(); }

    // The level to draw with, given how many level-0 peaks fall on one pixel.
    // Returns an index into `levels`.
    [[nodiscard]] std::size_t levelForPeaksPerPixel(double basePeaksPerPixel) const noexcept;
};

inline constexpr std::uint32_t kBasePeakSamples = 256;
inline constexpr std::uint32_t kLevelRatio = 8;

// Builds levels until one of them covers the whole take in a handful of peaks.
[[nodiscard]] PeakPyramid buildPyramid(const session::AudioBuffer& audio,
                                       std::uint32_t basePeakSamples = kBasePeakSamples);

// Same, from a level 0 built elsewhere -- io::buildTimelinePeaks produces one straight
// from a clip list, which is what an edit needs.
[[nodiscard]] PeakPyramid pyramidFromLevel(PeakLevel base);

// Collapses a level by kLevelRatio. Exposed for testing.
[[nodiscard]] PeakLevel reducePeaks(const PeakLevel& source, std::uint32_t ratio);

} // namespace omawave::io
