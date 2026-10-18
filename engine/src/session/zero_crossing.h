#pragma once

#include "audio_buffer.h"
#include "types.h"

#include <cstdint>

namespace omawave::session {

// Nearest zero crossing to `frame`, searching outward in both directions.
//
// BRIEF.md section 5: selections snap to zero crossings. Cutting anywhere else leaves
// a step in the waveform, which is audible as a click -- the whole reason the brief
// asks for this.
//
// Searches at most `maxDistance` frames either way and returns `frame` unchanged if
// nothing is found, so a selection in a continuously loud passage still lands
// somewhere sensible rather than jumping across the take.
//
// A crossing is the frame at which the signal's sign changes; the returned position is
// the later of the two frames, which is where a cut would start.
[[nodiscard]] FramePos nearestZeroCrossing(const AudioBuffer& audio, FramePos frame,
                                           FramePos maxDistance = 4800,
                                           std::uint32_t channel = 0) noexcept;

} // namespace omawave::session
