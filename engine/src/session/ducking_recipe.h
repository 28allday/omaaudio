#pragma once

#include "automation.h"
#include "types.h"

#include <vector>

namespace omawave::session {

// How a track's generated ducking was made, kept so it can be made again when
// the trigger tracks change. `baseline` is the volume curve before generation;
// `generated` is the curve installed last. A target curve that no longer equals
// `generated` was edited by hand: the recipe then stops following, and the
// person's edit is never overwritten.
struct DuckingRecipe {
    std::vector<TrackId> triggers;
    FramePos start = 0, end = 0;                 // half-open replacement range
    double thresholdDb = -36, reductionDb = 12;
    double attackSeconds = .1, holdSeconds = .2, releaseSeconds = .5;
    bool replaceInRange = false;
    AutomationCurve baseline{}, generated{};

    [[nodiscard]] bool valid() const noexcept {
        return !triggers.empty() && start >= 0 && end > start
            && thresholdDb >= -80 && thresholdDb <= 0 && reductionDb >= 0 && reductionDb <= 60
            && attackSeconds >= 0 && attackSeconds <= 10 && holdSeconds >= 0 && holdSeconds <= 10
            && releaseSeconds >= 0 && releaseSeconds <= 10;
    }
    [[nodiscard]] bool operator==(const DuckingRecipe&) const = default;
};

} // namespace omawave::session
