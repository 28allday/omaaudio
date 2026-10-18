#pragma once

#include <lv2/atom/forge.h>

#include "host/instance.h"

#include "lv2/host_features.h"
#include "lv2/worker.h"
#include "lv2/world.h"

#include <lv2/worker/worker.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct LilvInstanceImpl;
typedef struct LilvInstanceImpl LilvInstance;

namespace omawave::lv2 {

// One control port, as far as running the plugin needs to know.
//
// Display metadata -- units, scale points, port groups, logarithmic scaling -- is what
// the generic UI needs and arrives with it. What is here is what correctness needs:
// the range to clamp to, and whether the value is a switch or a whole number.
using ControlPort = host::ControlPort;

// An instantiated LV2 plugin, with every one of its ports connected.
//
// THREADING, and it is the whole design:
//
//   * create, activate, deactivate, destruction and state are the instantiation class.
//     Main or worker thread only, never while the audio thread is running this
//     instance.
//   * run() and setControl() are the audio class. They allocate nothing, lock nothing
//     and take no branch that touches the heap.
//
// Audio buffers are NOT owned here. The chain hands in pointers to its own scratch and
// they must stay valid until they are replaced -- one more reason instances are
// published to the audio thread by the same generation handshake as the render plan.
class Instance final : public host::Instance, public WorkerClient {
public:
    // Null on failure, with `error` set to something a person can act on. Refuses
    // rather than half-builds: a plugin missing a feature, or with a port type the host
    // cannot connect, is never handed back in a state where run() would be undefined.
    [[nodiscard]] static std::unique_ptr<Instance> create(const World& world,
                                                          const std::string& uri,
                                                          HostFeatures& features,
                                                          std::string& error);

    ~Instance() override;

    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;
    Instance(Instance&&) = delete;
    Instance& operator=(Instance&&) = delete;

    [[nodiscard]] const std::string& uri() const noexcept override { return uri_; }
    [[nodiscard]] const PluginInfo& info() const noexcept override { return info_; }
    [[nodiscard]] const std::vector<ControlPort>& controls() const noexcept override
    {
        return controls_;
    }

    // Main thread. `channel` is a position within the plugin's own audio ports, in
    // port order. A null buffer is only legal for a port the plugin marked optional.
    bool connectAudioIn(std::uint32_t channel, float* buffer) noexcept override;
    bool connectAudioOut(std::uint32_t channel, float* buffer) noexcept override;
    bool connectMainAudioIn(std::uint32_t channel, float* buffer) noexcept override;
    bool connectSidechainIn(std::uint32_t channel, float* buffer) noexcept override;
    [[nodiscard]] std::uint32_t mainAudioInCount() const noexcept override { return static_cast<std::uint32_t>(mainAudioIn_.size()); }
    [[nodiscard]] std::uint32_t sidechainInCount() const noexcept override { return static_cast<std::uint32_t>(sidechainIn_.size()); }

    [[nodiscard]] std::uint32_t audioInCount() const noexcept override
    {
        return static_cast<std::uint32_t>(audioIn_.size());
    }
    [[nodiscard]] std::uint32_t audioOutCount() const noexcept override
    {
        return static_cast<std::uint32_t>(audioOut_.size());
    }

    // True when every port that must be connected has been. The chain checks this
    // before it will run an instance, because the failure mode otherwise is a segfault
    // inside somebody else's code.
    [[nodiscard]] bool ready() const noexcept override;

    void activate() noexcept override;
    void deactivate() noexcept override;
    [[nodiscard]] bool active() const noexcept override { return active_; }

    // Audio thread. `frames` must not exceed the maxBlock the features were built with.
    void run(std::uint32_t frames) noexcept override;

    // Main thread, after the audio buffers are connected and before the instance is
    // published to the audio thread.
    //
    // Several plugins allocate on their first few runs and then never again -- measured
    // across the 253 usable plugins installed here, 28 of them do, all LSP, and every
    // one settles to zero within the first hundred blocks. That is lazy setup rather
    // than a leak, and it is entirely legal: LV2 only forbids allocation in run() for
    // plugins declaring hardRTCapable. Running those blocks HERE, on a thread where
    // allocation is allowed, is what keeps them out of the callback.
    void warmUp(std::uint32_t blocks, std::uint32_t frames) noexcept override;

    // Audio thread. Addressed by port index, clamped to the port's range. Silently
    // ignores an index that is not an input control, because a stale command from the
    // UI must not be able to write somewhere arbitrary.
    void setControl(std::uint32_t portIndex, float value) noexcept override;
    // A control port is one value per run(): points are exact because the host
    // splits runs at them, ramps step every 64 frames (engine/automation.cpp).
    [[nodiscard]] host::Timing timing() const noexcept override { return {false, 64, 0}; }

    // Main thread. What the model believes, not what the instance holds -- see the
    // note in the .cpp about who owns a control value.
    [[nodiscard]] float controlValue(std::uint32_t portIndex) const noexcept override;
    // Any thread: an output control's value as the last run() left it.
    [[nodiscard]] float outputValue(std::uint32_t portIndex) const noexcept override;

