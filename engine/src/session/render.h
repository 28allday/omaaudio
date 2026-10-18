#pragma once

#include "audio_buffer.h"
#include "clip.h"
#include "project.h"
#include "types.h"

#include <vector>
#include <atomic>

namespace omawave::session {

[[nodiscard]] AudioBuffer renderClipRange(const Project& project, const std::vector<Clip>& clips,
    FramePos start, FramePos end, std::uint32_t channels, const std::atomic<bool>* cancel = nullptr);

// Renders a clip list to a flat buffer, offline.
//
// Used for peak generation after an edit, and the same routine Phase 4's mixdown and
// Phase 7's export will need. Not called from the audio thread -- it allocates.
//
// Gaps between clips render as silence rather than being closed, because a caller
// asking for frames 0..N wants N frames back.
[[nodiscard]] AudioBuffer renderTimeline(const Project& project,
                                         const std::vector<Clip>& clips,
                                         std::uint32_t channels, const std::atomic<bool>* cancel = nullptr);

} // namespace omawave::session
