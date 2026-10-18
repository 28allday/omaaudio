#pragma once
#include "clip.h"
#include <string>

namespace omawave::session {
class Project;
// Scales clip-owned coordinates, never source/timeline placement. The caller
// sets the new clip length. All knots (including hidden/dormant ones) survive;
// rounding collisions and overflow throw without modifying the supplied clip.
void scaleClipEnvelopes(Clip&, FramePos numerator, FramePos denominator);
// Validate live and immutable recipe curves, and the combined automation budget.
[[nodiscard]] std::string validateClipEnvelopes(const Project&);
[[nodiscard]] std::string validateClipEnvelopes(const std::vector<Clip>&);
} // namespace omawave::session