    // Frames of delay this plugin adds, from its lv2:latency-designated output port.
    // Zero when it declares none. Read after run(), so it reflects the current setup.
    [[nodiscard]] std::uint32_t latencyFrames() const noexcept override;

    // True when the plugin exposes the worker interface -- it will call
    // schedule_work() from run() and expects work()/work_response() in return.
    [[nodiscard]] bool wantsWorker() const noexcept override { return workerIface_ != nullptr; }

    // Main thread, before the instance is published and before any run() that could
    // schedule. Without one, scheduled work runs synchronously on the calling thread,
    // which is correct for a standalone instance (warm-up, port probing, offline
    // render) and never what the audio thread gets.
    void setWorker(Worker* worker) noexcept override { worker_ = worker; }
    void setProcessContext(const host::ProcessContext& context) noexcept override { context_ = context; }

    // WorkerClient. performWork is the worker thread calling the plugin's work();
    // deliverResponse is the run() context calling its work_response(). Neither is
    // for anyone but Worker and run() to call.
    void performWork(std::uint32_t size, const void* data) noexcept override;
    void deliverResponse(std::uint32_t size, const void* data) noexcept override;

private:
    Instance() = default;

    std::string uri_;
    PluginInfo info_;
    LilvInstance* instance_ = nullptr;
    bool active_ = false;

    // Port buffers. Sized once at construction and never resized, because the audio
    // thread holds pointers into them: a reallocation is a use-after-free in the
    // callback, which is trap 15 all over again.
    std::vector<float> controlValues_;      // one slot per port, control ports only
    // Output controls, copied after each run() so a meter can read them from the UI
    // thread without racing the plugin's own writes. Sized once, like the buffers.
    std::vector<std::uint32_t> outputControls_;
    std::unique_ptr<std::atomic<float>[]> outputMirror_;
    std::vector<std::vector<std::uint8_t>> atomBuffers_;

    // The atom ports, gathered so run() does not have to walk every port to find them.
    // Which way a buffer is reset depends on its direction, so the direction is kept
    // here rather than re-derived.
    struct AtomPort {
        std::uint32_t index = 0;
        std::uint32_t capacity = 0;
        bool input = false;
        bool timePosition = false;      // atom:supports time:Position
    };
    // Transport for plug-ins that follow it. Audio thread only after construction.
    [[nodiscard]] bool positionDue(std::uint32_t frames) noexcept;
    void writePosition(std::uint8_t* buffer, std::uint32_t capacity) noexcept;
    host::ProcessContext context_{};
    LV2_Atom_Forge forge_{};
    struct TimeUrids { LV2_URID position = 0, frame = 0, speed = 0, bar = 0, barBeat = 0,
                       beatUnit = 0, beatsPerBar = 0, beatsPerMinute = 0; } time_{};
    double sampleRate_ = 0, sentTempo_ = 0;
    std::uint16_t sentNumerator_ = 0, sentDenominator_ = 0;
    std::int64_t expectedFrame_ = 0;
    bool positionSent_ = false, sentPlaying_ = false;
    std::vector<AtomPort> atomPorts_;

    std::vector<ControlPort> controls_;
    std::vector<std::uint32_t> audioIn_;    // port indices, in port order
    std::vector<std::uint32_t> mainAudioIn_, sidechainIn_; // indices into audioIn_
    std::vector<std::uint32_t> audioOut_;
    std::vector<bool> connected_;           // per port
    std::vector<bool> optional_;            // per port

    static LV2_Worker_Status scheduleWork(LV2_Worker_Schedule_Handle handle,
                                          std::uint32_t size, const void* data);
    static LV2_Worker_Status respondTrampoline(LV2_Worker_Respond_Handle handle,
                                               std::uint32_t size, const void* data);

    // The schedule feature is per instance -- its handle is this object -- so the
    // shared feature list is copied and extended per instance. The struct addresses
    // handed to the plugin stay valid because Instance is neither copyable nor
    // movable.
    const LV2_Worker_Interface* workerIface_ = nullptr;
    Worker* worker_ = nullptr;
    LV2_Worker_Schedule schedule_{};
    LV2_Feature scheduleFeature_{};
    std::vector<const LV2_Feature*> featureArray_;

    // Responses collected in synchronous mode (no Worker attached), delivered at the
    // top of the next run(). Only ever touched off the audio thread: an instance
    // without a Worker is never published to it.
    std::vector<std::vector<std::uint8_t>> syncResponses_;

    std::uint32_t portCount_ = 0;
    std::uint32_t latencyPort_ = kNoPort;
    std::uint32_t sequenceCapacity_ = 0;
    LV2_URID atomSequenceUrid_ = 0;
    LV2_URID atomChunkUrid_ = 0;

    static constexpr std::uint32_t kNoPort = 0xFFFFFFFFu;
};

} // namespace omawave::lv2
