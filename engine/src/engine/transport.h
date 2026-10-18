#pragma once

#include "commands.h"
#include "command_queue.h"
#include "control_handoff.h"
#include "loudness.h"
#include "meter_channel.h"
#include "output_scope.h"
#include "render_plan.h"
#include "recording.h"
#include "session/types.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

namespace omawave::engine {

// The realtime renderer. Audio methods obey BRIEF.md section 2: no allocation,
// no locks, no I/O, no logging. ControlAccess transfers exclusive ownership to
// the control thread between callbacks for state capture.
//
// It holds no ownership. The plan is borrowed; the engine keeps it alive until the
// acknowledged generation says the audio thread has moved on.
class Transport {
public:
    static constexpr std::size_t kQueueCapacity = 256;
    using Queue = CommandQueue<Command, kQueueCapacity>;

    // Fixed, because per-track state lives in arrays the audio thread indexes and a
    // growable container would mean allocating in the callback. 256 covers the stems
    // of a large production (raised from v1's 32, 9 October 2026); the arrays cost a
    // few kilobytes. Mixing time grows linearly per track (see bench_playback and
    // Preferences › Performance › Check this computer). Beyond 256 the routed
    // delay history (RoutingRender) and the track lanes would need rework.
    static constexpr std::size_t kMaxTracks = 256;
    static_assert(kMaxTracks == kMeterTracks,
                  "every rendered track has a meter slot, and no more");

    // How long a gain change takes to travel. 256 frames is 5.33 ms at 48 kHz, and one
    // quantum on the target box -- long enough that a mute does not click, short enough
    // that it still feels instant.
    static constexpr std::uint32_t kMaxCompensatedLatency = 262144;

    static constexpr float kGainRampFrames = 256.0f;

    // `sampleRate` only sizes the loudness meter's windows and weighting; the
    // transport itself counts frames and does not care.
    Transport(Queue& commands, MeterChannel& meters,
              std::uint32_t sampleRate = 48000, CaptureClock* capture = nullptr) noexcept
        : commands_(commands), meters_(meters), loudness_(sampleRate), capture_(capture),
          shuttleInput_(new float[std::size_t(kShuttleChannels) * kShuttleGrain]()),
          grain_(new float[std::size_t(kShuttleChannels) * kShuttleGrain]())
    {
    }

    // Main thread ONLY after the output callback has stopped. Drains old
    // commands before changing the meter rate; the next plan starts stopped.
    void configureStopped(std::uint32_t rate) noexcept;
    // Main thread, with no output callback: acknowledge plans without rendering.
    void flushStopped() noexcept;

    class ControlAccess {
        friend class Transport;
        Transport* transport_;
        ControlHandoff::Request request_;
        explicit ControlAccess(Transport& transport) noexcept
            : transport_(&transport), request_(transport.handoff_.request()) {}
    public:
        ControlAccess(ControlAccess&&) noexcept = default;
        [[nodiscard]] bool ready() const noexcept { return request_.ready(); }
        // The sole consumer migrates to the control thread while owned. Keep
        // playback/position unchanged except for commands explicitly queued.
        [[nodiscard]] bool drainCommands() noexcept {
            if (!ready()) return false;
            transport_->drainCommands();
            return true;
        }
        // Exclusive owner only. The next legal recording input for a fader or
        // effect lane; a missing track selects the master bus. This is an input
        // boundary, which can lead the audible playhead through compensation.
        [[nodiscard]] host::ParameterTime automationInputTime(std::optional<session::TrackId> track,
                                                             bool effect) noexcept {
            return ready() ? transport_->automationInputTime(track, effect) : host::ParameterTime{};
        }
    };
    // Control thread: retain and retry ready() later if a block is in progress.
    // While requested/owned, callbacks write silence without advancing the plan.
    [[nodiscard]] ControlAccess requestControl() noexcept { return ControlAccess(*this); }

    // Audio thread. Drains pending commands, renders `frames` into the output planes,
    // and publishes position and levels.
    void process(float* const* planes, std::uint32_t channelCount,
                 std::uint32_t frames) noexcept;

