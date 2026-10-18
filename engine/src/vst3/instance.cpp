#include "instance.h"
#include "host_context.h"
#include "engine/automation.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>

namespace omawave::vst3 {
using namespace Steinberg;
using namespace Steinberg::Vst;
namespace {
void require(bool valid, const char* message) { if (!valid) throw std::runtime_error(message); }
bool normalized(double value) noexcept { return std::isfinite(value) && value >= 0 && value <= 1; }
bool processingNotificationAccepted(tresult result) noexcept {
    // The SDK AudioEffect base implements this optional notification with
    // kNotImplemented. Actual processing errors still fail the instance.
    return result == kResultOk || result == kNotImplemented;
}
template<std::size_t N> bool validText(const char16 (&text)[N]) noexcept {
    const auto end = std::find(std::begin(text), std::end(text), char16{});
    if (end == std::end(text)) return false;
    for (auto it = std::begin(text); it != end; ++it) {
        if (*it >= 0xD800 && *it <= 0xDBFF) {
            if (++it == end || *it < 0xDC00 || *it > 0xDFFF) return false;
        } else if (*it >= 0xDC00 && *it <= 0xDFFF) return false;
    }
    return true;
}
struct FlagScope {
    bool& flag;
    explicit FlagScope(bool& value) : flag(value) { flag = true; }
    ~FlagScope() { flag = false; }
};
bool hasCategory(const std::string& categories, std::string_view name) {
    std::size_t begin = 0;
    while (begin < categories.size()) {
        auto end = categories.find('|', begin);
        if (end == std::string::npos) end = categories.size();
        if (std::string_view(categories).substr(begin, end - begin) == name) return true;
        begin = end + 1;
    }
    return false;
}
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<double>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
}

class Instance::Handler final : public IComponentHandler, public IComponentHandler2 {
public:
    explicit Handler(Instance* owner) : owner(owner) {}
    Instance* owner;
    tresult PLUGIN_API queryInterface(const TUID id, void** out) override {
        if (!out) return kInvalidArgument;
        *out = nullptr;
        if (!id) return kNoInterface;
        if (!std::memcmp(id, IComponentHandler::iid.toTUID(), 16)
            || !std::memcmp(id, FUnknown::iid.toTUID(), 16)) *out = static_cast<IComponentHandler*>(this);
        else if (!std::memcmp(id, IComponentHandler2::iid.toTUID(), 16)) *out = static_cast<IComponentHandler2*>(this);
        else return kNoInterface;
        addRef(); return kResultOk;
    }
    uint32 PLUGIN_API addRef() override { return refs_.fetch_add(1, std::memory_order_relaxed) + 1; }
    uint32 PLUGIN_API release() override {
        const auto left = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (!left) delete this;
        return left;
    }
    tresult PLUGIN_API beginEdit(ParamID id) override { return owner ? owner->edit(EditKind::Begin, id, 0) : kResultFalse; }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue value) override { return owner ? owner->edit(EditKind::Value, id, value) : kResultFalse; }
    tresult PLUGIN_API endEdit(ParamID id) override { return owner ? owner->edit(EditKind::End, id, 0) : kResultFalse; }
    tresult PLUGIN_API restartComponent(int32 flags) override {
        if (!owner || flags < 0) return kInvalidArgument;
        if (!owner->notificationOwner()) return kResultFalse;
        constexpr int32 supported = kReloadComponent | kIoChanged | kParamValuesChanged
            | kLatencyChanged | kParamTitlesChanged | kIoTitlesChanged;
        // Unsupported requests must not claim success or partially change state.
        if (flags & ~supported) return kNotImplemented;
        if (flags & kParamTitlesChanged) {
            owner->textChanged_ = true; ++owner->textRevision_;
        }
        if (flags & kParamValuesChanged) {
            owner->refreshFromSync_ = owner->settingController_;
            if (owner->refreshFromSync_) owner->refreshOrder_ = owner->syncOrder_;
            else {
                owner->valueVersion_.store(nextValueVersion_.fetch_add(1, std::memory_order_relaxed) + 1,
                    std::memory_order_release);
                owner->refreshOrder_ = owner->nextControlOrder_.fetch_add(1, std::memory_order_relaxed) + 1;
            }
            owner->refreshRequested_.store(true, std::memory_order_release);
            owner->stateDirty_.fetch_or(OtherEdit, std::memory_order_release);
        }
        owner->restart_.fetch_or(static_cast<std::uint32_t>(flags & ~kParamValuesChanged), std::memory_order_relaxed);
        return kResultOk;
    }
    tresult PLUGIN_API setDirty(TBool state) override {
        if (!owner || !owner->notificationOwner()) return kResultFalse;
        // False cannot acknowledge another edit or mark the host project clean.
        if (state && !owner->settingController_) owner->stateDirty_.fetch_or(OtherEdit, std::memory_order_release);
        return kResultOk;
    }
    tresult PLUGIN_API requestOpenEditor(FIDString) override {
        if (!owner || !owner->notificationOwner()) return kResultFalse;
        return kNotImplemented;
    }
    tresult PLUGIN_API startGroupEdit() override { return owner ? owner->groupEdit(true) : kResultFalse; }
    tresult PLUGIN_API finishGroupEdit() override { return owner ? owner->groupEdit(false) : kResultFalse; }
private:
    std::atomic<uint32> refs_{1};
};

Instance::Instance() = default;
std::unique_ptr<Instance> Instance::create(const std::string& bundle, const std::string& id,
    const InstanceOptions& options, std::string& error, const session::PluginState* state)
{
    error.clear();
    if (!onMainThread()) { error = "VST3 creation requires the process main thread"; return {}; }
    try {
        require(std::isfinite(options.sampleRate) && options.sampleRate >= 1000 && options.sampleRate <= 768000
            && options.maxBlock && options.maxBlock <= 65536 && (options.channels == 1 || options.channels == 2)
            && options.sidechainChannels <= 2, "Invalid VST3 processing configuration");
        auto module = Module::open(bundle, error);
        if (!module) return {};
        auto result = std::unique_ptr<Instance>(new Instance);
        result->options_ = options;
        result->prepare(module, id, state);
        return result;
    } catch (const std::exception& exception) { error = exception.what(); return {}; }
      catch (...) { error = "exception while preparing VST3 effect"; return {}; }
}

void Instance::prepare(const std::shared_ptr<Module>& module, const std::string& id, const session::PluginState* state)
{
    FUID parsed;
    require(parseClassId(id, parsed), "Invalid VST3 class identity");
    const auto canonical = classIdString(parsed.toTUID());
    const auto found = std::find_if(module->classes().begin(), module->classes().end(),
        [&](const auto& item) { return item.id == canonical && item.category == kVstAudioEffectClass; });
    require(found != module->classes().end(), "VST3 effect class is absent from its module");
    require(!hasCategory(found->subcategories, PlugType::kOnlyARA)
        && !hasCategory(found->subcategories, PlugType::kInstrument), "VST3 class requires an unsupported instrument or ARA host");
    require(!options_.offline || (!hasCategory(found->subcategories, PlugType::kNoOfflineProcess)
        && !hasCategory(found->subcategories, PlugType::kOnlyRealTime)), "VST3 effect does not support offline processing");
    require(options_.offline || !hasCategory(found->subcategories, PlugType::kOnlyOfflineProcess),
        "VST3 effect requires offline processing");
    info_.format = host::Format::Vst3; info_.uri = "vst3:" + canonical; info_.path = module->path();
    info_.name = found->name; info_.author = found->vendor; info_.className = found->subcategories;
    require(!state || (*state && state->uri() == info_.uri), "VST3 state belongs to a different plugin");
    handler_ = owned(new Handler(this));
    std::string error;
    component_ = Component::create(module, canonical, module->context(), handler_, error);
    if (!component_) throw std::runtime_error(error);
    if (state && !component_->restore(state->processor(), state->controller(), error))
        throw std::runtime_error(error);
    require(component_->processor()->canProcessSampleSize(kSample32) == kResultOk,
        "VST3 effect does not support 32-bit audio");
    prepareBuses();
    prepareParameters();
    info_.requiredIn = info_.totalIn = info_.requiredOut = info_.totalOut = options_.channels;
    info_.sidechainIn = options_.sidechainChannels;
    info_.layout = options_.channels == 1 ? host::Layout::Mono : host::Layout::Stereo;
    info_.hasStateInterface = true; info_.usable = true;
    context_.sampleRate = options_.sampleRate;
    context_.state = ProcessContext::kContTimeValid;
}

