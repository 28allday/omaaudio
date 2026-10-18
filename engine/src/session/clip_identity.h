#pragma once

#include <atomic>
#include <compare>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace omawave::session {

// Control/worker-side identities never rewind with a Project or Undo snapshot.
// Exhaustion refuses construction instead of reusing a still-observable token.
inline std::uint64_t freshClipToken()
{
    static std::atomic<std::uint64_t> next{1};
    auto value = next.load(std::memory_order_relaxed);
    for (;;) {
        if (value == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("clip identity space exhausted");
        if (next.compare_exchange_weak(value, value + 1,
                                      std::memory_order_relaxed, std::memory_order_relaxed))
            return value;
    }
}

// Identity of an editing view, independent of its shared processing region.
// Ordinary copies preserve it; installing an additional live view must fork it.
class ClipIdentity {
public:
    [[nodiscard]] std::uint64_t value() const noexcept { return value_; }
    [[nodiscard]] bool operator==(const ClipIdentity&) const noexcept = default;
    [[nodiscard]] auto operator<=>(const ClipIdentity&) const noexcept = default;
private:
    std::uint64_t value_ = freshClipToken();
};

} // namespace omawave::session
