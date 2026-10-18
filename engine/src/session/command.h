#pragma once

#include "state.h"
#include "types.h"

#include <cstddef>
#include <string>
#include <vector>

namespace omawave::session {

// One reversible edit.
//
// BRIEF.md section 5: "Undo is command-based, not buffer snapshots." Each entry is a
// named operation holding the session state either side of it -- clip descriptors and
// track parameters only, never audio. Cutting ten minutes out of a take stores a few
// dozen bytes here, not ten minutes of samples, because clips reference shared
// immutable sources.
//
// Storing both sides rather than an inverse operation is deliberate: an inverse for
// every edit is a second implementation of every edit, and the two drift. The cost is
// a few dozen bytes per clip, which is nothing next to the audio it describes.
struct EditCommand {
    std::string label;                 // shown in the UI: "undo delete"
    TrackId track = kInvalidTrack;     // which track it acted on, for refocusing
    SessionState before;
    SessionState after;
    std::uint64_t serial = 0; // stable within this stack, including across Undo/Redo
};

class UndoStack {
public:
    // Deep enough that nobody reaches the bottom in a session, bounded so a long
    // editing session cannot grow without limit.
    static constexpr std::size_t kMaxDepth = 256;
    static constexpr std::size_t kMaxOpaqueBytes = 512 * 1024 * 1024;
    UndoStack() = default;
    explicit UndoStack(std::size_t opaqueBytes) : opaqueLimit_(opaqueBytes) {}

    // Returns false when settings and insert identities/order are identical -- splitting on a clip
    // boundary, or applying 0 dB of gain, changes nothing, and an undo entry that
    // restores the state it was already in only makes Ctrl+Z feel broken.
    bool push(std::string label, TrackId track, SessionState before, SessionState after);

    [[nodiscard]] bool canUndo() const noexcept { return cursor_ > 0; }
    [[nodiscard]] bool canRedo() const noexcept { return cursor_ < commands_.size(); }

    // Labels of what undo and redo would do, empty when unavailable.
    [[nodiscard]] std::string undoLabel() const;
    [[nodiscard]] std::string redoLabel() const;

    // The command to reverse or reapply, or nullptr when there is nothing to do. The
    // caller installs `before` for undo and `after` for redo.
    [[nodiscard]] const EditCommand* undo() noexcept;
    [[nodiscard]] const EditCommand* redo() noexcept;

    void clear() noexcept;
    // Drops what redo would reapply, as a new edit does. For a run of temporary
    // edits (an effect preview) that has been undone and must not come back.
    void discardRedo();
    // Folds a consequence of the latest edit into it, so one Undo reverses both:
    // only when that edit is the newest entry and nothing was undone since.
    bool amendLatest(SessionState after);
    // Folds every entry pushed after `version` (a version() read earlier) into one
    // called `label`, so one Undo reverses a run of edits made as one gesture.
    // Only when they are the newest entries and none was undone since. Returns
    // whether one entry now stands for them (false also when they changed nothing
    // overall, in which case they are dropped).
    bool foldSince(std::uint64_t version, std::string label);

    [[nodiscard]] std::size_t depth() const noexcept { return commands_.size(); }
    [[nodiscard]] std::size_t cursor() const noexcept { return cursor_; }
    [[nodiscard]] std::uint64_t version() const noexcept { return version_; }

    // The whole history, oldest first, for a list a person can click through. The
    // entries at or past cursor() are what redo would reapply.
    [[nodiscard]] const std::vector<EditCommand>& commands() const noexcept
    {
        return commands_;
    }

private:
    std::vector<EditCommand> commands_;
    std::size_t cursor_ = 0;           // number of applied commands
    std::size_t opaqueLimit_ = kMaxOpaqueBytes;
    std::uint64_t version_ = 0;
    void trimOpaqueHistory();
};

} // namespace omawave::session
