#pragma once

#include "session/audio_buffer.h"
#include "session/types.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// Automatic speech alignment: re-times a second performance of the same words (a
// re-recorded line, a double) so its timing follows a reference recording. The two
// are compared as sequences of short spectral frames, matched by dynamic time
// warping, and the second is re-assembled from its own audio along the matched
// path (WSOLA), so its pitch and voice are unchanged. Worker thread; allocates.
namespace omawave::render {

struct SpeechAlignment {
    bool ok = false, cancelled = false;
    std::string error;
    // The aligned performance: the reference's length, the unaligned audio's channels.
    session::AudioBuffer audio;
    // For every 10 ms of reference time, where in the unaligned audio it was taken
    // from (frames). Monotonic.
    std::vector<session::FramePos> sourceAt;
    std::uint32_t hop = 0;              // frames between sourceAt entries
    double largestShiftMs = 0;          // the most the timing moved anywhere
};

// `tightness` 0..1: higher follows the reference more closely moment to moment;
// lower keeps the performance's own rhythm over short spans. Both inputs must be
// at `sampleRate` and at most 10 minutes long.
[[nodiscard]] SpeechAlignment alignSpeech(const session::AudioBuffer& reference,
    const session::AudioBuffer& unaligned, std::uint32_t sampleRate, double tightness = 0.7,
    const std::atomic<bool>* cancel = nullptr);

} // namespace omawave::render
