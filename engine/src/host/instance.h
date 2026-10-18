#pragma once

#include "editor.h"

#include "session/plugin_state.h"
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The format-neutral face of a hosted plugin (W09). LV2, CLAP and the candidate
// VST3 adapter implement it; the chain, the transport, the render plan and the
// application see only this: ports to connect, controls to set, a run() to
// call, a latency to read. Whatever a format needs beyond it stays inside its
// own implementation.
namespace omawave::lv2 { class Worker; }

namespace omawave::host {

enum class ProcessingMode : std::uint8_t { Realtime, Offline };
// Zero channels selects the catalogue's default layout for a control-only probe.
// Processing chains supply their actual per-instance channel count explicitly.
struct InstanceOptions {
    std::uint32_t channels = 0;
    ProcessingMode mode = ProcessingMode::Realtime;
};

enum class Format : std::uint8_t { Lv2, Clap, Vst3 };

[[nodiscard]] constexpr const char* formatName(Format format) noexcept
{
    return format == Format::Vst3 ? "VST3" : format == Format::Clap ? "CLAP" : "LV2";
}

// How a plugin's main audio ports relate to a track. A plugin that takes exactly
// one channel fits a mono track and drives a stereo one as two instances; two
// channels fits a stereo track; a plugin with optional ports may fit either.
enum class Layout : std::uint8_t {
    Unsupported = 0,
    Mono,
    Stereo,
    MonoOrStereo,
};

struct AudioConfiguration {
    std::uint32_t channels = 0, sidechainChannels = 0;
    bool operator==(const AudioConfiguration&) const = default;
};

// One entry in the catalogue. Everything the browser shows and everything the insert
// rules need, derived once at scan time so nothing later has to walk ports again.
struct PluginInfo {
    Format format = Format::Lv2;
    std::string uri;            // LV2 URI, "clap:" + plugin ID, or "vst3:" + class ID
    std::string name;
    std::string className;      // "Compressor Plugin", "Reverb Plugin", ...
    std::string author;
    std::string path;           // Native module file (CLAP) or bundle directory (VST3)

    // Main audio ports: sidechains excluded. `required` must be connected, `total`
    // is the most the plugin will accept.
    std::uint32_t requiredIn = 0;
    std::uint32_t totalIn = 0;
    std::uint32_t requiredOut = 0;
    std::uint32_t totalOut = 0;

    // Key inputs, counted so the browser can say "stereo, with a sidechain".
    std::uint32_t sidechainIn = 0;

    // Negotiated bus formats list one preferred key layout per main layout;
    // the first entry is the preferred probe configuration. Port-based formats
    // leave this empty and use the required/total ranges above.
    std::vector<AudioConfiguration> audioConfigurations;

    std::uint32_t controlIn = 0;    // how many knobs the generic UI will draw
    std::uint32_t atomPorts = 0;    // LV2 only: 315 of 360 have one; all must be connected

    Layout layout = Layout::Unsupported;
    bool needsWorker = false;       // LV2: requires worker:schedule
    bool hasStateInterface = false; // has state beyond its control ports

    // False when the layout fits neither a mono nor a stereo track, or a required
    // feature is missing. Unusable plugins stay in the catalogue and are shown greyed
    // out with the reason: a plugin simply absent from the list looks like a broken
    // install.
    bool usable = false;
    std::string unusableReason;

    // Can this plugin take a track of `channels` directly, one instance, no adaptation?
    [[nodiscard]] constexpr bool fits(std::uint32_t channels) const noexcept
    {
        if (!audioConfigurations.empty()) return configuration(channels) != nullptr;
        return requiredIn <= channels && channels <= totalIn
            && requiredOut <= channels && channels <= totalOut;
    }
    [[nodiscard]] constexpr const AudioConfiguration* configuration(std::uint32_t channels) const noexcept
    {
        for (const auto& layout : audioConfigurations) if (layout.channels == channels) return &layout;
        return nullptr;
    }
};

// A control the generic interface draws and the project stores by index.
//
// Display metadata -- units, scale points, port groups, logarithmic scaling -- is what
// the generic UI needs and arrives with it. What is here is what correctness needs:
// the range to clamp to, and whether the value is a switch or a whole number.
struct ControlPort {
    std::uint32_t index = 0;        // LV2: port index; CLAP: stable param_id (legacy projects: position)
    std::string symbol;
    std::string name;
    // CLAP/VST3 numeric identity, independent of legacy positional project keys.
    std::optional<std::uint32_t> parameterId{};

