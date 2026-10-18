#pragma once

#include "sample_edit.h"

#include <map>

namespace omawave::session {

// An index plus its expected descriptor: an edit must not silently select a
// different clip after a split, deletion, reorder or another session edit.
struct ClipAddress {
    TrackId track = kInvalidTrack;
    std::size_t index = 0;
    Clip expected;
    bool operator==(const ClipAddress& other) const noexcept {
        return track == other.track && index == other.index && expected.sameDescriptor(other.expected);
    }
};

struct ClipboardTrack {
    std::size_t offset = 0; // relative to the first selected track's display row
    std::string name, colour;
    TrackRole role = TrackRole::Dialogue;
    std::uint32_t channels = 1;
    std::vector<Clip> clips; // timeline positions relative to the common origin
    bool operator==(const ClipboardTrack&) const = default;
};

struct MultitrackClipboard {
    std::uint32_t sampleRate = 0;
    FramePos frames = 0; // includes leading, internal and trailing silence
    bool wholeClips = false;
    std::vector<ClipboardTrack> tracks;
    // Source indices alone are not an identity across project replacement.
    // The owner must retain these sources while the clipboard is alive.
    std::map<SourceId, Uuid> sources;
    // Keep names/colours even if the original group is later removed. A paste
    // creates fresh groups so copies never become linked to their originals.
    std::vector<ClipGroup> groups{};
    bool operator==(const MultitrackClipboard&) const = default;
};
struct ClipboardCapture {
    std::optional<MultitrackClipboard> clipboard;
    std::string error;
};

[[nodiscard]] ClipboardCapture copyTrackRange(const Project&, const std::vector<TrackId>&,
                                              FramePos start, FramePos end);
[[nodiscard]] ClipboardCapture copySelectedClips(const Project&, const std::vector<ClipAddress>&);

// Worker-only: shares original audio and creates virtual silence only where a
// copied range's trailing empty time needs a represented endpoint. A private
// snapshot means failure/cancellation never changes the caller's project.
// firstTrack == tracks().size() appends. Missing destination rows are created up
// to the clipboard's last offset, bounded by the caller's actual render limit.
[[nodiscard]] SampleEditResult pasteTracks(Project snapshot, const MultitrackClipboard&,
    std::size_t firstTrack, FramePos at, bool ripple, std::size_t maxTracks,
    const std::atomic<bool>* cancel = nullptr);

struct GroupEditResult {
    bool changed = false;
    std::string error;
};
struct ClipPropertyEdit {
    double gainDb = 0;
    bool relativeGain = true;
    bool replaceFades = false;
    FramePos fadeIn = 0, fadeOut = 0;
    // Zero uses absolute fade lengths. Otherwise lengths are fractions of this
    // reference duration, rounded independently to each selected clip's frames.
    FramePos fadeReferenceLength = 0;
    // Omitted shapes keep every member's own curve when replacing lengths.
    std::optional<ClipFadeShapes> fadeShapes{};
};
[[nodiscard]] GroupEditResult editSelectedClipProperties(Project&, const std::vector<ClipAddress>&,
                                                        const ClipPropertyEdit&);
struct ClipSelectionResult {
    std::vector<ClipAddress> clips;
    std::string error;
};
struct ClipStretchPlan {
    FramePos origin = 0, frames = 0;
    std::vector<Clip> clips; // planned descriptors, in input selection order
    bool changed = false;
    std::string error;
};
// Scale boundaries around the earliest selected start. Shared boundary rounding
// keeps adjacent clips adjacent. Unselected clips, markers and automation stay put.
[[nodiscard]] ClipStretchPlan planSelectedClipStretch(const Project&, const std::vector<ClipAddress>&,
    FramePos targetFrames, bool realtime, double pitchChange = 0);
struct SelectedClipStretchResult {
    SampleEditResult edit;
    std::vector<ClipAddress> clips; // final indices/descriptors in input order
};
// Rendered mode is worker-only; realtime mode changes descriptors only. Both
// validate the entire candidate before returning a project, and retain originals.
[[nodiscard]] SelectedClipStretchResult stretchSelectedClips(Project, const std::vector<ClipAddress>&,
    FramePos targetFrames, bool realtime, double pitchChange = 0, const std::atomic<bool>* cancel = nullptr);
struct ClipGroupResult {
    ClipGroupId group = kInvalidClipGroup;
    std::string error;
};
// Model-order expansion for linked selection. With linking suspended callers
// keep their original selection; membership itself remains in the project.
[[nodiscard]] ClipSelectionResult expandClipGroups(const Project&, const std::vector<ClipAddress>&);
[[nodiscard]] ClipGroupResult createClipGroup(Project&, const std::vector<ClipAddress>&,
                                             std::string name, std::string colour = {});
[[nodiscard]] GroupEditResult detachClipGroupMembers(Project&, const std::vector<ClipAddress>&);
// Descriptor-only atomic edits. Never copy/write audio or run on the callback.
[[nodiscard]] GroupEditResult deleteTrackRange(Project&, const std::vector<TrackId>&,
                                               FramePos start, FramePos end, bool ripple);
[[nodiscard]] GroupEditResult deleteSelectedClips(Project&, const std::vector<ClipAddress>&);
[[nodiscard]] GroupEditResult moveSelectedClips(Project&, const std::vector<ClipAddress>&,
                                               FramePos timeDelta, int trackDelta);

} // namespace omawave::session
