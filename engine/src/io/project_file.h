#pragma once

#include "io/peak_builder.h"
#include "session/project.h"
#include "session/selection.h"
#include "session/types.h"
#include "session/uuid.h"
#include "spectral/mask.h"
#include "spectral/repair.h"

#include <atomic>
#include <functional>
#include <vector>
#include <map>
#include <optional>
#include <string>

namespace omawave::io {

// Reading and writing a `.omaaudio` project (BRIEF.md section 4, amended by A7).
//
// A directory, not a zip. Section 4's layout is unchanged inside it:
//
//   voiceover.omaaudio/
//     manifest.toml      formatVersion, sample rate, timestamps
//     session.toml       tracks, their parameters, their clips
//     audio/<uuid>.wav   float32 WAV, written once on import
//     peaks/<uuid>.peak  level-0 peak cache, regenerable
//     state/<uuid>.omas  immutable opaque plugin snapshots (format 16)
//
// The reason for a directory is in amendment A7: section 4 also requires imported
// audio to be copied into the project, so a session is hundreds of megabytes, and a
// zip cannot update one entry in place. Every save would rewrite all of it to change
// a few kilobytes of TOML.

// The newest format this build reads. What it WRITES depends on the content
// (amendment A11): 1 for a project no older build would misread, 2 once a track
// carries an insert, 3 for a partial fade ramp (A29), 4 for a pitch-following
// harmonic mask (A31). Older builds refuse these to preserve their meaning. Roles, colours,
// heights and markers are additive keys an older reader ignores, so they do not
// move the number.
inline constexpr int kProjectFormatVersion = 25;
inline constexpr int kProjectFormatVersionPlain = 1;
inline constexpr int kProjectFormatVersionInserts = 2;
inline constexpr int kProjectFormatVersionPartialFades = 3;
inline constexpr int kProjectFormatVersionPitch = 4;
inline constexpr int kProjectFormatVersionAutomation = 5;
inline constexpr int kProjectFormatVersionCrossfade = 6;
inline constexpr int kProjectFormatVersionRouting = 7;
inline constexpr int kProjectFormatVersionScopedPrint = 8;
inline constexpr int kProjectFormatVersionSourceRate = 9;
inline constexpr int kProjectFormatVersionElastic = 10;
inline constexpr int kProjectFormatVersionMetadata = 11;
inline constexpr int kProjectFormatVersionVideo = 12;      // W11: a reference picture
inline constexpr int kProjectFormatVersionClapIds = 13;    // stable CLAP parameter identity
inline constexpr int kProjectFormatVersionMusicalTime = 14; // constant tempo and meter
inline constexpr int kProjectFormatVersionPortableVideo = 15; // bundle-relative reference picture
inline constexpr int kProjectFormatVersionPluginState = 16; // opaque instance state and precise plugin parameters
inline constexpr int kProjectFormatVersionStateControls = 17; // state-authoritative controls and later explicit overrides
inline constexpr int kProjectFormatVersionAutomationSegments = 18; // outgoing Hold/Linear overrides on automation points
inline constexpr int kProjectFormatVersionInstanceAutomation = 19; // independent actual-processor curves and bases
inline constexpr int kProjectFormatVersionClipGroups = 20; // persistent named clip membership
inline constexpr int kProjectFormatVersionClipEnvelopes = 22; // clip-owned volume and stereo balance curves
inline constexpr int kProjectFormatVersionFadeShapes = 21; // clip and recipe fade shapes/bends
inline constexpr int kProjectFormatVersionSmoothAutomation = 23; // smooth segments and retained phase windows
inline constexpr int kProjectFormatVersionClipEffects = 24; // shared clip effect regions and their inserts
inline constexpr int kProjectFormatVersionDuckingRecipe = 25; // generated ducking that follows its triggers
inline constexpr std::uint32_t kCachedPeakSamplesPerPeak = 256;

// What a project holds beyond the Project itself: which track had focus, what was
// selected, and the Spectral page's working state (amendment A22): the band, the
// mask with its feather, and the noise print. None of it is an undo step; all of it
// is work somebody did and would have to do again.
struct ProjectView {
    session::TrackId focus = session::kInvalidTrack;
    session::Selection selection;
    spectral::Band band;                    // both 0 = the whole spectrum
    spectral::MaskShapes mask;              // its feather rides with it
    spectral::NoisePrint noisePrint;
    std::string noisePrintSummary;
    // The Sound tab's own choices per track that are not effects (its loudness
    // target, its room echo amount): additive keys, so an older reader skips them.
    std::map<session::TrackId, std::map<std::string, double>> sound;
};

// Which pair of TOML files a save writes or a load reads (amendment A22).
//
//   Project   manifest.toml + session.toml at the root: the project as saved.
//   Recovery  recovery/manifest.toml + recovery/session.toml: the autosave, written
//             behind the user's back and never over the project. Both layers share
//             audio/, peaks/ and immutable state/. Only changed plugin snapshots
//             need new state files.
enum class Layer { Project, Recovery };

struct SaveResult {
    bool ok = false;
    std::string error;
    bool cancelled = false;
};
struct SaveOptions {
    const std::atomic<bool>* cancel = nullptr;
    std::function<void(const std::string& phase, float progress)> progress;
    // A collected bundle can rebuild disposable peaks on its first open.
    bool buildPeakCaches = true;
};

struct LoadResult {
    bool ok = false;
    std::string error;

