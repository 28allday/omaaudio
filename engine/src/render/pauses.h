#pragma once

#include "session/types.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace omawave::render {

// Finding the pauses between words (Tidy pauses).
//
// The caller measures the level of each short window of the range, taking the
// loudest of the tracks looked at, so a pause is where all of them are quiet at
// once and nobody's answer is cut. Everything here is about that list of levels.
struct PauseSettings {
    bool autoThreshold = true;      // 10 dB above the background noise
    double thresholdDb = -44;       // when not automatic
    double minSeconds = 0.5;        // shorter gaps are part of the speech
    bool keepBreaths = true;        // a breath splits a gap rather than being in it
};

struct PauseScan {
    double thresholdDb = 0;         // the level used
    double noiseFloorDb = 0;        // the background noise it was set from
    std::vector<std::pair<session::FramePos, session::FramePos>> pauses;   // [start, end)
};

// The windows' RMS levels in dB, the first starting at `start`, each `window`
// frames long (the last may be shorter: `end` is where the range stops).
[[nodiscard]] PauseScan findPauses(const std::vector<float>& levelsDb, session::FramePos start,
                                   session::FramePos end, std::uint32_t window, double rate,
                                   const PauseSettings& settings);

// The level of each window of one block of audio planes, in dB (−120 for silence).
void windowLevels(const float* const* planes, std::uint32_t channels, std::size_t frames,
                  std::uint32_t window, std::vector<float>& out);

} // namespace omawave::render
