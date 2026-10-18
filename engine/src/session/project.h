#pragma once

#include "audio_buffer.h"
#include "marker.h"
#include "metadata.h"
#include "musical_time.h"
#include "video.h"
#include "track.h"
#include "types.h"
#include "uuid.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace omawave::session {

struct ClipGroup {
    ClipGroupId id = kInvalidClipGroup;
    std::string name;
    std::string colour; // empty inherits the track, otherwise #RRGGBB
    [[nodiscard]] bool valid() const noexcept;
    bool operator==(const ClipGroup&) const = default;
};

// A rendered transition retains its inputs so subsequent adjustments never
// process a previous blend. References point to earlier immutable sources.
struct CrossfadeRecipe {
    Clip left{}, right{};
    FramePos start = 0, end = 0;
    bool equalPower = false;
    bool operator==(const CrossfadeRecipe&) const = default;
};

// Imported audio, immutable after it is added to the Project. Project snapshots
// share these sources; edits and repairs add a source instead of changing one.
//
// BRIEF.md section 4: imported audio is copied into the project, never referenced in
// place. `originalPath` is kept for display only -- nothing reads from it after import.
struct AudioSource {
    SourceId id = kInvalidSource;

    // Names this source's files inside a saved project (BRIEF.md section 4). Assigned
    // at import and preserved across save and load, unlike `id`, which is only an index
    // into this run's source list.
    Uuid uuid;

    std::string name;
    std::string originalPath;
    AudioBuffer audio;
    std::uint32_t sampleRate = 0; // actual stored rate; unused sources survive rate-change undo

    // The rate the file was in before import, when it differed from the project rate.
    // Non-null means the audio was resampled on the way in.
    std::optional<std::uint32_t> originalSampleRate;
    std::optional<CrossfadeRecipe> crossfade{};

    // What the file said about itself, kept as read (markers in this source's
    // stored frames). Shown, never merged into the project's own metadata.
    AudioMetadata provenance;
};

// The project model. Plain C++20, no Qt, unit-testable headless (BRIEF.md section 3).
//
// This class is the reason the engine is a separate process: the boundary is
// structural, not a matter of discipline. If this file ever includes a Qt header, the
// engine stops linking.
class Project {
public:
    // The graph uses one project rate; conversion happens off-thread before install.
    explicit Project(std::uint32_t sampleRate) : sampleRate_(sampleRate) {}

    [[nodiscard]] std::uint32_t sampleRate() const noexcept { return sampleRate_; }

    // The project's own broadcast description (see metadata.h).
    [[nodiscard]] ProjectMetadata& metadata() noexcept { return metadata_; }
    [[nodiscard]] const ProjectMetadata& metadata() const noexcept { return metadata_; }
    // The reference picture, if any (see video.h).
    [[nodiscard]] VideoReference& video() noexcept { return video_; }
    [[nodiscard]] const VideoReference& video() const noexcept { return video_; }

    [[nodiscard]] MusicalTime& musicalTime() noexcept { return musicalTime_; }
    [[nodiscard]] const MusicalTime& musicalTime() const noexcept { return musicalTime_; }

    [[nodiscard]] const std::vector<ClipGroup>& clipGroups() const noexcept { return clipGroups_; }
    [[nodiscard]] ClipGroup* clipGroup(ClipGroupId id) noexcept;
    [[nodiscard]] const ClipGroup* clipGroup(ClipGroupId id) const noexcept;
    // IDs never rewind across Undo. Exhaustion fails instead of reusing an ID.
    [[nodiscard]] ClipGroupId addClipGroup(std::string name, std::string colour = {});
    bool insertClipGroup(ClipGroup group);
    bool removeClipGroup(ClipGroupId id);
    void restoreClipGroups(const std::vector<ClipGroup>& groups);
    [[nodiscard]] std::string validateClipGroups() const;

    // Worker/main thread with playback stopped when changing rate. Live clips
    // must refer to sources at this rate; unused sources may retain an older rate.
    void setSampleRate(std::uint32_t rate) noexcept { sampleRate_ = rate; }
    [[nodiscard]] std::uint32_t defaultExportBits() const noexcept { return defaultExportBits_; }
    void setDefaultExportBits(std::uint32_t bits) noexcept { defaultExportBits_ = bits; }

    // Takes ownership. The buffer must already be at the project rate; resampling is
    // the importer's job (section 2: resample on import, never mid-graph).
    SourceId addSource(std::string name, std::string originalPath, AudioBuffer audio,
                       std::optional<std::uint32_t> originalSampleRate = std::nullopt,
                       Uuid uuid = Uuid{}, std::optional<CrossfadeRecipe> crossfade = std::nullopt,
                       std::uint32_t storedRate = 0, AudioMetadata provenance = {});

    [[nodiscard]] const AudioSource* source(SourceId id) const noexcept;

    // Loading resolves a clip's source by uuid, because that is what session.toml
    // stores -- a SourceId is only meaningful within one run.
    [[nodiscard]] const AudioSource* sourceByUuid(const Uuid& uuid) const noexcept;
    [[nodiscard]] std::optional<SourceId> sourceIdByUuid(const Uuid& uuid) const noexcept;

