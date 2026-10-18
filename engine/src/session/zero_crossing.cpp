#include "zero_crossing.h"

#include <algorithm>
#include <cmath>

namespace omawave::session {
namespace {

// Treats a sign change through this band as a crossing. Exact zero is rare in real
// audio, and room tone hovering either side of it would otherwise produce a crossing
// on almost every frame, which is no more useful than not snapping at all.
constexpr float kSilenceFloor = 1e-6f;

[[nodiscard]] bool crossesAt(const std::span<const float>& p, std::size_t i) noexcept
{
    const float a = p[i - 1];
    const float b = p[i];
    if (std::fabs(a) < kSilenceFloor && std::fabs(b) < kSilenceFloor)
        return false;
    return (a <= 0.0f && b > 0.0f) || (a >= 0.0f && b < 0.0f);
}

} // namespace

FramePos nearestZeroCrossing(const AudioBuffer& audio, FramePos frame,
                             FramePos maxDistance, std::uint32_t channel) noexcept
{
    if (audio.empty() || channel >= audio.channels())
        return frame;

    const auto frames = static_cast<FramePos>(audio.frames());
    const FramePos clamped = std::clamp(frame, FramePos{0}, frames);
    if (frames < 2)
        return clamped;

    const std::span<const float> plane = audio.plane(channel);

    // Walk outward from the requested frame so the first hit is the nearest one.
    for (FramePos d = 0; d <= maxDistance; ++d) {
        const FramePos forward = clamped + d;
        if (forward >= 1 && forward < frames
            && crossesAt(plane, static_cast<std::size_t>(forward))) {
            return forward;
        }

        if (d == 0)
            continue;

        const FramePos backward = clamped - d;
        if (backward >= 1 && backward < frames
            && crossesAt(plane, static_cast<std::size_t>(backward))) {
            return backward;
        }

        // Both directions have run off the end; nothing further to find.
        if (forward >= frames && backward < 1)
            break;
    }

    return clamped;
}

} // namespace omawave::session
