#pragma once

#include "session/types.h"

#include <array>
#include <atomic>
#include <cmath>
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace omawave::engine {

// How many tracks carry a meter. Mirrors Transport::kMaxTracks (transport.h asserts
// they agree); declared here so this header stays free of the transport's.
inline constexpr std::size_t kMeterTracks = 256;

// Audio thread -> UI. Lock-free and deliberately lossy: the UI wants the latest
// playhead position, not every one it missed (BRIEF.md section 3).
//
// Display accessors can observe a position and level from adjacent callbacks.
// Controller events use the separate bounded timeline reader for a coherent
// position/playback snapshot. Neither path locks or retries on the audio writer.
class MeterChannel {
public:
    // --- audio thread ---
    void publish(session::FramePos position, bool playing, float peakLeft,
                 float peakRight, std::uint32_t playbackEpoch) noexcept
    {
        // Bounded single-writer publication for controller event timestamps.
        // Field releases let a reader detecting any new field also observe the
        // opening sequence change. All fields remain atomic during a raced read.
        timelineSequence_.fetch_add(1, std::memory_order_relaxed);
        position_.store(position, std::memory_order_release);
        peakLeft_.store(peakLeft, std::memory_order_relaxed);
        peakRight_.store(peakRight, std::memory_order_relaxed);
        // One atomic snapshot distinguishes a queued Play from a completed short
        // pass, even when both are observed with playing == false.
        playback_.store((std::uint64_t(playbackEpoch) << 1) | std::uint64_t(playing),
                        std::memory_order_release);
        timelineSequence_.fetch_add(1, std::memory_order_release);
    }

    // One track's post-fader, pre-master peak for the block, by plan slot. The slot
    // is the track's index in the plan the audio thread is running, which is the
    // order the main thread published it in.
    void publishTrack(std::size_t slot, float peakLeft, float peakRight) noexcept
    {
        if (slot >= kMeterTracks)
            return;
        trackLeft_[slot].store(peakLeft, std::memory_order_relaxed);
        trackRight_[slot].store(peakRight, std::memory_order_relaxed);
    }

    // Tells the main thread which plan generation the audio thread is actually using,
    // so the previous one can be freed only once it is provably unreferenced.
    void acknowledgePlan(std::uint32_t generation) noexcept
    {
        acknowledgedPlan_.store(generation, std::memory_order_release);
    }

    // How much of each audio block the callback used (1 = the whole deadline), and
    // whether PipeWire reported that the previous cycle missed its deadline. Single
    // writer: the smoothing state lives in the atomics themselves.
    void publishLoad(float load, double blockSeconds, bool dropout) noexcept
    {
        if (!(load >= 0.0f)) load = 0.0f;
        // About half a second of smoothing and a peak that halves every second.
        const float smoothing = float(std::min(1.0, blockSeconds / 0.5));
        const float decay = float(std::exp2(-blockSeconds));
        const float average = loadAverage_.load(std::memory_order_relaxed);
        loadAverage_.store(average + (load - average) * smoothing, std::memory_order_relaxed);
        loadPeak_.store(std::max(load, loadPeak_.load(std::memory_order_relaxed) * decay), std::memory_order_relaxed);
        if (load > 1.0f) overloads_.fetch_add(1, std::memory_order_relaxed);
        if (dropout) dropouts_.fetch_add(1, std::memory_order_relaxed);
        loadSamples_.fetch_add(1, std::memory_order_relaxed);
    }
    [[nodiscard]] float loadAverage() const noexcept { return loadAverage_.load(std::memory_order_relaxed); }
    [[nodiscard]] float loadPeak() const noexcept { return loadPeak_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint32_t overloads() const noexcept { return overloads_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint32_t dropouts() const noexcept { return dropouts_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t loadSamples() const noexcept { return loadSamples_.load(std::memory_order_relaxed); }

    void publishLatency(std::uint32_t frames, bool priming, bool unsupported) noexcept {
        latency_.store(frames, std::memory_order_relaxed);
        priming_.store(priming, std::memory_order_relaxed);
        unsupportedLatency_.store(unsupported, std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint32_t latencyFrames() const noexcept { return latency_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool priming() const noexcept { return priming_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool unsupportedLatency() const noexcept { return unsupportedLatency_.load(std::memory_order_relaxed); }
    // --- UI / main thread ---
    [[nodiscard]] session::FramePos position() const noexcept
    {
        return position_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool playing() const noexcept
    {
        return playbackState().playing;
    }

    struct PlaybackState { bool playing; std::uint32_t epoch; };
    struct TimelineState {
        session::FramePos position = 0;
        bool playing = false;
        std::uint32_t epoch = 0;
        bool valid = false;
    };
    // Main/control reader. Unlike the lossy display accessors, this joins the
    // position and playback state from one publication. Never spin indefinitely
    // or block the writer: a busy publication can explicitly have no timestamp.
    [[nodiscard]] TimelineState timelineState() const noexcept {
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            const auto before = timelineSequence_.load(std::memory_order_acquire);
            if (before & 1) continue;
            const auto position = position_.load(std::memory_order_acquire);
            const auto playback = playback_.load(std::memory_order_acquire);
            if (timelineSequence_.load(std::memory_order_acquire) == before)
                return {position, bool(playback & 1), std::uint32_t(playback >> 1), true};
        }
        return {};
    }
    [[nodiscard]] PlaybackState playbackState() const noexcept
    {
        const auto state = playback_.load(std::memory_order_relaxed);
        return {bool(state & 1), std::uint32_t(state >> 1)};
    }

    [[nodiscard]] float peakLeft() const noexcept
    {
        return peakLeft_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] float peakRight() const noexcept
    {
        return peakRight_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint32_t acknowledgedPlan() const noexcept
    {
        return acknowledgedPlan_.load(std::memory_order_acquire);
    }

    [[nodiscard]] float trackPeakLeft(std::size_t slot) const noexcept
    {
        return slot < kMeterTracks ? trackLeft_[slot].load(std::memory_order_relaxed)
                                   : 0.0f;
    }

    [[nodiscard]] float trackPeakRight(std::size_t slot) const noexcept
    {
        return slot < kMeterTracks ? trackRight_[slot].load(std::memory_order_relaxed)
                                   : 0.0f;
    }

private:
    std::atomic<std::uint32_t> latency_{0};
    std::atomic<bool> priming_{false}, unsupportedLatency_{false};
    std::atomic<session::FramePos> position_{0};
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    std::atomic<std::uint64_t> playback_{0};
    std::atomic<std::uint64_t> timelineSequence_{0};
    std::atomic<float> peakLeft_{0.0f};
    std::atomic<float> peakRight_{0.0f};
    std::atomic<std::uint32_t> acknowledgedPlan_{0};
    std::array<std::atomic<float>, kMeterTracks> trackLeft_{};
    std::array<std::atomic<float>, kMeterTracks> trackRight_{};
    std::atomic<float> loadAverage_{0.0f}, loadPeak_{0.0f};
    std::atomic<std::uint32_t> overloads_{0}, dropouts_{0};
    std::atomic<std::uint64_t> loadSamples_{0};
};

} // namespace omawave::engine
