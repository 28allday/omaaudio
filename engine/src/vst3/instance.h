#pragma once

#include "host/instance.h"
#include "engine/command_queue.h"
#include "component.h"
#include "bus_layout.h"
#include "parameters.h"
#include <pluginterfaces/vst/ivstprocesscontext.h>
#include <pluginterfaces/gui/iplugview.h>
#include <array>
#include <atomic>
#include <span>
#include <utility>

namespace omawave::engine { class AutomationRecorder; }
namespace omawave::vst3 {

struct InstanceOptions {
    double sampleRate = 48000;
    std::uint32_t maxBlock = 1024, channels = 2, sidechainChannels = 0;
    bool offline = false;
};

// Effect adapter registered in the common factory through explicit VST3 catalogue
// discovery. Application discovery remains gated until end-to-end integration.
// Create/configure/activate/deactivate/destroy on the actual process main thread.
// run executes on one exclusive processing thread. setControl, scheduleControl
// and setContext belong to that owner (or main before publication).
// Controller callbacks run on main and enter a bounded SPSC queue. They never
// call into the processor. controlValue, diagnostics and restart flags are atomic.
class Instance final : public host::Instance {
public:
    static std::unique_ptr<Instance> create(const std::string& bundle, const std::string& classId,
        const InstanceOptions&, std::string& error, const session::PluginState* state = nullptr);
    ~Instance() override;
    const std::string& uri() const noexcept override { return info_.uri; }
    const host::PluginInfo& info() const noexcept override { return info_; }
    const std::vector<host::ControlPort>& controls() const noexcept override { return controls_; }
    // Coherent metadata from preparation or an exclusive live refresh. Callers
    // must not retain references across refreshConfiguration().
    std::span<const Steinberg::Vst::ParameterInfo> parameters() const noexcept { return metadata_; }
    bool connectAudioIn(std::uint32_t, float*) noexcept override;
    bool connectMainAudioIn(std::uint32_t, float*) noexcept override;
    bool connectSidechainIn(std::uint32_t, float*) noexcept override;
    bool connectAudioOut(std::uint32_t, float*) noexcept override;
    std::uint32_t mainAudioInCount() const noexcept override { return options_.channels; }
    std::uint32_t sidechainInCount() const noexcept override { return options_.sidechainChannels; }
    std::uint32_t audioInCount() const noexcept override { return mainAudioInCount() + sidechainInCount(); }
    std::uint32_t audioOutCount() const noexcept override { return options_.channels; }
    bool ready() const noexcept override;
    void activate() noexcept override;
    void deactivate() noexcept override;
    bool active() const noexcept override { return active_; }
    void run(std::uint32_t) noexcept override;
    void setControl(std::uint32_t, float) noexcept override;
    bool scheduleControl(std::uint32_t, float, std::uint32_t) noexcept override;
    void setParameter(std::uint32_t, double) noexcept override;
    void setParameterAtVersion(std::uint32_t, double, std::uint64_t) noexcept override;
    std::uint64_t parameterVersion() const noexcept override { return valueVersion_.load(std::memory_order_acquire); }
    bool scheduleParameter(std::uint32_t, double, std::uint32_t) noexcept override;
    bool processParameterEvents() const noexcept override { return true; }
    bool processAutomationSpans() const noexcept override { return true; }
    void setAutomationRecording(const host::AutomationRecording&) noexcept override;
    void setAutomationPlayback(const host::AutomationPlayback& playback) noexcept override { automationPlayback_ = playback; }
    bool automationPlaybackAllowed(std::uint32_t id) const noexcept override {
        return !recording_.active() || id != recording_.control;
    }
    double parameterValue(std::uint32_t id) const noexcept override { return normalizedValue(id); }
    host::Timing timing() const noexcept override;
    float controlValue(std::uint32_t) const noexcept override;
    // Preserve SDK double precision, also exposed by common parameterValue().
    double normalizedValue(std::uint32_t) const noexcept;
    // Main, safe alongside processing: format the supplied normalized value and
    // current units without setting/flushing a parameter or synchronizing state.
    std::string parameterText(std::uint32_t, double) const override;
    bool takeParameterTextChanged() noexcept override;
    std::uint32_t latencyFrames() const noexcept override { return latency_; }
    std::uint32_t tailFrames() const noexcept { return tail_; }
    bool restartRequested() const noexcept override { return restartFlags() != 0; }
    std::uint32_t restartFlags() const noexcept { return restart_.load(std::memory_order_relaxed); }
    bool canRefreshConfiguration() const noexcept override;
    bool refreshConfiguration(std::string& error) override;

    // Unpublished, exclusively owned. Main calls use one joined processing
    // thread; offline workers run directly. Connected audio/context are preserved.
    void warmUp(std::uint32_t blocks, std::uint32_t frames) noexcept override;
    bool warmUpNeedsProcessingThread() const noexcept override { return true; }
    bool beginWarmUp() noexcept override;
    bool warmUpBlock(std::uint32_t frames) noexcept override;
    bool endWarmUp() noexcept override;

