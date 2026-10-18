#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace omawave::engine {

// Single-producer single-consumer ring buffer.
//
// BRIEF.md section 3: "Communication UI -> audio is by command queue only." The
// producer is the engine main thread draining IPC; the consumer is the audio thread.
// Neither side allocates or blocks (section 2).
//
// Capacity must be a power of two so the wrap is a mask rather than a modulo.
template <typename T, std::size_t Capacity>
class CommandQueue {
    static_assert(std::is_trivially_copyable_v<T>,
                  "commands cross to the audio thread, so they must be trivially "
                  "copyable -- anything needing an allocation is prepared on a worker "
                  "and handed over as a pointer");
    static_assert((Capacity & (Capacity - 1)) == 0 && Capacity > 1,
                  "capacity must be a power of two");

public:
    // Producer side. Returns false when full rather than blocking or growing.
    [[nodiscard]] bool push(const T& item) noexcept
    {
        const std::size_t w = write_.load(std::memory_order_relaxed);
        const std::size_t next = (w + 1) & kMask;
        if (next == read_.load(std::memory_order_acquire))
            return false;                       // full

        buffer_[w] = item;
        write_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side, called from the audio thread.
    [[nodiscard]] bool pop(T& out) noexcept
    {
        const std::size_t r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire))
            return false;                       // empty

        out = buffer_[r];
        read_.store((r + 1) & kMask, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return read_.load(std::memory_order_acquire)
            == write_.load(std::memory_order_acquire);
    }

    // Producer-side preflight. The consumer can only make more room afterward.
    [[nodiscard]] bool full() const noexcept
    {
        return ((write_.load(std::memory_order_relaxed) + 1) & kMask)
            == read_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool hasRoom(std::size_t count) const noexcept
    {
        const auto used = (write_.load(std::memory_order_relaxed) - read_.load(std::memory_order_acquire)) & kMask;
        return count <= kMask - used;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity - 1; }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    std::array<T, Capacity> buffer_{};

    // Separated so the two sides do not contend on the same cache line.
    alignas(64) std::atomic<std::size_t> write_{0};
    alignas(64) std::atomic<std::size_t> read_{0};
};

} // namespace omawave::engine