void Instance::prepareBuses(bool acknowledge)
{
    // This preparation satisfies earlier bus requests. A request made during
    // negotiation/setup is newer and must survive for the graph owner to handle.
    if (acknowledge) restart_.fetch_and(~std::uint32_t(kIoChanged | kIoTitlesChanged), std::memory_order_relaxed);
    auto* component = component_->component(); auto* processor = component_->processor();
    auto layout = negotiateAudioLayout(component, processor, options_.channels, options_.sidechainChannels);
    std::vector<Bus> inputs, outputs;
    std::vector<AudioBusBuffers> inputBuffers, outputBuffers;
    std::vector<float> scratch;
    const auto adopt = [](auto& target, auto& source) {
        target.reserve(source.size());
        for (auto& bus : source) target.emplace_back(std::move(bus));
    };
    adopt(inputs, layout.inputs); adopt(outputs, layout.outputs);
    ProcessSetup setup{options_.offline ? kOffline : kRealtime, kSample32,
        static_cast<int32>(options_.maxBlock), options_.sampleRate};
    require(processor->setupProcessing(setup) == kResultOk, "VST3 processing setup failed");
    for (const auto direction : {kInput, kOutput}) {
        auto& list = direction == kInput ? inputs : outputs;
        for (std::size_t i = 0; i < list.size(); ++i) {
            require(component->activateBus(kAudio, direction, i, list[i].selected) == kResultOk,
                "VST3 audio bus activation failed");
            list[i].enabled = list[i].selected;
        }
        const auto count = component->getBusCount(kEvent, direction);
        require(count >= 0 && count <= 64, "Invalid VST3 event bus count");
        for (int32 i = 0; i < count; ++i)
            require(component->activateBus(kEvent, direction, i, false) == kResultOk,
                "VST3 effect requires unsupported event buses");
    }
    std::size_t channels = 0;
    for (const auto& bus : inputs) channels += bus.info.channelCount;
    for (const auto& bus : outputs) channels += bus.info.channelCount;
    require(channels * options_.maxBlock <= 16 * 1024 * 1024, "VST3 bus scratch exceeds the 64 MiB limit");
    scratch.resize(channels * options_.maxBlock);
    dry_.resize(std::size_t{options_.channels} * options_.maxBlock);
    std::size_t position = 0;
    const auto buffers = [&](auto& list, auto& buffers) {
        buffers.resize(list.size());
        for (std::size_t i = 0; i < list.size(); ++i) {
            auto& bus = list[i]; bus.pointers.resize(bus.info.channelCount);
            for (auto& ptr : bus.pointers) { ptr = scratch.data() + position; position += options_.maxBlock; }
            buffers[i].numChannels = bus.info.channelCount;
            buffers[i].channelBuffers32 = bus.pointers.data();
        }
    };
    buffers(inputs, inputBuffers); buffers(outputs, outputBuffers);
    inputs_ = std::move(inputs); outputs_ = std::move(outputs);
    inputBuffers_ = std::move(inputBuffers); outputBuffers_ = std::move(outputBuffers);
    scratch_ = std::move(scratch);
    mainInput_ = layout.mainInput; mainOutput_ = layout.mainOutput; keyInput_ = layout.keyInput;
}

void Instance::prepareParameters()
{
    auto* controller = component_->controller();
    // Initialization/state restore can invalidate metadata and values. Nothing
    // is published yet: take one coherent bounded snapshot before allocating
    // processing queues. Later live requests remain the graph owner's work.
    bool coherent = false;
    for (unsigned attempt = 0; attempt < 3 && !coherent; ++attempt) {
        const auto titles = textRevision_, values = parameterVersion();
        const auto count = controller ? controller->getParameterCount() : 0;
        require(count >= 0 && count <= 16384, "Invalid VST3 parameter count");
        std::vector<ParameterInfo> metadata(count);
        std::vector<double> initial(count);
        for (int32 i = 0; i < count; ++i) {
            auto& p = metadata[i];
            require(controller->getParameterInfo(i, p) == kResultOk && p.id <= kMaxParamId && p.stepCount >= 0
                && normalized(p.defaultNormalizedValue) && validText(p.title) && validText(p.shortTitle)
                && validText(p.units), "Invalid VST3 parameter metadata");
            initial[i] = controller->getParamNormalized(p.id);
            require(normalized(initial[i]), "Invalid VST3 controller parameter value");
        }
        const auto finalCount = controller ? controller->getParameterCount() : 0;
        if (finalCount != count || titles != textRevision_ || values != parameterVersion()) continue;
        metadata_ = std::move(metadata); held_ = std::move(initial); coherent = true;
    }
    require(coherent, "VST3 parameter metadata or values changed repeatedly during preparation");
    const auto count = metadata_.size();
    nativeGestures_.resize(count);
    automationRecorder_ = std::make_unique<engine::AutomationRecorder>();
    pending_ = held_; dirty_.resize(count);
    refreshed_.resize(count); pendingVersions_.assign(count, parameterVersion()); appliedVersions_.resize(count);
    pendingOrders_.assign(count, refreshOrder_); appliedOrders_.resize(count); syncValues_.resize(count); syncOrders_.resize(count);
    pendingAutomationOrders_.resize(count);
    appliedAutomationOrders_.resize(count); renderedAutomationOrders_.resize(count);
    values_ = std::make_unique<std::atomic<double>[]>(count);
    publishedOrders_ = std::make_unique<std::atomic<std::uint64_t>[]>(count);
    std::vector<ParamID> ids;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& p = metadata_[i];
        values_[i].store(held_[i], std::memory_order_relaxed);
        dirty_[i] = writable(i) ? 2 : 0; ids.push_back(p.id); slots_.emplace_back(p.id, i);
        if (p.flags & ParameterInfo::kIsHidden) continue;
        host::ControlPort control;
        control.index = p.id; control.symbol = std::to_string(p.id); control.name = utf8(p.title, std::size(p.title));
        control.parameterId = p.id;
        control.defaultValue = p.defaultNormalizedValue;
        control.toggled = p.stepCount == 1; control.output = (p.flags & ParameterInfo::kIsReadOnly) != 0;
        control.automatable = (p.flags & ParameterInfo::kCanAutomate) != 0;
        // Discrete VST3 values remain normalized; marking them integer would
        // round all intermediate steps away in the common automation renderer.
        if (!control.output) ++info_.controlIn;
        controls_.push_back(std::move(control));
    }
    std::sort(slots_.begin(), slots_.end());
    for (std::size_t i = 1; i < slots_.size(); ++i)
        require(slots_[i - 1].first != slots_[i].first, "Duplicate VST3 parameter identity");
    scheduled_.resize(options_.maxBlock * 4 + count * 2 + 1);
    // One extra, exclusively reserved block holds sample-accurate return points.
    inChanges_ = std::make_unique<ParameterInput>(ids, scheduled_.size() + count + kEditCapacity + options_.maxBlock, options_.maxBlock);
    outChanges_ = std::make_unique<ParameterOutput>(ids, options_.maxBlock);
    // Only requests satisfied by this snapshot are acknowledged. In particular,
    // never swallow a component reload, bus/latency or unsupported restart reason.
    restart_.fetch_and(~std::uint32_t(kParamTitlesChanged), std::memory_order_relaxed);
    refreshRequested_.store(false, std::memory_order_release);
    textChanged_ = false;
}

bool Instance::canRefreshConfiguration() const noexcept {
    return !(restartFlags() & kReloadComponent);
}

void Instance::refreshParameterMetadata() {
    FlagScope reading(refreshing_);
    auto* controller = component_->controller();
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        restart_.fetch_and(~std::uint32_t(kParamTitlesChanged), std::memory_order_relaxed);
        const auto revision = textRevision_;
        const auto count = controller ? controller->getParameterCount() : 0;
        require(count >= 0 && static_cast<std::size_t>(count) == metadata_.size(),
            "VST3 parameter information change altered parameter identities");
        std::vector<ParameterInfo> metadata(count);
        std::vector<bool> seen(count, false);
        for (int32 i = 0; i < count; ++i) {
            ParameterInfo parameter{};
            require(controller->getParameterInfo(i, parameter) == kResultOk
                && parameter.id <= kMaxParamId && parameter.stepCount >= 0
                && normalized(parameter.defaultNormalizedValue) && validText(parameter.title)
                && validText(parameter.shortTitle) && validText(parameter.units), "Invalid VST3 parameter metadata");
            const auto at = slot(parameter.id);
            require(at < metadata.size() && !seen[at], "VST3 parameter information change altered parameter identities");
            seen[at] = true; metadata[at] = parameter;
        }
        if ((controller ? controller->getParameterCount() : 0) != count || revision != textRevision_) continue;
        std::vector<host::ControlPort> controls;
        std::uint32_t inputs = 0;
        for (const auto& parameter : metadata) {
            if (parameter.flags & ParameterInfo::kIsHidden) continue;
            host::ControlPort control;
            control.index = parameter.id; control.symbol = std::to_string(parameter.id);
            control.name = utf8(parameter.title, std::size(parameter.title)); control.parameterId = parameter.id;
            control.defaultValue = parameter.defaultNormalizedValue;
            control.toggled = parameter.stepCount == 1;
            control.output = (parameter.flags & ParameterInfo::kIsReadOnly) != 0;
            control.automatable = (parameter.flags & ParameterInfo::kCanAutomate) != 0;
            if (!control.output) ++inputs;
            controls.push_back(std::move(control));
        }
        // Values, queues, orders, gestures and automation keep their stable ID
        // slots. An information refresh is not a parameter edit or state restore.
        metadata_ = std::move(metadata); controls_ = std::move(controls); info_.controlIn = inputs;
        textChanged_ = true;
        return;
    }
    throw std::runtime_error("VST3 parameter metadata changed repeatedly during live refresh");
}