    // Test accessors. Not called from the audio thread in production.
    [[nodiscard]] session::FramePos position() const noexcept { return position_; }
    [[nodiscard]] bool playing() const noexcept { return playing_; }
    [[nodiscard]] int speed() const noexcept { return speed_; }
    static constexpr int kMaxSpeed = 8;
    [[nodiscard]] std::uint64_t captureEpoch() const noexcept { return captureEpoch_; }
    [[nodiscard]] float gain() const noexcept { return gain_; }
    // Audio thread, from the device callback: time spent against the block's deadline.
    void recordLoad(std::uint64_t elapsedNs, std::uint64_t budgetNs, bool dropout) noexcept
    {
        if (budgetNs == 0) return;
        meters_.publishLoad(float(double(elapsedNs) / double(budgetNs)), double(budgetNs) * 1e-9, dropout);
    }
    // The newest programme output, for live spectrum and phase displays.
    [[nodiscard]] const OutputScope& scope() const noexcept { return scope_; }
    [[nodiscard]] session::FramePos timelineLength() const noexcept
    {
        return plan_ != nullptr ? plan_->length : 0;
    }
    [[nodiscard]] float trackGain(session::TrackId id) const noexcept;
    [[nodiscard]] float masterTrim() const noexcept { return masterTrim_; }
    [[nodiscard]] bool masterBypassed() const noexcept { return masterBypass_; }
    [[nodiscard]] float masterMakeup() const noexcept { return masterMakeup_; }

    // The R128 meter on the mix: after the master bus, before the monitor level, so
    // turning the listening level down does not change what the programme measures.
    // Its readings are atomics, so the GUI thread reads it directly.
    [[nodiscard]] const LoudnessMeter& loudness() const noexcept { return loudness_; }

private:
    ControlHandoff handoff_;
    // Everything a track multiplies its audio by, as five independent lanes so that a
    // pan move and a gain move can be in flight at once without fighting each other.
    // Ramped toward the target at block granularity; the product of gain and pan is
    // then interpolated per frame.
    struct Lanes {
        float gain = 1.0f;
        float monoL = 1.0f;
        float monoR = 1.0f;
        float stereoL = 1.0f;
        float stereoR = 1.0f;
    };

    void checkLatencyTransition(float* const* planes, std::uint32_t channels, std::uint32_t frames, session::FramePos before, bool wasPlaying) noexcept;
    bool processLatency(float* const* planes, std::uint32_t channels, std::uint32_t frames) noexcept;
    bool processRouting(float* const* planes, std::uint32_t channels, std::uint32_t frames) noexcept;
    session::FramePos routingOrigin_ = 0, routingFrame_ = 0;
    [[nodiscard]] session::FramePos latencyTime(session::FramePos raw) const noexcept;
    std::array<std::uint32_t, kMaxTracks> trackLatencies_{};
    std::uint32_t masterLatency_ = 0, totalLatency_ = 0, primeRemaining_ = 0;
    session::FramePos primeOrigin_ = 0;
    bool latencyReset_ = true;
    [[nodiscard]] host::ParameterTime automationInputTime(std::optional<session::TrackId>, bool effect) noexcept;
    void drainCommands() noexcept;
    void countIn(float* const* planes, std::uint32_t channels, std::uint32_t frames) noexcept;
    // Adds the beat click for timeline frames [blockStart, blockStart + frames).
    void metronome(float* const* planes, std::uint32_t channels, std::uint32_t frames,
                   session::FramePos blockStart) noexcept;
    session::FramePos captureOutputFrames_ = 0;
    [[nodiscard]] host::ProcessContext pluginContext(session::FramePos frame, bool preroll = false) const noexcept {
        return plan_ ? processContext(*plan_, frame, playing_, playbackEpoch_, followingCapture_,
            looping_, loopStart_, std::min(loopEnd_, plan_->length), preroll) : host::ProcessContext{};
    }
    void processBlock(float* const* planes, std::uint32_t channelCount,
                      std::uint32_t frames) noexcept;

    // Everything process() does after taking its commands, at normal speed.
    void render(float* const* planes, std::uint32_t channelCount, std::uint32_t frames) noexcept;

