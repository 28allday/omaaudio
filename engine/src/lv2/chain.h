#pragma once

#include "lv2/host_features.h"
#include "lv2/instance.h"
#include "lv2/world.h"
#include "session/track.h"

#include <atomic>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace omawave::lv2 {

// The largest block a single run() is ever given. PipeWire's max-quantum on the target
// box is 2048; the chain works in sub-blocks of this whatever the callback is handed,
// which is what makes bufsize:boundedBlockLength an honest claim rather than a promise
// the host breaks the first time the graph is resized.
inline constexpr std::uint32_t kMaxBlock = 2048;

// One track's inserts, in order, with the buffers they run through.
//
// The chain owns the scratch. Instance deliberately owns no audio buffer, so somebody
// has to, and it has to be whoever knows how many there are and how long they live:
// pointers handed to connect_port must stay valid for as long as the audio thread may
// still be running the instance, which is exactly the chain's own lifetime.
//
// Audio flows through two buffers, ping-ponged: slot 0 reads A and writes B, slot 1
// reads B and writes A, and so on. `input()` is where the track's audio goes in and
// `output()` is where the result comes out -- which of the two that is depends on how
// many slots ran, so ask rather than assume.
//
// THREADING is Instance's, one level up:
//
//   * build and destruction belong to the control owner (main for VST3).
//     Warmup transfers exclusive processing ownership before publication. A chain the audio
//     thread may still be running is retired, never destroyed, exactly as a replaced
//     Project or RenderPlan is.
//   * process() is the audio thread. It allocates nothing and locks nothing; the only
//     third-party code it runs is run(), which is the audio class by the LV2 spec.
class Chain {
public:
    // `channels` is the track's channel count -- what goes in and what comes out. A
    // plugin whose port counts do not match is adapted (dual-mono) or refused.
    Chain(std::uint32_t channels, std::uint32_t maxBlock, host::ProcessingMode mode = host::ProcessingMode::Realtime);
    ~Chain();

    Chain(const Chain&) = delete;
    Chain& operator=(const Chain&) = delete;
    Chain(Chain&&) = delete;
    Chain& operator=(Chain&&) = delete;

    // Main thread. Appends one insert, instantiating it and applying `insert`'s stored
    // control values. False with `error` set when the plugin is missing, refuses to
    // instantiate, or cannot be adapted to this channel count -- and in that case the
    // chain is left exactly as it was.
    [[nodiscard]] bool append(const World& world, HostFeatures& features,
                              const session::Insert& insert, std::string& error);

    // Main or exclusively owning offline worker thread, after everything is
    // appended and before the chain is published.
    // Runs silence through every instance so the ones that allocate lazily do it here
    // rather than in the callback. See Instance::warmUp.
    void warmUp() noexcept;
    // An exclusively owned offline chain can warm on its processing worker.
    // Cancellation is checked between blocks; a false result must not be published.
    [[nodiscard]] bool warmUp(const std::atomic<bool>* cancel) noexcept;
    // Control owner, quiescent, after worker warmup and before live publication.
    // Main-owned warmUp calls this automatically after joining. Private offline
    // chains that are only rendered and destroyed need no notification cleanup.
    // Idempotent: a later call cannot discard newly received live edits.
    [[nodiscard]] bool finishWarmUp() noexcept;
    // Keep model slot numbering when a plugin cannot be instantiated.
    void appendMissing(session::InsertIdentity identity = {});
    // Immutable logical insert identity, retained even in retired/missing chains.
    // Observers copy the token on the control thread before retaining a reference.
    [[nodiscard]] const session::InsertIdentity* insertIdentity(std::size_t slot) const noexcept;
    // Immutable distinction between a live insert and a retained missing slot.
    [[nodiscard]] bool hasProcessor(std::size_t slot) const noexcept;
    // Main thread only: the processor whose native editor a window may embed.
    // Dual-mono slots expose each actual instance; null when there is none.
    [[nodiscard]] host::Instance* editorProcessor(std::size_t slot, std::size_t actual = 0) noexcept {
        return slot < slots_.size() && actual < slots_[slot].instances.size()
            ? slots_[slot].instances[actual].get() : nullptr;
    }
    [[nodiscard]] std::size_t instanceCount(std::size_t slot) const noexcept;
    // Immutable port metadata; safe to read on GUI/audio threads after construction.
    [[nodiscard]] const std::vector<ControlPort>* controls(std::size_t slot) const noexcept;