    double minimum = 0.0;
    double maximum = 1.0;
    double defaultValue = 0.0;

    bool toggled = false;
    bool integer = false;
    bool output = false;            // a meter or a readout, not a knob
    bool automatable = true;        // manual controls need not accept timeline automation

    [[nodiscard]] double clamp(double value) const noexcept
    {
        if (value < minimum)
            return minimum;
        if (value > maximum)
            return maximum;
        return value;
    }
};

// How precisely a plugin can be told WHEN a control changes (W10).
//
//   * sampleOffsets: the plugin takes a value with a frame offset inside a
//     run() (CLAP parameter events). Automation points land on their exact
//     frame and ramps are rendered one value per `rampStep` frames, up to
//     `eventCapacity` values per run; past that the host widens the step for
//     that run rather than drop values.
//   * otherwise a control is one value per run() (LV2 control ports). The host
//     splits runs at automation points, so points are still exact, and steps
//     ramps every `rampStep` frames, which is the plugin's real resolution.
struct Timing {
    bool sampleOffsets = false;
    std::uint32_t rampStep = 64;
    std::uint32_t eventCapacity = 0;
};

// Native output, consumed on the instance's owning main thread. A control key
// can be absent (hidden parameters), or differ from the native id in old projects.
// Offsets are local to the producing process block; they are not session frames.
enum class ParameterEventKind : std::uint8_t { Begin, Value, End, GroupBegin, GroupEnd };
// Host timeline sampled at a native controller callback, before deferred polling.
// Invalid means no live clock was available; a block offset is not a substitute.
struct ParameterTime {
    std::int64_t frame = 0;
    std::uint64_t document = 0;
    std::uint32_t playbackEpoch = 0;
    bool playing = false, valid = false;
    // Nonzero only for an audio process segment, scoped to its owning Chain.
    // The completed-segment reader is sampled BEFORE draining event queues.
    std::uint64_t processSequence = 0, loopIteration = 0;
    bool operator==(const ParameterTime&) const = default;
};
using ParameterClock = ParameterTime (*)(void*) noexcept;
enum class AutomationMode : std::uint8_t { Read, Write, Touch, Latch };
// A shared acceptance order distinguishes an older queued host gesture from a
// newer native edit, including the two processors in a dual-mono insert.
inline std::uint64_t nextAutomationRevision() noexcept {
    static std::atomic<std::uint64_t> next{0};
    return next.fetch_add(1, std::memory_order_relaxed) + 1;
}
// Immutable plan policy, applied by the processing owner before automation.
// A pass changes at arm/Play; manualRevision changes only for explicit host UI
// input. Publishing recorded curve points must not echo old values into DSP.
struct AutomationRecording {
    std::uint32_t control = UINT32_MAX;
    AutomationMode mode = AutomationMode::Read;
    std::uint64_t pass = 0, manualRevision = 0;
    double manualValue = 0;
    bool manualActive = false;
    std::uint32_t automatchFrames = 0;
    std::uint64_t armRevision = 0;
    // A host move may be released before any callback sees it. The processing
    // owner applies an unseen revision once; delayed GUI feedback must never
    // extend a gesture whose input has already been processed.
    bool manualPending = false;
    [[nodiscard]] bool active() const noexcept { return pass && mode != AutomationMode::Read && control != UINT32_MAX; }
    bool operator==(const AutomationRecording&) const = default;
};
// Borrowed for the upcoming process segment; its render plan owns context.
// Replace before every run; never dereference from main-thread notifications.
struct AutomationPlayback {
    const void* context = nullptr;
    double (*valueAt)(const void*, std::int64_t) noexcept = nullptr;
};
// Value-only snapshot for the processing owner. No main-thread clock is read
// from run(). A valid offline timeline has document == 0 and cannot record edits.
// Hosts split runs at discontinuities; time describes sample zero of THIS run.
struct ProcessContext {
    ParameterTime time{};
    std::int64_t loopStart = 0, loopEnd = 0;
    double tempo = 0; // quarter notes/minute; zero means unavailable
    std::uint16_t numerator = 0, denominator = 0;
    bool looping = false, recording = false, preroll = false;

