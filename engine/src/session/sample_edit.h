#pragma once

#include "project.h"

#include <atomic>
#include <optional>
#include <string>

namespace omawave::session {

// Worker-only sample processing. Copies share immutable original sources; the
// caller must check its session/edit revision before installing a returned model.
// No project means no change (or cancellation/error, as indicated below).
struct SampleEditResult {
    std::optional<Project> project;
    bool cancelled = false;
    std::string error;
    // The edit ran and the audio came out bit for bit the same: no project, as
    // there is nothing to undo, but the person is told rather than left guessing.
    bool unchanged = false;
};

[[nodiscard]] bool validCrossfadeRecipe(const Project&, const CrossfadeRecipe&,
    std::size_t frames, std::uint32_t channels);
struct CrossfadeTarget {
    CrossfadeRecipe recipe;
    FramePos minimum = 0, maximum = 0;
};
[[nodiscard]] std::optional<CrossfadeTarget> editableCrossfade(const Project&, TrackId, std::size_t clip);
[[nodiscard]] SampleEditResult adjustCrossfade(Project snapshot, TrackId, std::size_t clip,
    FramePos start, FramePos end, bool equalPower, const std::atomic<bool>* cancel = nullptr);

[[nodiscard]] SampleEditResult stretchClip(Project snapshot, TrackId track, std::size_t clip,
    FramePos targetFrames, const std::atomic<bool>* cancel = nullptr);
[[nodiscard]] SampleEditResult crossfadeSelection(Project snapshot, TrackId track,
    FramePos start, FramePos end, bool equalPower = false, const std::atomic<bool>* cancel = nullptr);

[[nodiscard]] SampleEditResult normalizeSelection(Project snapshot, TrackId track,
    FramePos start, FramePos end, float target, const std::atomic<bool>* cancel = nullptr);

[[nodiscard]] SampleEditResult reverseSelection(Project snapshot, TrackId track,
    FramePos start, FramePos end, const std::atomic<bool>* cancel = nullptr);
[[nodiscard]] SampleEditResult convertTrackChannels(Project snapshot, TrackId track,
    std::uint32_t channels, const std::atomic<bool>* cancel = nullptr);

} // namespace omawave::session