    // Processing owner: this describes the NEXT block. Sample rate always comes
    // from setup. No tempo/signature is invented when the caller has no context.
    // Host integration must supply actual transport positions at seeks/loops.
    // System clock, bar position and MIDI-clock distance expire after one block.
    bool setContext(const Steinberg::Vst::ProcessContext&) noexcept;
    void setProcessContext(const host::ProcessContext&) noexcept override;

    enum Diagnostic : std::uint32_t {
        InvalidBlock = 1, InvalidControl = 2, InputOverflow = 4, OutputOverflow = 8,
        ProcessFailed = 16, ActivationFailed = 32, WrongThread = 64,
        WarmupFailed = 128, InvalidContext = 256, NonfiniteAudio = 512,
    };
    std::uint32_t diagnostics() const noexcept { return diagnostics_.load(std::memory_order_relaxed); }
    enum class EditKind { Begin, Value, End };
    struct Edit { EditKind kind; Steinberg::Vst::ParamID id; double value; }; // value is significant for Value only
    using EditListener = void (*)(void*, const Edit&) noexcept;
    // Main only. Optional concrete observer; common notifications remain available
    // independently for model/history consumers.
    void setEditListener(EditListener listener, void* context) noexcept;
    // Main, safe alongside processing. Pending native edits/invalidation refuse
    // stale synchronization. Controller calls never occur inside run().
    bool syncController() noexcept;
    bool serviceMainThread() noexcept override { return syncController(); }
    bool parameterFlushRequested() const noexcept override;
    // Main, processor AND parameter producers quiescent. Reads invalidated
    // controller values before delivering a bounded zero-frame processing call.
    bool flushParameterChanges(std::string& error) override;
    std::optional<host::UnrenderedParameter> unrenderedParameter(std::uint32_t) const noexcept override;
    host::ParameterTime nextInputTime() const noexcept override;
    bool hasOpaqueState() const noexcept override { return true; }
    // Main, processor and parameter producers quiescent. Active pending edits
    // are flushed with zero audio frames on a joined processing thread. Inactive
    // pending edits and scheduled future automation require explicit completion.
    std::optional<session::PluginState> captureState(std::string& error) override;
    Steinberg::Vst::IComponentHandler* componentHandler() const noexcept;
    // Main. The controller's own editor in an X11 window (kPlatformTypeX11EmbedWindowID).
    bool hasEditor() const noexcept override;
    bool openEditor(std::uint64_t parent, host::EditorHost&, host::EditorSize&, std::string& error) override;
    bool resizeEditor(std::uint32_t& width, std::uint32_t& height) noexcept override;
    void closeEditor() noexcept override;
    bool editorOpen() const noexcept override { return view_ != nullptr; }
    bool nextParameterEvent(host::ParameterEvent&) noexcept override;
    bool takeStateDirty() noexcept override;
    host::StateChanges takeStateChanges() noexcept override;
    bool takeNotificationOverflow() noexcept override;
    std::uint32_t notificationDiagnostics() const noexcept override;

private:
    class Handler;
    class Frame;
    Steinberg::IPtr<Steinberg::IPlugView> view_;
    Steinberg::IPtr<Frame> frame_;
    mutable int editorAvailable_ = -1;     // -1 unknown, 0 none, 1 X11 editor
    Instance();
    void prepare(const std::shared_ptr<Module>&, const std::string&, const session::PluginState*);
    void prepareBuses(bool acknowledge = true);
    void prepareParameters();
    void refreshParameterMetadata();
    void refreshBusTitles();
    std::size_t slot(std::uint32_t) const noexcept;
    bool writable(std::size_t) const noexcept;
    double normalise(std::size_t, double) const noexcept;
    Steinberg::tresult edit(EditKind, Steinberg::Vst::ParamID, double) noexcept;
    Steinberg::tresult groupEdit(bool begin) noexcept;
    bool notificationOwner() noexcept;
    bool refreshControllerValues(std::string& error);
    void notify(const host::ParameterEvent&, bool audio) noexcept;
    void notificationLost() noexcept;
    void diagnose(Diagnostic flag) noexcept { diagnostics_.fetch_or(flag, std::memory_order_relaxed); }
    void copyInputs(std::uint32_t) noexcept;
    void copyOutputs(std::uint32_t, bool dry) noexcept;
    void advanceContext(std::uint32_t) noexcept;

    struct Bus : AudioBusDescription {
        explicit Bus(AudioBusDescription description) : AudioBusDescription(std::move(description)) {}
        std::vector<float*> pointers;
        bool enabled = false;
    };
    struct Change { std::uint32_t id = 0, offset = 0; double value = 0; std::uint64_t version = 0, order = 0; };
    struct ControllerEdit { Change change{}; EditKind kind{}; std::uint64_t revision = 0; };
    static constexpr std::uint32_t kEditCapacity = 1024;
    static constexpr std::uint32_t kControllerCapacity = 8192;
    InstanceOptions options_;
    host::PluginInfo info_;
    Steinberg::IPtr<Handler> handler_;
    std::unique_ptr<Component> component_;
    std::vector<Bus> inputs_, outputs_;
    std::vector<Steinberg::Vst::AudioBusBuffers> inputBuffers_, outputBuffers_;
    std::vector<float> scratch_, dry_;
    std::array<float*, 2> main_{}, key_{}, out_{};
    std::size_t mainInput_ = 0, mainOutput_ = 0, keyInput_ = 0;

