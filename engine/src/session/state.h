#pragma once

#include "clip.h"
#include "marker.h"
#include "metadata.h"
#include "video.h"
#include "project.h"
#include "track.h"
#include "types.h"

#include <cstddef>
#include <string>
#include <vector>

namespace omawave::session {

// A value snapshot of everything an edit can change: every track, its parameters, and
// its clip list. Audio is not part of it -- clips reference shared, immutable sources,
// so a snapshot of an eight-track hour-long session is tens of kilobytes of
// descriptors, not gigabytes of samples.
//
// This is what the undo stack stores either side of each edit (BRIEF.md section 5).
// Phase 3 stored a single clip list; widening it to the whole session is what makes
// track add, remove, rename and reorder undoable without a second implementation of
// every operation.
struct TrackState {
    TrackId id = kInvalidTrack;
    std::string name;
    // Role and colour are undoable like a rename; height is not (see restoreState).
    TrackRole role = TrackRole::Dialogue;
    std::string colour;
    float gain = 1.0f;
    float pan = 0.0f;
    bool muted = false;
    bool soloed = false;
    AutomationCurve volumeAutomation{};
    AutomationCurve panAutomation{};
    std::vector<Clip> clips;

    // Chain membership is part of the snapshot, so add, remove, reorder and bypass are
    // undoable for free. Control tweaks are not, matching what gain and pan already do:
    // a knob move is not an undo step any more than a fader move is.
    std::vector<Insert> inserts;
    TrackRouting routing{};
    std::optional<DuckingRecipe> ducking{};

    [[nodiscard]] bool operator==(const TrackState&) const = default;
};

struct SessionState {
    std::vector<TrackState> tracks;

    // Markers are session-wide, so they snapshot alongside the tracks: adding,
    // moving, renaming and deleting one are undo steps for free.
    std::vector<Marker> markers;

    // The master bus chain, for the same reason a track's inserts are: adding,
    // removing, reordering and bypassing a master insert undo like any edit. The
    // trim is not here, exactly as a fader is not.
    std::vector<Insert> masterInserts;
    AutomationCurve masterVolumeAutomation{};
    std::uint32_t sampleRate = 0; // 0 in a default empty state preserves the current rate
    std::uint32_t defaultExportBits = 24;
    // The project's broadcast description edits like anything else: one undo step.
    ProjectMetadata metadata;
    // The reference picture and its placement: importing, moving or clearing
    // it undoes like any edit.
    VideoReference video;
    MusicalTime musicalTime;
    std::vector<ClipGroup> clipGroups{};

    [[nodiscard]] bool operator==(const SessionState&) const;
    // Settings can be equal after reordering/replacing identical inserts.
    // History must still distinguish their logical membership and order.
    [[nodiscard]] bool sameInsertInstances(const SessionState& other) const noexcept;

    [[nodiscard]] FramePos length() const noexcept;
};

// Reads the whole track set out of a project.
[[nodiscard]] SessionState captureState(const Project& project);

// Prepare an edit restore without rewinding unrelated plug-in settings. Ordinary
// session fields and rack membership come from `to`; for logical inserts present
// in all three states, newer setting fields unchanged by from -> to stay current.
// Transient revisions distinguish newer user edits from an older Redo state. This
// does not mutate old history or transplant state between replacement instances.
[[nodiscard]] SessionState stateForPluginAwareEdit(const SessionState& current,
                                                  const SessionState& from,
                                                  const SessionState& to);

// Puts it back, exactly. Tracks that are in the project but not the snapshot are
// removed; tracks in the snapshot but not the project are re-created with their
// original ids, which is what makes "undo the track I just deleted" work.
//
// Lane height is the one field deliberately NOT in the snapshot: it is how the
// session is being looked at, not what is in it, and an undo that shrank the lane
// you just expanded would be a bug. A surviving track keeps the height it has; a
// track brought back by undo comes back at the default.
void restoreState(Project& project, const SessionState& state);

} // namespace omawave::session
