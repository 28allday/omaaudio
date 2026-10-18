#pragma once

#include "types.h"

#include <cstdint>
#include <limits>
#include <numeric>
#include <utility>

namespace omawave::session {

// Constant session tempo: used for display and hosted effect synchronization.
// It does not stretch clips or change their sample positions.
// BPM always counts quarter notes; the denominator says which note is a beat.
struct MusicalTime {
    bool enabled = false;
    std::int32_t tempoMilli = 120000;
    std::int32_t beats = 4;
    std::int32_t unit = 4;

    [[nodiscard]] bool valid() const noexcept
    {
        return tempoMilli >= 1000 && tempoMilli <= 480000 && beats >= 1 && beats <= 32
            && unit >= 1 && unit <= 32 && (unit & (unit - 1)) == 0;
    }
    [[nodiscard]] std::int32_t ticksPerBeat() const noexcept { return valid() ? 3840 / unit : 0; }
    [[nodiscard]] bool operator==(const MusicalTime&) const noexcept = default;

    // Tick containing a sample; the first sample on or after a tick's time.
    // -1 for invalid settings, negative input or a result outside FramePos.
    [[nodiscard]] FramePos tickAt(FramePos sample, std::uint32_t rate) const noexcept
    {
        return convert(sample, rate, false);
    }
    [[nodiscard]] FramePos sampleAt(FramePos tick, std::uint32_t rate) const noexcept
    {
        return convert(tick, rate, true);
    }

private:
    [[nodiscard]] FramePos convert(FramePos value, std::uint32_t rate, bool toSamples) const noexcept
    {
        if (!valid() || !rate || value < 0) return -1;
        // 960 ticks/quarter divided by 60 seconds and 1000 milli-BPM.
        std::uint64_t numerator = std::uint64_t(tempoMilli) * 16;
        std::uint64_t denominator = std::uint64_t(rate) * 1000;
        const auto common = std::gcd(numerator, denominator);
        numerator /= common; denominator /= common;
        if (toSamples) std::swap(numerator, denominator);
        const auto count = std::uint64_t(value);
        // Even at uint32's largest rate, the reduced remainder product fits
        // uint64 (the ratio always has a common factor of at least eight).
        const auto remainder = (count % denominator) * numerator;
        const auto tail = remainder / denominator + (toSamples && remainder % denominator != 0 ? 1 : 0);
        constexpr auto maximum = std::uint64_t(std::numeric_limits<FramePos>::max());
        if (tail > maximum || count / denominator > (maximum - tail) / numerator) return -1;
        return FramePos(count / denominator * numerator + tail);
    }
};

} // namespace omawave::session
