#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace omawave::engine {
// PipeWire graph ticks and stream frames have different rates. Callback-safe,
// bounded conversion; -1 means the timing report cannot be used.
inline std::int64_t streamDelayNs(std::int64_t delay, std::uint32_t numerator,
    std::uint32_t denominator, std::uint64_t queued, std::uint64_t buffered,
    std::uint32_t sampleRate) noexcept
{
    if (!numerator || !denominator || !sampleRate) return -1;
    const long double ns = std::max(std::int64_t{0}, delay) * 1.0e9L * numerator / denominator
        + (static_cast<long double>(queued) + buffered) * 1.0e9L / sampleRate;
    if (!std::isfinite(ns) || ns > 2.0e9L) return -1;
    return static_cast<std::int64_t>(std::round(ns));
}

inline std::int64_t recordingAlignment(std::int64_t inputNs, std::int64_t outputNs,
                                       std::uint32_t rate) noexcept
{
    if (inputNs <= 0 || outputNs <= 0 || !rate) return std::numeric_limits<std::int64_t>::min();
    const long double frames = (static_cast<long double>(outputNs) - inputNs) * rate / 1.0e9L;
    if (std::abs(frames) > static_cast<long double>(rate) * 2) return std::numeric_limits<std::int64_t>::min();
    return static_cast<std::int64_t>(std::round(frames));
}
} // namespace omawave::engine