    [[nodiscard]] bool hasMusic() const noexcept {
        return std::isfinite(tempo) && tempo > 0 && tempo <= 10000
            && numerator && denominator;
    }
    [[nodiscard]] bool hasLoop() const noexcept { return looping && loopStart >= 0 && loopEnd > loopStart; }
    [[nodiscard]] ParameterTime recordTime() const noexcept {
        auto result = time;
        result.valid = time.valid && time.document && !preroll && time.frame >= 0;
        return result;
    }
};
// Sparse processing acknowledgements; these do not create native settings edits.
enum class AutomationFeedback : std::uint8_t { Manual, Value, Read, End, Ramp };
struct ParameterEvent {
    ParameterEventKind kind{};
    std::uint32_t id = UINT32_MAX, control = UINT32_MAX;
    double value = 0;
    std::uint32_t offset = 0, flags = 0;
    // Optional native edit group, scoped to the actual instance. Boundaries have
    // no parameter id/control. A group token is not a session timestamp.
    std::uint64_t group = 0;
    ParameterTime time{};
    // Native controller input that the format permits recording. Feedback and
    // non-automatable controls remain ordinary state notifications.
    bool recordable = false;
    // Nonzero for host recording feedback from the processing owner. This
    // acknowledges an applied control or boundary, without a native state edit.
    std::uint64_t automationPass = 0;
    AutomationFeedback automationFeedback = AutomationFeedback::Manual;
    // A native release may precede the next effective Read input. Preserve its
    // native timestamp, then close recording with a processing End boundary.
    bool automationEndDeferred = false;
    // Processed linear span, inclusive endpoints, before integer quantisation.
    std::uint32_t automationSpanFrames = 0;
    double automationEndValue = 0;
    double automationStep = 0; // exact planned slope, also valid for one-sample spans
    std::uint32_t automationGridFrames = 1; // block controls hold between these grid frames
    // Acceptance order shared with explicit host edits. A processed span can
    // acknowledge its input without confusing a newer queued controller value.
    std::uint64_t automationRevision = 0;
};
// An explicitly accepted input flushed into DSP without rendering samples.
// Its time is the next legal process position, never the earlier UI timestamp.
struct UnrenderedParameter {
    double value = 0;
    std::uint64_t revision = 0;
    ParameterTime time{};
};
enum NotificationDiagnostic : std::uint32_t {
    InvalidOutput = 1, OutputOverflow = 2, WrongNotificationThread = 4, MainCallbackFailed = 8
};

// A readout may be transient OR persistent. Its live value is already available
// in the control cache; consumers must compare coherent opaque state before
// treating it as an edit. It must remain pending across failed captures.
struct StateChanges {
    bool edited = false;
    bool readout = false;
    // Every edit is represented by a queued ParameterEvent::Value. Consumers
    // opting in can account for each as either automation or a settings edit.
    bool parametersOnly = false;
};

// An instantiated plugin with every port it needs connected.
//
// THREADING, and it is the whole design:
//
//   * creation, activation, deactivation, destruction and state are the
//     instantiation class, never concurrent with processing. LV2/CLAP permit
//     main or worker ownership; VST3 requires the process main thread. Offline
//     VST3 preparation/retirement must be dispatched there explicitly.
//   * run() and setControl() are the audio class. They allocate nothing, lock
//     nothing and take no branch that touches the heap.
//
// Audio buffers are NOT owned here. The chain hands in pointers to its own scratch
// and they must stay valid until they are replaced.
class Instance : public EmbeddedEditor {
public:
    virtual ~Instance() = default;
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;

    [[nodiscard]] virtual const std::string& uri() const noexcept = 0;
    [[nodiscard]] virtual const PluginInfo& info() const noexcept = 0;
    [[nodiscard]] virtual const std::vector<ControlPort>& controls() const noexcept = 0;

    // Main-thread wiring. A buffer of nullptr disconnects an optional port; a
    // required port refuses it.
    virtual bool connectAudioIn(std::uint32_t channel, float* buffer) noexcept = 0;
    virtual bool connectAudioOut(std::uint32_t channel, float* buffer) noexcept = 0;
    virtual bool connectMainAudioIn(std::uint32_t channel, float* buffer) noexcept = 0;
    virtual bool connectSidechainIn(std::uint32_t channel, float* buffer) noexcept = 0;
    [[nodiscard]] virtual std::uint32_t mainAudioInCount() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t sidechainInCount() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t audioInCount() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t audioOutCount() const noexcept = 0;

    [[nodiscard]] virtual bool ready() const noexcept = 0;
    virtual void activate() noexcept = 0;
    virtual void deactivate() noexcept = 0;
    [[nodiscard]] virtual bool active() const noexcept = 0;