void Instance::refreshBusTitles() {
    restart_.fetch_and(~std::uint32_t(kIoTitlesChanged), std::memory_order_relaxed);
    std::vector<BusInfo> names;
    auto* component = component_->component();
    for (const auto direction : {kInput, kOutput}) {
        const auto& buses = direction == kInput ? inputs_ : outputs_;
        require(component->getBusCount(kAudio, direction) == static_cast<int32>(buses.size()),
            "VST3 bus title change altered the bus count");
        for (std::size_t i = 0; i < buses.size(); ++i) {
            BusInfo info{};
            require(component->getBusInfo(kAudio, direction, i, info) == kResultOk && validText(info.name)
                && info.mediaType == kAudio && info.direction == direction
                && info.channelCount == buses[i].info.channelCount && info.busType == buses[i].info.busType
                && info.flags == buses[i].info.flags, "VST3 bus title change altered its configuration");
            names.push_back(info);
        }
    }
    std::size_t index = 0;
    for (auto* buses : {&inputs_, &outputs_}) for (auto& bus : *buses) bus.info = names[index++];
}

bool Instance::refreshConfiguration(std::string& error) {
    error.clear();
    if (!onMainThread()) { error = "VST3 configuration refresh requires the process main thread"; return false; }
    if (reconfiguring_ || syncing_ || refreshing_ || capturing_ || formatting_ || warming_) {
        error = "VST3 controller or processing callback is already in progress"; return false;
    }
    if (!canRefreshConfiguration() || failed_) { error = "VST3 processor requires recreation"; return false; }
    const auto requested = restartFlags();
    if (!requested) return true;
    FlagScope changing(reconfiguring_);
    try {
        if (requested & (kIoChanged | kLatencyChanged)) {
            resumeAfterReconfigure_ = resumeAfterReconfigure_ || active_;
            // A request raised by deactivate/setup/activate/query is later work.
            restart_.fetch_and(~(requested & std::uint32_t(kIoChanged | kIoTitlesChanged | kLatencyChanged)), std::memory_order_relaxed);
            if (processing_) {
                bool stopped = false;
                std::thread worker([&] {
                    try { stopped = processingNotificationAccepted(component_->processor()->setProcessing(false)); }
                    catch (...) {}
                });
                worker.join();
                require(stopped, "VST3 processor refused to stop for configuration refresh");
                processing_ = false;
            }
            if (active_) {
                require(component_->component()->setActive(false) == kResultOk,
                    "VST3 processor refused to deactivate for configuration refresh");
                active_ = false;
            }
            if (requested & kIoChanged) prepareBuses(false);
            if (resumeAfterReconfigure_) {
                require(component_->component()->setActive(true) == kResultOk,
                    "VST3 processor could not reactivate after configuration refresh");
                active_ = true;
                // A new layout can invalidate lazily allocated DSP scratch.
                // Match the initial chain's bounded preparation on an owned
                // processing thread; live callbacks must never pay that cost.
                // Unlike initial publication, retain every notification here.
                bool warmed = false;
                std::thread worker([&] {
                    if (!beginWarmUp()) return;
                    warmed = true;
                    for (unsigned block = 0; block < 100 && warmed; ++block)
                        warmed = warmUpBlock(options_.maxBlock);
                    warmed = endWarmUp() && warmed;
                });
                worker.join();
                if (!warmed) deactivate();
                require(warmed, "VST3 processor warm-up failed after configuration refresh");
                const auto latency = component_->processor()->getLatencySamples();
                const auto tail = component_->processor()->getTailSamples();
                latency_ = latency; tail_ = tail;
                resumeAfterReconfigure_ = false;
            }
        }
        // A bus change is prepared before flushing: the processor may already
        // expect its new bus count. Previously accepted edits still reach it before
        // a flags/step-count refresh changes which future controls are writable.
        if (!flushParameterChanges(error)) throw std::runtime_error(error);
        if (requested & kParamTitlesChanged) refreshParameterMetadata();
        if ((requested & kIoTitlesChanged) && !(requested & kIoChanged)) refreshBusTitles();
        return true;
    } catch (const std::exception& exception) { error = exception.what(); }
      catch (...) { error = "exception while refreshing VST3 configuration"; }
    // An unsuccessful bus/setup/activation remains inactive and keeps its live
    // state for retry. Never process with a partially negotiated buffer layout.
    if (!active_) latency_ = 0;
    restart_.fetch_or(requested, std::memory_order_relaxed);
    return false;
}

// The editor's frame and, on Linux, its run loop. Plug-ins register descriptors
// and timers here; the window host serves them on the main thread.
class Instance::Frame final : public IPlugFrame, public Linux::IRunLoop {
public:
    explicit Frame(host::EditorHost& host) : host_(&host) {}
    tresult PLUGIN_API queryInterface(const TUID id, void** out) override {
        if (!out) return kInvalidArgument;
        *out = nullptr;
        if (!id) return kNoInterface;
        if (!std::memcmp(id, IPlugFrame::iid.toTUID(), 16) || !std::memcmp(id, FUnknown::iid.toTUID(), 16))
            *out = static_cast<IPlugFrame*>(this);
        else if (!std::memcmp(id, Linux::IRunLoop::iid.toTUID(), 16)) *out = static_cast<Linux::IRunLoop*>(this);
        else return kNoInterface;
        addRef(); return kResultOk;
    }
    uint32 PLUGIN_API addRef() override { return refs_.fetch_add(1, std::memory_order_relaxed) + 1; }
    uint32 PLUGIN_API release() override {
        const auto left = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (!left) delete this;
        return left;
    }
    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* rect) override {
        if (!view || !rect || rect->getWidth() <= 0 || rect->getHeight() <= 0) return kInvalidArgument;
        if (!host_ || !host_->requestEditorResize(std::uint32_t(rect->getWidth()), std::uint32_t(rect->getHeight())))
            return kResultFalse;
        return view->onSize(rect);
    }
    tresult PLUGIN_API registerEventHandler(Linux::IEventHandler* handler, Linux::FileDescriptor fd) override {
        if (!handler || fd < 0 || !host_) return kInvalidArgument;
        handler->addRef();
        const auto id = host_->addEditorDescriptor(fd, [handler, fd] { handler->onFDIsSet(fd); });
        if (!id) { handler->release(); return kResultFalse; }
        descriptors_.push_back({handler, id});
        return kResultOk;
    }
    tresult PLUGIN_API unregisterEventHandler(Linux::IEventHandler* handler) override {
        return drop(descriptors_, handler, false) ? kResultOk : kInvalidArgument;
    }
    tresult PLUGIN_API registerTimer(Linux::ITimerHandler* handler, Linux::TimerInterval milliseconds) override {
        if (!handler || !host_) return kInvalidArgument;
        handler->addRef();
        const auto id = host_->addEditorTimer(std::uint32_t(std::max<Linux::TimerInterval>(1, milliseconds)),
                                              [handler] { handler->onTimer(); });
        if (!id) { handler->release(); return kResultFalse; }
        timers_.push_back({handler, id});
        return kResultOk;
    }
    tresult PLUGIN_API unregisterTimer(Linux::ITimerHandler* handler) override {
        return drop(timers_, handler, true) ? kResultOk : kInvalidArgument;
    }
    // The editor is gone: nothing the plug-in left registered may call it again.
    void detach() noexcept {
        while (!descriptors_.empty()) (void)drop(descriptors_, descriptors_.front().handler, false);
        while (!timers_.empty()) (void)drop(timers_, timers_.front().handler, true);
        host_ = nullptr;
    }
private:
    struct Registration { FUnknown* handler; std::uint32_t id; };
    bool drop(std::vector<Registration>& list, FUnknown* handler, bool timer) noexcept {
        bool found = false;
        for (auto it = list.begin(); it != list.end();) {
            if (it->handler != handler) { ++it; continue; }
            if (host_) { if (timer) host_->removeEditorTimer(it->id); else host_->removeEditorDescriptor(it->id); }
            it->handler->release();
            it = list.erase(it);
            found = true;
        }
        return found;
    }
    host::EditorHost* host_;
    std::atomic<uint32> refs_{1};
    std::vector<Registration> descriptors_, timers_;
};

bool Instance::hasEditor() const noexcept
{
    if (editorAvailable_ >= 0) return editorAvailable_ == 1;
    if (view_) return true;
    auto* controller = component_ ? component_->controller() : nullptr;
    editorAvailable_ = 0;
    if (!controller) return false;
    IPlugView* probe = controller->createView(ViewType::kEditor);
    if (!probe) return false;
    editorAvailable_ = probe->isPlatformTypeSupported(kPlatformTypeX11EmbedWindowID) == kResultTrue ? 1 : 0;
    probe->release();
    return editorAvailable_ == 1;
}

