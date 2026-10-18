#pragma once
#include "session/types.h"
#include "lv2/chain.h"
#include <array>
#include <cstdint>
#include <vector>

namespace omawave::engine {
struct RenderPlan;
struct RoutingLevel {
    float gain = 1, monoL = 1, monoR = 1, stereoL = 1, stereoR = 1;
};
struct RoutingTime {
    session::FramePos origin = 0, loopStart = 0, loopEnd = 0;
    bool looping = false;
    std::uint32_t playbackEpoch = 0;
    bool recording = false;
    [[nodiscard]] session::FramePos at(session::FramePos raw) const noexcept {
        if (looping && loopEnd > loopStart && raw >= loopEnd)
            return loopStart + (raw - loopStart) % (loopEnd - loopStart);
        return raw;
    }
};

// Constructed off the audio thread. Mutable state belongs to ONE renderer and
// survives curve-only plan updates; immutable clips/routes are passed each call.
class RoutingRender {
public:
    static constexpr std::size_t maxTracks = 256;   // Transport::kMaxTracks
    // Memory for delay compensation, shared by every track and channel: 128 MiB keeps
    // about 0.68 s of plug-in delay at 256 stereo tracks (48 kHz), more with fewer.
    static constexpr std::size_t historyBytes = std::size_t(128) * 1024 * 1024;
    static constexpr std::uint32_t maxChannels = 8, maxLatency = 262144;
    RoutingRender(std::size_t tracks, std::uint32_t channels);
    // Callback-safe latency graph refresh. False means the fixed capacity cannot
    // represent this graph. Changed latency requires reset/priming by the caller.
    bool prepare(const RenderPlan& plan, bool masterBypass, bool& changed) noexcept;
    bool prepareElastic(const RenderPlan& plan, session::FramePos raw,
        std::uint32_t frames, const RoutingTime& time) const noexcept;
    void reset() noexcept { written_ = 0; initialized_ = false; }
    [[nodiscard]] std::uint32_t latency() const noexcept { return total_; }
    // Caller has prepared this graph. A lane is delayed from the graph's raw
    // clock by its upstream path (effect) or its completed path (fader).
    [[nodiscard]] std::uint32_t automationDelay(std::size_t track, bool effect) const noexcept {
        return track < tracks_ ? (effect ? upstream_[track] : path_[track]) : longest_;
    }
    [[nodiscard]] std::uint32_t supportedLatency() const noexcept { return capacity_ - lv2::kMaxBlock; }
    [[nodiscard]] bool fits(std::size_t tracks, std::uint32_t channels) const noexcept { return tracks == tracks_ && channels == channels_; }
    [[nodiscard]] std::uint32_t block(const RenderPlan& plan, session::FramePos raw,
        std::uint32_t maximum, const RoutingTime& time, bool masterBypass) const noexcept;
    void process(const RenderPlan& plan, session::FramePos raw, std::uint32_t frames,
        float* const* output, const RoutingLevel* start, const RoutingLevel* end,
        const RoutingTime& time, float masterTrim, bool masterBypass, float masterMakeup,
        float* peakLeft, float* peakRight) noexcept;
private:
    [[nodiscard]] float history(std::size_t track, bool pre, std::uint32_t channel, std::int64_t frame) const noexcept;
    void remember(std::size_t track, bool pre, std::uint32_t channel, std::int64_t frame, float value) noexcept;
    std::size_t tracks_;
    std::uint32_t channels_, capacity_, longest_ = 0, master_ = 0, total_ = 0;
    std::array<std::uint32_t, maxTracks> upstream_{}, path_{}, delays_{};
    std::vector<float> history_;
    std::int64_t written_ = 0;
    static constexpr std::size_t maxEdges = maxTracks * (session::kMaxTrackSends + 2);
    std::array<float,maxTracks> muteGain_{}, ownGain_{};
    std::array<float,maxEdges> edgeGain_{};
    bool initialized_ = false;
};
} // namespace omawave::engine
