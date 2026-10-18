#pragma once

#include "session/audio_buffer.h"
#include "session/types.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// Remix: re-arranges a piece of music to a new duration by skipping or repeating
// passages where the music matches itself, joined with short equal-power
// crossfades. The opening and ending are kept. Worker thread; allocates.
namespace omawave::render {

struct RemixSegment {
    session::FramePos from = 0, to = 0;   // source frames, played in order
};

struct Remix {
    bool ok = false, cancelled = false;
    std::string error;
    session::AudioBuffer audio;            // the remixed music
    std::vector<RemixSegment> segments;    // what was played, before crossfades
    std::size_t seams = 0;                 // joins between segments
    double matchQuality = 0;               // 0..1, how alike the joined passages are (worst seam)
};

struct RemixSettings {
    session::FramePos targetFrames = 0;
    double crossfadeSeconds = 0.08;
    double keepStartSeconds = 4;           // left untouched at each end
    double keepEndSeconds = 4;
    double minimumSkipSeconds = 2;         // the shortest passage skipped or repeated
    double toleranceSeconds = 0;           // 0: 2% of the target, at least half a second
};

// `audio` is the music as it plays, at `sampleRate`, at most 20 minutes.
[[nodiscard]] Remix remixToDuration(const session::AudioBuffer& audio, std::uint32_t sampleRate,
    const RemixSettings& settings, const std::atomic<bool>* cancel = nullptr);

} // namespace omawave::render