bool Instance::openEditor(std::uint64_t parent, host::EditorHost& host, host::EditorSize& size, std::string& error)
{
    if (view_) { error = "the effect's editor is already open"; return false; }
    auto* controller = component_ ? component_->controller() : nullptr;
    if (!controller || !parent) { error = "this effect has no editor of its own"; return false; }
    IPtr<IPlugView> view = owned(controller->createView(ViewType::kEditor));
    if (!view) { editorAvailable_ = 0; error = "this effect has no editor of its own"; return false; }
    if (view->isPlatformTypeSupported(kPlatformTypeX11EmbedWindowID) != kResultTrue) {
        editorAvailable_ = 0; error = "this effect's editor does not support X11 windows"; return false;
    }
    IPtr<Frame> frame = owned(new Frame(host));
    view->setFrame(frame);
    if (view->attached(reinterpret_cast<void*>(static_cast<std::uintptr_t>(parent)), kPlatformTypeX11EmbedWindowID) != kResultOk) {
        view->setFrame(nullptr); frame->detach();
        error = "the effect's editor could not open"; return false;
    }
    ViewRect rect{};
    if (view->getSize(&rect) != kResultOk || rect.getWidth() <= 0 || rect.getHeight() <= 0) rect = ViewRect(0, 0, 640, 400);
    size = {std::uint32_t(rect.getWidth()), std::uint32_t(rect.getHeight()), view->canResize() == kResultTrue};
    view_ = view; frame_ = frame; editorAvailable_ = 1;
    return true;
}

bool Instance::resizeEditor(std::uint32_t& width, std::uint32_t& height) noexcept
{
    if (!view_ || view_->canResize() != kResultTrue) return false;
    ViewRect rect(0, 0, int32(width), int32(height));
    (void)view_->checkSizeConstraint(&rect);
    if (view_->onSize(&rect) != kResultOk) return false;
    width = std::uint32_t(std::max(1, rect.getWidth())); height = std::uint32_t(std::max(1, rect.getHeight()));
    return true;
}

void Instance::closeEditor() noexcept
{
    if (!view_) return;
    (void)view_->removed();
    view_->setFrame(nullptr);
    if (frame_) frame_->detach();
    view_ = nullptr; frame_ = nullptr;
}

Instance::~Instance()
{
    assert(onMainThread());
    closeEditor();
    if (handler_) handler_->owner = nullptr;
    deactivate();
    if (component_) for (const auto direction : {kInput, kOutput}) {
        auto& list = direction == kInput ? inputs_ : outputs_;
        for (std::size_t i = 0; i < list.size(); ++i)
            if (list[i].enabled) (void)component_->component()->activateBus(kAudio, direction, i, false);
    }
    component_.reset();
}

bool Instance::connectAudioIn(std::uint32_t channel, float* buffer) noexcept {
    return channel < options_.channels ? connectMainAudioIn(channel, buffer) : connectSidechainIn(channel - options_.channels, buffer);
}
bool Instance::connectMainAudioIn(std::uint32_t channel, float* buffer) noexcept {
    if (channel >= options_.channels) return false;
    main_[channel] = buffer; return buffer != nullptr;
}
bool Instance::connectAudioOut(std::uint32_t channel, float* buffer) noexcept {
    if (channel >= options_.channels) return false;
    out_[channel] = buffer; return buffer != nullptr;
}
bool Instance::connectSidechainIn(std::uint32_t channel, float* buffer) noexcept {
    if (channel >= options_.sidechainChannels) return false;
    key_[channel] = buffer; return true;
}
bool Instance::ready() const noexcept {
    for (std::uint32_t c = 0; c < options_.channels; ++c) if (!main_[c] || !out_[c]) return false;
    return component_ != nullptr;
}
void Instance::activate() noexcept {
    if (!onMainThread()) { diagnose(WrongThread); return; }
    if (active_) return;
    if (!ready() || failed_) { diagnose(ActivationFailed); return; }
    try {
        if (component_->component()->setActive(true) != kResultOk) { diagnose(ActivationFailed); return; }
        active_ = true;
        // Some processors establish their delay at activation. Refresh both
        // snapshots here, before publication, including after reactivation.
        restart_.fetch_and(~std::uint32_t(kLatencyChanged), std::memory_order_relaxed);
        const auto latency = component_->processor()->getLatencySamples();
        const auto tail = component_->processor()->getTailSamples();
        latency_ = latency; tail_ = tail;
    } catch (...) {
        diagnose(ActivationFailed); failed_ = true;
        restart_.fetch_or(kReloadComponent, std::memory_order_relaxed);
        try { (void)component_->component()->setActive(false); } catch (...) {}
        active_ = false;
    }
}
void Instance::deactivate() noexcept {
    if (!onMainThread()) { diagnose(WrongThread); return; }
    if (!active_) return;
    if (processing_) { (void)component_->processor()->setProcessing(false); processing_ = false; }
    if (component_->component()->setActive(false) != kResultOk) diagnose(ActivationFailed);
    active_ = false;
}

std::size_t Instance::slot(std::uint32_t id) const noexcept {
    const auto found = std::lower_bound(slots_.begin(), slots_.end(), id,
        [](const auto& entry, auto key) { return entry.first < key; });
    return found != slots_.end() && found->first == id ? found->second : slots_.size();
}
bool Instance::writable(std::size_t index) const noexcept {
    return index < metadata_.size() && !(metadata_[index].flags & (ParameterInfo::kIsReadOnly | ParameterInfo::kIsHidden));
}
double Instance::normalise(std::size_t index, double value) const noexcept {
    value = std::clamp(value, 0.0, 1.0);
    const auto steps = metadata_[index].stepCount;
    return steps ? std::min<double>(steps, std::floor(value * (static_cast<double>(steps) + 1))) / steps : value;
}
void Instance::setControl(std::uint32_t id, float value) noexcept {
    setParameter(id, value);
}
void Instance::setParameter(std::uint32_t id, double value) noexcept {
    setParameterAtVersion(id, value, parameterVersion());
}
void Instance::setParameterAtVersion(std::uint32_t id, double value, std::uint64_t version) noexcept {
    const auto at = slot(id);
    if (!writable(at) || !std::isfinite(value)) { diagnose(InvalidControl); return; }
    if (version == UINT64_MAX) version = parameterVersion();
    if (version < parameterVersion()) return;
    pending_[at] = normalise(at, value); dirty_[at] = true; pendingVersions_[at] = version;
    pendingOrders_[at] = nextControlOrder_.fetch_add(1, std::memory_order_relaxed) + 1;
    pendingAutomationOrders_[at] = host::nextAutomationRevision();
}
bool Instance::scheduleControl(std::uint32_t id, float value, std::uint32_t offset) noexcept {
    return scheduleParameter(id, value, offset);
}
bool Instance::scheduleParameter(std::uint32_t id, double value, std::uint32_t offset) noexcept {
    const auto at = slot(id);
    if (!writable(at) || !(metadata_[at].flags & ParameterInfo::kCanAutomate)
        || !std::isfinite(value) || offset >= options_.maxBlock) { diagnose(InvalidControl); return false; }
    if (scheduledCount_ == scheduled_.size()) { diagnose(InputOverflow); return false; }
    scheduled_[scheduledCount_++] = {id, offset, normalise(at, value), 0,
        nextControlOrder_.fetch_add(1, std::memory_order_relaxed) + 1}; return true;
}
host::Timing Instance::timing() const noexcept { return {true, 1, static_cast<uint32_t>(scheduled_.size())}; }
void Instance::setAutomationRecording(const host::AutomationRecording& policy) noexcept {
    const auto at = slot(policy.control);
    recording_ = policy.active() && writable(at) && (metadata_[at].flags & ParameterInfo::kCanAutomate)
        ? policy : host::AutomationRecording{};
}
float Instance::controlValue(std::uint32_t id) const noexcept {
    return static_cast<float>(normalizedValue(id));
}
double Instance::normalizedValue(std::uint32_t id) const noexcept {
    const auto at = slot(id);
    return at < metadata_.size() ? values_[at].load(std::memory_order_relaxed) : 0;
}

std::string Instance::parameterText(std::uint32_t id, double value) const {
    if (!onMainThread() || !normalized(value) || formatting_ || syncing_ || refreshing_ || capturing_) return {};
    const auto at = slot(id);
    auto* controller = component_->controller();
    if (!controller || at >= metadata_.size()) return {};
    FlagScope formatting(formatting_);
    const auto revision = textRevision_;
    try {
        // Read presentation from this controller. The immutable processing
        // metadata can still await an owner rebuild after kParamTitlesChanged;
        // that flag also covers steps/defaults/flags, not just display labels.
        ParameterInfo current{}; String128 text{};
        if (controller->getParameterInfo(static_cast<int32>(at), current) != kResultOk
            || current.id != id || !validText(current.units)
            || controller->getParamStringByValue(id, value, text) != kResultOk
            || !validText(text) || !text[0] || revision != textRevision_) return {};
        auto result = utf8(text, std::size(text));
        const auto units = utf8(current.units, std::size(current.units));
        if (!units.empty()) { result += ' '; result += units; }
        return result;
    } catch (...) { return {}; }
}
bool Instance::takeParameterTextChanged() noexcept {
    if (!notificationOwner()) return false;
    return std::exchange(textChanged_, false);
}

