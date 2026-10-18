#pragma once

#include "session/disk_audio.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <unordered_map>

namespace omawave::engine {

// One live transport, one GUI producer, one I/O worker. The audio budget is fixed
// for the life of a cache (32 MiB unless a larger one is configured); source
// descriptors may grow with the session. Callback methods never perform
// I/O, allocations, locks, waits or thread wakeups. Offline plans need no cache.
//
// Pages live in sets of `ways` slots; hashing (source, page) names two sets a page
// may use, so a lookup costs the same however large the cache is. One set is the
// original fully associative cache. Page memory is reserved up front and becomes resident only
// as the worker fills it: a stereo page costs a quarter of an eight-channel one.
class StreamCache {
    struct Slot;
public:
    static constexpr std::size_t kPageFrames = 8192;
    static constexpr std::size_t kSlots = 128;
    static constexpr std::size_t kChannels = 8;
    static constexpr std::size_t kAudioBytes = kPageFrames * kSlots * kChannels * sizeof(float);
    static constexpr std::size_t kRequestCapacity = 256;
    static constexpr std::size_t kSlotBytes = kPageFrames * kChannels * sizeof(float);
    struct Config {
        std::size_t count = kSlots;  // slots; a multiple of ways
        std::size_t ways = kSlots;   // slots searched per lookup
        unsigned readAhead = 2;      // pages loaded beyond the one requested
        // The largest set-associative cache within `bytes`, never below the default.
        [[nodiscard]] static Config forBytes(std::size_t bytes) noexcept;
        [[nodiscard]] std::size_t bytes() const noexcept { return count * kSlotBytes; }
        [[nodiscard]] bool operator==(const Config&) const noexcept = default;
    };

    class Lease {
    public:
        Lease() = default;
        ~Lease();
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        [[nodiscard]] explicit operator bool() const noexcept { return slot_ != nullptr; }
        [[nodiscard]] float at(std::uint32_t channel, std::size_t localFrame) const noexcept;
        [[nodiscard]] std::size_t frames() const noexcept;
    private:
        friend class StreamCache;
        explicit Lease(Slot* slot) : slot_(slot) {}
        Slot* slot_ = nullptr;
    };

    StreamCache();
    explicit StreamCache(Config config);
    ~StreamCache();
    [[nodiscard]] const Config& config() const noexcept { return config_; }
    [[nodiscard]] unsigned readAhead() const noexcept { return config_.readAhead; }
    // The size that was asked for. It differs from config() only for a cache
    // built at the default because the larger reservation was refused, so that
    // the same request finds this cache again instead of failing again.
    [[nodiscard]] const Config& requested() const noexcept { return requested_; }
    void standInFor(const Config& wanted) noexcept { requested_ = wanted; }
    StreamCache(const StreamCache&) = delete;
    StreamCache& operator=(const StreamCache&) = delete;
    // GUI thread only, before a plan using the source is published. Ownership
    // protects queued raw pointers. New sessions/pruning replace the cache.
    void retain(std::shared_ptr<const session::DiskAudio> source);
    void prefetch(const session::DiskAudio* source, std::size_t frame, bool readAhead = true) noexcept;
    // Single audio consumer. A miss requests the page and returns an empty lease.
    [[nodiscard]] Lease read(const session::DiskAudio* source, std::size_t page) noexcept;
    void prefetchRealtime(const session::DiskAudio* source, std::size_t frame, bool readAhead = true) noexcept;
    [[nodiscard]] std::uint64_t misses() const noexcept { return misses_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t readErrors() const noexcept { return readErrors_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t droppedRequests() const noexcept { return dropped_.load(std::memory_order_relaxed); }

private:
    struct Slot {
        std::atomic<int> pins{0}; // -1 means worker owns the slot; nonnegative reader count
        std::atomic<const session::DiskAudio*> source{nullptr};
        std::atomic<std::size_t> page{0};
        std::atomic<std::uint64_t> used{0};
        std::size_t frames = 0;
        std::uint32_t channels = 0;
        // Audio reader only while pinned; the worker resets it when replacing
        // the page under its exclusive pin.
        bool aheadRequested = false;
        float* audio = nullptr; // kSlotBytes inside audio_, written by the worker only
    };
    struct Request { const session::DiskAudio* source = nullptr; std::size_t page = 0; unsigned ahead = 0; };
    // Separate SPSC queues avoid making the GUI and callback compete as producers.
    struct Queue {
        std::array<Request, kRequestCapacity> entries{};
        alignas(64) std::atomic<std::size_t> write{0};
        alignas(64) std::atomic<std::size_t> read{0};
        bool push(Request request) noexcept;
        bool pop(Request& request) noexcept;
    };
    bool request(Queue& queue, const session::DiskAudio* source, std::size_t page, unsigned ahead) noexcept;
    using Sets = std::array<std::size_t, 2>; // first slot of each candidate set
    [[nodiscard]] Sets setsOf(const session::DiskAudio* source, std::size_t page) const noexcept;
    void load(Request request);
    void work();
    Config config_, requested_;
    std::size_t sets_ = 1;
    float* audio_ = nullptr; // one anonymous mapping, resident only where filled
    std::unique_ptr<Slot[]> slots_;
    std::unordered_map<const session::DiskAudio*, std::shared_ptr<const session::DiskAudio>> sources_;
    Queue guiQueue_, audioQueue_;
    std::atomic<bool> stopping_{false};
    std::atomic<std::uint64_t> clock_{1}, misses_{0}, readErrors_{0}, dropped_{0};
    std::thread worker_;
};

} // namespace omawave::engine
