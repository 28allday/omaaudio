#pragma once

#include "clip.h"
#include "types.h"

#include <vector>
#include <unordered_map>

namespace omawave::session {

class Project;
struct Track;

// Clip-list edits. Pure functions over a clip list, with no reference to Project, so
// they are trivially testable and reusable by the undo commands.
//
// Every function keeps the list sorted by timelineStart and free of overlaps, which
// the renderer and the audio thread both rely on.
namespace edits {

using ClipRackForkMap = std::unordered_map<EffectOwner, std::shared_ptr<const ClipRack>>;
// A real copy gets new view identities and one new rack per copied region.
// Reuse the map across all copied lanes in the same operation.
void forkClipCopies(std::vector<Clip>& clips, ClipRackForkMap& forks);
// Separate processing regions whose remaining views no longer form one run.
// Surviving contiguous views keep their original immutable rack and identities.
void partitionClipRacks(std::vector<Clip>& clips);
void partitionClipRacks(std::vector<Track>& tracks);

// Slip changes only the source offset, with fixed timeline geometry and envelopes.
[[nodiscard]] bool canSlipClip(const Project& project, TrackId track, std::size_t index,
                                FramePos sourceOffset) noexcept;
bool slipClip(Project& project, TrackId track, std::size_t index, FramePos sourceOffset);

// Trim moves a clip's edges over its source, never the audio's place in time:
// an edge can go out as far as the source, a clip effect's prepared region and
// a stored envelope's start allow, and no further than the next clip.
struct ClipTrimLimits {
    bool valid = false;
    FramePos earliestStart = 0;   // left edge furthest out
    FramePos latestEnd = 0;       // right edge furthest out
};
[[nodiscard]] ClipTrimLimits clipTrimLimits(const Project& project, TrackId track, std::size_t index) noexcept;
// [start, end) is the clip's new span: at least one frame, inside the limits.
// A whole fade keeps to its edge, shortened to fit the new length. Returns false
// and changes nothing when refused or unchanged.
bool trimClip(Project& project, TrackId track, std::size_t index, FramePos start, FramePos end);

// Splits whichever clip contains `position`. No-op on a clip boundary or outside the
// timeline -- splitting where a split already exists must not create a zero-length clip.
void splitAt(std::vector<Clip>& clips, FramePos position);

// Move a whole clip without overwriting another. Position is its new left edge;
// touching an adjacent boundary is allowed. Source data/envelopes are unchanged.
[[nodiscard]] bool canMoveClip(const Project& project, TrackId from, std::size_t index,
                                TrackId to, FramePos position) noexcept;
bool moveClip(Project& project, TrackId from, std::size_t index,
              TrackId to, FramePos position);

// Removes [start, end) and closes the gap, moving later clips earlier. This is what
// cut and "delete, close the gap" do; plain delete leaves the gap (A13, and see
// clearRange below).
void removeRange(std::vector<Clip>& clips, FramePos start, FramePos end);

// Deletes one interval from every track, closing the same amount of time on all
// of them regardless of mute/solo. Bounds are clamped to the project timeline.
// Sequence markers remain at their absolute times, as with other clip edits.
bool removeRangeAllTracks(Project& project, FramePos start, FramePos end);

// Removes [start, end) and leaves the gap: later clips stay exactly where they are
// and the range renders as silence. This is what delete does -- amendment A13, the
// owner's call: pulling a breath out of a voiceover must not slide everything after
// it, and closing the gap is its own deliberate command.
void clearRange(std::vector<Clip>& clips, FramePos start, FramePos end);

// Removes each range from the given tracks only (Tidy pauses): their clips close
// up, their volume, pan and effect automation lose the same time, and markers move
// with them when asked; other tracks do not move. Ranges are [start, end) on the
// timeline as it is now and must not overlap. False, with nothing changed, when an
// automation limit would be passed.
bool removeRangesOnTracks(Project& project, const std::vector<TrackId>& tracks,
                          std::vector<std::pair<FramePos, FramePos>> ranges, bool moveMarkers);

// Removes everything outside [start, end) and moves what is left to the origin.
void trimToRange(std::vector<Clip>& clips, FramePos start, FramePos end);

// Extracts a copy of [start, end) rebased to start at zero, leaving the list alone.
[[nodiscard]] std::vector<Clip> copyRange(const std::vector<Clip>& clips,
                                          FramePos start, FramePos end);

// Inserts `incoming` at `position`, pushing everything at or after it later. This
// is what paste does in ripple mode.
void insertAt(std::vector<Clip>& clips, const std::vector<Clip>& incoming,
              FramePos position);

// Lays `incoming` down at `position` over whatever is there: the range it covers is
// cleared first and nothing after it moves. This is what paste does when ripple is
// off -- the timeline keeps its length and everything else keeps its place.
void overwriteAt(std::vector<Clip>& clips, const std::vector<Clip>& incoming,
                 FramePos position);

// Split a rendered descriptor along current editing-group intervals, keeping its
// audio offsets/envelopes. Coordinates are absolute; gaps remain ungrouped. Equal
// adjacent membership is coalesced. Invalid geometry throws without mutating input.
[[nodiscard]] std::vector<Clip> withClipGroups(const Clip& rendered,
                                              const std::vector<Clip>& ownership);
// Replace the rendered interval while inheriting editing membership. `ownership`
// may alias `clips`; all pieces are prepared before the destination is changed.
void overwriteRendered(std::vector<Clip>& clips, const Clip& rendered,
                       const std::vector<Clip>& ownership);

// Applies a linear gain multiplier across [start, end), splitting at the edges so the
// change lands only on the selection.
void applyGain(std::vector<Clip>& clips, FramePos start, FramePos end, float gain);

// Fades across [start, end). A fade-in ramps from silence at `start`; a fade-out ramps
// to silence at `end`. Each replaces its own direction across the selection, keeps
// clip gain and the opposite fade, and continues through clip boundaries and gaps.
void applyFadeIn(std::vector<Clip>& clips, FramePos start, FramePos end);
void applyFadeOut(std::vector<Clip>& clips, FramePos start, FramePos end);

// Total timeline length.
[[nodiscard]] FramePos totalLength(const std::vector<Clip>& clips) noexcept;

// Debug/test helper: the invariants everything else assumes.
[[nodiscard]] bool isWellFormed(const std::vector<Clip>& clips) noexcept;

} // namespace edits
} // namespace omawave::session