    // Audio thread. Reads `frames` from input(), leaves `frames` in output().
    // `frames` must not exceed maxBlock: the transport sub-blocks, this does not.
    // Average processing cost per frame (nanoseconds), smoothed over recent blocks;
    // cost × sample rate / 1e9 is this chain's share of real time.
    [[nodiscard]] double nanosecondsPerFrame() const noexcept { return costPerFrame_.load(std::memory_order_relaxed); }
    void process(std::uint32_t frames, bool useSidechain = false,
                 const host::ProcessContext& context = {}) noexcept;
    // Called by a new exclusive processing owner after the host handoff, before
    // processing resumes. Preserves DSP, controls, notifications and clocks;
    // this operation does not provide synchronization or reset the processor.
    void adoptProcessingThread() noexcept;
    // Owning main thread: freeze this BEFORE consuming native notifications.
    // An event from a later segment cannot be advanced by this older snapshot.
    [[nodiscard]] host::ParameterTime processedTime() const noexcept;
    [[nodiscard]] bool processParameterEvents(std::size_t slot) const noexcept;
    [[nodiscard]] bool processAutomationSpans(std::size_t slot) const noexcept;
    void setAutomationRecording(std::size_t slot, const host::AutomationRecording&,
                                const host::AutomationPlayback& = {}) noexcept;
    void setInstanceAutomationRecording(std::size_t slot,
        const std::array<host::AutomationRecording, 2>&,
        const std::array<host::AutomationPlayback, 2>&) noexcept;
    void setAutomationControl(std::size_t slot, std::uint32_t port, double value, std::size_t actual = SIZE_MAX) noexcept;
    // Each slot has its own key scratch: the graph compensates the latency of
    // preceding inserts before filling it. Unrouted processing clears every key.
    [[nodiscard]] float* sidechainInput(std::size_t slot, std::uint32_t channel) noexcept;
    [[nodiscard]] bool hasSidechain(std::size_t slot) const noexcept;
    [[nodiscard]] std::uint32_t slotLatency(std::size_t slot) const noexcept;

    // Audio thread. Planar, one pointer per channel, maxBlock frames each.
    [[nodiscard]] float* input(std::uint32_t channel) noexcept;
    [[nodiscard]] const float* output(std::uint32_t channel) const noexcept;

    [[nodiscard]] host::ProcessingMode processingMode() const noexcept { return mode_; }
    [[nodiscard]] std::uint32_t channels() const noexcept { return channels_; }
    [[nodiscard]] std::uint32_t maxBlock() const noexcept { return maxBlock_; }
    [[nodiscard]] std::size_t size() const noexcept { return slots_.size(); }
    [[nodiscard]] bool empty() const noexcept { return slots_.empty(); }

    // Sum of every unbypassed slot's reported latency. A bypassed plugin contributes
    // nothing because its audio does not go through it.
    // Atomic snapshot: UI callers never read a plugin's live control buffer.
    [[nodiscard]] std::uint32_t latencyFrames() const noexcept { return latency_.load(std::memory_order_relaxed); }

    // Audio thread. Bypass is a per-slot switch rather than a rebuild, so switching it
    // cannot allocate and cannot drop a block.
    void setBypass(std::size_t slot, bool bypass) noexcept;
    [[nodiscard]] bool bypassed(std::size_t slot) const noexcept;

