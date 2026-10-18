#pragma once

#include <cstdint>
#include <string>

// Frame rates and timecode for the video reference (W11).
//
// A frame rate is a ratio, because 29.97 is 30000/1001 and rounding it makes
// a picture drift a frame every few minutes. Timecode is hh:mm:ss:ff; the
// drop-frame variant (hh:mm:ss;ff) exists only for 29.97 and 59.94, where it
// skips two (four) frame NUMBERS at the top of every minute except each tenth
// so that the numbers stay close to the wall clock. No frames are dropped; the
// count is the same either way and both are exact conversions of the same
// frame index.
namespace omawave::video {

struct FrameRate {
    std::int64_t num = 0;
    std::int64_t den = 1;

    [[nodiscard]] bool valid() const noexcept { return num > 0 && den > 0; }
    [[nodiscard]] double fps() const noexcept { return valid() ? static_cast<double>(num) / static_cast<double>(den) : 0.0; }
    // 30000/1001 and 60000/1001 are the rates drop-frame counting is defined for.
    [[nodiscard]] bool dropFrameCapable() const noexcept;
    // Whole frames per second as timecode counts them: 30 for 29.97, 24 for 23.976.
    [[nodiscard]] std::int64_t nominal() const noexcept;
    // "25", "29.97", "23.976", "30000/1001" as a person expects to read it.
    [[nodiscard]] std::string text() const;
    [[nodiscard]] std::string ratioText() const;
    [[nodiscard]] bool operator==(const FrameRate&) const noexcept = default;
};

// "30000/1001", "25", "29.97" (the decimal forms 23.976, 29.97, 59.94 map to
// their 1001 ratios). Invalid text gives an invalid rate.
[[nodiscard]] FrameRate parseFrameRate(const std::string& text);

// The frame a moment falls in, and where a frame starts. An invalid rate or
// non-finite time returns zero; finite positions outside int64 clamp to its bounds.
[[nodiscard]] std::int64_t frameIndexAt(double seconds, FrameRate rate) noexcept;
[[nodiscard]] double frameStart(std::int64_t index, FrameRate rate) noexcept;

// Timecode text for a frame index (negative indices are shown with a leading
// minus and counted back from zero). dropFrame is honoured only for a capable
// rate; otherwise the non-drop form is written.
[[nodiscard]] std::string timecode(std::int64_t frameIndex, FrameRate rate, bool dropFrame);
// The frame index of "hh:mm:ss:ff" or "hh:mm:ss;ff" (a ';' or a ',' before the
// frames means drop-frame, a ':' non-drop unless `dropFrame` says otherwise),
// or -1 for text that is not a timecode or names a dropped number.
[[nodiscard]] std::int64_t parseTimecode(const std::string& text, FrameRate rate, bool dropFrame);

} // namespace omawave::video