    // Audio class.
    [[nodiscard]] virtual bool processParameterEvents() const noexcept { return false; }
    // Every recorded interval is acknowledged explicitly. A GUI poll must not
    // extend a held point to the latest processing position for these adapters.
    [[nodiscard]] virtual bool processAutomationSpans() const noexcept { return false; }
    virtual void setAutomationRecording(const AutomationRecording&) noexcept {}
    virtual void setAutomationPlayback(const AutomationPlayback&) noexcept {}
    [[nodiscard]] virtual bool automationPlaybackAllowed(std::uint32_t) const noexcept { return true; }
    virtual void setProcessContext(const ProcessContext&) noexcept {}
    // Main thread. A native editor embedded into an X11 window the host owns.
    // openEditor reports the editor's size; the host keeps it alive and calls
    // closeEditor before the instance is destroyed. resizeEditor offers a size
    // and returns the one the editor accepted.
    [[nodiscard]] virtual bool hasEditor() const noexcept { return false; }
    [[nodiscard]] bool openEditor(std::uint64_t, EditorHost&, EditorSize&, std::string& error) override {
        error = "this effect has no editor of its own"; return false;
    }
    bool resizeEditor(std::uint32_t&, std::uint32_t&) noexcept override { return false; }
    void closeEditor() noexcept override {}
    [[nodiscard]] virtual bool editorOpen() const noexcept { return false; }
    // New exclusive processing owner, before its first run. Ownership must
    // already have been transferred by the host; this does not synchronize,
    // start/stop processing, reset DSP or discard queued controls. Adapters
    // with a symbolic audio-thread identity update it here without allocation.
    virtual void adoptProcessingThread() noexcept {}
    virtual void run(std::uint32_t frames) noexcept = 0;
    virtual void setControl(std::uint32_t index, float value) noexcept = 0;
    // A value that takes effect `offset` frames into the next run(). An instance
    // without sample offsets applies it at the start of the run and says so.
    virtual bool scheduleControl(std::uint32_t index, float value, std::uint32_t offset) noexcept
    {
        (void)offset;
        setControl(index, value);
        return false;
    }
    [[nodiscard]] virtual Timing timing() const noexcept { return {}; }
    [[nodiscard]] virtual float controlValue(std::uint32_t index) const noexcept = 0;
    // Any thread: the last value an output control (a meter or readout) held after a
    // run(). Lock-free and approximate by design; 0 for a format without outputs.
    [[nodiscard]] virtual float outputValue(std::uint32_t index) const noexcept { (void)index; return 0.0f; }
    // Precise formats override these. Existing float formats keep their declared
    // resolution; nonfinite/out-of-float-range input never narrows unchecked.
    virtual void setParameter(std::uint32_t index, double value) noexcept {
        if (std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max())
            setControl(index, static_cast<float>(value));
    }
    // A wholesale native value change supersedes older untimed host commands.
    // Versions must be comparable across actual instances of one logical insert.
    // Capture the version when accepting a queued command, not when delivering it.
    // UINT64_MAX means a direct command accepted at execution. Formats without
    // native value rescans keep their ordinary setter behavior.
    [[nodiscard]] virtual std::uint64_t parameterVersion() const noexcept { return 0; }
    virtual void setParameterAtVersion(std::uint32_t index, double value, std::uint64_t) noexcept {
        setParameter(index, value);
    }
    // Owning main thread. Format the supplied plain value using this live
    // processor's presentation, without changing or flushing its parameters.
    // Empty means unsupported/invalid: callers retain their numeric fallback.
    [[nodiscard]] virtual std::string parameterText(std::uint32_t, double) const { return {}; }
    [[nodiscard]] virtual bool takeParameterTextChanged() noexcept { return false; }
    virtual bool scheduleParameter(std::uint32_t index, double value, std::uint32_t offset) noexcept {
        return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max()
            && scheduleControl(index, static_cast<float>(value), offset);
    }
    [[nodiscard]] virtual double parameterValue(std::uint32_t index) const noexcept { return controlValue(index); }
    [[nodiscard]] virtual std::uint32_t latencyFrames() const noexcept = 0;

    // Before publication, on the main or exclusively owning offline worker thread:
    // run silence so lazy allocation never reaches the live audio callback.
    virtual void warmUp(std::uint32_t blocks, std::uint32_t frames) noexcept = 0;

