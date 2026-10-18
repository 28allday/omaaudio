#pragma once

#include "audio_buffer.h"
#include "project.h"
#include "types.h"

#include <cstdint>

namespace omawave::session {

// Add one unprocessed track to an existing buffer, preserving per-clip pan laws.
void mixTrack(const Project& project, const Track& track, AudioBuffer& output, bool anySoloed, const std::atomic<bool>* cancel = nullptr);

// Sums every track through its gain, mute, solo and pan into one buffer, offline.
//
// BRIEF.md section 5 asks for mixdown in Phase 4; section 5's Phase 7 export is what
// will consume it. Not called from the audio thread -- it allocates a buffer as long
// as the whole timeline.
//
// This is deliberately NOT engine::Transport's code reused. The audio thread must not
// reach into the Session layer (section 3), so the two walk the same model separately
// and a test asserts they agree sample for sample. That test is the contract; if it
// ever fails, one of the two has drifted and the mix no longer sounds like the
// monitor.
//
// The master monitor gain is not applied. It is a listening level, not part of the
// mix, and baking it into an export is how a quiet render happens.
[[nodiscard]] AudioBuffer mixdown(const Project& project, std::uint32_t channels);

} // namespace omawave::session
