#pragma once

#include "repair.h"
#include <atomic>

namespace omawave::spectral {

enum class ChannelScope { All, Left, Right, Mid, Side };

// Independent of display mode. Mid/side use (L +/- R)/2. Merging adds only the
// selected component's delta, preserving the other component to float precision.
[[nodiscard]] session::AudioBuffer repairChannels(const session::AudioBuffer&, ChannelScope,
    const std::atomic<bool>* cancel = nullptr);
void mergeRepairChannels(session::AudioBuffer&, const session::AudioBuffer&, ChannelScope,
    const std::atomic<bool>* cancel = nullptr);

struct RestorationResult {
    bool changed = false;
    bool cancelled = false;
    std::size_t restored = 0;
    std::size_t skipped = 0;
};

// Hard flat peaks, not soft saturation. Independent channel models use only
// unclipped training windows. Uncertain/long plateaus stay intact. Atomic: errors
// or cancellation never write a partial repair to audio. Threshold is amplitude.
[[nodiscard]] RestorationResult restoreClippedPeaks(
    session::AudioBuffer&, std::uint32_t rate, Region, const MaskSource&, float threshold,
    const StftConfig&, const std::atomic<bool>* cancel = nullptr);

// Bounded late-tail attenuation from a decaying per-bin energy envelope. This is
// not a dry-signal reconstruction or an early-reflection remover. decaySeconds
// is the envelope's T60. Attacks and sustained bins are left at unity gain.
[[nodiscard]] RestorationResult reduceReverb(
    session::AudioBuffer&, std::uint32_t rate, Region, const MaskSource&, float amountDb,
    float decaySeconds, float strength, const StftConfig&,
    const std::atomic<bool>* cancel = nullptr);

} // namespace omawave::spectral
