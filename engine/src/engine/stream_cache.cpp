#include "stream_cache.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <limits>
#include <new>
#include <utility>
#include <sys/mman.h>

namespace omawave::engine {
static_assert(std::atomic<int>::is_always_lock_free);
static_assert(std::atomic<std::size_t>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<const session::DiskAudio*>::is_always_lock_free);

StreamCache::Lease::~Lease()
{
    if (slot_) slot_->pins.fetch_sub(1, std::memory_order_release);
}
StreamCache::Lease::Lease(Lease&& other) noexcept : slot_(std::exchange(other.slot_, nullptr)) {}
StreamCache::Lease& StreamCache::Lease::operator=(Lease&& other) noexcept
{
    if (this != &other) {
        if (slot_) slot_->pins.fetch_sub(1, std::memory_order_release);
        slot_ = std::exchange(other.slot_, nullptr);
    }
    return *this;
}
float StreamCache::Lease::at(std::uint32_t channel, std::size_t frame) const noexcept
{
    if (!slot_ || channel >= slot_->channels || frame >= slot_->frames) return 0;
    return slot_->audio[std::size_t(channel) * kPageFrames + frame];
}
std::size_t StreamCache::Lease::frames() const noexcept { return slot_ ? slot_->frames : 0; }

bool StreamCache::Queue::push(Request request) noexcept
{
    const auto at = write.load(std::memory_order_relaxed);
    if (at - read.load(std::memory_order_acquire) >= kRequestCapacity) return false;
    entries[at % kRequestCapacity] = request;
    write.store(at + 1, std::memory_order_release); return true;
}
bool StreamCache::Queue::pop(Request& request) noexcept
{
    const auto at = read.load(std::memory_order_relaxed);
    if (at == write.load(std::memory_order_acquire)) return false;
    request = entries[at % kRequestCapacity];
    read.store(at + 1, std::memory_order_release); return true;
}

StreamCache::Config StreamCache::Config::forBytes(std::size_t bytes) noexcept
{
    // Eight ways need enough sets that a busy session's live pages rarely meet
    // in one; below that the fully associative default serves better.
    constexpr std::size_t kWays = 8, kMinimum = 1024, kMaximum = 65536;
    const auto wanted = bytes / kSlotBytes;
    if (wanted < kMinimum) return {};
    Config out;
    out.count = std::min(wanted, kMaximum) / kWays * kWays;
    out.ways = kWays;
    out.readAhead = unsigned(std::clamp<std::size_t>(out.count / 512, 2, 16));
    return out;
}

StreamCache::StreamCache() : StreamCache(Config{}) {}
StreamCache::StreamCache(Config config) : config_(config)
{
    if (!config_.count || !config_.ways || config_.count % config_.ways || !config_.readAhead)
        config_ = {};
    requested_ = config_;
    sets_ = config_.count / config_.ways;
    // Untouched pages cost nothing, so the reservation can be the whole budget.
    void* memory = ::mmap(nullptr, config_.bytes(), PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (memory == MAP_FAILED) throw std::bad_alloc();
    // Huge pages would make whole neighbouring slots resident on first touch.
    (void)::madvise(memory, config_.bytes(), MADV_NOHUGEPAGE);
    audio_ = static_cast<float*>(memory);
    try {
        slots_ = std::make_unique<Slot[]>(config_.count);
        for (std::size_t i = 0; i < config_.count; ++i)
            slots_[i].audio = audio_ + i * (kPageFrames * kChannels);
        worker_ = std::thread([this] { work(); });
    } catch (...) { ::munmap(audio_, config_.bytes()); throw; }
}
StreamCache::~StreamCache()
{
    stopping_.store(true, std::memory_order_release);
    if (worker_.joinable()) worker_.join();
    ::munmap(audio_, config_.bytes());
}
StreamCache::Sets StreamCache::setsOf(const session::DiskAudio* source, std::size_t page) const noexcept
{
    if (sets_ == 1) return {0, 0};
    auto hash = (std::uint64_t(reinterpret_cast<std::uintptr_t>(source)) >> 4) ^ (std::uint64_t(page) * 0x9E3779B97F4A7C15ull);
    hash ^= hash >> 29; hash *= 0xBF58476D1CE4E5B9ull; hash ^= hash >> 32;
    // A page may live in either of two sets, so one crowded set does not evict
    // what the other still has room for.
    const auto first = std::size_t(hash % sets_);
    const auto second = (first + 1 + std::size_t((hash >> 32) % (sets_ - 1))) % sets_;
    return {first * config_.ways, second * config_.ways};
}

void StreamCache::retain(std::shared_ptr<const session::DiskAudio> source)
{
    if (source) sources_.emplace(source.get(), std::move(source));
}
bool StreamCache::request(Queue& queue, const session::DiskAudio* source, std::size_t page, unsigned ahead) noexcept
{
    if (!source || page > (source->frames() - 1) / kPageFrames) return false;
    if (queue.push({source, page, ahead})) return true;
    dropped_.fetch_add(1, std::memory_order_relaxed);
    return false;
}
void StreamCache::prefetch(const session::DiskAudio* source, std::size_t frame, bool readAhead) noexcept
{
    if (source && frame < source->frames()) request(guiQueue_, source, frame / kPageFrames, readAhead ? config_.readAhead : 0);
}
void StreamCache::prefetchRealtime(const session::DiskAudio* source, std::size_t frame, bool readAhead) noexcept
{
    if (source && frame < source->frames()) request(audioQueue_, source, frame / kPageFrames, readAhead ? config_.readAhead : 0);
}

StreamCache::Lease StreamCache::read(const session::DiskAudio* source, std::size_t page) noexcept
{
    if (!source || page > (source->frames() - 1) / kPageFrames) return {};
    const auto sets = setsOf(source, page);
    for (std::size_t n = 0, scanned = sets_ == 1 ? config_.ways : 2 * config_.ways; n < scanned; ++n) {
        auto& slot = slots_[n < config_.ways ? sets[0] + n : sets[1] + n - config_.ways];
        if (slot.source.load(std::memory_order_relaxed) != source || slot.page.load(std::memory_order_relaxed) != page) continue;
        int pins = slot.pins.load(std::memory_order_acquire);
        if (pins < 0 || !slot.pins.compare_exchange_strong(pins, pins + 1, std::memory_order_acquire)) continue;
        if (slot.source.load(std::memory_order_relaxed) == source && slot.page.load(std::memory_order_relaxed) == page) {
            slot.used.store(clock_.fetch_add(1, std::memory_order_relaxed), std::memory_order_relaxed);
            // Keep the record with the pinned page: source-address collisions
            // and interleaved clips from one take must not flood the queue.
            // A full queue leaves the next hit able to retry. Explicit seek,
            // loop and callback prefetches still refresh residency independently.
            if (!slot.aheadRequested)
                slot.aheadRequested = page == (source->frames() - 1) / kPageFrames
                    || request(audioQueue_, source, page + 1, config_.readAhead - 1);
            return Lease(&slot);
        }
        slot.pins.fetch_sub(1, std::memory_order_release);
    }
    misses_.fetch_add(1, std::memory_order_relaxed);
    request(audioQueue_, source, page, config_.readAhead);
    return {};
}

void StreamCache::load(Request request)
{
    if (!request.source || request.page > (request.source->frames() - 1) / kPageFrames) return;
    const auto sets = setsOf(request.source, request.page);
    const auto scanned = sets_ == 1 ? config_.ways : 2 * config_.ways;
    const auto slotAt = [&](std::size_t n) { return n < config_.ways ? sets[0] + n : sets[1] + n - config_.ways; };
    for (std::size_t n = 0; n < scanned; ++n) {
        auto& existing = slots_[slotAt(n)];
        if (existing.source.load(std::memory_order_relaxed) == request.source
            && existing.page.load(std::memory_order_relaxed) == request.page) {
            existing.used.store(clock_.fetch_add(1, std::memory_order_relaxed), std::memory_order_relaxed);
            return;
        }
    }
    std::size_t chosen = config_.count;
    auto oldest = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t n = 0; n < scanned; ++n) {
        const auto i = slotAt(n);
        if (slots_[i].pins.load(std::memory_order_acquire) != 0) continue;
        const auto age = slots_[i].used.load(std::memory_order_relaxed);
        if (age < oldest) { oldest = age; chosen = i; }
    }
    if (chosen == config_.count) return;
    auto& slot = slots_[chosen]; int expected = 0;
    if (!slot.pins.compare_exchange_strong(expected, -1, std::memory_order_acquire)) return;
    slot.source.store(nullptr, std::memory_order_relaxed);
    const auto at = request.page * kPageFrames;
    const auto frames = std::min(kPageFrames, request.source->frames() - at);
    const auto channels = std::min<std::uint32_t>(kChannels, request.source->channels());
    bool ok = true;
    for (std::uint32_t c = 0; c < channels; ++c) {
        const std::span<float> out(slot.audio + std::size_t(c) * kPageFrames, frames);
        if (!request.source->readPlane(c, at, out)) { ok = false; break; }
    }
    if (ok) {
        slot.frames = frames; slot.channels = channels;
        slot.aheadRequested = false;
        slot.page.store(request.page, std::memory_order_relaxed);
        slot.source.store(request.source, std::memory_order_relaxed);
        slot.used.store(clock_.fetch_add(1, std::memory_order_relaxed), std::memory_order_relaxed);
    } else readErrors_.fetch_add(1, std::memory_order_relaxed);
    slot.pins.store(0, std::memory_order_release);
}

void StreamCache::work()
{
    while (!stopping_.load(std::memory_order_acquire)) {
        bool didWork = false;
        // Bound each turn so shutdown and the other producer cannot starve.
        for (std::size_t i = 0; i < kRequestCapacity; ++i) {
            Request request; bool got = audioQueue_.pop(request);
            if (!got) got = guiQueue_.pop(request);
            if (!got || stopping_.load(std::memory_order_acquire)) break;
            didWork = true;
            load(request);
            for (unsigned ahead = 1; ahead <= request.ahead; ++ahead)
                load({request.source, request.page + ahead});
        }
        if (!didWork) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

} // namespace omawave::engine
