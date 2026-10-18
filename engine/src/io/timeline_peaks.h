#pragma once

#include "peak_builder.h"
#include "session/clip.h"
#include "session/project.h"

#include <cstdint>
#include <atomic>
#include <vector>

namespace omawave::io {

// Level-0 peaks straight from a clip list, without materialising the timeline.
//
// The obvious route is to render the timeline to a buffer and run buildPeaks over it,
// and that is what session::renderTimeline is for. It is the wrong tool here: peaks
// are rebuilt after every edit, and a 30-minute take is a 345 MB allocation each time.
// Walking the clips costs the same arithmetic with none of the memory.
//
// Envelopes are included, so a faded or gained region draws as it will sound.
[[nodiscard]] PeakLevel buildTimelinePeaks(const session::Project& project,
                                           const std::vector<session::Clip>& clips,
                                           std::uint32_t samplesPerPeak,
                                           std::uint32_t channels, const std::atomic<bool>* cancel = nullptr);

} // namespace omawave::io
