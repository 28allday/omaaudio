#pragma once

#include "automation.h"
#include "session/clip.h"
#include <memory>
#include <span>
#include <vector>

namespace omawave::engine {

// One shared processing region, exclusively owned by one renderer at a time.
// Split views use the same runtime. Source reading, latency compensation and
// clip envelopes are outside this kernel: input is dry, output is post-effects.
// Construction allocates and validates on the control/worker side. The chain
// is already restored/warmed and must outlive this runtime and its audio owner.
class ClipEffectRuntime {
public:
    static constexpr std::size_t maxSlots = 64, maxControls = 4096;
    static constexpr std::uint32_t maxChannels = 8;
    enum class Status {
        Ready,
        NeedsPreparation,
        InvalidRange,
        InvalidContext,
        ProgramChanged,
        LatencyChanged,
    };

    ClipEffectRuntime(std::shared_ptr<const session::ClipRack> rack, lv2::Chain& chain,
                      session::FramePos timelineOrigin);
    ClipEffectRuntime(const ClipEffectRuntime&) = delete;
    ClipEffectRuntime& operator=(const ClipEffectRuntime&) = delete;

    // Pure preflight. Failure changes no DSP, controls, cursor or output. A
    // renderer can check all voices before processing any part of a mix block.
    [[nodiscard]] Status preflight(session::FramePos local, std::uint32_t frames,
                                   const host::ProcessContext& global) const noexcept;
    // Input contains exactly channels() planar pointers; a null plane is
    // silence, including origin padding and latency flushing. No source I/O,
    // allocation, resets or ownership changes take place during processing.
    [[nodiscard]] Status process(session::FramePos local,
                                 std::span<const float* const> input,
                                 std::uint32_t frames,
                                 const host::ProcessContext& global) noexcept;
    // Explicit continuous-history loop wrap. Only an exact verified loop end
    // to matching loop start can rebase; a seek needs a newly prepared history.
    [[nodiscard]] Status continueLoop(session::FramePos local,
                                      const host::ProcessContext& global) noexcept;
    // Caller has already transferred exclusive ownership. This preserves DSP,
    // pending controls, notification state and the prepared cursor.
    void adoptProcessingThread() noexcept { chain_.adoptProcessingThread(); }

    [[nodiscard]] session::EffectOwner owner() const noexcept { return rack_->owner; }
    [[nodiscard]] const std::shared_ptr<const session::ClipRack>& rack() const noexcept { return rack_; }
    [[nodiscard]] std::uint32_t channels() const noexcept { return channels_; }
    [[nodiscard]] std::uint32_t maxBlock() const noexcept { return maxBlock_; }
    [[nodiscard]] std::uint32_t latencyFrames() const noexcept { return latency_; }
    [[nodiscard]] session::FramePos nextLocal() const noexcept { return nextLocal_; }
    [[nodiscard]] session::FramePos timelineOrigin() const noexcept { return timelineOrigin_; }
    [[nodiscard]] std::uint32_t outputFrames() const noexcept { return outputFrames_; }
    [[nodiscard]] std::span<const float> output(std::uint32_t channel) const noexcept;

private:
    [[nodiscard]] Status programStatus() const noexcept;
    [[nodiscard]] bool samePlayback(const host::ProcessContext&) const noexcept;
    std::shared_ptr<const session::ClipRack> rack_;
    lv2::Chain& chain_;
    ControlAutomations controls_;
    std::vector<float> output_;
    std::uint32_t channels_, maxBlock_, latency_, outputFrames_ = 0;
    session::FramePos timelineOrigin_, endLocal_, nextLocal_ = 0;
    host::ProcessContext previous_{};
    bool started_ = false, failed_ = false;
};

} // namespace omawave::engine
