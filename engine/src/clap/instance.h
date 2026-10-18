#pragma once

#include "clap/probe_host.h"
#include "host/instance.h"
#include "engine/command_queue.h"

#include <clap/clap.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace omawave::clap {

class Catalogue;

// A CLAP plugin behind the neutral instance interface.
//
// Parameters are the controls: setControl() records a value from any thread and
// run() delivers every changed value as a CLAP_EVENT_PARAM_VALUE at offset 0;
// scheduleControl() (audio thread) queues a value at a frame offset inside the
// next run, which is how automation reaches the plugin at exact frames. The
// event list the plugin reads is sorted by time, as CLAP requires. Audio ports are
// pointers the chain connects; an unconnected optional channel is fed silence
// and an unwanted output drains into scratch, because CLAP wants every channel
// of a port to be a valid buffer. A plugin's restart request is recorded and
// honoured the next time the chain is rebuilt, since reactivation is not for
// the audio thread; latencyFrames() is what the plugin reported at activation.
class Instance final : public host::Instance {
public:
    [[nodiscard]] static std::unique_ptr<Instance> create(const Catalogue& catalogue, const host::PluginInfo& info,
                                                          double sampleRate, std::uint32_t maxBlock,
                                                          std::string& error, bool legacyControlIndices = false,
                                                          const session::PluginState* state = nullptr);
    ~Instance() override;

    [[nodiscard]] const std::string& uri() const noexcept override { return info_.uri; }
    [[nodiscard]] const host::PluginInfo& info() const noexcept override { return info_; }
    [[nodiscard]] const std::vector<host::ControlPort>& controls() const noexcept override { return controls_; }

    bool connectAudioIn(std::uint32_t channel, float* buffer) noexcept override;
    bool connectAudioOut(std::uint32_t channel, float* buffer) noexcept override;
    bool connectMainAudioIn(std::uint32_t channel, float* buffer) noexcept override;
    bool connectSidechainIn(std::uint32_t channel, float* buffer) noexcept override;
    [[nodiscard]] std::uint32_t mainAudioInCount() const noexcept override { return static_cast<std::uint32_t>(mainIn_.size()); }
    [[nodiscard]] std::uint32_t sidechainInCount() const noexcept override { return static_cast<std::uint32_t>(sidechainIn_.size()); }
    [[nodiscard]] std::uint32_t audioInCount() const noexcept override { return mainAudioInCount() + sidechainInCount(); }
    [[nodiscard]] std::uint32_t audioOutCount() const noexcept override { return static_cast<std::uint32_t>(out_.size()); }

    [[nodiscard]] bool ready() const noexcept override;
    void activate() noexcept override;
    void deactivate() noexcept override;
    [[nodiscard]] bool active() const noexcept override { return active_; }

    void adoptProcessingThread() noexcept override { host_.markAudioThread(); }
    void run(std::uint32_t frames) noexcept override;
    void setControl(std::uint32_t index, float value) noexcept override;
    bool scheduleControl(std::uint32_t index, float value, std::uint32_t offset) noexcept override;
    void setParameter(std::uint32_t index, double value) noexcept override;
    void setParameterAtVersion(std::uint32_t index, double value, std::uint64_t version) noexcept override;
    [[nodiscard]] std::uint64_t parameterVersion() const noexcept override { return host_.valueRescanVersion(); }
    [[nodiscard]] std::string parameterText(std::uint32_t index, double value) const override;
    [[nodiscard]] bool takeParameterTextChanged() noexcept override;
    bool scheduleParameter(std::uint32_t index, double value, std::uint32_t offset) noexcept override;
    [[nodiscard]] double parameterValue(std::uint32_t index) const noexcept override;
    [[nodiscard]] host::Timing timing() const noexcept override;
    [[nodiscard]] float controlValue(std::uint32_t index) const noexcept override;
    [[nodiscard]] std::uint32_t latencyFrames() const noexcept override { return latency_.load(std::memory_order_relaxed); }
    void warmUp(std::uint32_t blocks, std::uint32_t frames) noexcept override;
    void setProcessContext(const host::ProcessContext& context) noexcept override { processContext_ = context; }
    [[nodiscard]] bool processParameterEvents() const noexcept override { return true; }
    void setAutomationRecording(const host::AutomationRecording&) noexcept override;
    void setAutomationPlayback(const host::AutomationPlayback& playback) noexcept override { automationPlayback_ = playback; }
    [[nodiscard]] bool automationPlaybackAllowed(std::uint32_t) const noexcept override;

