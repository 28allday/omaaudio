#pragma once
#include "clip.h"
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace omawave::session {
// Exact nearest-sample scaling, including fades inherited from a longer clip.
// No floating conversion or overflowing intermediate product at frame limits.
inline FramePos scaledFrames(FramePos value, FramePos numerator, FramePos denominator)
{
    constexpr auto limit = std::uint64_t(std::numeric_limits<FramePos>::max());
    if (value < 0 || numerator < 0 || denominator <= 0)
        throw std::runtime_error("invalid frame scale");
    const auto d = std::uint64_t(denominator), n = std::uint64_t(numerator);
    const auto whole = std::uint64_t(value) / d, part = std::uint64_t(value) % d;
    if (n && whole > limit / n) throw std::runtime_error("scaled duration exceeds the timeline");
    const auto base = whole * n;
    std::uint64_t quotient = 0, remainder = 0;
    for (int bit = 62; bit >= 0; --bit) {
        quotient *= 2; remainder *= 2;
        if (remainder >= d) { remainder -= d; ++quotient; }
        if ((n >> bit) & 1) {
            remainder += part;
            if (remainder >= d) { remainder -= d; ++quotient; }
        }
    }
    if (remainder >= d / 2 + d % 2) ++quotient;
    if (quotient > limit - base) throw std::runtime_error("scaled duration exceeds the timeline");
    return FramePos(base + quotient);
}
} // namespace omawave::session
