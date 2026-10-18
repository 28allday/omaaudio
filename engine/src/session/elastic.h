#pragma once
#include "project.h"
#include <string>

namespace omawave::session {
// Metadata only. A changed result is committed by the caller as one undo step.
[[nodiscard]] std::string setElasticClip(Project& project, TrackId track, std::size_t clip,
                                        FramePos frames, float semitones);
[[nodiscard]] ElasticSpec elasticInput(const Clip& clip);
// Builds one descriptor without mutating the project or checking neighbours.
// Group callers validate collisions and the elastic run limit on the final set.
[[nodiscard]] Clip makeElasticClip(const Project&, const Clip&, FramePos frames, float semitones);
// Preserve inherited effect automation coordinates when a view is retimed or
// its dry mapping changes. Prepare a fresh immutable region before assignment.
void rescaleClipEffects(Clip& clip, const Clip& original, FramePos numerator, FramePos denominator);
[[nodiscard]] std::string validateElastic(const Project& project);
} // namespace omawave::session