    // CLAP particulars, for the tests and the rebuild that honours a restart.
    [[nodiscard]] bool restartRequested() const noexcept override { return host_.restartRequested(); }
    [[nodiscard]] bool latencyChangeReported() const noexcept { return host_.latencyChangedFlag(); }
    [[nodiscard]] bool hasOpaqueState() const noexcept override { return hasState(); }
    [[nodiscard]] std::optional<session::PluginState> captureState(std::string& error) override;
    // Owner main thread, plugin deactivated or idle: project-context state blob.
    // Low-level load may mutate on failure; only use it on a disposable candidate.
    [[nodiscard]] bool saveState(std::vector<std::uint8_t>& out) const noexcept;
    [[nodiscard]] bool loadState(std::span<const std::uint8_t> in) noexcept;
    [[nodiscard]] bool hasState() const noexcept { return state_ && state_->save && state_->load; }
    // Native notifications through the common host contract. Only
    // the owning main thread consumes these; process/flush is the sole producer.
    // IDs stay native even for hidden parameters or legacy positional controls.
    using EventKind = host::ParameterEventKind;
    using ParameterEvent = host::ParameterEvent;
    static constexpr std::size_t kNotificationCapacity = 1023;
    using NotificationDiagnostic = host::NotificationDiagnostic;
    static constexpr auto InvalidOutput = host::InvalidOutput, OutputOverflow = host::OutputOverflow,
        WrongNotificationThread = host::WrongNotificationThread, MainCallbackFailed = host::MainCallbackFailed;
    [[nodiscard]] std::uint32_t notificationDiagnostics() const noexcept override { return notificationDiagnostics_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool nextParameterEvent(ParameterEvent&) noexcept override;
    [[nodiscard]] bool takeStateDirty() noexcept override;
    [[nodiscard]] host::StateChanges takeStateChanges() noexcept override;
    [[nodiscard]] bool takeNotificationOverflow() noexcept override;
    // At most one coalesced callback per call. Does not flush active parameters
    // or invoke any model code; callback requests made inside it wait until next time.
    [[nodiscard]] bool serviceMainThread() noexcept override;
    [[nodiscard]] bool parameterFlushRequested() const noexcept override { return host_.flushRequested() || host_.valueRescanRequested(); }
    // Main, processing and parameter producers quiescent. Includes host untimed
    // edits, request_flush and native value rescans without advancing scheduled
    // automation. Audio processing cannot service main-thread value queries.
    [[nodiscard]] bool flushParameterChanges(std::string& error) override;
    // The plugin itself, for tests that read a fixture's trace extension.
    [[nodiscard]] const clap_plugin_t* raw() const noexcept { return plugin_; }
    // Main. The plug-in's own editor embedded in an X11 window (CLAP_WINDOW_API_X11).
    bool hasEditor() const noexcept override;
    bool openEditor(std::uint64_t parent, host::EditorHost&, host::EditorSize&, std::string& error) override;
    bool resizeEditor(std::uint32_t& width, std::uint32_t& height) noexcept override;
    void closeEditor() noexcept override;
    bool editorOpen() const noexcept override { return editorOpen_; }
    // Events queued for the next run, and how many the last run delivered.
    [[nodiscard]] std::uint32_t pendingEvents() const noexcept { return eventCount_; }
    [[nodiscard]] std::uint32_t deliveredEvents() const noexcept { return delivered_; }

private:
    Instance() = default;
    void flushParameters() noexcept;    // audio thread: changed values into the event list

    void pushEvent(std::size_t param, double value, std::uint32_t offset, bool playback = false, bool returning = false) noexcept;
    void sortEvents() noexcept;
    void publishRecordingEvents() noexcept;
    void publishRecordingFeedback(host::AutomationFeedback, std::uint32_t offset, double value = 0, bool recordable = true) noexcept;
    void finishRecordingRelease(std::uint32_t offset, bool recordable = true) noexcept;
    bool restoreRecordingValue() noexcept;
    void prepareAutomatch(std::uint32_t frames) noexcept;
    void publishAutomatchSpan(std::uint32_t end) noexcept;
    [[nodiscard]] double automatchRunValue(std::uint32_t offset) const noexcept;
    [[nodiscard]] std::size_t controlSlot(std::uint32_t id) const noexcept;

    // The in-event list the plugin reads: preallocated, read through `order_` so
    // it comes out sorted by time without moving the events.
    static uint32_t eventsSize(const clap_input_events_t* list) noexcept;
    static const clap_event_header_t* eventsGet(const clap_input_events_t* list, uint32_t index) noexcept;
    static bool eventsTryPush(const clap_output_events_t*, const clap_event_header_t*) noexcept;
    void publishInputValues() noexcept;
    void advanceFeedbackInputs(std::uint32_t offset) noexcept;
    void invalidateFeedbackValues() noexcept;
    [[nodiscard]] bool readNativeValues(std::vector<double>&) const;
    void publishNativeValues(const std::vector<double>&) noexcept;
    [[nodiscard]] bool refreshRescannedValues(std::string& error);
    struct NativeParameter {
        clap_id id;
        std::size_t control;
        double minimum, maximum;
        bool integer;
        // Processing/exclusive-state owner only, including hidden IDs. The UI
        // atomic cache can contain later host commands and is not this baseline.
        double value = 0;
        bool known = false;
        bool readOnly = false;
        bool gesturing = false;
    };
    std::vector<NativeParameter> nativeParameters_;
    engine::CommandQueue<ParameterEvent, kNotificationCapacity + 1> notifications_;
    static_assert(std::atomic<std::size_t>::is_always_lock_free);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);
    std::atomic<std::uint32_t> notificationDiagnostics_{0};
    std::atomic<bool> notificationOverflow_{false};
    std::atomic<bool> nativeStateDirty_{false};
    std::atomic<bool> readoutStatePending_{false};
    bool servicingMain_ = false, acceptingOutput_ = false;
    std::uint32_t outputFrames_ = 0, lastOutputTime_ = 0, feedbackInput_ = 0;
    host::ProcessContext processContext_{};
    host::AutomationRecording recording_{}, appliedRecording_{};
    host::AutomationPlayback automationPlayback_{};
    struct Automatch {
        bool pending = false, active = false;
        double from = 0, to = 0;
        std::uint32_t elapsed = 0, duration = 0;
        host::ParameterTime identity{};
        std::int64_t nextFrame = 0;
        // Frozen while the immutable input list is consumed, even on re-grab.
        double runFrom = 0, runTo = 0;
        std::uint32_t runElapsed = 0, runDuration = 0, spanStart = UINT32_MAX;
    } automatch_;
    bool recordingTouch_ = false, recordingLatched_ = false;
    struct RecordingFeedback {
        bool followingRead = true, endPending = false, valueKnown = false;
        double value = 0;
    } recordingFeedback_;
    host::ParameterTime outputTime_{};
    [[nodiscard]] clap_event_transport_t transportContext() const noexcept;
    class OutputScope {
        Instance& owner_;
    public:
        OutputScope(Instance& owner, std::uint32_t frames, host::ParameterTime time = {}) noexcept : owner_(owner) {
            owner_.outputFrames_ = frames; owner_.lastOutputTime_ = 0;
            owner_.feedbackInput_ = 0; owner_.acceptingOutput_ = true;
            owner_.outputTime_ = time;
        }
        ~OutputScope() {
            owner_.advanceFeedbackInputs(UINT32_MAX);
            owner_.acceptingOutput_ = false;
        }
    };

