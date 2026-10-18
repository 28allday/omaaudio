#pragma once

#include <cstdint>

namespace omawave::session {

enum class FadeShape : std::uint8_t { Linear = 0, Smooth = 1 };

[[nodiscard]] constexpr bool validFadeCurve(FadeShape shape, float bend) noexcept
{
    // Bounds also reject infinities and NaN without any callback-time work.
    return (shape == FadeShape::Linear || shape == FadeShape::Smooth)
        && bend >= -1.0f && bend <= 1.0f;
}

// The same bounded curve is used by the session renderer and immutable playback
// plan. No allocation, initialization, transcendental function or mutable state.
[[nodiscard]] constexpr float fadeCurveAt(float t, FadeShape shape, float bend) noexcept
{
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    if (shape == FadeShape::Linear && bend == 0.0f) return t;
    const float x = shape == FadeShape::Smooth ? t * t * (3.0f - 2.0f * t) : t;
    const float k = bend >= 0.0f ? 1.0f - 0.875f * bend : 1.0f - 7.0f * bend;
    const float result = x / (k + (1.0f - k) * x);
    return result <= 0.0f ? 0.0f : result >= 1.0f ? 1.0f : result;
}

struct ClipFadeShapes {
    FadeShape fadeInShape = FadeShape::Linear, fadeOutShape = FadeShape::Linear;
    float fadeInCurve = 0, fadeOutCurve = 0;
    [[nodiscard]] constexpr bool valid() const noexcept
    {
        return validFadeCurve(fadeInShape, fadeInCurve) && validFadeCurve(fadeOutShape, fadeOutCurve);
    }
    constexpr bool operator==(const ClipFadeShapes&) const noexcept = default;
};

} // namespace omawave::session
