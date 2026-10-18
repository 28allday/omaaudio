#pragma once
#include "sample_edit.h"

namespace omawave::session {
// Worker-only RNNoise 0.2/default-model processing, before track inserts/fader.
// Whole physical channels; preserves exact selection timing and outside audio.
[[nodiscard]] SampleEditResult cleanupVoice(Project snapshot, TrackId track,
    FramePos start, FramePos end, float mix, const std::atomic<bool>* cancel = nullptr);
} // namespace omawave::session