    ProbeHost host_;
    host::PluginInfo info_;
    const clap_plugin_t* plugin_ = nullptr;
    const clap_plugin_params_t* params_ = nullptr;
    const clap_plugin_latency_t* latencyExt_ = nullptr;
    const clap_plugin_state_t* state_ = nullptr;
    const clap_plugin_gui_t* gui_ = nullptr;
    bool editorOpen_ = false;
    const clap_plugin_state_context_t* stateContext_ = nullptr;

    std::vector<host::ControlPort> controls_;
    std::vector<clap_id> paramIds_;
    std::vector<std::pair<std::uint32_t, std::size_t>> controlSlots_;
    static_assert(std::atomic<double>::is_always_lock_free);
    std::vector<std::atomic<double>> values_;
    // Separate delivery values: native output must never overwrite a host edit
    // waiting for its next process/flush, even when the displayed cache changes.
    std::vector<std::atomic<double>> pendingValues_;
    std::vector<std::atomic<std::uint64_t>> pendingVersions_;
    std::vector<std::atomic<bool>> dirty_;
    std::vector<std::uint32_t> lastInputTimes_;
    std::vector<clap_event_param_value_t> events_;
    std::vector<std::uint8_t> playbackEvents_, returnEvents_;
    std::vector<std::uint32_t> order_;
    std::uint32_t eventCount_ = 0;
    std::uint32_t delivered_ = 0;
    bool sorted_ = true;
    clap_input_events_t inEvents_{};
    clap_output_events_t outEvents_{};

    std::vector<float*> mainIn_, sidechainIn_, out_;
    std::vector<float> silence_, sink_;
    std::vector<const float*> inPtrs_, keyPtrs_;
    std::vector<float*> outPtrs_;
    // One buffer per port the plugin declares, in its own port order: a plugin
    // indexes them by that count, not by the count the host happens to pass.
    // The first main pair and the first key input are wired; any further port
    // reads silence or writes to scratch.
    enum class PortRole : std::uint8_t { Main, Key, Spare };
    struct AudioPort { std::uint32_t channels = 0; PortRole role = PortRole::Spare; };
    std::vector<AudioPort> inPorts_, outPorts_;
    std::vector<clap_audio_buffer_t> inBuffers_, outBuffers_;
    std::vector<std::vector<float*>> inSpare_, outSpare_;
    void passThrough(std::uint32_t frames) noexcept;

    double sampleRate_ = 48000.0;
    std::uint32_t maxBlock_ = 0;
    bool active_ = false;
    bool processing_ = false;
    std::atomic<std::uint32_t> latency_{0};
    std::int64_t steadyTime_ = 0;
};

} // namespace omawave::clap
