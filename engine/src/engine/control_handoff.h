#pragma once

#include <atomic>
#include <utility>

namespace omawave::engine {

// Exclusive ownership between one audio callback and a control owner. The
// callback makes one bounded attempt; it never waits. A pending request prevents
// the next callback from entering, and becomes ready when the current one exits.
// Acquire/release also transfers non-atomic processor and queue-consumer state.
class ControlHandoff {
    static constexpr unsigned processing = 1, requested = 2;
    std::atomic<unsigned> state_{0};
    static_assert(std::atomic<unsigned>::is_always_lock_free);

public:
    class Processing {
        friend class ControlHandoff;
        ControlHandoff* owner_ = nullptr;
        explicit Processing(ControlHandoff* owner) noexcept : owner_(owner) {}
    public:
        Processing(const Processing&) = delete;
        Processing& operator=(const Processing&) = delete;
        Processing(Processing&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)) {}
        ~Processing() { if (owner_) owner_->state_.fetch_and(~processing, std::memory_order_release); }
        explicit operator bool() const noexcept { return owner_ != nullptr; }
    };

    class Request {
        friend class ControlHandoff;
        ControlHandoff* owner_ = nullptr;
        explicit Request(ControlHandoff* owner) noexcept : owner_(owner) {}
    public:
        Request(const Request&) = delete;
        Request& operator=(const Request&) = delete;
        Request(Request&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)) {}
        ~Request() { if (owner_) owner_->state_.fetch_and(~requested, std::memory_order_release); }
        // Only this request may inspect or change the processor once ready.
        // Destruction cancels a pending request or releases acquired ownership.
        [[nodiscard]] bool ready() const noexcept {
            return owner_ && owner_->state_.load(std::memory_order_acquire) == requested;
        }
        explicit operator bool() const noexcept { return owner_ != nullptr; }
    };

    [[nodiscard]] Processing enter() noexcept {
        unsigned expected = 0;
        return Processing(state_.compare_exchange_strong(expected, processing,
            std::memory_order_acquire, std::memory_order_relaxed) ? this : nullptr);
    }
    [[nodiscard]] Request request() noexcept {
        const auto previous = state_.fetch_or(requested, std::memory_order_acq_rel);
        return Request((previous & requested) ? nullptr : this);
    }
};

} // namespace omawave::engine
