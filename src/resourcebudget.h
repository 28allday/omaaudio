#pragma once

#include <algorithm>
#include <cstddef>

// How much memory OmaAudio aims to use for audio it can always read again.
// Pure functions over a memory size in MiB, so the tests can hand them any machine.
//
// The budget is a target for reusable audio, not a cap on the process: the
// project's own sources, plug-ins, graphics drivers and an export in progress
// are outside it. Half goes to the playback cache, sized for eight-channel pages
// and resident only for the channels a source really has; the rest is headroom
// left to the system's own file cache, which serves the offline readers.
namespace omaaudio::resources {

inline constexpr int minimumMiB = 1024;
inline constexpr int largestMiB = 65536;
inline constexpr int automaticCeilingMiB = 8192;

[[nodiscard]] constexpr int wholeSteps(int mib) noexcept { return mib / 256 * 256; }

// Three quarters of the machine, at most.
[[nodiscard]] constexpr int maximumMiB(int systemMiB) noexcept
{
    return std::clamp(wholeSteps(systemMiB / 4 * 3), minimumMiB, largestMiB);
}

// A quarter of the machine, up to 8 GiB.
[[nodiscard]] constexpr int automaticMiB(int systemMiB) noexcept
{
    return std::clamp(wholeSteps(systemMiB / 4), minimumMiB, std::min(automaticCeilingMiB, maximumMiB(systemMiB)));
}

// Zero asks for the automatic budget; anything else is held to the machine.
[[nodiscard]] constexpr int requestedMiB(int requested, int systemMiB) noexcept
{
    return requested <= 0 ? 0 : std::clamp(requested, minimumMiB, maximumMiB(systemMiB));
}

[[nodiscard]] constexpr int effectiveMiB(int requested, int systemMiB) noexcept
{
    const int held = requestedMiB(requested, systemMiB);
    return held ? held : automaticMiB(systemMiB);
}

struct Budget {
    int total = minimumMiB;
    int playback = minimumMiB / 2;
    [[nodiscard]] constexpr std::size_t playbackBytes() const noexcept { return std::size_t(playback) << 20; }
    [[nodiscard]] static constexpr Budget forMemory(int mib) noexcept
    {
        mib = std::clamp(mib, minimumMiB, largestMiB);
        return {mib, mib / 2};
    }
};

// Installed memory, or 4 GiB when the system will not say.
[[nodiscard]] int systemMemoryMiB() noexcept;

} // namespace omaaudio::resources