tresult Instance::edit(EditKind kind, ParamID id, double value) noexcept {
    if (!notificationOwner()) return kResultFalse;
    const auto at = slot(id);
    if (!writable(at) || (kind == EditKind::Value && !normalized(value))) {
        notificationDiagnostics_.fetch_or(host::InvalidOutput, std::memory_order_relaxed); return kInvalidArgument;
    }
    if (settingController_) return kResultOk;
    const auto packet = controllerWrite_.load(std::memory_order_relaxed);
    if (packet - controllerRead_.load(std::memory_order_acquire) == kControllerCapacity) {
        controllerLoss_.fetch_add(1, std::memory_order_release);
        diagnose(InputOverflow); notificationLost(); return kOutOfMemory;
    }
    auto& edit = edits_[packet % kControllerCapacity];
    edit.kind = kind;
    edit.revision = host::nextAutomationRevision();
    if (kind == EditKind::Value) {
        const auto write = editWrite_.load(std::memory_order_relaxed);
        if (write - editRead_.load(std::memory_order_acquire) == kEditCapacity) { diagnose(InputOverflow); return kOutOfMemory; }
        value = normalise(at, value); edit.change = {id, 0, value, parameterVersion(),
            nextControlOrder_.fetch_add(1, std::memory_order_relaxed) + 1};
        editWrite_.store(write + 1, std::memory_order_release);
        stateDirty_.fetch_or(ParameterEdit, std::memory_order_release);
    } else {
        value = normalizedValue(id);
        edit.change = {id, 0, value, parameterVersion(), 0};
    }
    controllerWrite_.store(packet + 1, std::memory_order_release);
    const auto eventKind = kind == EditKind::Begin ? host::ParameterEventKind::Begin
        : kind == EditKind::End ? host::ParameterEventKind::End : host::ParameterEventKind::Value;
    host::ParameterEvent event{eventKind, id, id, value, 0, 0, group_, group_ ? groupTime_ : parameterTime(),
        bool(metadata_[at].flags & ParameterInfo::kCanAutomate)};
    event.automationRevision = edit.revision;
    notify(event, false);
    if (listener_) listener_(listenerContext_, {kind, id, value});
    return kResultOk;
}

bool Instance::notificationOwner() noexcept {
    if (onMainThread()) return true;
    diagnose(WrongThread);
    notificationDiagnostics_.fetch_or(host::WrongNotificationThread, std::memory_order_relaxed);
    return false;
}
void Instance::notificationLost() noexcept {
    notificationOverflow_.store(true, std::memory_order_release);
    notificationDiagnostics_.fetch_or(host::OutputOverflow, std::memory_order_relaxed);
}
void Instance::notify(const host::ParameterEvent& event, bool audio) noexcept {
    if (!(audio ? audioNotifications_ : controllerNotifications_).push(event)) notificationLost();
}
bool Instance::nextParameterEvent(host::ParameterEvent& event) noexcept {
    if (!notificationOwner()) return false;
    if (preferRecordingNotification_ && automationRecorder_ && automationRecorder_->next(event)) {
        preferRecordingNotification_ = false; return true;
    }
    // Fair between sources while preserving each producer's ordering. Feedback
    // never supplies controller gesture boundaries or invokes a controller.
    auto& first = preferAudioNotification_ ? audioNotifications_ : controllerNotifications_;
    auto& second = preferAudioNotification_ ? controllerNotifications_ : audioNotifications_;
    if (first.pop(event) || second.pop(event)) {
        preferAudioNotification_ = !preferAudioNotification_; preferRecordingNotification_ = true; return true;
    }
    preferRecordingNotification_ = false;
    return automationRecorder_ && automationRecorder_->next(event);
}
bool Instance::takeStateDirty() noexcept {
    const auto changes = takeStateChanges();
    return changes.edited || changes.readout;
}
host::StateChanges Instance::takeStateChanges() noexcept {
    if (!notificationOwner()) return {};
    const auto sources = stateDirty_.exchange(0, std::memory_order_acq_rel);
    return {sources != 0, readoutStatePending_.exchange(false, std::memory_order_acq_rel),
            sources == ParameterEdit};
}
bool Instance::takeNotificationOverflow() noexcept {
    if (!notificationOwner()) return false;
    if (automationRecorder_ && automationRecorder_->takeOverflow()) notificationLost();
    return notificationOverflow_.exchange(false, std::memory_order_acq_rel);
}
std::uint32_t Instance::notificationDiagnostics() const noexcept {
    // These are common flags, not the adapter's differently numbered diagnostics.
    return notificationDiagnostics_.load(std::memory_order_relaxed);
}
tresult Instance::groupEdit(bool begin) noexcept {
    if (!notificationOwner()) return kResultFalse;
    if (settingController_) return kResultOk;
    if (begin == (group_ != 0)) {
        notificationDiagnostics_.fetch_or(host::InvalidOutput, std::memory_order_relaxed); return kResultFalse;
    }
    if (begin) {
        if (++nextGroup_ == 0) ++nextGroup_;
        group_ = nextGroup_;
        groupTime_ = parameterTime();
        notify({host::ParameterEventKind::GroupBegin, UINT32_MAX, UINT32_MAX, 0, 0, 0, group_, groupTime_, true}, false);
    } else {
        // Grouping preserves notification identity for one automation timestamp;
        // it must not defer audible parameter delivery until a drag finishes.
        notify({host::ParameterEventKind::GroupEnd, UINT32_MAX, UINT32_MAX, 0, 0, 0, group_, groupTime_, true}, false);
        group_ = 0;
        groupTime_ = {};
    }
    return kResultOk;
}
void Instance::setEditListener(EditListener listener, void* context) noexcept {
    if (!onMainThread()) { diagnose(WrongThread); return; }
    listener_ = listener; listenerContext_ = context;
}
IComponentHandler* Instance::componentHandler() const noexcept { return handler_.get(); }
bool Instance::parameterFlushRequested() const noexcept {
    return refreshRequested_.load(std::memory_order_acquire)
        || editPublished_.load(std::memory_order_acquire) != editWrite_.load(std::memory_order_acquire);
}
bool Instance::syncController() noexcept {
    if (!notificationOwner() || syncing_ || refreshing_ || capturing_ || formatting_) return false;
    // The processor releases queue space before processing. Only its completed
    // publication acknowledges a native value; dequeuing is not sufficient.
    if (parameterFlushRequested()) return false;
    auto* controller = component_->controller();
    if (!controller) return true;
    // Values and their accepted order must belong to the same publication. All
    // copied fields are atomic; a racing publication simply defers this poll.
    const auto publication = publication_.load(std::memory_order_acquire);
    if (publication & 1) return false;
    for (std::size_t i = 0; i < metadata_.size(); ++i) {
        syncValues_[i] = values_[i].load(std::memory_order_acquire);
        syncOrders_[i] = publishedOrders_[i].load(std::memory_order_acquire);
    }
    if (publication_.load(std::memory_order_acquire) != publication) return false;
    bool ok = true; FlagScope syncing(syncing_);
    try {
        for (std::size_t i = 0; i < metadata_.size(); ++i) {
            const auto current = controller->getParamNormalized(metadata_[i].id);
            if (parameterFlushRequested()) return false;
            if (!normalized(current)) {
                notificationDiagnostics_.fetch_or(host::MainCallbackFailed, std::memory_order_relaxed); return false;
            }
            // Setting an unchanged program can trigger dependent parameter
            // changes again in standard SDK controllers. Do not restart it.
            if (current == syncValues_[i]) continue;
            syncOrder_ = syncOrders_[i];
            {
                FlagScope setting(settingController_);
                if (controller->setParamNormalized(metadata_[i].id, syncValues_[i]) != kResultOk) ok = false;
            }
            if (parameterFlushRequested()) return false;
        }
    } catch (...) {
        notificationDiagnostics_.fetch_or(host::MainCallbackFailed, std::memory_order_relaxed); ok = false;
    }
    return ok;
}

bool Instance::refreshControllerValues(std::string& error) {
    if (!refreshRequested_.exchange(false, std::memory_order_acq_rel)) return true;
    FlagScope reading(refreshing_);
    const auto version = parameterVersion();
    try {
        auto* controller = component_->controller();
        if (!controller) throw std::runtime_error("VST3 invalidation has no controller");
        // Read the entire native set, including hidden/read-only IDs, before
        // changing any cache or input. Reentrant requests are deferred, bounded
        // to one attempt per call, rather than clearing a newer request.
        for (std::size_t i = 0; i < metadata_.size(); ++i) {
            refreshed_[i] = controller->getParamNormalized(metadata_[i].id);
            if (!normalized(refreshed_[i])) throw std::runtime_error("VST3 controller returned an invalid parameter value");
        }
        if (refreshRequested_.load(std::memory_order_acquire)) {
            error = "VST3 controller values changed while being read"; return false;
        }
        const auto later = [&](std::uint64_t acceptedVersion, std::uint64_t order) {
            return acceptedVersion >= version && (!refreshFromSync_ || order > refreshOrder_);
        };
        publication_.fetch_add(1, std::memory_order_acq_rel);
        for (std::size_t i = 0; i < metadata_.size(); ++i) {
            if (metadata_[i].flags & ParameterInfo::kIsReadOnly) {
                held_[i] = refreshed_[i]; values_[i].store(refreshed_[i], std::memory_order_release);
                appliedVersions_[i] = version; appliedOrders_[i] = refreshOrder_;
                publishedOrders_[i].store(refreshOrder_, std::memory_order_release);
            } else if (dirty_[i] != 1 || !later(pendingVersions_[i], pendingOrders_[i])) {
                // A derived program change belongs to the original command's
                // order. A later independent knob/automation change survives,
                // including one that already reached the processor.
                const bool preserve = later(appliedVersions_[i], appliedOrders_[i]);
                pending_[i] = preserve ? held_[i] : refreshed_[i]; dirty_[i] = 2;
                pendingVersions_[i] = version;
                pendingOrders_[i] = preserve ? appliedOrders_[i] : refreshOrder_;
            }
        }
        publication_.fetch_add(1, std::memory_order_release);
        return true;
    } catch (const std::exception& exception) { error = exception.what(); }
      catch (...) { error = "exception while reading VST3 controller values"; }
    refreshRequested_.store(true, std::memory_order_release);
    notificationDiagnostics_.fetch_or(host::MainCallbackFailed, std::memory_order_relaxed);
    return false;
}

