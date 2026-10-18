#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

namespace omawave::engine {

// Two planar channels. One producer and one consumer; never reset while either
// is running. Mono callers supply the same plane twice (the writer uses one).
template<std::size_t Capacity>
class AudioRing {
    static_assert(Capacity > 1 && (Capacity & (Capacity - 1)) == 0);
public:
    bool write(const float* left, const float* right, std::size_t frames) noexcept
    {
        const auto w = written_.load(std::memory_order_relaxed);
        if (frames > Capacity - (w - read_.load(std::memory_order_acquire)))
            return false;
        for (std::size_t i = 0; i < frames; ++i) {
            audio_[0][(w + i) & (Capacity - 1)] = left ? left[i] : 0;
            audio_[1][(w + i) & (Capacity - 1)] = right ? right[i] : 0;
        }
        written_.store(w + frames, std::memory_order_release);
        return true;
    }
    std::size_t read(float* left, float* right, std::size_t frames) noexcept
    {
        const auto r = read_.load(std::memory_order_relaxed);
        const auto count = std::min<std::size_t>(frames, written_.load(std::memory_order_acquire) - r);
        for (std::size_t i = 0; i < count; ++i) {
            if (left) left[i] = audio_[0][(r + i) & (Capacity - 1)];
            if (right) right[i] = audio_[1][(r + i) & (Capacity - 1)];
        }
        read_.store(r + count, std::memory_order_release);
        return count;
    }
    void mix(float* const* planes, std::uint32_t channels, std::size_t frames, float gain) noexcept
    {
        const auto r = read_.load(std::memory_order_relaxed);
        const auto count = std::min<std::size_t>(frames, written_.load(std::memory_order_acquire) - r);
        for (std::uint32_t c = 0; c < std::min(channels, 2U); ++c)
            if (planes[c])
                for (std::size_t i = 0; i < count; ++i)
                    planes[c][i] += audio_[c][(r + i) & (Capacity - 1)] * gain;
        read_.store(r + count, std::memory_order_release);
    }
    [[nodiscard]] bool empty() const noexcept
    {
        return read_.load(std::memory_order_acquire) == written_.load(std::memory_order_acquire);
    }
private:
    std::array<std::array<float, Capacity>, 2> audio_{};
    alignas(64) std::atomic<std::uint64_t> written_{0};
    alignas(64) std::atomic<std::uint64_t> read_{0};
};
} // namespace omawave::engine
