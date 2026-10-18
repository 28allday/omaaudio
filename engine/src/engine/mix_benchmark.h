#pragma once

#include <atomic>
#include <cstdint>
#include <string>

// How long this computer takes to mix generated sessions, measured with the real
// transport (no audio device, no effects). Worker thread; allocates; takes a few
// seconds. Used by Preferences › Performance › Check this computer.
namespace omawave::engine {

struct MixScenario {
    unsigned tracks = 8, block = 256, points = 0, rate = 48000, blocks = 1500;
    bool bus = false;   // every track routed into the last one
};

struct MixTiming {
    bool ok = false;
    std::string error;
    double p50Us = 0, p99Us = 0, maxUs = 0;
};

[[nodiscard]] MixTiming measureMixing(const MixScenario& scenario, const std::atomic<bool>* cancel = nullptr);

struct ComputerCheck {
    bool ok = false, cancelled = false;
    std::string error;
    unsigned rate = 0, block = 0;
    double deadlineUs = 0;          // one block's duration
    double share = 0.5;             // of each block given to mixing; effects get the rest
    // Per-track mixing cost (µs per block, 99th percentile) for three kinds of track.
    double plainUs = 0, automatedUs = 0, busUs = 0;
    // Tracks that fit in `share` of each block.
    unsigned plainTracks = 0, automatedTracks = 0, busTracks = 0;
};

[[nodiscard]] ComputerCheck checkComputer(unsigned rate, unsigned block, double share = 0.5,
                                          const std::atomic<bool>* cancel = nullptr);

} // namespace omawave::engine