bool Instance::flushParameterChanges(std::string& error) {
    error.clear();
    if (!onMainThread()) { error = "VST3 parameter flush requires the process main thread"; return false; }
    if (syncing_ || refreshing_ || capturing_ || formatting_) { error = "VST3 controller/state callback is already in progress"; return false; }
    if (failed_ || warming_) { error = "VST3 processor is not ready for a parameter flush"; return false; }
    if (active_ && !ready()) { error = "VST3 active processor has disconnected audio buffers"; return false; }
    if (scheduledCount_) { error = "VST3 parameter flush requires scheduled automation to finish"; return false; }
    const bool native = editRead_.load(std::memory_order_acquire) != editWrite_.load(std::memory_order_acquire);
    const bool explicitPending = native || refreshRequested_.load(std::memory_order_acquire)
        || std::find(dirty_.begin(), dirty_.end(), 1) != dirty_.end();
    if (!active_ && explicitPending) { error = "VST3 pending edits need an active processor before capture"; return false; }
    try {
        if (!refreshControllerValues(error)) return false;
        if (active_ && (native || std::any_of(dirty_.begin(), dirty_.end(), [](auto value) { return value != 0; }))) {
            const auto savedContext = context_; const bool wasProcessing = processing_;
            warming_ = true;
            try {
                std::thread flush([&] {
                    run(0);
                    if (!wasProcessing && processing_) {
                        if (!processingNotificationAccepted(component_->processor()->setProcessing(false))) {
                            failed_ = true; diagnose(ProcessFailed); restart_.fetch_or(kReloadComponent, std::memory_order_relaxed);
                        }
                        processing_ = false;
                    }
                });
                flush.join();
            } catch (...) { warming_ = false; context_ = savedContext; throw; }
            warming_ = false; context_ = savedContext;
            if (failed_) { error = "VST3 could not deliver pending parameters before state capture"; return false; }
        }
        if (!syncController()) { error = "VST3 controller could not synchronize after parameter flush"; return false; }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); }
      catch (...) { error = "exception while flushing VST3 parameters"; }
    return false;
}

std::optional<session::PluginState> Instance::captureState(std::string& error) {
    if (!flushParameterChanges(error)) return {};
    FlagScope capturing(capturing_);
    try {
        auto state = component_->capture(error);
        if (!state) return {};
        if (parameterFlushRequested()) { error = "VST3 parameters changed during state capture"; return {}; }
        return session::PluginState::create(uri(), std::move(state->processor), std::move(state->controller), error);
    } catch (const std::exception& exception) { error = exception.what(); return {}; }
      catch (...) { error = "exception while capturing VST3 state"; return {}; }
}

std::optional<host::UnrenderedParameter> Instance::unrenderedParameter(std::uint32_t id) const noexcept {
    const auto at = slot(id);
    if (!onMainThread() || failed_ || at >= metadata_.size() || appliedVersions_[at] < parameterVersion()
        || !appliedAutomationOrders_[at] || appliedAutomationOrders_[at] == renderedAutomationOrders_[at]) return {};
    return host::UnrenderedParameter{held_[at], appliedAutomationOrders_[at], nextInputTime()};
}

host::ParameterTime Instance::nextInputTime() const noexcept {
    if (!onMainThread() || failed_) return {};
    auto time = processContext_.recordTime();
    if (time.valid && processContext_.hasLoop() && time.frame >= processContext_.loopEnd) {
        time.loopIteration += 1 + (time.frame - processContext_.loopEnd) / (processContext_.loopEnd - processContext_.loopStart);
        time.frame = processContext_.loopStart + (time.frame - processContext_.loopStart) % (processContext_.loopEnd - processContext_.loopStart);
    }
    time.processSequence = 0; // a future input boundary, not a processed span
    return time;
}

void Instance::copyInputs(std::uint32_t frames) noexcept {
    // Isolate input memory as well as output memory. A failed or in-place
    // processor cannot destroy the dry audio used for failure recovery.
    for (std::uint32_t c = 0; c < options_.channels; ++c) {
        auto* dry = dry_.data() + c * options_.maxBlock;
        if (main_[c] && !warming_) std::copy_n(main_[c], frames, dry);
        else std::fill_n(dry, frames, 0.0f);
    }
    for (std::size_t i = 0; i < inputs_.size(); ++i) {
        auto& bus = inputs_[i]; inputBuffers_[i].silenceFlags = 0;
        for (std::size_t c = 0; c < bus.pointers.size(); ++c) {
            const float* source = nullptr;
            if (i == mainInput_) source = dry_.data() + c * options_.maxBlock;
            else if (i == keyInput_ && !warming_) source = key_[c];
            if (source) std::copy_n(source, frames, bus.pointers[c]);
            else std::fill_n(bus.pointers[c], frames, 0.0f);
        }
    }
    for (std::size_t i = 0; i < outputs_.size(); ++i) {
        outputBuffers_[i].silenceFlags = 0;
        for (auto* ptr : outputs_[i].pointers) std::fill_n(ptr, frames, 0.0f);
    }
}
void Instance::copyOutputs(std::uint32_t frames, bool dry) noexcept {
    if (warming_) return;
    for (std::uint32_t c = 0; c < options_.channels; ++c) if (out_[c]) {
        if (dry) std::copy_n(dry_.data() + c * options_.maxBlock, frames, out_[c]);
        else if (outputBuffers_[mainOutput_].silenceFlags & (uint64{1} << c)) std::fill_n(out_[c], frames, 0.0f);
        else std::copy_n(outputs_[mainOutput_].pointers[c], frames, out_[c]);
    }
}