    std::optional<session::Project> project;
    ProjectView view;

    // Level-0 peaks read from the cache, by source uuid. Empty entries mean "rebuild";
    // the caller never has to know whether a file was missing or unusable.
    std::map<std::string, PeakLevel> peaks;
};

// Writes the bundle at `path`, creating it if needed.
//
// Complete source WAVs are retained; absent/incomplete sources are written to unique
// temporary files and durably published. A fsynced .save-pending.toml journal holds
// both metadata tables while the ordinary files are replaced. Load uses a complete
// journal after interruption; the next save finishes it before writing another.
// Success means the files and directory entries have been synced. Cooperating
// readers/writers lock the bundle directory; a conflicting operation returns busy.
//
// Worker thread only: this writes hundreds of megabytes and must not touch the loop
// thread, let alone the audio thread.
[[nodiscard]] SaveResult saveProject(const session::Project& project,
                                     const ProjectView& view, const std::string& path,
                                     Layer layer = Layer::Project, const SaveOptions& options = {});

// Reads a bundle. Never mutates the caller's current session -- it builds a whole new
// Project and hands it over, so a project that fails to load leaves what is open alone.
// Reads a pending journal without modifying it, and refuses truncated/mismatched audio.
[[nodiscard]] LoadResult loadProject(const std::string& path, Layer layer = Layer::Project);

// The recovery layer: whether a bundle has one, and taking it away (after a real save,
// or when the user says the autosave is not wanted). Removing what is not there is
// not an error. Returns an error message, empty on success.
[[nodiscard]] bool hasRecovery(const std::string& path);
[[nodiscard]] std::string discardRecovery(const std::string& path);

struct UnusedAudioFile {
    std::string name; // canonical UUID.wav, relative to the bundle's audio directory
    std::uint64_t bytes = 0, device = 0, inode = 0;
    std::int64_t modifiedSeconds = 0, modifiedNanoseconds = 0;
    bool operator==(const UnusedAudioFile&) const = default;
};
struct AudioCleanupPreview {
    bool ok = false;
    std::string error;
    std::vector<UnusedAudioFile> files;
};
struct AudioCleanupResult {
    bool ok = false, cancelled = false;
    std::string error;
    std::size_t removed = 0;
    std::uint64_t bytes = 0;
};
// Worker housekeeping under the bundle lock. Protect both metadata layers and
// caller-owned sources (including undo/clipboard), revalidate file identities on
// apply, and only unlink the previewed unreferenced canonical UUID.wav files.
[[nodiscard]] AudioCleanupPreview inspectUnusedAudio(const std::string& path,
    const std::vector<std::string>& protectedUuids = {});
[[nodiscard]] AudioCleanupResult cleanupUnusedAudio(const std::string& path,
    const std::vector<UnusedAudioFile>& preview, const std::vector<std::string>& protectedUuids = {},
    const std::atomic<bool>* cancel = nullptr);

// True when `clips` is one clip covering its whole source at unity with no fades: the
// state a freshly imported, unedited track is in, and the only case where a cached
// per-source peak level is also the track's timeline peak level.
[[nodiscard]] bool isWholeSourceClip(const session::Project& project,
                                     const std::vector<session::Clip>& clips);

} // namespace omawave::io