    // Shared-chain warmup, before publication with exclusive ownership. Formats
    // that require a processing thread say so before the chain dispatches the
    // entire batch. Every successful begin is paired with end on that same thread,
    // including cancellation/failure; no creation, activation or notification
    // reads occur here. Legacy formats retain their existing block warmup.
    [[nodiscard]] virtual bool warmUpNeedsProcessingThread() const noexcept { return false; }
    [[nodiscard]] virtual bool beginWarmUp() noexcept { return active(); }
    [[nodiscard]] virtual bool warmUpBlock(std::uint32_t frames) noexcept {
        warmUp(1, frames); return active();
    }
    [[nodiscard]] virtual bool endWarmUp() noexcept { return true; }

    // LV2's worker:schedule. Other formats want none.
    // The plugin asked to be deactivated and activated again (a changed latency,
    // usually). Any thread; the owner rebuilds the chain from the main thread.
    [[nodiscard]] virtual bool restartRequested() const noexcept { return false; }
    // Some formats distinguish live configuration changes from a full reload.
    // The main owner must exclude processing and parameter producers before
    // refreshing. Failure retains the request for an explicit retry.
    [[nodiscard]] virtual bool canRefreshConfiguration() const noexcept { return false; }
    [[nodiscard]] virtual bool refreshConfiguration(std::string& error) {
        error = "This effect requires recreation"; return false;
    }

    // Owning main thread, safe alongside processing: bounded queue consumption,
    // atomic dirty flags and at most one coalesced main callback per service call.
    // A format without these notifications leaves the defaults in place. Flags
    // in ParameterEvent retain the source format's flags (including CLAP's LIVE
    // and DONT_RECORD); neither flag makes a changed value clean.
    [[nodiscard]] virtual bool nextParameterEvent(ParameterEvent&) noexcept { return false; }
    // Main owner only; attach before publication. The context must outlive this
    // instance. Private/offline instances stay unbound. Processing never calls it.
    void setParameterClock(ParameterClock clock, void* context) noexcept {
        parameterClock_ = clock; parameterClockContext_ = context;
    }
    [[nodiscard]] virtual bool takeStateDirty() noexcept { return false; }
    // Legacy consumers of takeStateDirty remain conservative. An owner using
    // this classification is responsible for Save/close/recovery verification.
    [[nodiscard]] virtual StateChanges takeStateChanges() noexcept { return {takeStateDirty(), false}; }
    // Acknowledge loss since the previous call, independently of sticky
    // diagnostics. Lost gesture boundaries require a coherent state capture;
    // this flag alone does not imply changed state or an undoable edit.
    [[nodiscard]] virtual bool takeNotificationOverflow() noexcept { return false; }
    [[nodiscard]] virtual bool serviceMainThread() noexcept { return true; }
    [[nodiscard]] virtual std::uint32_t notificationDiagnostics() const noexcept { return 0; }
    [[nodiscard]] virtual bool parameterFlushRequested() const noexcept { return false; }
    // Owning main thread, processor AND parameter producers quiescent. The owner
    // must transfer exclusive access before calling this, including when bypassed.
    [[nodiscard]] virtual bool flushParameterChanges(std::string& error) { error.clear(); return true; }
    // Exclusive control owner only, after a successful parameter flush. This
    // describes accepted intent, not an audio-processing acknowledgement.
    [[nodiscard]] virtual std::optional<UnrenderedParameter> unrenderedParameter(std::uint32_t) const noexcept { return {}; }
    // Same exclusive owner. The next legal input frame, even if an earlier
    // captured intent has since been consumed or superseded by Read playback.
    [[nodiscard]] virtual ParameterTime nextInputTime() const noexcept { return {}; }
    [[nodiscard]] virtual bool wantsWorker() const noexcept { return false; }
    virtual void setWorker(lv2::Worker*) noexcept {}

    // Control owner only, while the processor and all parameter producers are
    // quiescent. Capture must include pending untimed edits or explicitly fail;
    // scheduled future automation must not be advanced to make a snapshot.
    // Restore deliberately belongs to factories creating unpublished candidates,
    // never to a mutable live-instance virtual method. Unsupported formats retain
    // their parameter model; LV2 external-file assets need a separate contract.
    [[nodiscard]] virtual bool hasOpaqueState() const noexcept { return false; }
    [[nodiscard]] virtual std::optional<session::PluginState> captureState(std::string& error) {
        error = "opaque state capture is unsupported for this plugin"; return {};
    }

protected:
    Instance() = default;
    [[nodiscard]] ParameterTime parameterTime() const noexcept {
        return parameterClock_ ? parameterClock_(parameterClockContext_) : ParameterTime{};
    }
private:
    ParameterClock parameterClock_ = nullptr;
    void* parameterClockContext_ = nullptr;
};

} // namespace omawave::host
