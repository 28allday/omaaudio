#pragma once
#include "types.h"
#include <algorithm>
#include <cstddef>
#include <cmath>
#include <limits>
#include <vector>
#include <string>

namespace omawave::session {
struct PanGains;
struct Track;
struct Insert;
class Project;
enum class AutomationInterpolation : std::uint8_t { Default, Linear, Hold, Smooth };
struct AutomationPoint {
    FramePos frame = 0;
    double value = 0;
    AutomationInterpolation interpolation = AutomationInterpolation::Default;
    double smoothStart = 0, smoothEnd = 1;
    bool operator==(const AutomationPoint&) const = default;
};
// Normalized smoothstep slice. Callers validate 0 <= start < end <= 1 off-thread.
[[nodiscard]] double smoothAutomationFraction(double t, double start = 0, double end = 1) noexcept;
struct AutomationCurve {
    static constexpr std::size_t kMaxPoints = 4096;
    bool enabled = false;
    bool hold = false;
    std::vector<AutomationPoint> points{};
    bool operator==(const AutomationCurve&) const = default;
    [[nodiscard]] bool active() const noexcept { return enabled && !points.empty(); }
    [[nodiscard]] bool hasData() const noexcept { return enabled || hold || !points.empty(); }
    [[nodiscard]] bool valid(double minimum, double maximum) const noexcept;
    [[nodiscard]] double preciseValueAt(FramePos frame, double fallback) const noexcept;
    [[nodiscard]] AutomationInterpolation interpolationAt(FramePos frame) const noexcept;
    [[nodiscard]] bool holds(const AutomationPoint& point) const noexcept {
        return point.interpolation == AutomationInterpolation::Hold
            || (point.interpolation == AutomationInterpolation::Default && hold);
    }
    [[nodiscard]] bool hasRamp(FramePos start, FramePos end) const noexcept;
    [[nodiscard]] float valueAt(FramePos frame, float fallback) const noexcept
    { return static_cast<float>(preciseValueAt(frame, fallback)); }
    [[nodiscard]] FramePos nextPoint(FramePos frame) const noexcept;
    // The frame a block-constant control reads its value at: the last `step`
    // grid line at or before `frame`, or the last point if that is later.
    [[nodiscard]] FramePos controlFrame(FramePos frame, FramePos step = 64) const noexcept;
    bool setPoint(FramePos frame, double value);
    bool erasePoint(FramePos frame);
    // Explicit all-tracks time deletion only. Other edits keep timeline automation fixed.
    bool removeTime(FramePos start, FramePos end);
    // Inclusive recorded span, with original values on either side. Atomic at
    // the point limit; constant runs are compacted without changing their audio.
    // A nonconstant span is linear even when the surrounding curve holds.
    bool writeInterval(FramePos start, FramePos end, double first, double last, double fallback);
    // Inclusive replacement with ordered points at both bounds. Retains outside
    // samples, slicing Smooth phase windows; precision/point-limit failure is atomic.
    bool replaceInterval(FramePos start, FramePos end, const std::vector<AutomationPoint>& replacement, double fallback, bool compact = false);
    // The same raw ramp heard through a block-control grid. Preserve the held
    // sample on either side, without introducing an off-grid endpoint change.
    bool writeControlInterval(FramePos start, FramePos end, double first, double last,
                              double fallback, FramePos step);
};

// Off-thread validation for track/master/insert curves and their static bases.
// Dormant curves must remain valid because they can later be enabled.
[[nodiscard]] std::string validateAutomation(const std::vector<Insert>& inserts);
[[nodiscard]] std::string validateAutomation(const Track& track);
[[nodiscard]] std::string validateAutomation(const Project& project);

// A renderer-local view of an immutable curve. Reuse the current segment across
// samples; search only when crossing a point or seeking outside that segment.
// Backward reads (another channel, overlapping clip, or loop) are supported.
// The curve must outlive the cursor and must not change while it is being read.
// No allocation, shared mutable state, or unbounded walk through skipped points.
class AutomationCursor {
public:
    explicit AutomationCursor(const AutomationCurve& curve) noexcept
        : curve_(curve.active() ? &curve : nullptr) {}

    [[nodiscard]] float valueAt(FramePos frame, float fallback) noexcept
    { return static_cast<float>(preciseValueAt(frame, fallback)); }

    [[nodiscard]] double preciseValueAt(FramePos frame, double fallback) noexcept
    {
        if (!curve_) return fallback;
        locate(frame);
        const auto& points = curve_->points;
        if (next_ == 0) return points.front().value;
        const auto& left = points[next_ - 1];
        if (next_ == points.size() || curve_->holds(left)) return left.value;
        const auto& right = points[next_];
        // Keep valueAt's arithmetic, including its division, for identical output.
        const double t = static_cast<double>(frame - left.frame)
            / static_cast<double>(right.frame - left.frame);
        return std::lerp(left.value,right.value,left.interpolation == AutomationInterpolation::Smooth
            ? smoothAutomationFraction(t,left.smoothStart,left.smoothEnd) : t);
    }

    [[nodiscard]] FramePos nextPoint(FramePos frame) noexcept
    {
        if (!curve_) return std::numeric_limits<FramePos>::max();
        locate(frame);
        return next_ == curve_->points.size()
            ? std::numeric_limits<FramePos>::max() : curve_->points[next_].frame;
    }

    [[nodiscard]] bool holdingAt(FramePos frame) noexcept
    {
        if (!curve_) return true;
        locate(frame);
        return next_ == 0 || next_ == curve_->points.size() || curve_->holds(curve_->points[next_ - 1]);
    }

private:
    void locate(FramePos frame) noexcept
    {
        const auto& points = curve_->points;
        if ((next_ != 0 && frame < points[next_ - 1].frame)
            || (next_ != points.size() && frame >= points[next_].frame)) {
            next_ = static_cast<std::size_t>(std::upper_bound(points.begin(), points.end(), frame,
                [](FramePos f, const auto& p) { return f < p.frame; }) - points.begin());
        }
    }

    const AutomationCurve* curve_;
    std::size_t next_ = 0;
};
// Table initialized before main; per-sample pan does no transcendental work.
[[nodiscard]] PanGains automationPan(float position) noexcept;
inline constexpr std::size_t kMaxProjectAutomationPoints = 65536;
} // namespace omawave::session