    std::vector<Steinberg::Vst::ParameterInfo> metadata_;
    std::vector<host::ControlPort> controls_;
    std::vector<std::pair<std::uint32_t, std::size_t>> slots_;
    std::vector<double> held_, pending_, refreshed_;
    std::vector<std::uint64_t> pendingVersions_, appliedVersions_;
    std::vector<std::uint64_t> pendingOrders_, appliedOrders_;
    std::vector<std::uint64_t> pendingAutomationOrders_;
    std::vector<std::uint64_t> appliedAutomationOrders_, renderedAutomationOrders_;
    std::vector<double> syncValues_;
    std::vector<std::uint64_t> syncOrders_;
    // 0 clean, 1 explicit host command, 2 initial/refreshed native baseline.
    std::vector<std::uint8_t> dirty_;
    std::unique_ptr<std::atomic<double>[]> values_;
    std::unique_ptr<std::atomic<std::uint64_t>[]> publishedOrders_;
    // Main takes one bounded snapshot attempt; audio never waits for it.
    std::atomic<std::uint64_t> publication_{0}, nextControlOrder_{0};
    std::vector<Change> scheduled_;
    std::uint32_t scheduledCount_ = 0;
    std::unique_ptr<ParameterInput> inChanges_;
    std::unique_ptr<ParameterOutput> outChanges_;
    // Values and boundaries share one publication so a short gesture cannot
    // lose its final value or release. The original 1024-value quota is retained.
    std::array<ControllerEdit, kControllerCapacity> edits_{};
    std::atomic<std::uint32_t> controllerRead_{0}, controllerWrite_{0};
    std::atomic<std::uint64_t> controllerLoss_{0};
    std::atomic<std::uint32_t> editRead_{0}, editWrite_{0};
    // Queue space can be released before process() returns. UI synchronization
    // waits for the consumed edit's values to be published, not just dequeued.
    std::atomic<std::uint32_t> editPublished_{0};
    inline static std::atomic<std::uint64_t> nextValueVersion_{0};
    // Global epochs make invalidations comparable across a dual-mono insert.
    std::atomic<std::uint64_t> valueVersion_{0};
    std::atomic<bool> refreshRequested_{false};
    std::atomic<std::uint32_t> diagnostics_{0}, restart_{0};
    // Distinct producers: controller callbacks/main and process feedback/audio.
    // Both are consumed only on main. No cross-producer total order is implied.
    engine::CommandQueue<host::ParameterEvent, 2048> controllerNotifications_, audioNotifications_;
    std::atomic<std::uint32_t> notificationDiagnostics_{0};
    enum EditSource : unsigned { ParameterEdit = 1, OtherEdit = 2 };
    std::atomic<unsigned> stateDirty_{0};
    std::atomic<bool> notificationOverflow_{false};
    std::atomic<bool> readoutStatePending_{false};
    std::uint64_t nextGroup_ = 0, group_ = 0;
    host::ParameterTime groupTime_{};
    bool preferAudioNotification_ = false;
    bool preferRecordingNotification_ = false;
    struct NativeGesture {
        double value = 0;
        std::uint64_t revision = 0, version = 0;
        std::uint32_t implicitFrames = 0;
        bool active = false, pending = false;
    };
    std::vector<NativeGesture> nativeGestures_; // processing owner
    std::unique_ptr<engine::AutomationRecorder> automationRecorder_;
    host::AutomationRecording recording_{}, appliedRecording_{};
    host::AutomationPlayback automationPlayback_{}, warmUpAutomationPlayback_{};
    host::ProcessContext processContext_{}, warmUpProcessContext_{};
    std::uint64_t recordingRevision_ = 0, recordingOrder_ = 0;
    std::uint64_t appliedControllerLoss_ = 0, blockedRecordingPass_ = 0, recordingVersion_ = 0;
    double recordingValue_ = 0;
    Steinberg::Vst::ProcessContext context_{};
    Steinberg::Vst::ProcessContext warmUpContext_{};
    std::uint32_t latency_ = 0, tail_ = 0;
    bool active_ = false, processing_ = false, failed_ = false, warming_ = false;
    bool syncing_ = false, settingController_ = false, refreshing_ = false, capturing_ = false;
    // Control owner; reconfiguring_ is also read by the joined warm-up/flush
    // worker and remains unchanged until that worker finishes.
    bool reconfiguring_ = false, resumeAfterReconfigure_ = false;
    mutable bool formatting_ = false; // main only; reject nested controller/state entry
    bool textChanged_ = false;
    std::uint64_t textRevision_ = 0; // main only; detect reentrant presentation changes
    bool refreshFromSync_ = false;
    std::uint64_t syncOrder_ = 0, refreshOrder_ = 0;
    EditListener listener_ = nullptr;
    void* listenerContext_ = nullptr;
};
}