void Instance::run(std::uint32_t frames) noexcept {
    if (frames > options_.maxBlock) { diagnose(InvalidBlock); scheduledCount_ = 0; return; }
    copyInputs(frames);
    if (!active_ || failed_ || !ready()) { copyOutputs(frames, true); scheduledCount_ = 0; return; }
    if (!processing_) {
        if (!processingNotificationAccepted(component_->processor()->setProcessing(true))) {
            diagnose(ProcessFailed); failed_ = true; restart_.fetch_or(kReloadComponent, std::memory_order_relaxed);
            copyOutputs(frames, true); scheduledCount_ = 0; return;
        }
        processing_ = true;
    }
    (void)inChanges_->begin(frames, held_); (void)outChanges_->begin(frames);
    const auto recordingSlot = slot(recording_.control);
    double inputValue = recordingSlot < held_.size() ? held_[recordingSlot] : 0;
    // Initial/refreshed baselines precede ordered untimed edits. Commands
    // from before a native invalidation cannot overwrite its new values.
    const auto accept = [&](std::size_t i, double value, std::uint64_t version, std::uint64_t order, std::uint32_t offset,
                            std::uint64_t automationRevision = 0) {
        if (version < parameterVersion() || order < appliedOrders_[i]) return false;
        if (inChanges_->push(metadata_[i].id, value, offset)) {
            if (i == recordingSlot && !offset) inputValue = value;
            appliedVersions_[i] = version; appliedOrders_[i] = order;
            appliedAutomationOrders_[i] = automationRevision; return true;
        }
        return false;
    };
    for (std::size_t i = 0; i < metadata_.size(); ++i) if (dirty_[i] == 2)
        accept(i, pending_[i], pendingVersions_[i], pendingOrders_[i], 0);
    auto packet = controllerRead_.load(std::memory_order_relaxed);
    const auto packetEnd = controllerWrite_.load(std::memory_order_acquire);
    auto read = editRead_.load(std::memory_order_relaxed);
    const auto lost = controllerLoss_.load(std::memory_order_acquire);
    const bool lostBoundary = lost != appliedControllerLoss_;
    if (lostBoundary) {
        appliedControllerLoss_ = lost; blockedRecordingPass_ = recording_.pass;
        for (auto& gesture : nativeGestures_) gesture = {};
    }
    for (auto& gesture : nativeGestures_) if (gesture.version < parameterVersion()) gesture = {};
    while (packet != packetEnd) {
        const auto edit = edits_[packet % kControllerCapacity]; const auto& change = edit.change;
        const auto at = slot(change.id); auto& gesture = nativeGestures_[at];
        if (change.version >= parameterVersion()) {
            gesture.version = change.version;
            if (edit.kind == EditKind::Begin) {
                if (!gesture.pending) gesture.value = held_[at];
                gesture.active = gesture.pending = true; gesture.implicitFrames = 0;
                gesture.revision = edit.revision;
            } else if (edit.kind == EditKind::End) {
                gesture.active = false; gesture.implicitFrames = 0;
            } else if (accept(at, change.value, change.version, change.order, 0, edit.revision)) {
                gesture.value = change.value; gesture.revision = edit.revision; gesture.pending = true;
                // Wheel/text controls may omit Begin/End. A short inactivity
                // timeout, measured by the audio owner, supplies their release.
                if (!gesture.active) gesture.implicitFrames = static_cast<std::uint32_t>(std::ceil(options_.sampleRate * .2));
            }
            if (lostBoundary) gesture = {};
        }
        if (edit.kind == EditKind::Value) ++read;
        ++packet;
    }
    controllerRead_.store(packet, std::memory_order_release);
    editRead_.store(read, std::memory_order_release);
    for (std::size_t i = 0; i < metadata_.size(); ++i) if (dirty_[i] == 1)
        accept(i, pending_[i], pendingVersions_[i], pendingOrders_[i], 0, pendingAutomationOrders_[i]);
    const auto manualBase = inputValue;
    const auto directOrder = recordingSlot < dirty_.size() && dirty_[recordingSlot] == 1
        && pendingVersions_[recordingSlot] >= parameterVersion() && pendingOrders_[recordingSlot] == appliedOrders_[recordingSlot]
        ? pendingAutomationOrders_[recordingSlot] : 0;
    // Automation occurs at its sample positions, not its queue insertion order.
    std::sort(scheduled_.begin(), scheduled_.begin() + scheduledCount_, [](const Change& a, const Change& b) {
        return a.offset != b.offset ? a.offset < b.offset : a.order < b.order;
    });
    for (std::uint32_t i = 0; i < scheduledCount_; ++i) {
        const auto& change = scheduled_[i];
        if (change.offset >= std::max(1u, frames)) { diagnose(InvalidControl); continue; }
        accept(slot(change.id), change.value, parameterVersion(),
            nextControlOrder_.fetch_add(1, std::memory_order_relaxed) + 1, change.offset);
    }
    scheduledCount_ = 0;
    std::array<engine::RecordedSpan, 2> recorded{};
    const auto time = processContext_.recordTime();
    if (!warming_ && recordingVersion_ != parameterVersion()) {
        recordingVersion_ = parameterVersion(); appliedRecording_ = {};
        (void)automationRecorder_->begin(engine::RecordingLane{}, host::AutomationPlayback{}, processContext_, 0);
    }
    if (!warming_ && frames && recording_.active() && processContext_.time.frame >= 0
        && time.frame <= std::numeric_limits<std::int64_t>::max() - frames) {
        const auto at = slot(recording_.control); const auto& native = nativeGestures_[at];
        const bool newPass = recording_.pass != appliedRecording_.pass || recording_.control != appliedRecording_.control
            || recording_.mode != appliedRecording_.mode;
        const auto arm = recordingRevision_;
        if (newPass) { recordingOrder_ = 0; recordingValue_ = manualBase; }
        const bool pendingManual = recording_.manualPending && recording_.manualRevision != recording_.armRevision
            && (newPass || recording_.manualRevision != appliedRecording_.manualRevision);
        const bool manualChanged = recording_.manualRevision != recording_.armRevision
            && (newPass || recording_.manualRevision != appliedRecording_.manualRevision)
            && (recording_.manualActive || pendingManual || recording_.mode == host::AutomationMode::Write);
        if (manualChanged && recording_.manualRevision > recordingOrder_) {
            recordingValue_ = normalise(at, recording_.manualValue);
            recordingOrder_ = recording_.manualRevision; ++recordingRevision_;
        }
        if (directOrder > recordingOrder_) {
            recordingValue_ = manualBase; recordingOrder_ = directOrder; ++recordingRevision_;
        }
        if ((native.active || native.pending || native.implicitFrames) && native.revision > recordingOrder_) {
            recordingValue_ = native.value; recordingOrder_ = native.revision; ++recordingRevision_;
        }
        auto policy = recording_; policy.manualValue = recordingValue_; policy.manualRevision = recordingRevision_;
        policy.manualActive = policy.manualActive || pendingManual;
        policy.manualPending = false; // the combined native/manual revision is owned here
        const auto hold = policy.manualActive || native.active ? frames
            : native.implicitFrames ? std::min(frames, native.implicitFrames) : native.pending ? 1u : 0u;
        if (time.valid && time.playing && !automationRecorder_->prepareCapacity(policy.pass, hold && hold < frames ? 2 : 1))
            policy = {};
        struct Playback { const Instance& instance; std::size_t at; } playback{*this, at};
        const host::AutomationPlayback readValue{&playback, [](const void* opaque, std::int64_t frame) noexcept {
            const auto& p = *static_cast<const Playback*>(opaque); const auto& source = p.instance.automationPlayback_;
            const auto value = source.valueAt ? source.valueAt(source.context, frame) : p.instance.held_[p.at];
            return std::isfinite(value) ? std::clamp(value, 0., 1.) : p.instance.held_[p.at];
        }};
        const auto prepare = [&](unsigned index, std::uint32_t offset, std::uint32_t count, bool touching) {
            policy.manualActive = touching;
            auto context = processContext_; context.time.frame += offset;
            recorded[index] = automationRecorder_->begin({policy.pass == blockedRecordingPass_ ? host::AutomationRecording{} : policy,
                {}, held_[at], arm}, readValue, context, count);
            auto& span = recorded[index]; span.event.offset = offset; span.event.id = recording_.control;
            span.event.automationRevision = recordingOrder_;
            if (context.time.processSequence) {
                span.event.time.processSequence = context.time.processSequence;
                span.event.time.loopIteration = context.time.loopIteration;
            }
        };
        if (hold) prepare(0, 0, hold, true);
        if (hold < frames) prepare(hold ? 1 : 0, hold, frames - hold, false);
        // Own this lane's complete input while armed. That keeps sparse/flat
        // Read separate from explicit host commands and guarantees exact Read
        // restoration even when a return ends partway through this block.
        for (std::uint32_t offset = 0; offset < frames; ++offset) {
            auto value = readValue.valueAt(readValue.context, time.frame + offset);
            for (const auto& span : recorded) if (offset >= span.event.offset
                && offset - span.event.offset < span.event.automationSpanFrames) value = span.valueAt(offset - span.event.offset);
            (void)accept(at, normalise(at, value), parameterVersion(),
                nextControlOrder_.fetch_add(1, std::memory_order_relaxed) + 1, offset);
        }
        appliedRecording_ = recording_;
    } else if (!warming_ && frames) {
        (void)automationRecorder_->begin(engine::RecordingLane{}, host::AutomationPlayback{}, processContext_, 0);
        appliedRecording_ = {};
    }
    inChanges_->seal();
    if (inChanges_->rejected()) diagnose(InputOverflow);
    ProcessData data;
    data.processMode = options_.offline ? kOffline : kRealtime;
    data.numSamples = frames; data.numInputs = inputs_.size(); data.numOutputs = outputs_.size();
    data.inputs = inputBuffers_.data(); data.outputs = outputBuffers_.data();
    data.inputParameterChanges = inChanges_.get(); data.outputParameterChanges = outChanges_.get(); data.processContext = &context_;
    bool ok = component_->processor()->process(data) == kResultOk;
    if (!ok) diagnose(ProcessFailed);
    // A rejected feedback point means the processor's final value is unknown.
    // Do not interpolate the next block from a partial queue: require a rebuild.
    if (outChanges_->rejected()) {
        diagnose(OutputOverflow); ok = false;
        if (!warming_ || reconfiguring_) { notificationLost(); stateDirty_.fetch_or(OtherEdit, std::memory_order_release); }
    }
    if (ok) for (std::uint32_t c = 0; c < options_.channels; ++c) {
        if (outputBuffers_[mainOutput_].silenceFlags & (uint64{1} << c)) continue;
        const auto* audio = outputs_[mainOutput_].pointers[c];
        if (!std::all_of(audio, audio + frames, [](float value) { return std::isfinite(value); })) { ok = false; diagnose(NonfiniteAudio); }
    }
    if (!ok) {
        failed_ = true; restart_.fetch_or(kReloadComponent, std::memory_order_relaxed); copyOutputs(frames, true);
    } else {
        for (std::size_t i = 0; i < metadata_.size(); ++i) { held_[i] = inChanges_->finalValue(i); dirty_[i] = false; }
        for (int32 i = 0; i < outChanges_->getParameterCount(); ++i) {
            auto* queue = outChanges_->getParameterData(i);
            if (!queue->getPointCount()) continue;
            int32 offset = 0; double value = 0;
            const auto id = queue->getParameterId(); const auto at = slot(id);
            auto previous = inChanges_->valueAt(at, 0);
            const auto lastInput = inChanges_->valueAt(at, frames);
            for (int32 point = 0; point < queue->getPointCount(); ++point) {
                if (queue->getPoint(point, offset, value) != kResultOk) continue;
                const auto input = inChanges_->valueAt(at, static_cast<uint32>(offset));
                const double expected = input.offset > previous.offset ? input.value : previous.value;
                const bool changed = value != expected;
                previous = {offset, value};
                // Host and processor changes share a sample timeline. Feedback
                // wins a tie, but cannot replace a later host automation point.
                if (offset >= lastInput.offset) {
                    held_[at] = value;
                    if (changed) {
                        appliedVersions_[at] = parameterVersion();
                        appliedOrders_[at] = nextControlOrder_.fetch_add(1, std::memory_order_relaxed) + 1;
                        appliedAutomationOrders_[at] = 0;
                    }
                }
                // Initial warm-up is unpublished. Live reconfiguration already
                // belongs to a document: its changed state must remain visible.
                if ((!warming_ || reconfiguring_) && changed) {
                    if (recording_.active() && id == recording_.control) {
                        // Processor feedback can change the result independently
                        // of this input ramp. Never persist an unverified span.
                        recorded = {}; blockedRecordingPass_ = recording_.pass; notificationLost();
                    }
                    if (metadata_[at].flags & (ParameterInfo::kIsReadOnly | ParameterInfo::kIsHidden)) {
                        // The live cache/controller still receives every latest
                        // value. Persistence is decided only by state capture.
                        readoutStatePending_.store(true, std::memory_order_release);
                    } else {
                        stateDirty_.fetch_or(OtherEdit, std::memory_order_release);
                        notify({host::ParameterEventKind::Value, id, id, value, static_cast<uint32>(offset), 0, 0}, true);
                    }
                }
            }
        }
        publication_.fetch_add(1, std::memory_order_acq_rel);
        for (std::size_t i = 0; i < metadata_.size(); ++i) {
            values_[i].store(held_[i], std::memory_order_release);
            publishedOrders_[i].store(appliedOrders_[i], std::memory_order_release);
            if (frames && !warming_) renderedAutomationOrders_[i] = appliedAutomationOrders_[i];
        }
        publication_.fetch_add(1, std::memory_order_release);
        editPublished_.store(read, std::memory_order_release);
        for (const auto& span : recorded) automationRecorder_->commit(span);
        if (recording_.active()) automationRecorder_->appliedValue(held_[slot(recording_.control)]);
        copyOutputs(frames, false);
    }
    if (frames && !warming_) for (auto& native : nativeGestures_) {
        native.pending = false;
        native.implicitFrames -= std::min(native.implicitFrames, frames);
    }
    automationPlayback_ = {};
    advanceContext(frames);
}

