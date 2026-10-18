#pragma once

#include "types.h"

#include <algorithm>

namespace omawave::session {

// A range on the timeline. Owned by Session because Phase 3's cut, trim, fade and gain
// all operate on it -- keeping it in the UI would only mean moving it later.
//
// Always normalised: start <= end. A selection with start == end is a cursor position
// rather than a range, and `empty()` says so.
struct Selection {
    FramePos start = 0;
    FramePos end = 0;

    // Which lane the range was selected on. Ordinary edits act on that track;
    // A30's explicit all-tracks delete uses the same time bounds across the session.
    TrackId track = kInvalidTrack;
    int channel = -1; // all channels, or one physical channel
    bool operator==(const Selection&) const = default;

    [[nodiscard]] constexpr bool empty() const noexcept { return start >= end; }
    [[nodiscard]] constexpr FramePos length() const noexcept
    {
        return empty() ? 0 : end - start;
    }

    // Orders the two edges and clamps them to the take, so a backwards drag or a drag
    // off the end of the window still produces a valid range.
    [[nodiscard]] static constexpr Selection normalised(FramePos a, FramePos b,
                                                        FramePos limit,
                                                        TrackId track) noexcept
    {
        FramePos lo = a < b ? a : b;
        FramePos hi = a < b ? b : a;
        lo = std::clamp(lo, FramePos{0}, limit);
        hi = std::clamp(hi, FramePos{0}, limit);
        return Selection{.start = lo, .end = hi, .track = track};
    }
};

} // namespace omawave::session
