#include "command.h"
#include "effect_racks.h"

#include <utility>
#include <limits>

namespace omawave::session {

bool UndoStack::push(std::string label, TrackId track, SessionState before,
                     SessionState after)
{
    if (before == after && before.sameInsertInstances(after))
        return false;

    // A new edit after undoing discards the redo branch, which is what every editor
    // does and what users expect.
    commands_.resize(cursor_);

    commands_.push_back(EditCommand{
        .label = std::move(label),
        .track = track,
        .before = std::move(before),
        .after = std::move(after),
        .serial = version_ + 1,
    });
    ++version_;
    cursor_ = commands_.size();

    if (commands_.size() > kMaxDepth) {
        const std::size_t excess = commands_.size() - kMaxDepth;
        commands_.erase(commands_.begin(),
                        commands_.begin() + static_cast<std::ptrdiff_t>(excess));
        cursor_ = commands_.size();
    }
    trimOpaqueHistory();
    return true;
}

void UndoStack::trimOpaqueHistory()
{
    // Snapshot copies share immutable payloads. Count each UUID once, newest
    // commands first, so ordinary edits do not multiply the same stored bytes.
    PluginStateBudget budget(opaqueLimit_, std::numeric_limits<std::size_t>::max());
    std::string error;
    const auto add = [&](const SessionState& state) {
        bool valid = true;
        forEachEffectRack(state, [&](EffectOwner, const auto& rack) {
            if (!valid) return;
            for (const auto& insert : rack) for (const auto& opaque : insert.instanceState)
                if (!budget.add(opaque, error)) { valid = false; return; }
        });
        return valid;
    };
    for (std::size_t i = commands_.size(); i > 0; --i) {
        const auto& command = commands_[i - 1];
        if (add(command.before) && add(command.after)) continue;
        commands_.erase(commands_.begin(), commands_.begin() + static_cast<std::ptrdiff_t>(i));
        cursor_ = commands_.size();
        break;
    }
}

std::string UndoStack::undoLabel() const
{
    if (!canUndo())
        return {};
    return commands_[cursor_ - 1].label;
}

std::string UndoStack::redoLabel() const
{
    if (!canRedo())
        return {};
    return commands_[cursor_].label;
}

const EditCommand* UndoStack::undo() noexcept
{
    if (!canUndo())
        return nullptr;
    --cursor_; ++version_;
    return &commands_[cursor_];
}

const EditCommand* UndoStack::redo() noexcept
{
    if (!canRedo())
        return nullptr;
    const EditCommand* command = &commands_[cursor_];
    ++cursor_; ++version_;
    return command;
}

bool UndoStack::amendLatest(SessionState after)
{
    if (cursor_ == 0 || cursor_ != commands_.size()) return false;
    commands_.back().after = std::move(after);
    ++version_;
    trimOpaqueHistory();
    return true;
}

bool UndoStack::foldSince(std::uint64_t version, std::string label)
{
    if (cursor_ != commands_.size()) return false;
    std::size_t first = commands_.size();
    while (first > 0 && commands_[first - 1].serial > version) --first;
    if (first == commands_.size()) return false;
    EditCommand folded{.label = std::move(label), .track = commands_[first].track,
                       .before = std::move(commands_[first].before), .after = std::move(commands_.back().after),
                       .serial = version_ + 1};
    commands_.resize(first);
    ++version_;
    if (folded.before == folded.after && folded.before.sameInsertInstances(folded.after)) {
        cursor_ = commands_.size();
        return false;
    }
    commands_.push_back(std::move(folded));
    cursor_ = commands_.size();
    trimOpaqueHistory();
    return true;
}

void UndoStack::discardRedo()
{
    if (cursor_ == commands_.size()) return;
    commands_.resize(cursor_);
    ++version_;
}

void UndoStack::clear() noexcept
{
    commands_.clear();
    cursor_ = 0; ++version_;
}

} // namespace omawave::session