void Instance::setProcessContext(const host::ProcessContext& source) noexcept {
    processContext_ = source;
    ProcessContext context{};
    context.sampleRate = options_.sampleRate;
    context.continousTimeSamples = context_.continousTimeSamples;
    context.state = context_.state & ProcessContext::kContTimeValid;
    if (source.time.valid) {
        context.projectTimeSamples = source.time.frame;
        if (source.time.playing) context.state |= ProcessContext::kPlaying;
        if (source.recording) context.state |= ProcessContext::kRecording;
        if (source.hasLoop()) context.state |= ProcessContext::kCycleActive;
        if (source.hasMusic()) {
            const auto beats = [&](std::int64_t frame) { return frame * (source.tempo / (60 * options_.sampleRate)); };
            context.tempo = source.tempo;
            context.projectTimeMusic = beats(source.time.frame);
            const double bar = source.numerator * 4. / source.denominator;
            context.barPositionMusic = std::floor(context.projectTimeMusic / bar) * bar;
            context.timeSigNumerator = source.numerator; context.timeSigDenominator = source.denominator;
            context.state |= ProcessContext::kTempoValid | ProcessContext::kProjectTimeMusicValid
                | ProcessContext::kBarPositionValid | ProcessContext::kTimeSigValid;
            if (source.hasLoop()) {
                context.cycleStartMusic = beats(source.loopStart); context.cycleEndMusic = beats(source.loopEnd);
                context.state |= ProcessContext::kCycleValid;
            }
        }
    }
    context_ = context;
}

bool Instance::setContext(const ProcessContext& context) noexcept {
    constexpr auto allowed = ProcessContext::kPlaying | ProcessContext::kCycleActive | ProcessContext::kRecording
        | ProcessContext::kSystemTimeValid | ProcessContext::kContTimeValid | ProcessContext::kProjectTimeMusicValid
        | ProcessContext::kBarPositionValid | ProcessContext::kCycleValid | ProcessContext::kTempoValid
        | ProcessContext::kTimeSigValid | ProcessContext::kChordValid | ProcessContext::kSmpteValid | ProcessContext::kClockValid;
    const auto flags = context.state;
    if ((flags & ~allowed)
        || ((flags & ProcessContext::kTempoValid) && (!std::isfinite(context.tempo) || context.tempo <= 0 || context.tempo > 10000))
        || ((flags & ProcessContext::kProjectTimeMusicValid) && !std::isfinite(context.projectTimeMusic))
        || ((flags & ProcessContext::kBarPositionValid) && !std::isfinite(context.barPositionMusic))
        || ((flags & ProcessContext::kCycleValid) && (!std::isfinite(context.cycleStartMusic) || !std::isfinite(context.cycleEndMusic)
            || context.cycleEndMusic < context.cycleStartMusic))
        || ((flags & ProcessContext::kTimeSigValid) && (context.timeSigNumerator <= 0 || context.timeSigDenominator <= 0))
        || ((flags & ProcessContext::kSmpteValid) && (!context.frameRate.framesPerSecond || context.frameRate.flags > 3))) {
        diagnose(InvalidContext); return false;
    }
    context_ = context; context_.sampleRate = options_.sampleRate; processContext_ = {}; return true;
}
void Instance::advanceContext(std::uint32_t frames) noexcept {
    if (processContext_.time.playing) {
        if (processContext_.time.frame <= std::numeric_limits<std::int64_t>::max() - frames) processContext_.time.frame += frames;
        else processContext_.time.valid = false;
    }
    if (frames) context_.state &= ~(ProcessContext::kSystemTimeValid | ProcessContext::kBarPositionValid | ProcessContext::kClockValid);
    const auto advance = [&](int64& time) {
        if (time <= std::numeric_limits<int64>::max() - frames) time += frames;
        else { time = std::numeric_limits<int64>::max(); diagnose(InvalidContext); }
    };
    if (context_.state & ProcessContext::kContTimeValid) advance(context_.continousTimeSamples);
    if (context_.state & ProcessContext::kPlaying) {
        advance(context_.projectTimeSamples);
        if ((context_.state & (ProcessContext::kTempoValid | ProcessContext::kProjectTimeMusicValid))
            == (ProcessContext::kTempoValid | ProcessContext::kProjectTimeMusicValid))
            context_.projectTimeMusic += frames * context_.tempo / (60 * options_.sampleRate);
    }
}
bool Instance::beginWarmUp() noexcept {
    if (onMainThread()) { diagnose(WrongThread); return false; }
    if (!active_ || !ready() || failed_ || warming_ || processing_) { diagnose(WarmupFailed); return false; }
    warmUpContext_ = context_; warmUpProcessContext_ = processContext_; warmUpAutomationPlayback_ = automationPlayback_;
    setProcessContext({}); warming_ = true;
    return true;
}
bool Instance::warmUpBlock(std::uint32_t frames) noexcept {
    if (onMainThread()) { diagnose(WrongThread); return false; }
    if (!warming_ || !frames || frames > options_.maxBlock) { diagnose(WarmupFailed); return false; }
    run(frames);
    if (failed_) diagnose(WarmupFailed);
    return !failed_;
}
bool Instance::endWarmUp() noexcept {
    if (onMainThread()) { diagnose(WrongThread); return false; }
    if (!warming_) { diagnose(WarmupFailed); return false; }
    if (processing_) {
        if (!processingNotificationAccepted(component_->processor()->setProcessing(false))) {
            diagnose(WarmupFailed); failed_ = true;
            restart_.fetch_or(kReloadComponent, std::memory_order_relaxed);
        }
        processing_ = false;
    }
    warming_ = false; context_ = warmUpContext_; processContext_ = warmUpProcessContext_;
    automationPlayback_ = warmUpAutomationPlayback_;
    return !failed_;
}
void Instance::warmUp(std::uint32_t blocks, std::uint32_t frames) noexcept {
    if (!blocks || !frames) return;
    if (frames > options_.maxBlock) { diagnose(WarmupFailed); return; }
    const auto work = [&] {
        if (!beginWarmUp()) return;
        for (std::uint32_t i = 0; i < blocks; ++i) if (!warmUpBlock(frames)) break;
        (void)endWarmUp();
    };
    if (onMainThread()) {
        try { std::thread processing(work); processing.join(); }
        catch (...) { diagnose(WarmupFailed); }
    } else {
        work();
    }
}
}
