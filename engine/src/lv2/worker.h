#pragma once

#include <lv2/worker/worker.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <semaphore>
#include <thread>

namespace omawave::lv2 {

// What the worker moves work for. Instance implements this; the indirection exists so
// the rings and the thread can be tested without instantiating a real plugin.
class WorkerClient {
public:
    // Worker thread. Calls the plugin's work(), which may allocate, block and take as
    // long as it likes -- that is the whole point of the extension.
    virtual void performWork(std::uint32_t size, const void* data) noexcept = 0;

    // The run() context. Hands a response produced by work() to the plugin's
    // work_response().
    virtual void deliverResponse(std::uint32_t size, const void* data) noexcept = 0;

protected:
    ~WorkerClient() = default;
};

// The worker:schedule extension: one thread and two rings.
//
// A plugin calls schedule_work() from inside run() to get something slow -- loading an
// impulse response, resizing a buffer, reconfiguring a filter bank -- done off the
// audio thread. The host queues the request (RT to worker ring), runs the plugin's
// work() on the worker thread, queues whatever work() responds with (worker to RT
// ring), and delivers those responses back on the audio thread at the top of the next
// cycle. 194 of the LSP plugins installed here take the feature when it is offered,
// and the x42 convolvers refuse to instantiate without it.
//
// One Worker is shared by a chain -- one thread per chain that needs it, not one per
// plugin -- and records carry the client they belong to, so a shared ring still
// delivers each response to the instance that asked.
//
// THREADING. Both rings are single-producer single-consumer, and the design leans on
// who the producers are:
//
//   * requests: written by whoever is running the instances, read by the worker
//     thread. Before a chain is published that writer is the main thread (warmUp);
//     after, it is the audio thread. Never both at once -- publication is ordered by
//     the same generation handshake that publishes the chain itself.
//   * responses: written by the worker thread, read by whoever runs the instances,
//     with the same handoff.
//
// schedule() and deliverResponses() allocate nothing and lock nothing; the semaphore
// release is a wake, not a wait, which is the same class of call every RT host makes
// here. The worker thread is the only place work() runs, so a plugin's work() is
// never re-entered.
class Worker {
public:
    // Per-request payload cap. Scheduled messages in the wild are small -- a pointer,
    // a URID and a value -- and a request that does not fit is refused with
    // ERR_NO_SPACE, which the spec obliges the plugin to cope with.
    static constexpr std::uint32_t kMaxPayload = 1024;

    Worker();
    ~Worker();  // Joins the thread. Undelivered work and responses are dropped, which
                // is only reachable through retirement -- nothing will run these
                // instances again.

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    Worker(Worker&&) = delete;
    Worker& operator=(Worker&&) = delete;

    // The run() context (through the schedule feature). Queues `data` for the worker
    // thread and wakes it.
    [[nodiscard]] LV2_Worker_Status schedule(WorkerClient* client, std::uint32_t size,
                                             const void* data) noexcept;

    // Worker thread (through the respond callback handed to work()). Queues a
    // response for the run() side.
    [[nodiscard]] LV2_Worker_Status respond(WorkerClient* client, std::uint32_t size,
                                            const void* data) noexcept;

    // The run() context. Delivers every queued response to the client it belongs to.
    void deliverResponses() noexcept;

private:
    struct Record {
        WorkerClient* client = nullptr;
        std::uint32_t size = 0;
        std::array<std::uint8_t, kMaxPayload> data;
    };

    // The same single-producer single-consumer shape as engine's CommandQueue, but
    // record-oriented: push and pop hand out a slot to fill or read in place, so a
    // payload is copied exactly once on each side. Not shared with engine/ because
    // lv2/ does not include engine/ -- engine/ already includes lv2/.
    template <std::size_t Capacity>
    class Ring {
        static_assert((Capacity & (Capacity - 1)) == 0 && Capacity > 1,
                      "capacity must be a power of two");

    public:
        // Producer. The slot to fill, or null when full. Not committed until
        // commitPush(); a producer that walks away leaves the ring unchanged.
        [[nodiscard]] Record* beginPush() noexcept
        {
            const std::size_t w = write_.load(std::memory_order_relaxed);
            if (((w + 1) & kMask) == read_.load(std::memory_order_acquire))
                return nullptr;
            return &buffer_[w];
        }

        void commitPush() noexcept
        {
            const std::size_t w = write_.load(std::memory_order_relaxed);
            write_.store((w + 1) & kMask, std::memory_order_release);
        }

        // Consumer. The oldest record, or null when empty. Stays valid until pop().
        [[nodiscard]] Record* peek() noexcept
        {
            const std::size_t r = read_.load(std::memory_order_relaxed);
            if (r == write_.load(std::memory_order_acquire))
                return nullptr;
            return &buffer_[r];
        }

        void pop() noexcept
        {
            const std::size_t r = read_.load(std::memory_order_relaxed);
            read_.store((r + 1) & kMask, std::memory_order_release);
        }

    private:
        static constexpr std::size_t kMask = Capacity - 1;
        std::array<Record, Capacity> buffer_{};
        alignas(64) std::atomic<std::size_t> write_{0};
        alignas(64) std::atomic<std::size_t> read_{0};
    };

    void loop();

    static constexpr std::size_t kDepth = 32;
    Ring<kDepth> requests_;
    Ring<kDepth> responses_;

    std::counting_semaphore<> wake_{0};
    std::atomic<bool> running_{true};
    std::thread thread_;
};

} // namespace omawave::lv2