    // Shuttle (transport_shuttle.cpp). Faster than normal, it renders speed times
    // as many frames and averages each run of them into one, so effects, latency
    // and automation see the timeline as they always do. Backwards, it renders a
    // short stretch forwards ending at the playhead and plays it reversed.
    static constexpr std::uint32_t kShuttleBlock = 1024;        // output frames per pass
    static constexpr std::uint32_t kShuttleChannels = 8;
    static constexpr std::uint32_t kShuttleGrain = kShuttleBlock * kMaxSpeed;
    void shuttle(float* const* planes, std::uint32_t channelCount, std::uint32_t frames) noexcept;
    bool fillBackwards(std::uint32_t channels, std::uint32_t speed) noexcept;
    void leaveBackwards() noexcept;

    // One track's clips, through its inserts, into the output planes. Split out rather
    // than folded into process(): a track with no chain must keep the exact path -- and
    // the exact summation order -- it had before Phase 5, and the surest way to
    // guarantee that is for it not to share a code path with this at all.
    // `trackPeak` receives the track's own peak for the block -- after the fader and
    // the pan, before the master level -- for the per-track meters.
    void processChained(const PlanTrack& track, float* const* planes,
                        std::uint32_t channelCount, std::uint32_t frames,
                        session::FramePos blockStart, const Lanes& start,
                        const Lanes& end, float* trackPeak) noexcept;

    // Every track into the output planes at unity monitor level: the direct path
    // for a track without inserts, processChained for one with. Advances the ramps
    // whether or not anything is active, which is why it is called either way.
    void renderTracks(float* const* planes, std::uint32_t channelCount,
                      std::uint32_t frames, session::FramePos blockStart, bool active,
                      float maxDelta) noexcept;

    // The summed mix through the master bus chain, in place, in sub-blocks of the
    // chain's maximum.
    void applyMasterChain(float* const* planes, std::uint32_t channelCount,
                          std::uint32_t frames) noexcept;

    // Rebinds per-track ramp state to a new plan, matching tracks by id so a surviving
    // track keeps the gain it was already at.
    void adoptPlan(const RenderPlan* plan) noexcept;

    // Slot holding `id`, or kMaxTracks when there is none.
    [[nodiscard]] std::size_t slotOf(session::TrackId id) const noexcept;

    // Moves `current` toward `target` by at most `maxDelta`.
    [[nodiscard]] static float approach(float current, float target,
                                        float maxDelta) noexcept;

    Queue& commands_;
    MeterChannel& meters_;

    const RenderPlan* plan_ = nullptr;
    std::uint32_t planGeneration_ = 0;

    session::FramePos position_ = 0;
    bool playing_ = false;
    std::uint32_t playbackEpoch_ = 0;
    bool looping_ = false;
    session::FramePos loopStart_ = 0, loopEnd_ = 0;
    float gain_ = 1.0f;                 // master monitor level
    OutputScope scope_;
    bool metronomeOn_ = false;
    float metronomeGain_ = 0.25f;
    float masterTrim_ = 1.0f;           // the bus trim, before its inserts
    bool masterBypass_ = false;         // before/after: skip trim and chain...
    float masterMakeup_ = 1.0f;         // ...and apply this instead, level-matched
    LoudnessMeter loudness_;
    CaptureClock* capture_ = nullptr;
    std::uint64_t captureEpoch_ = 0;
    bool followingCapture_ = false;

    std::array<session::TrackId, kMaxTracks> trackIds_{};
    std::array<Lanes, kMaxTracks> current_{};
    std::array<Lanes, kMaxTracks> target_{};
    std::size_t trackCount_ = 0;

    // Scratch for adoptPlan, a member so remapping does not allocate.
    std::array<session::TrackId, kMaxTracks> scratchIds_{};
    std::array<Lanes, kMaxTracks> scratchLanes_{};

    int speed_ = 1;
    bool backwards_ = false, seeked_ = false;
    session::FramePos audible_ = 0;                 // backwards: the playhead heard
    session::FramePos grainStart_ = 0, grainEnd_ = 0;   // what grain_ holds
    std::unique_ptr<float[]> shuttleInput_;          // kShuttleChannels x kShuttleGrain
    std::unique_ptr<float[]> grain_;                 // kShuttleChannels x kShuttleGrain
};

} // namespace omawave::engine