    // Audio thread. Ignores a slot or port that does not exist, because a command in
    // flight across a chain rebuild must never write somewhere arbitrary.
    [[nodiscard]] std::uint64_t parameterVersion(std::size_t slot) const noexcept;
    void setControl(std::size_t slot, std::uint32_t port, double value, std::uint64_t version = UINT64_MAX,
                    std::size_t actual = SIZE_MAX) noexcept;
    // Audio thread. The same, `offset` frames into the next process(); an
    // instance without sample offsets takes it at the start. See host::Timing.
    void scheduleControl(std::size_t slot, std::uint32_t port, double value, std::uint32_t offset,
                         std::size_t actual = SIZE_MAX) noexcept;
    [[nodiscard]] host::Timing timing(std::size_t slot) const noexcept;
    // What a slot's first instance holds for a control. For tests and
    // diagnostics with the transport idle; not synchronised with process().
    [[nodiscard]] double controlValue(std::size_t slot, std::uint32_t port, std::size_t actual = 0) const noexcept;
    // Any thread: an output control (a meter) as the last process() left it, the
    // largest of a slot's instances (dual mono shows the busier side).
    [[nodiscard]] double outputValue(std::size_t slot, std::uint32_t port) const noexcept;
    // Owning main thread; the first processor supplies the logical rack's text.
    [[nodiscard]] std::string parameterText(std::size_t slot, std::uint32_t port, double value) const;
    // Immutable capability; missing/parameter-only slots retain the saved model.
    [[nodiscard]] bool hasOpaqueState(std::size_t slot) const noexcept;
    // Control owner, all processing/parameter producers suspended. Captures the
    // complete ordered pair or leaves the caller's previous snapshots unchanged.
    [[nodiscard]] bool captureState(std::size_t slot, std::vector<session::PluginState>& out, std::string& error);
    // Exclusive control owner: capture complete per-instance state and the
    // corresponding control precedence into a model copy. Preserves automation
    // bases and unknown controls. Failure leaves the entire destination unchanged.
    [[nodiscard]] bool captureInsertState(std::size_t slot, session::Insert& out, std::string& error,
                                          bool includeInputControls = false);
    // Exclusive control owner, immediately after coherent capture. Keep actual
    // mono inputs separate: a shared representative value can erase differences.
    using InputValues = std::vector<std::vector<std::pair<std::uint32_t, double>>>;
    [[nodiscard]] InputValues capturedInputValues(std::size_t slot) const;
    // Any thread: an instance has asked to be restarted. The slots themselves
    // do not change once the chain is published, only the flag does.
    [[nodiscard]] bool restartRequested() const noexcept;
    [[nodiscard]] bool canRefreshConfiguration() const noexcept;
    // Main, with all processing and parameter producers excluded. Includes
    // bypassed processors and independent mono instances. Refreshes latency even
    // on partial failure; the caller must republish compensation/presentation.
    [[nodiscard]] bool refreshConfiguration(std::string& error);

    // Owning main thread. A listener is synchronous and may not mutate/reenter
    // this chain or retain its addresses. Slot + instance identify actual mono
    // processors within THIS chain lifetime; deferred work needs its own session,
    // runtime generation, insert identity and state revision validation.
    using ParameterListener = void (*)(void*, std::size_t slot, std::size_t instance,
                                      const host::ParameterEvent&) noexcept;
    using StateListener = void (*)(void*, std::size_t slot, std::size_t instance) noexcept;
    struct Notifications {
        bool dirty = false;
        bool readout = false;
        bool overflow = false;
        bool textChanged = false;
        std::uint32_t newDiagnostics = 0;
        std::size_t events = 0;
    };
    // Enough for both bounded VST3 producers (2048 each). Fair source polling
    // drains all accepted main-thread events at Stop without an unbounded loop.
    static constexpr std::size_t kNotificationPollLimit = 4096; // per actual instance
    void serviceMainThread() noexcept;
    // Bind live controller notifications to their owner before publication.
    // Private render/probe chains deliberately retain no live document clock.
    void setParameterClock(host::ParameterClock clock, void* context) noexcept;
    // parameterEditsHandled opts into accounting for every Value event in the
    // listener. Coalesced parameter-only dirty flags then create no second edit;
    // independent state and notification overflow retain the conservative path.
    // Dual-mono process events merge by segment/sample, preserving each source's
    // FIFO. Normal polls defer an incomplete segment; a Stop/edit boundary can
    // drain every already-accepted event without waiting for the other processor.
    [[nodiscard]] Notifications consumeNotifications(ParameterListener = nullptr, void* = nullptr,
                                                     StateListener = nullptr, StateListener overflowListener = nullptr,
                                                     StateListener readoutListener = nullptr,
                                                     bool parameterEditsHandled = false,
                                                     bool drainInFlight = false) noexcept;
    [[nodiscard]] bool parameterFlushRequested() const noexcept;
    // Main/exclusive; only requested flushes. A failure keeps the adapter request
    // pending. No processing, host command delivery or transport handoff occurs here.
    [[nodiscard]] bool flushParameterChanges(std::string& error);
    [[nodiscard]] std::optional<host::UnrenderedParameter> unrenderedParameter(
        std::size_t slot, std::uint32_t port, std::size_t actual) const noexcept;
    [[nodiscard]] host::ParameterTime nextInputTime(std::size_t slot, std::size_t actual) const noexcept;

private:
    std::atomic<double> costPerFrame_{0.0};