    // Main/worker housekeeping. Current clips plus caller-protected history and
    // clipboard ids retain all recursive crossfade dependencies. Removed entries
    // become vacant; ids are never renumbered or reused within this session.
    [[nodiscard]] std::vector<bool> usedSources(const std::vector<SourceId>& protectedSources = {}) const;
    std::size_t pruneSources(const std::vector<SourceId>& protectedSources = {});

    // Only the source handles are copied for a save/autosave/render snapshot, not
    // the audio. Each immutable source has a stable address across imports and
    // copies. Shared ownership is managed off the audio thread: the render plan
    // still holds raw pointers, protected by the existing retirement handshake.
    // Vacant entries are possible after pruning: use source(id), or skip nulls.
    [[nodiscard]] const std::deque<std::shared_ptr<const AudioSource>>& sources() const noexcept
    {
        return sources_;
    }

    // --- tracks -----------------------------------------------------------------
    //
    // TrackIds are handed out from a counter and never reused, so a removed track
    // cannot be confused with the one that took its place. Lookup is a linear scan,
    // which is the right trade at the track counts a session actually reaches.

    TrackId addTrack(std::string name);

    // Re-adds a track with its original id, for undo. Ids stay unique because the
    // counter is never rewound.
    void insertTrack(std::size_t index, Track track);

    bool removeTrack(TrackId id);

    // Moves a track to a new position in the display order.
    bool moveTrack(TrackId id, std::size_t toIndex);

    [[nodiscard]] Track* track(TrackId id) noexcept;
    [[nodiscard]] const Track* track(TrackId id) const noexcept;

    // Position in the track list, for indexing the render plan.
    [[nodiscard]] std::optional<std::size_t> trackIndex(TrackId id) const noexcept;

    [[nodiscard]] std::vector<Track>& tracks() noexcept { return tracks_; }
    [[nodiscard]] const std::vector<Track>& tracks() const noexcept { return tracks_; }

    // True when at least one track is soloed, which is what turns every other track's
    // effective gain to zero.
    [[nodiscard]] bool anySoloed() const noexcept;

    // Longest track, in frames. Zero for an empty project.
    [[nodiscard]] FramePos length() const noexcept;

    // --- markers ----------------------------------------------------------------
    //
    // Kept sorted by position, so the UI's list and the lane's flags read in
    // timeline order without every consumer sorting for itself. Ids come from a
    // counter and are never reused, for the same reason track ids are not.

    MarkerId addMarker(Marker marker);
    // An asynchronous command may publish its identity before inserting it.
    // Reservations remain unique even if that command is later cancelled.
    [[nodiscard]] MarkerId reserveMarkerId() noexcept;

    // Re-adds a marker with its original id, for undo.
    void insertMarker(Marker marker);

    bool removeMarker(MarkerId id);

    [[nodiscard]] Marker* marker(MarkerId id) noexcept;
    [[nodiscard]] const Marker* marker(MarkerId id) const noexcept;

    // Call after changing a marker's position through the pointer above.
    void sortMarkers();

    [[nodiscard]] std::vector<Marker>& markers() noexcept { return markers_; }
    [[nodiscard]] const std::vector<Marker>& markers() const noexcept { return markers_; }

    // --- the master bus ---------------------------------------------------------
    //
    // What the summed tracks go through before the output: a trim, then inserts.
    // The monitor level is NOT here -- it is a listening level the transport owns
    // and a mixdown never applies. The trim and the inserts are part of the mix,
    // so an export renders through them (render/master_render.h) exactly as the
    // transport plays them. `mixdown()` stays the plain sum; the bus sits on top.

    [[nodiscard]] std::vector<Insert>& masterInserts() noexcept { return masterInserts_; }
    [[nodiscard]] const std::vector<Insert>& masterInserts() const noexcept
    {
        return masterInserts_;
    }

    // Linear, applied before the inserts. Like a fader it is not an undo step.
    [[nodiscard]] float masterTrim() const noexcept { return masterTrim_; }
    void setMasterTrim(float linear) noexcept { masterTrim_ = linear; }

    [[nodiscard]] AutomationCurve& masterVolumeAutomation() noexcept { return masterVolumeAutomation_; }
    [[nodiscard]] const AutomationCurve& masterVolumeAutomation() const noexcept { return masterVolumeAutomation_; }
    [[nodiscard]] std::size_t automationPointCount() const noexcept;

private:
    std::uint32_t sampleRate_;
    ProjectMetadata metadata_;
    VideoReference video_;
    MusicalTime musicalTime_;
    std::vector<ClipGroup> clipGroups_;
    std::uint32_t nextClipGroupId_ = 0;
    std::uint32_t defaultExportBits_ = 24;
    std::deque<std::shared_ptr<const AudioSource>> sources_;
    std::vector<Track> tracks_;
    std::uint32_t nextTrackId_ = 0;
    std::vector<Marker> markers_;
    std::uint32_t nextMarkerId_ = 0;
    std::vector<Insert> masterInserts_;
    float masterTrim_ = 1.0f;
    AutomationCurve masterVolumeAutomation_{};
};

} // namespace omawave::session
