#pragma once

#include "master_render.h"

namespace omawave::render {

struct DuckingSettings {
    session::FramePos start = 0, end = 0; // half-open replacement range
    double thresholdDb = -36, reductionDb = 12;
    double attackSeconds = .1, holdSeconds = .2, releaseSeconds = .5;
    bool replaceInRange = false;
};

struct DuckingResult {
    session::AutomationCurve curve;
    std::size_t regions = 0;
    session::FramePos analyzedFrames = 0;
    bool changed = false, cancelled = false, replacementRequired = false;
    std::string error;
};

// Analyze each trigger independently after its inserts, fader, automation and
// incoming routes. Solo is ignored, Mute retained. Stereo detector power is the
// maximum channel RMS in 10 ms windows; triggers combine as activity regions,
// never as samples, so opposite polarity cannot cancel a ducking trigger.
//
// Each triggerChains entry is aligned with triggers and MUST contain fresh,
// privately owned/warmed processors for that isolated graph. A processor cannot
// be shared by two passes; graph resets do not reset plugin DSP state. No active
// insert may be silently omitted. The empty outer list is for effect-free audio.
//
// Returns one proposed volume multiplier curve, never mutates the project.
// Manual points outside [start,end) retain their audio/interpolation. Nonempty
// disabled curves are refused; activating their dormant points would change
// outside audio. Silent analysis and zero reduction leave the original intact.
[[nodiscard]] DuckingResult generateDucking(const session::Project&, session::TrackId target,
    const std::vector<session::TrackId>& triggers, const DuckingSettings&,
    const std::vector<std::vector<TrackChain>>& triggerChains = {},
    const std::atomic<bool>* cancel = nullptr, const ProgressFn& progress = {});

} // namespace omawave::render