    friend struct ChainWarmupTest; // owned adapter tests; no application factory bypass
    [[nodiscard]] std::uint32_t measureLatency() const noexcept;
    std::atomic<std::uint32_t> latency_{0};
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    // One insert. Usually one instance; a mono plugin on a stereo track is two,
    // driven identically from shared control values, which keeps it a dual-mono chain
    // rather than quietly making it a stereo one.
    struct Slot {
        explicit Slot(session::InsertIdentity key) : identity(std::move(key)) {}
        session::InsertIdentity identity;
        std::vector<std::unique_ptr<host::Instance>> instances;
        // Owning control thread only. One lookahead per actual instance; public
        // construction permits one processor or the two halves of dual mono.
        std::array<std::optional<host::ParameterEvent>, 2> pendingEvents;
        // Control owner only. Reuse immutable file identities for unchanged
        // captures; repeated autosaves must not create duplicate state files.
        std::vector<session::PluginState> capturedState;
        bool bypass = false;
        bool dualMono = false;
        std::vector<float> sidechain;
    };

    // Which buffer a slot reads, given how many slots precede it.
    [[nodiscard]] float* plane(std::size_t buffer, std::uint32_t channel) noexcept;

    const host::ProcessingMode mode_;
    std::uint32_t channels_;
    std::uint32_t maxBlock_;
    const std::thread::id controlThread_ = std::this_thread::get_id();
    bool warmUpNotificationsPending_ = false; // exclusive owner, transferred by join

    // Two interleaved-by-channel scratch planes, allocated once. The audio thread holds
    // pointers into these through connect_port, so they are sized at construction and
    // never resized -- a reallocation here is a use-after-free in the callback.
    std::vector<float> scratch_;

    std::vector<Slot> slots_;
    std::size_t activeBuffer_ = 0;      // which buffer holds the result after process()
    std::uint32_t reportedNotificationDiagnostics_ = 0; // owning main thread only
    // Single processing writer, bounded main reader. Every shared field is
    // atomic; readers never make the callback wait or allocate.
    void publishProcessedTime(const host::ParameterTime&) noexcept;
    host::ProcessContext previousProcess_{};
    std::size_t recordingSlot_ = SIZE_MAX;
    std::array<host::AutomationRecording, 2> recording_{};
    std::uint64_t processSequence_ = 0, processLoopIteration_ = 0;
    std::atomic<std::uint32_t> processPublication_{0};
    std::atomic<std::int64_t> processedFrame_{0};
    std::atomic<std::uint64_t> processedDocument_{0}, processedPlayback_{0},
        processedSequence_{0}, processedLoopIteration_{0};
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<std::int64_t>::is_always_lock_free);

    // One worker thread for the whole chain, created the first time an appended
    // instance asks for one. Declared after slots_ so destruction joins the thread
    // BEFORE the instances it performs work on are freed -- members are destroyed in
    // reverse order, and getting this backwards is a use-after-free on the worker
    // thread during retirement.
    std::unique_ptr<Worker> worker_;
};

} // namespace omawave::lv2

namespace omawave::host {
// One instance of whatever format `uri` names, from the world's catalogue: LV2
// through lilv with the host features, CLAP through its catalogue at the
// features' sample rate and block size; VST3 uses its isolated catalogue entry.
// VST3 construction/activation/retirement requires the process main thread.
// `options` supplies layout/mode; `error` says why creation was refused.
[[nodiscard]] std::unique_ptr<Instance> createInstance(const lv2::World& world, const std::string& uri,
                                                       lv2::HostFeatures& features, std::string& error, bool legacyClapControls = false,
                                                       const session::PluginState* state = nullptr, InstanceOptions options = {});
} // namespace omawave::host
