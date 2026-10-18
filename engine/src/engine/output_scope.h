#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

// The newest stretch of the programme output, for live displays (spectrum and
// phase while playing). The audio thread writes with relaxed atomic stores and
// never waits; a reader copies the newest frames and may see a block being
// replaced, which a display tolerates. Lock-free and allocation-free.
namespace omawave::engine {

class OutputScope {
public:
    static constexpr std::size_t kFrames = 8192;   // a power of two
    static_assert((kFrames & (kFrames - 1)) == 0);

    // Called from the audio callback with the programme (before the monitor level).
    void write(const float* const* planes, std::uint32_t channels, std::uint32_t frames) noexcept
    {
        if (channels == 0 || planes == nullptr) return;
        const auto at = written_.load(std::memory_order_relaxed);
        for (std::uint32_t f = 0; f < frames; ++f) {
            const auto index = (at + f) & (kFrames - 1);
            const float left = planes[0] ? planes[0][f] : 0.0f;
            const float right = channels > 1 && planes[1] ? planes[1][f] : left;
            left_[index].store(left, std::memory_order_relaxed);
            right_[index].store(right, std::memory_order_relaxed);
        }
        written_.store(at + frames, std::memory_order_release);
    }

    // Copies the newest `frames` (at most kFrames) into left/right, oldest first.
    // Returns how many frames have ever been written, so a reader can tell
    // whether anything new arrived since it last looked.
    std::uint64_t read(float* left, float* right, std::size_t frames) const noexcept
    {
        frames = frames > kFrames ? kFrames : frames;
        const auto written = written_.load(std::memory_order_acquire);
        const auto start = written - (written < frames ? written : frames);
        for (std::size_t i = 0; i < frames; ++i) {
            if (start + i >= written) { left[i] = right[i] = 0.0f; continue; }
            const auto index = (start + i) & (kFrames - 1);
            left[i] = left_[index].load(std::memory_order_relaxed);
            right[i] = right_[index].load(std::memory_order_relaxed);
        }
        return written;
    }

    [[nodiscard]] std::uint64_t written() const noexcept { return written_.load(std::memory_order_acquire); }

private:
    std::array<std::atomic<float>, kFrames> left_{};
    std::array<std::atomic<float>, kFrames> right_{};
    std::atomic<std::uint64_t> written_{0};
};

} // namespace omawave::engine
