#pragma once
#include "sample_edit.h"
namespace omawave::session {
// Rounds absolute frame positions while preserving seconds. Checked overflow.
[[nodiscard]] FramePos ratePosition(FramePos frame, std::uint32_t from, std::uint32_t to);
// Worker only; retains old sources for undo and rescales all frame-based metadata.
[[nodiscard]] SampleEditResult convertProjectRate(Project, std::uint32_t rate,
    std::uint32_t exportBits, const std::atomic<bool>* cancel = nullptr);
} // namespace omawave::session
