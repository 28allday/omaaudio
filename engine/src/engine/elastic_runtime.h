#pragma once
#include "session/project.h"
#include <atomic>
#include <memory>
#include <vector>

namespace omawave::engine {
struct PlanTrack;
struct RenderPlan;
// One worker per plan family; eight bounded stereo pages per contiguous clip run.
// Callback requests/reads are lock-free and never run DSP or touch source storage.
class ElasticRuntime {
    struct Slot;
    struct Impl;
public:
    static constexpr std::size_t pageFrames = 2048, pages = 8, maxVoices = 64;
    struct Voice {
        std::shared_ptr<const session::AudioSource> source;
        session::ElasticSpec spec;
        session::FramePos timeline = 0, offset = 0, length = 0;
        bool operator==(const Voice&) const = default;
    };
    class Lease {
    public:
        Lease() = default;
        ~Lease();
        Lease(Lease&&) noexcept;
        Lease& operator=(Lease&&) noexcept;
        explicit operator bool() const noexcept { return slot_ != nullptr; }
        float at(unsigned channel, std::size_t frame) const noexcept;
    private:
        friend class ElasticRuntime;
        explicit Lease(Slot* slot) : slot_(slot) {}
        Slot* slot_ = nullptr;
    };
    ElasticRuntime(std::vector<Voice> voices, std::uint32_t rate);
    ~ElasticRuntime();
    bool matches(const std::vector<Voice>& voices, std::uint32_t rate) const;
    bool prepare(std::size_t voice, session::FramePos start, std::uint32_t frames) noexcept;
    Lease read(std::size_t voice, std::size_t page) noexcept;
    void clearPreparing() noexcept { preparing_.store(false,std::memory_order_relaxed); }
    bool preparing() const noexcept { return preparing_.load(std::memory_order_relaxed); }
    bool failed() const noexcept;
private:
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> preparing_{false};
};
void prepareElastic(RenderPlan& plan, const RenderPlan* previous = nullptr);
bool prepareElasticTrack(const PlanTrack& track, session::FramePos start, std::uint32_t frames) noexcept;
bool prepareElasticBlock(const RenderPlan& plan, session::FramePos start, std::uint32_t frames) noexcept;
} // namespace omawave::engine
