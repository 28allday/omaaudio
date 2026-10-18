#include "instance.h"

#include "clap/catalogue.h"
#include "session/plugin_state.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace omawave::clap {

std::unique_ptr<Instance> Instance::create(const Catalogue& catalogue, const host::PluginInfo& info,
                                           double sampleRate, std::uint32_t maxBlock, std::string& error, bool legacyControlIndices,
                                           const session::PluginState* state)
{
    error.clear();
    if (state && (!*state || state->uri() != info.uri || state->controller())) {
        error = "CLAP state has a different identity or an unsupported controller stream"; return {};
    }
    try {
    std::unique_ptr<Instance> self(new Instance());
    self->info_ = info;
    self->sampleRate_ = sampleRate;
    self->maxBlock_ = maxBlock;
    self->plugin_ = catalogue.create(info.uri, self->host_.raw());
    if (self->plugin_ == nullptr) {
        error = "plugin not installed: " + info.uri;
        return nullptr;
    }
    if (!self->plugin_->init(self->plugin_)) {
        error = info.name + " refused to initialise";
        return nullptr;
    }
    const clap_plugin_t* p = self->plugin_;
    self->params_ = static_cast<const clap_plugin_params_t*>(p->get_extension(p, CLAP_EXT_PARAMS));
    self->latencyExt_ = static_cast<const clap_plugin_latency_t*>(p->get_extension(p, CLAP_EXT_LATENCY));
    self->state_ = static_cast<const clap_plugin_state_t*>(p->get_extension(p, CLAP_EXT_STATE));
    self->gui_ = static_cast<const clap_plugin_gui_t*>(p->get_extension(p, CLAP_EXT_GUI));
    self->stateContext_ = static_cast<const clap_plugin_state_context_t*>(p->get_extension(p, CLAP_EXT_STATE_CONTEXT));
    if (state && !self->loadState(state->processor())) {
        error = info.name + " rejected its saved state"; return {};
    }

    // Ports, by the catalogue's rule: the first main pair, the first other input
    // as the key. Every port is recorded so each gets a buffer at process time.
    std::uint32_t mainIn = 0, mainOut = 0, key = 0;
    if (const auto* ports = static_cast<const clap_plugin_audio_ports_t*>(p->get_extension(p, CLAP_EXT_AUDIO_PORTS))) {
        for (bool input : {true, false}) {
            const uint32_t n = ports->count(p, input);
            bool haveMain = false, haveKey = false;
            for (uint32_t index = 0; index < n; ++index) {
                clap_audio_port_info_t pi{};
                AudioPort port;
                if (ports->get(p, index, input, &pi)) {
                    port.channels = pi.channel_count;
                    const bool main = (pi.flags & CLAP_AUDIO_PORT_IS_MAIN) != 0 || index == 0;
                    if (main && !haveMain) { port.role = PortRole::Main; haveMain = true; (input ? mainIn : mainOut) = pi.channel_count; }
                    else if (input && !haveKey) { port.role = PortRole::Key; haveKey = true; key = pi.channel_count; }
                }
                (input ? self->inPorts_ : self->outPorts_).push_back(port);
            }
        }
    }
    if (mainIn != info.requiredIn || mainOut != info.requiredOut || key != info.sidechainIn) {
        error = info.name + " reports different audio ports than when it was scanned; rescan plug-ins";
        return nullptr;
    }
    self->mainIn_.assign(mainIn, nullptr);
    self->out_.assign(mainOut, nullptr);
    self->sidechainIn_.assign(key, nullptr);
    self->silence_.assign(maxBlock, 0.0f);
    self->sink_.assign(maxBlock, 0.0f);
    self->inPtrs_.assign(mainIn, nullptr);
    self->keyPtrs_.assign(key, nullptr);
    self->outPtrs_.assign(mainOut, nullptr);
    self->inBuffers_.assign(self->inPorts_.size(), clap_audio_buffer_t{});
    self->outBuffers_.assign(self->outPorts_.size(), clap_audio_buffer_t{});
    self->inSpare_.resize(self->inPorts_.size());
    self->outSpare_.resize(self->outPorts_.size());
    for (std::size_t i = 0; i < self->inPorts_.size(); ++i)
        if (self->inPorts_[i].role == PortRole::Spare) self->inSpare_[i].assign(self->inPorts_[i].channels, self->silence_.data());
    for (std::size_t i = 0; i < self->outPorts_.size(); ++i)
        if (self->outPorts_[i].role == PortRole::Spare) self->outSpare_[i].assign(self->outPorts_[i].channels, self->sink_.data());

    // Parameters become controls, in the plugin's own order.
    if (self->params_ != nullptr) {
        const uint32_t n = self->params_->count(p);
        if (n > 16384) { error = info.name + " has too many parameters"; return {}; }
        std::unordered_set<clap_id> identities;
        for (uint32_t i = 0; i < n; ++i) {
            clap_param_info_t pi{};
            if (!self->params_->get_info(p, i, &pi)) continue;
            if (pi.id == CLAP_INVALID_ID || !identities.insert(pi.id).second
                || !std::isfinite(pi.min_value) || !std::isfinite(pi.max_value)
                || pi.min_value > pi.max_value || !std::isfinite(pi.default_value)
                || pi.default_value < pi.min_value || pi.default_value > pi.max_value
                || ((pi.flags & CLAP_PARAM_IS_STEPPED) && std::ceil(pi.min_value) > std::floor(pi.max_value))) {
                error = info.name + " has invalid parameter metadata"; return {};
            }
            const bool hidden = (pi.flags & CLAP_PARAM_IS_HIDDEN) != 0;
            self->nativeParameters_.push_back({pi.id, hidden ? SIZE_MAX : self->controls_.size(),
                pi.min_value, pi.max_value, (pi.flags & CLAP_PARAM_IS_STEPPED) != 0, pi.default_value,
                false, (pi.flags & CLAP_PARAM_IS_READONLY) != 0});
            if (hidden) continue;
            host::ControlPort port;
            port.index = legacyControlIndices ? static_cast<std::uint32_t>(self->controls_.size()) : pi.id;
            port.parameterId = pi.id;
            port.symbol = pi.name;
            port.name = pi.module[0] != '\0' ? std::string(pi.module) + " / " + pi.name : std::string(pi.name);
            port.minimum = pi.min_value;
            port.maximum = pi.max_value;
            port.defaultValue = pi.default_value;
            port.integer = (pi.flags & CLAP_PARAM_IS_STEPPED) != 0;
            port.toggled = port.integer && pi.min_value == 0.0 && pi.max_value == 1.0;
            port.output = (pi.flags & CLAP_PARAM_IS_READONLY) != 0;
            port.automatable = (pi.flags & CLAP_PARAM_IS_AUTOMATABLE) != 0;
            self->controls_.push_back(std::move(port));
            self->paramIds_.push_back(pi.id);
        }
        self->values_ = std::vector<std::atomic<double>>(self->controls_.size());
        self->pendingValues_ = std::vector<std::atomic<double>>(self->controls_.size());
        self->pendingVersions_ = std::vector<std::atomic<std::uint64_t>>(self->controls_.size());
        self->dirty_ = std::vector<std::atomic<bool>>(self->controls_.size());
        self->lastInputTimes_.assign(self->controls_.size(), UINT32_MAX);
        for (auto& parameter : self->nativeParameters_) {
            double current = parameter.value;
            parameter.known = self->params_->get_value(p, parameter.id, &current);
            if (!parameter.known) current = parameter.value;
            if (!std::isfinite(current) || current < parameter.minimum || current > parameter.maximum
                || (parameter.integer && current != std::trunc(current))) {
                error = info.name + " has an invalid current parameter value"; return {};
            }
            parameter.value = current;
            const auto i = parameter.control;
            if (i == SIZE_MAX) continue;
            self->values_[i].store(current, std::memory_order_relaxed);
            self->pendingValues_[i].store(current, std::memory_order_relaxed);
            self->pendingVersions_[i].store(0, std::memory_order_relaxed);
            self->dirty_[i].store(false, std::memory_order_relaxed);
            self->controlSlots_.emplace_back(self->controls_[i].index, i);
        }
        std::sort(self->controlSlots_.begin(), self->controlSlots_.end());
        std::sort(self->nativeParameters_.begin(), self->nativeParameters_.end(),
            [](const auto& a, const auto& b) { return a.id < b.id; });
    }
    // Room for a block-start value per parameter plus a value per frame for
    // four ramping parameters; automation widens its step beyond that.
    const std::size_t capacity = self->paramIds_.size() + static_cast<std::size_t>(maxBlock) * 4;
    self->events_.assign(capacity, clap_event_param_value_t{});
    self->playbackEvents_.assign(capacity, 0);
    self->returnEvents_.assign(capacity, 0);
    self->order_.assign(capacity, 0);
    self->inEvents_.ctx = self.get();
    self->inEvents_.size = &Instance::eventsSize;
    self->inEvents_.get = &Instance::eventsGet;
    self->outEvents_.ctx = self.get();
    self->outEvents_.try_push = &Instance::eventsTryPush;
    return self;
    } catch (const std::exception& exception) { error = info.name + ": " + exception.what(); return {}; }
      catch (...) { error = info.name + ": exception while preparing the plugin"; return {}; }
}

bool Instance::hasEditor() const noexcept
{
    return gui_ && gui_->is_api_supported && gui_->create && gui_->set_parent && gui_->destroy
        && gui_->is_api_supported(plugin_, CLAP_WINDOW_API_X11, false);
}

bool Instance::openEditor(std::uint64_t parent, host::EditorHost& host, host::EditorSize& size, std::string& error)
{
    if (editorOpen_) { error = "the effect's editor is already open"; return false; }
    if (!hasEditor() || !parent) { error = "this effect has no X11 editor of its own"; return false; }
    host_.attachEditor(&host, plugin_);
    if (!gui_->create(plugin_, CLAP_WINDOW_API_X11, false)) {
        host_.attachEditor(nullptr, nullptr);
        error = "the effect's editor could not be created"; return false;
    }
    if (gui_->set_scale) (void)gui_->set_scale(plugin_, 1.0);
    std::uint32_t width = 640, height = 400;
    if (gui_->get_size && !gui_->get_size(plugin_, &width, &height)) { width = 640; height = 400; }
    clap_window_t window{};
    window.api = CLAP_WINDOW_API_X11;
    window.x11 = static_cast<clap_xwnd>(parent);
    if (!gui_->set_parent(plugin_, &window)) {
        gui_->destroy(plugin_); host_.attachEditor(nullptr, nullptr);
        error = "the effect's editor could not attach to its window"; return false;
    }
    if (gui_->show) (void)gui_->show(plugin_);
    size = {std::max<std::uint32_t>(1, width), std::max<std::uint32_t>(1, height),
            gui_->can_resize && gui_->can_resize(plugin_)};
    editorOpen_ = true;
    return true;
}

bool Instance::resizeEditor(std::uint32_t& width, std::uint32_t& height) noexcept
{
    if (!editorOpen_ || !gui_->can_resize || !gui_->can_resize(plugin_) || !gui_->set_size) return false;
    if (gui_->adjust_size) (void)gui_->adjust_size(plugin_, &width, &height);
    return gui_->set_size(plugin_, width, height);
}

void Instance::closeEditor() noexcept
{
    if (!editorOpen_) return;
    if (gui_->hide) (void)gui_->hide(plugin_);
    gui_->destroy(plugin_);
    host_.attachEditor(nullptr, nullptr);
    editorOpen_ = false;
}

Instance::~Instance()
{
    closeEditor();
    if (plugin_ != nullptr) {
        if (active_) deactivate();
        plugin_->destroy(plugin_);
    }
}

bool Instance::connectMainAudioIn(std::uint32_t channel, float* buffer) noexcept
{
    if (channel >= mainIn_.size()) return false;
    mainIn_[channel] = buffer;
    return true;
}

bool Instance::connectSidechainIn(std::uint32_t channel, float* buffer) noexcept
{
    if (channel >= sidechainIn_.size()) return false;
    sidechainIn_[channel] = buffer;
    return true;
}

bool Instance::connectAudioIn(std::uint32_t channel, float* buffer) noexcept
{
    if (channel < mainIn_.size()) return connectMainAudioIn(channel, buffer);
    return connectSidechainIn(channel - static_cast<std::uint32_t>(mainIn_.size()), buffer);
}

bool Instance::connectAudioOut(std::uint32_t channel, float* buffer) noexcept
{
    if (channel >= out_.size()) return false;
    out_[channel] = buffer;
    return true;
}

bool Instance::ready() const noexcept
{
    // The main pair must be wired; keys and extra outputs may be left to scratch.
    for (float* p : mainIn_) if (p == nullptr) return false;
    for (float* p : out_) if (p == nullptr) return false;
    return plugin_ != nullptr;
}

void Instance::activate() noexcept
{
    if (active_ || plugin_ == nullptr) return;
    host_.clearRestartRequest();
    host_.clearLatencyChanged();
    // Values set before activation reach the plugin now, through params.flush,
    // which CLAP allows on the main thread while the plugin is inactive: a plugin
    // is entitled to read its parameters in activate() (the fixture sizes its
    // latency from one), the same courtesy Chain extends to LV2 control ports.
    flushParameters();
    const bool requested = host_.takeFlushRequest();
    if (params_ != nullptr && (eventCount_ > 0 || requested)) {
        publishInputValues();
        OutputScope output(*this, 0);
        params_->flush(plugin_, &inEvents_, &outEvents_);
    }
    eventCount_ = 0;
    sorted_ = true;
    if (!plugin_->activate(plugin_, sampleRate_, 1, maxBlock_)) return;
    active_ = true;
    processing_ = false;
    latency_.store(latencyExt_ != nullptr ? latencyExt_->get(plugin_) : 0, std::memory_order_relaxed);
}

void Instance::deactivate() noexcept
{
    if (!active_ || plugin_ == nullptr) return;
    if (processing_) {
        // The previous processor has joined/retired before deactivation. CLAP's
        // audio thread is symbolic: the main thread owns this final stop call.
        host_.markAudioThread();
        plugin_->stop_processing(plugin_);
        processing_ = false;
    }
    plugin_->deactivate(plugin_);
    active_ = false;
}

std::size_t Instance::controlSlot(std::uint32_t id) const noexcept
{
    const auto it = std::lower_bound(controlSlots_.begin(), controlSlots_.end(), id,
        [](const auto& entry, std::uint32_t key) { return entry.first < key; });
    return it != controlSlots_.end() && it->first == id ? it->second : controls_.size();
}

void Instance::setControl(std::uint32_t id, float value) noexcept
{
    setParameter(id,value);
}

void Instance::setParameter(std::uint32_t id, double value) noexcept
{
    setParameterAtVersion(id, value, parameterVersion());
}

void Instance::setParameterAtVersion(std::uint32_t id, double value, std::uint64_t version) noexcept
{
    if (version == UINT64_MAX) version = parameterVersion();
    if (version < parameterVersion()) return;
    const auto index = controlSlot(id);
    if (index >= controls_.size() || controls_[index].output || !std::isfinite(value)) return;
    const auto& port = controls_[index];
    value = port.clamp(value);
    if (port.integer) value = std::clamp(std::trunc(value),std::ceil(port.minimum),std::floor(port.maximum));
    values_[index].store(value, std::memory_order_relaxed);
    pendingValues_[index].store(value, std::memory_order_relaxed);
    pendingVersions_[index].store(version, std::memory_order_relaxed);
    dirty_[index].store(true, std::memory_order_release);
}

float Instance::controlValue(std::uint32_t id) const noexcept
{
    const double limit = std::numeric_limits<float>::max();
    return static_cast<float>(std::clamp(parameterValue(id),-limit,limit));
}

double Instance::parameterValue(std::uint32_t id) const noexcept
{
    const auto index = controlSlot(id);
    if (index >= controls_.size()) return 0.0f;
    return values_[index].load(std::memory_order_relaxed);
}

std::string Instance::parameterText(std::uint32_t id, double value) const
{
    if (!host_.onMainThread() || !params_ || !params_->value_to_text || !std::isfinite(value)) return {};
    const auto slot = controlSlot(id);
    if (slot >= controls_.size() || value < controls_[slot].minimum || value > controls_[slot].maximum) return {};
    try {
        // Bounded stack storage, with explicit termination validation. Formatting
        // is main-thread work even while process() runs; never an audio query.
        std::array<char, 1024> buffer{};
        if (!params_->value_to_text(plugin_, paramIds_[slot], value, buffer.data(), buffer.size())) return {};
        const auto end = std::find(buffer.begin(), buffer.end(), '\0');
        if (end == buffer.end()) return {};
        return {buffer.begin(), end};
    } catch (...) { return {}; }
}

bool Instance::takeParameterTextChanged() noexcept
{
    if (!host_.onMainThread()) {
        notificationDiagnostics_.fetch_or(WrongNotificationThread, std::memory_order_relaxed); return false;
    }
    return host_.takeTextRescanRequest();
}

uint32_t Instance::eventsSize(const clap_input_events_t* list) noexcept
{
    return static_cast<const Instance*>(list->ctx)->eventCount_;
}

const clap_event_header_t* Instance::eventsGet(const clap_input_events_t* list, uint32_t index) noexcept
{
    const auto* self = static_cast<const Instance*>(list->ctx);
    return index < self->eventCount_ ? &self->events_[self->order_[index]].header : nullptr;
}

bool Instance::eventsTryPush(const clap_output_events_t* list, const clap_event_header_t* header) noexcept
{
    auto& self = *static_cast<Instance*>(list->ctx);
    auto reject = [&](NotificationDiagnostic reason) {
        self.notificationDiagnostics_.fetch_or(reason, std::memory_order_relaxed); return false;
    };
    if (!header || !self.acceptingOutput_ || header->size < sizeof(*header)
        || header->space_id != CLAP_CORE_EVENT_SPACE_ID || header->time < self.lastOutputTime_
        || (self.outputFrames_ ? header->time >= self.outputFrames_ : header->time != 0))
        return reject(InvalidOutput);
    ParameterEvent event{};
    event.offset = header->time; event.flags = header->flags;
    if (header->type == CLAP_EVENT_PARAM_VALUE) {
        if (header->size < sizeof(clap_event_param_value_t)) return reject(InvalidOutput);
        const auto& value = *reinterpret_cast<const clap_event_param_value_t*>(header);
        if (value.note_id != -1 || value.port_index != -1 || value.channel != -1 || value.key != -1
            || !std::isfinite(value.value)) return reject(InvalidOutput);
        event.kind = EventKind::Value; event.id = value.param_id; event.value = value.value;
    } else if (header->type == CLAP_EVENT_PARAM_GESTURE_BEGIN || header->type == CLAP_EVENT_PARAM_GESTURE_END) {
        if (header->size < sizeof(clap_event_param_gesture_t)) return reject(InvalidOutput);
        event.kind = header->type == CLAP_EVENT_PARAM_GESTURE_BEGIN ? EventKind::Begin : EventKind::End;
        event.id = reinterpret_cast<const clap_event_param_gesture_t*>(header)->param_id;
    } else return reject(InvalidOutput);
    const auto it = std::lower_bound(self.nativeParameters_.begin(), self.nativeParameters_.end(), event.id,
        [](const auto& parameter, clap_id id) { return parameter.id < id; });
    if (it == self.nativeParameters_.end() || it->id != event.id) return reject(InvalidOutput);
    if (event.kind == EventKind::Value && (event.value < it->minimum || event.value > it->maximum
        || (it->integer && event.value != std::trunc(event.value)))) return reject(InvalidOutput);
    const auto slot = it->control;
    if (slot != SIZE_MAX) event.control = self.controls_[slot].index;
    self.advanceFeedbackInputs(event.offset);
    // This input already took effect at the native event's sample. A following
    // native value can replace its last point; an unrelated event loses none.
    self.publishAutomatchSpan(event.offset + 1);
    if (self.recording_.active() && event.control == self.recording_.control && event.kind != EventKind::End)
        self.finishRecordingRelease(event.offset);
    event.time = self.outputTime_;
    if (event.time.playing && event.time.valid) {
        if (event.time.frame <= std::numeric_limits<std::int64_t>::max() - event.offset)
            event.time.frame += event.offset;
        else event.time.valid = false;
    }
    event.recordable = slot != SIZE_MAX && self.controls_[slot].automatable
        && !it->readOnly && !(event.flags & CLAP_EVENT_DONT_RECORD);
    if (event.kind != EventKind::Value) {
        event.value = it->value;
        event.recordable = event.recordable && it->known;
    }
    if (self.recording_.active() && event.control == self.recording_.control) {
        if (event.kind == EventKind::End) {
            if (self.recordingTouch_ && !self.recording_.manualActive
                && self.recording_.mode == host::AutomationMode::Touch && self.recording_.automatchFrames
                && !self.recordingFeedback_.followingRead)
                self.automatch_.pending = true;
            event.automationEndDeferred = self.recordingFeedback_.endPending || (self.outputFrames_ && self.recordingTouch_
                && self.recording_.mode == host::AutomationMode::Touch && !self.recording_.manualActive
                && !self.recordingFeedback_.followingRead);
            self.recordingFeedback_.endPending = event.automationEndDeferred;
            self.recordingTouch_ = false;
        }
        if (event.recordable) {
            if (event.kind != EventKind::End) self.automatch_.pending = self.automatch_.active = false;
            if (event.kind == EventKind::Begin) self.recordingTouch_ = true;
            if (self.recording_.mode == host::AutomationMode::Latch
                && (event.kind == EventKind::Begin || event.kind == EventKind::Value)) self.recordingLatched_ = true;
            if (event.kind != EventKind::End && (self.recordingTouch_ || self.recordingLatched_
                || self.recording_.manualActive || self.recording_.mode == host::AutomationMode::Write)) {
                self.recordingFeedback_.followingRead = false;
                self.recordingFeedback_.value = event.value;
                self.recordingFeedback_.valueKnown = true;
            }
        }
    }
    if (event.kind == EventKind::Begin) it->gesturing = true;
    if (event.kind == EventKind::End) it->gesturing = false;
    const bool readout = event.kind == EventKind::Value && it->readOnly && !it->gesturing;
    bool changed = true;
    if (event.kind == EventKind::Value) {
        // Compare at this sample, not with the UI cache's final/pending value.
        // Each sorted input is visited at most once per process/flush call.
        changed = !it->known || event.value != it->value;
        it->value = event.value; it->known = true;
        // A read-only value outside a gesture still needs state verification,
        // but does not need one edit-queue entry per meter sample. Writable and
        // explicitly gestured changes retain their ordinary edit boundaries.
        // LIVE/DONT_RECORD do not decide persistence.
        if (changed) (readout ? self.readoutStatePending_ : self.nativeStateDirty_)
            .store(true, std::memory_order_release);
    }
    if (event.kind == EventKind::Value && slot != SIZE_MAX
        && (self.lastInputTimes_[slot] == UINT32_MAX || event.offset >= self.lastInputTimes_[slot])
        && !self.dirty_[slot].load(std::memory_order_acquire))
        self.values_[slot].store(event.value, std::memory_order_relaxed);
    self.lastOutputTime_ = header->time;
    // Empty gestures still retain their boundaries. Identical value echoes need
    // no additional history/capture and must not exhaust that bounded queue.
    if (!changed || readout) return true;
    if (!self.notifications_.push(event)) {
        self.notificationOverflow_.store(true, std::memory_order_release);
        return reject(OutputOverflow);
    }
    return true;
}

void Instance::advanceFeedbackInputs(std::uint32_t offset) noexcept
{
    while (feedbackInput_ < eventCount_) {
        const auto& event = events_[order_[feedbackInput_]];
        if (event.header.time > offset) break;
        const auto it = std::lower_bound(nativeParameters_.begin(), nativeParameters_.end(), event.param_id,
            [](const auto& parameter, clap_id id) { return parameter.id < id; });
        if (it != nativeParameters_.end() && it->id == event.param_id) {
            if (returnEvents_[order_[feedbackInput_]] && recording_.active()
                && it->control != SIZE_MAX && controls_[it->control].index == recording_.control) {
                if (automatch_.spanStart == UINT32_MAX) automatch_.spanStart = event.header.time;
                recordingFeedback_.followingRead = true;
            } else if (playbackEvents_[order_[feedbackInput_]] && recording_.active()
                && it->control != SIZE_MAX && controls_[it->control].index == recording_.control) {
                publishAutomatchSpan(event.header.time);
                if (recordingFeedback_.endPending) finishRecordingRelease(event.header.time);
                else if (!recordingFeedback_.followingRead)
                    publishRecordingFeedback(host::AutomationFeedback::Read, event.header.time);
                recordingFeedback_.followingRead = true;
            }
            it->value = event.value; it->known = true;
        }
        ++feedbackInput_;
    }
}

void Instance::invalidateFeedbackValues() noexcept
{
    for (auto& parameter : nativeParameters_) parameter.known = false;
}

bool Instance::readNativeValues(std::vector<double>& current) const
{
    current.clear(); current.reserve(nativeParameters_.size());
    for (const auto& parameter : nativeParameters_) {
        double value = 0;
        if (!params_ || !params_->get_value || !params_->get_value(plugin_, parameter.id, &value)
            || !std::isfinite(value) || value < parameter.minimum || value > parameter.maximum
            || (parameter.integer && value != std::trunc(value))) return false;
        current.push_back(value);
    }
    return true;
}

void Instance::publishNativeValues(const std::vector<double>& current) noexcept
{
    for (std::size_t i = 0; i < current.size(); ++i) {
        auto& parameter = nativeParameters_[i];
        parameter.value = current[i]; parameter.known = true;
        if (parameter.control == SIZE_MAX) continue;
        values_[parameter.control].store(current[i], std::memory_order_relaxed);
        pendingValues_[parameter.control].store(current[i], std::memory_order_relaxed);
    }
}

bool Instance::nextParameterEvent(ParameterEvent& event) noexcept
{
    if (!host_.onMainThread()) {
        notificationDiagnostics_.fetch_or(WrongNotificationThread, std::memory_order_relaxed); return false;
    }
    return notifications_.pop(event);
}

bool Instance::takeStateDirty() noexcept
{
    const auto changes = takeStateChanges();
    return changes.edited || changes.readout;
}

host::StateChanges Instance::takeStateChanges() noexcept
{
    if (!host_.onMainThread()) {
        notificationDiagnostics_.fetch_or(WrongNotificationThread, std::memory_order_relaxed); return {};
    }
    const bool native = nativeStateDirty_.exchange(false, std::memory_order_acq_rel);
    const bool readout = readoutStatePending_.exchange(false, std::memory_order_acq_rel);
    const bool independent = host_.takeStateDirty();
    return {independent || native, readout, native && !independent};
}

bool Instance::takeNotificationOverflow() noexcept
{
    if (!host_.onMainThread()) {
        notificationDiagnostics_.fetch_or(WrongNotificationThread, std::memory_order_relaxed); return false;
    }
    return notificationOverflow_.exchange(false, std::memory_order_acq_rel);
}

bool Instance::serviceMainThread() noexcept
{
    if (!host_.onMainThread()) {
        notificationDiagnostics_.fetch_or(WrongNotificationThread, std::memory_order_relaxed); return false;
    }
    if (servicingMain_) return false;
    if (!host_.takeCallbackRequest()) return true;
    servicingMain_ = true;
    bool ok = false;
    try { if (plugin_ && plugin_->on_main_thread) { plugin_->on_main_thread(plugin_); ok = true; } }
    catch (...) {}
    servicingMain_ = false;
    if (!ok) notificationDiagnostics_.fetch_or(MainCallbackFailed, std::memory_order_relaxed);
    return ok;
}

void Instance::publishInputValues() noexcept
{
    std::fill(lastInputTimes_.begin(), lastInputTimes_.end(), UINT32_MAX);
    for (std::uint32_t i = 0; i < eventCount_; ++i) {
        const auto& event = events_[order_[i]];
        const auto it = std::lower_bound(nativeParameters_.begin(), nativeParameters_.end(), event.param_id,
            [](const auto& parameter, clap_id id) { return parameter.id < id; });
        if (it == nativeParameters_.end() || it->id != event.param_id || it->control == SIZE_MAX) continue;
        const auto slot = it->control;
        lastInputTimes_[slot] = event.header.time;
        if (!dirty_[slot].load(std::memory_order_acquire)) values_[slot].store(event.value, std::memory_order_relaxed);
    }
}

void Instance::pushEvent(std::size_t param, double value, std::uint32_t offset, bool playback, bool returning) noexcept
{
    if (eventCount_ >= events_.size()) return;
    clap_event_param_value_t& e = events_[eventCount_];
    e.header.size = sizeof e;
    e.header.time = offset;
    e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    e.header.type = CLAP_EVENT_PARAM_VALUE;
    e.header.flags = 0;
    e.param_id = paramIds_[param];
    e.cookie = nullptr;
    e.note_id = -1;
    e.port_index = -1;
    e.channel = -1;
    e.key = -1;
    e.value = value;
    playbackEvents_[eventCount_] = playback;
    returnEvents_[eventCount_] = returning;
    order_[eventCount_] = eventCount_;
    if (eventCount_ > 0 && events_[order_[eventCount_ - 1]].header.time > offset) sorted_ = false;
    ++eventCount_;
}

void Instance::sortEvents() noexcept
{
    if (sorted_) return;
    // Indices sorted by (time, arrival), so two values for one parameter at the
    // same frame keep the order they were given in. No allocation: the index
    // array is preallocated and std::sort works in place.
    const auto* events = events_.data();
    std::sort(order_.begin(), order_.begin() + eventCount_, [events](std::uint32_t a, std::uint32_t b) {
        return events[a].header.time != events[b].header.time ? events[a].header.time < events[b].header.time : a < b;
    });
    sorted_ = true;
}

void Instance::setAutomationRecording(const host::AutomationRecording& requested) noexcept
{
    auto next = requested;
    const auto index = controlSlot(next.control);
    if (!next.active() || index >= controls_.size() || controls_[index].output || !controls_[index].automatable
        || !std::isfinite(next.manualValue))
        next = {};
    else {
        const auto& port = controls_[index];
        next.manualValue = port.clamp(next.manualValue);
        if (port.integer) next.manualValue = std::clamp(std::trunc(next.manualValue), std::ceil(port.minimum), std::floor(port.maximum));
    }
    if (next.active() && next.manualPending && next.manualRevision != next.armRevision
        && (next.pass != appliedRecording_.pass || next.control != appliedRecording_.control
            || next.mode != appliedRecording_.mode || next.manualRevision != appliedRecording_.manualRevision))
        next.manualActive = true;
    const bool newPass = next.pass != recording_.pass || next.control != recording_.control || next.mode != recording_.mode;
    const bool manualChanged = next.manualRevision != recording_.manualRevision;
    if (newPass) {
        recordingTouch_ = recordingLatched_ = false;
        recordingFeedback_ = {};
        automatch_ = {};
    }
    if (!newPass && recording_.manualActive && !next.manualActive && !recordingTouch_
        && next.mode == host::AutomationMode::Touch && next.automatchFrames)
        automatch_.pending = true;
    if (next.manualActive) automatch_.pending = automatch_.active = false;
    if (manualChanged) recordingFeedback_.valueKnown = false;
    recording_ = next;
    // Taking ownership without moving the control must retain the value the
    // processor is already using, not replay an older host request.
    if (next.active() && (newPass || manualChanged)
        && (next.mode == host::AutomationMode::Write || next.manualActive))
        setParameter(next.control, next.manualValue);
}

void Instance::publishRecordingFeedback(host::AutomationFeedback feedback, std::uint32_t offset, double value, bool recordable) noexcept
{
    auto time = processContext_.recordTime();
    if (!recording_.active() || !time.valid || !time.playing
        || time.frame > std::numeric_limits<std::int64_t>::max() - offset) return;
    time.frame += offset;
    ParameterEvent event{};
    event.kind = feedback == host::AutomationFeedback::End ? EventKind::End : EventKind::Value;
    event.control = recording_.control; event.value = value; event.offset = offset; event.time = time;
    event.automationPass = recording_.pass; event.automationFeedback = feedback; event.recordable = recordable;
    if (!notifications_.push(event)) {
        notificationOverflow_.store(true, std::memory_order_release);
        notificationDiagnostics_.fetch_or(OutputOverflow, std::memory_order_relaxed);
    }
}

void Instance::finishRecordingRelease(std::uint32_t offset, bool recordable) noexcept
{
    if (!recordingFeedback_.endPending) return;
    publishRecordingFeedback(host::AutomationFeedback::End, offset, 0, recordable);
    recordingFeedback_.endPending = false;
    recordingFeedback_.followingRead = true;
}

bool Instance::restoreRecordingValue() noexcept
{
    if (!recording_.active() || !recordingFeedback_.valueKnown
        || (!recordingTouch_ && !recordingLatched_) || !recordingFeedback_.followingRead) return false;
    const auto index = controlSlot(recording_.control);
    if (index >= controls_.size() || dirty_[index].load(std::memory_order_acquire)
        || eventCount_ >= events_.size()) return false;
    // A process input list is immutable once delivered. Restore a native value
    // displaced by that call's queued Read inputs at the next segment instead.
    pushEvent(index, recordingFeedback_.value, 0);
    recordingFeedback_.followingRead = false;
    return true;
}

bool Instance::automationPlaybackAllowed(std::uint32_t id) const noexcept
{
    return !recording_.active() || id != recording_.control
        || (recording_.mode != host::AutomationMode::Write && !recording_.manualActive
            && !recordingTouch_ && !automatch_.pending && !automatch_.active
            && !(recording_.mode == host::AutomationMode::Latch && recordingLatched_));
}

double Instance::automatchRunValue(std::uint32_t offset) const noexcept
{
    const auto elapsed = std::min<std::uint64_t>(std::uint64_t(automatch_.runElapsed) + offset, automatch_.runDuration);
    return automatch_.runDuration ? std::lerp(automatch_.runFrom, automatch_.runTo,
        static_cast<double>(elapsed) / automatch_.runDuration) : automatch_.runTo;
}

void Instance::prepareAutomatch(std::uint32_t frames) noexcept
{
    automatch_.spanStart = UINT32_MAX;
    if ((!automatch_.pending && !automatch_.active) || !frames) return;
    const auto time = processContext_.recordTime();
    const auto index = controlSlot(recording_.control);
    if (!recording_.active() || !automationPlayback_.valueAt || index >= controls_.size()) {
        automatch_ = {}; return;
    }
    const auto mapped = [&](std::int64_t frame) {
        if (processContext_.hasLoop() && frame >= processContext_.loopEnd)
            return processContext_.loopStart + (frame - processContext_.loopStart)
                % (processContext_.loopEnd - processContext_.loopStart);
        return frame;
    };
    if (automatch_.identity.valid && (time.document != automatch_.identity.document
        || time.playbackEpoch != automatch_.identity.playbackEpoch
        || time.frame != mapped(automatch_.nextFrame))) automatch_.pending = automatch_.active = false;
    if (!time.valid || !time.playing) automatch_.pending = automatch_.active = false;
    if (automatch_.pending) {
        automatch_.pending = false;
        const auto id = paramIds_[index];
        const auto parameter = std::lower_bound(nativeParameters_.begin(), nativeParameters_.end(), id,
            [](const auto& current, clap_id key) { return current.id < key; });
        if (parameter != nativeParameters_.end() && parameter->id == id && parameter->known
            && time.frame <= std::numeric_limits<std::int64_t>::max() - recording_.automatchFrames) {
            automatch_.from = parameter->value;
            automatch_.to = automationPlayback_.valueAt(automationPlayback_.context,
                mapped(time.frame + recording_.automatchFrames));
            automatch_.duration = recording_.automatchFrames; automatch_.elapsed = 0;
            automatch_.active = automatch_.duration && std::isfinite(automatch_.to);
        }
    }
    automatch_.identity = time;
    automatch_.runFrom = automatch_.from; automatch_.runTo = automatch_.to;
    automatch_.runElapsed = automatch_.elapsed; automatch_.runDuration = automatch_.duration;
    const auto remaining = automatch_.active ? automatch_.duration - automatch_.elapsed : 0;
    const auto& port = controls_[index];
    // timing() reserves this capacity before scheduling other controls. Never
    // overwrite a live input list or allocate an emergency event buffer.
    if (events_.size() - eventCount_ < frames) {
        automatch_.active = false;
        notificationDiagnostics_.fetch_or(OutputOverflow, std::memory_order_relaxed);
        return;
    }
    for (std::uint32_t offset = 0; offset < frames; ++offset) {
        if (time.frame > std::numeric_limits<std::int64_t>::max() - offset) break;
        const bool returning = offset < remaining;
        double value = returning ? automatchRunValue(offset)
            : automationPlayback_.valueAt(automationPlayback_.context, time.frame + offset);
        value = port.clamp(value);
        if (port.toggled) value = value >= .5 ? 1 : 0;
        else if (port.integer) value = std::clamp(std::round(value), std::ceil(port.minimum), std::floor(port.maximum));
        pushEvent(index, value, offset, !returning, returning);
    }
}

void Instance::publishAutomatchSpan(std::uint32_t end) noexcept
{
    const auto start = automatch_.spanStart;
    automatch_.spanStart = UINT32_MAX;
    if (start == UINT32_MAX || end <= start) return;
    auto time = processContext_.recordTime();
    if (!recording_.active() || !time.valid || !time.playing
        || time.frame > std::numeric_limits<std::int64_t>::max() - end) return;
    time.frame += start;
    ParameterEvent event{};
    event.kind = EventKind::Value; event.control = recording_.control; event.time = time;
    event.offset = start; event.automationPass = recording_.pass; event.recordable = true;
    event.automationFeedback = host::AutomationFeedback::Ramp;
    event.value = automatchRunValue(start); event.automationEndValue = automatchRunValue(end - 1);
    event.automationSpanFrames = end - start;
    event.automationStep = (automatch_.runTo - automatch_.runFrom) / automatch_.runDuration;
    if (!notifications_.push(event)) {
        notificationOverflow_.store(true, std::memory_order_release);
        notificationDiagnostics_.fetch_or(OutputOverflow, std::memory_order_relaxed);
    }
}

void Instance::publishRecordingEvents() noexcept
{
    if (!recording_.active()) { appliedRecording_ = {}; return; }
    const auto time = processContext_.recordTime();
    if (!time.valid || !time.playing) return;
    const bool newPass = recording_.pass != appliedRecording_.pass || recording_.control != appliedRecording_.control
        || recording_.mode != appliedRecording_.mode;
    const bool started = recording_.manualActive && (newPass || !appliedRecording_.manualActive);
    const bool changed = (newPass || started || recording_.manualRevision != appliedRecording_.manualRevision)
        && (recording_.mode == host::AutomationMode::Write || recording_.manualActive);
    const bool ended = !newPass && !recording_.manualActive && appliedRecording_.manualActive;
    if (!started && !changed && !ended) { appliedRecording_ = recording_; return; }
    const auto index = controlSlot(recording_.control);
    const auto id = index < paramIds_.size() ? paramIds_[index] : UINT32_MAX;
    const auto parameter = std::lower_bound(nativeParameters_.begin(), nativeParameters_.end(), id,
        [](const auto& current, clap_id key) { return current.id < key; });
    bool known = parameter != nativeParameters_.end() && parameter->id == id && parameter->known;
    double value = known ? parameter->value : 0;
    // The UI cache may already contain a later queued value. A gesture begins
    // at sample zero: combine the processing owner's previous value with only
    // the sorted inputs applied at that sample, without consuming feedback.
    for (std::uint32_t i = 0; i < eventCount_; ++i) {
        const auto& input = events_[order_[i]];
        if (input.header.time != 0) break;
        if (input.param_id == id) { value = input.value; known = true; }
    }
    if ((started || changed) && !known) return;
    if (started || changed) {
        recordingFeedback_.followingRead = false;
        recordingFeedback_.value = value;
        recordingFeedback_.valueKnown = true;
    }
    const auto emit = [&](EventKind kind) {
        ParameterEvent event{};
        event.kind = kind; event.control = recording_.control;
        event.id = id; event.value = value; event.time = time;
        event.automationPass = recording_.pass; event.recordable = known;
        event.automationRevision = recording_.manualRevision;
        if (!notifications_.push(event)) {
            notificationOverflow_.store(true, std::memory_order_release);
            notificationDiagnostics_.fetch_or(OutputOverflow, std::memory_order_relaxed);
        }
    };
    if (started) emit(EventKind::Begin);
    if (changed) emit(EventKind::Value);
    if (ended) emit(EventKind::End);
    appliedRecording_ = recording_;
}

bool Instance::scheduleControl(std::uint32_t id, float value, std::uint32_t offset) noexcept
{
    return scheduleParameter(id,value,offset);
}

bool Instance::scheduleParameter(std::uint32_t id, double value, std::uint32_t offset) noexcept
{
    const auto index = controlSlot(id);
    if (index >= controls_.size() || controls_[index].output || !std::isfinite(value)) return false;
    value = controls_[index].clamp(value);
    if (controls_[index].integer) value = std::clamp(std::trunc(value),std::ceil(controls_[index].minimum),std::floor(controls_[index].maximum));
    values_[index].store(value, std::memory_order_relaxed);
    if (offset >= maxBlock_) offset = maxBlock_ - 1;
    if (eventCount_ >= timing().eventCapacity) {
        // Over budget: the value waits as an ordinary knob move for the first
        // flush that has room.
        pendingValues_[index].store(value, std::memory_order_relaxed);
        // Scheduled automation still takes effect after a native preset. It is
        // not an older untimed knob command merely because the buffer was full.
        pendingVersions_[index].store(UINT64_MAX, std::memory_order_relaxed);
        dirty_[index].store(true, std::memory_order_release);
        return false;
    }
    dirty_[index].store(false, std::memory_order_relaxed);   // the event carries it
    pushEvent(index, value, offset, true);
    return true;
}

host::Timing Instance::timing() const noexcept
{
    // Keep a dedicated block of the existing preallocated storage for a Touch
    // return. Exact keyframes and other controls cannot consume this reserve.
    const auto reserve = recording_.active() && recording_.mode == host::AutomationMode::Touch
        && recording_.automatchFrames ? maxBlock_ : 0;
    return {true, 1, static_cast<std::uint32_t>(events_.size() - reserve)};
}

void Instance::flushParameters() noexcept
{
    // Values set through setControl() since the last run go first, at offset 0;
    // scheduled events are already in the list.
    for (std::size_t i = 0; i < paramIds_.size(); ++i) {
        if (eventCount_ >= events_.size()) break;   // no room: these stay dirty for the next block
        if (!dirty_[i].exchange(false, std::memory_order_acq_rel)) continue;
        if (pendingVersions_[i].load(std::memory_order_relaxed) < parameterVersion()) continue;
        pushEvent(i, pendingValues_[i].load(std::memory_order_relaxed), 0);
    }
    sortEvents();
}

void Instance::run(std::uint32_t frames) noexcept
{
    if (plugin_ == nullptr || !active_) return;
    if (frames > maxBlock_) frames = maxBlock_;
    for (std::size_t c = 0; c < mainIn_.size(); ++c) inPtrs_[c] = mainIn_[c] != nullptr ? mainIn_[c] : silence_.data();
    for (std::size_t c = 0; c < sidechainIn_.size(); ++c) keyPtrs_[c] = sidechainIn_[c] != nullptr ? sidechainIn_[c] : silence_.data();
    for (std::size_t c = 0; c < out_.size(); ++c) outPtrs_[c] = out_[c] != nullptr ? out_[c] : sink_.data();
    if (!processing_) {
        host_.markAudioThread();
        // A plugin that will not start leaves the track's audio as it came in.
        if (!plugin_->start_processing(plugin_)) { passThrough(frames); return; }
        processing_ = true;
    }
    (void)host_.takeFlushRequest();
    const bool restoredRecording = restoreRecordingValue();
    prepareAutomatch(frames);
    flushParameters();
    publishInputValues();
    publishRecordingEvents();
    if (restoredRecording)
        publishRecordingFeedback(host::AutomationFeedback::Value, 0, recordingFeedback_.value);

    for (std::size_t i = 0; i < inPorts_.size(); ++i) {
        clap_audio_buffer_t& buffer = inBuffers_[i];
        buffer = clap_audio_buffer_t{};
        buffer.channel_count = inPorts_[i].channels;
        buffer.data32 = inPorts_[i].role == PortRole::Main ? const_cast<float**>(inPtrs_.data())
                      : inPorts_[i].role == PortRole::Key ? const_cast<float**>(keyPtrs_.data())
                      : inSpare_[i].data();
    }
    for (std::size_t i = 0; i < outPorts_.size(); ++i) {
        clap_audio_buffer_t& buffer = outBuffers_[i];
        buffer = clap_audio_buffer_t{};
        buffer.channel_count = outPorts_[i].channels;
        buffer.data32 = outPorts_[i].role == PortRole::Main ? outPtrs_.data() : outSpare_[i].data();
    }

    const auto transport = transportContext();
    clap_process_t process{};
    process.steady_time = steadyTime_;
    process.frames_count = frames;
    process.transport = processContext_.time.valid ? &transport : nullptr;
    process.audio_inputs = inBuffers_.data();
    process.audio_inputs_count = static_cast<uint32_t>(inBuffers_.size());
    process.audio_outputs = outBuffers_.data();
    process.audio_outputs_count = static_cast<uint32_t>(outBuffers_.size());
    process.in_events = &inEvents_;
    process.out_events = &outEvents_;
    clap_process_status status;
    { OutputScope output(*this, frames, processContext_.recordTime()); status = plugin_->process(plugin_, &process); }
    if (status != CLAP_PROCESS_ERROR) publishAutomatchSpan(frames);
    else automatch_ = {};
    if (automatch_.active) {
        automatch_.elapsed += std::min(frames, automatch_.duration - automatch_.elapsed);
        automatch_.active = automatch_.elapsed < automatch_.duration;
    }
    // A release can still be pending between segments. It belongs to the same
    // completed timeline as an active return, including before its first sample.
    automatch_.identity = processContext_.recordTime();
    if (processContext_.time.frame <= std::numeric_limits<std::int64_t>::max() - frames)
        automatch_.nextFrame = processContext_.time.frame + frames;
    else automatch_.pending = automatch_.active = false;
    automationPlayback_ = {};
    // Even a failed block must release recorder ownership. It cannot extend a
    // captured span through samples the processor declined to produce.
    finishRecordingRelease(frames, status != CLAP_PROCESS_ERROR);
    if (status == CLAP_PROCESS_ERROR) recordingFeedback_ = {};
    delivered_ = eventCount_;
    eventCount_ = 0;
    sorted_ = true;
    // The plugin declined this block: pass the input through rather than leave garbage.
    if (status == CLAP_PROCESS_ERROR) { invalidateFeedbackValues(); passThrough(frames); }
    steadyTime_ += frames;
}

clap_event_transport_t Instance::transportContext() const noexcept
{
    const auto& source = processContext_;
    clap_event_transport_t result{};
    result.header = {sizeof(result), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0};
    if (!source.time.valid) return result;
    if (source.time.playing) result.flags |= CLAP_TRANSPORT_IS_PLAYING;
    if (source.recording) result.flags |= CLAP_TRANSPORT_IS_RECORDING;
    if (source.preroll) result.flags |= CLAP_TRANSPORT_IS_WITHIN_PRE_ROLL;
    // The native fixed-point domain is smaller than the host sample domain.
    // Leave unavailable fields unflagged instead of narrowing out of range.
    const auto fixed = [](long double value, std::int64_t factor, std::int64_t& out) {
        const auto scaled = value * factor;
        if (!std::isfinite(scaled) || scaled < -9223372036854775808.L || scaled >= 9223372036854775808.L)
            return false;
        out = static_cast<std::int64_t>(scaled); return true;
    };
    const auto seconds = [&](std::int64_t frame, clap_sectime& out) {
        return fixed(static_cast<long double>(frame) / sampleRate_, CLAP_SECTIME_FACTOR, out);
    };
    if (seconds(source.time.frame, result.song_pos_seconds)
        && (!source.hasLoop() || (seconds(source.loopStart, result.loop_start_seconds)
                              && seconds(source.loopEnd, result.loop_end_seconds))))
        result.flags |= CLAP_TRANSPORT_HAS_SECONDS_TIMELINE;
    if (source.hasMusic()) {
        result.tempo = source.tempo; result.tsig_num = source.numerator; result.tsig_denom = source.denominator;
        result.flags |= CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
        const auto beats = [&](std::int64_t frame) {
            return static_cast<long double>(frame) * source.tempo / (60 * sampleRate_);
        };
        const auto position = beats(source.time.frame);
        const long double barLength = source.numerator * 4.L / source.denominator;
        const auto bar = std::floor(position / barLength);
        if (bar >= INT32_MIN && bar <= INT32_MAX
            && fixed(position, CLAP_BEATTIME_FACTOR, result.song_pos_beats)
            && fixed(bar * barLength, CLAP_BEATTIME_FACTOR, result.bar_start)
            && (!source.hasLoop() || (fixed(beats(source.loopStart), CLAP_BEATTIME_FACTOR, result.loop_start_beats)
                                  && fixed(beats(source.loopEnd), CLAP_BEATTIME_FACTOR, result.loop_end_beats)))) {
            result.bar_number = static_cast<std::int32_t>(bar);
            result.flags |= CLAP_TRANSPORT_HAS_BEATS_TIMELINE;
        }
    }
    if (source.hasLoop() && (result.flags & (CLAP_TRANSPORT_HAS_SECONDS_TIMELINE | CLAP_TRANSPORT_HAS_BEATS_TIMELINE)))
        result.flags |= CLAP_TRANSPORT_IS_LOOP_ACTIVE;
    return result;
}

void Instance::passThrough(std::uint32_t frames) noexcept
{
    for (std::size_t c = 0; c < out_.size() && c < inPtrs_.size(); ++c)
        if (out_[c] != nullptr && inPtrs_[c] != nullptr && out_[c] != inPtrs_[c])
            std::memcpy(out_[c], inPtrs_[c], frames * sizeof(float));
}

void Instance::warmUp(std::uint32_t blocks, std::uint32_t frames) noexcept
{
    if (!active_) return;
    const auto previousContext = processContext_; processContext_ = {};
    const auto previousRecording = recording_; recording_ = {};
    const auto previousApplied = appliedRecording_; appliedRecording_ = {};
    const bool previousTouch = recordingTouch_, previousLatch = recordingLatched_;
    const auto previousFeedback = recordingFeedback_; recordingFeedback_ = {};
    const auto previousAutomatch = automatch_; automatch_ = {};
    const auto previousPlayback = automationPlayback_; automationPlayback_ = {};
    recordingTouch_ = recordingLatched_ = false;
    frames = std::min(frames, maxBlock_);
    for (std::size_t c = 0; c < mainIn_.size(); ++c)
        if (mainIn_[c] != nullptr) std::fill_n(mainIn_[c], frames, 0.0f);
    for (std::uint32_t b = 0; b < blocks; ++b) run(frames);
    // Warm-up ran on this thread; the real audio thread announces itself on its first run.
    if (processing_) { plugin_->stop_processing(plugin_); processing_ = false; }
    steadyTime_ = 0;
    processContext_ = previousContext;
    recording_ = previousRecording; recordingTouch_ = previousTouch; recordingLatched_ = previousLatch;
    appliedRecording_ = previousApplied;
    recordingFeedback_ = previousFeedback;
    automatch_ = previousAutomatch; automationPlayback_ = previousPlayback;
}

namespace {
struct VectorStream {
    std::vector<std::uint8_t>* out = nullptr;
    std::span<const std::uint8_t> in;
    std::size_t position = 0;
    bool failed = false;
};
int64_t writeBytes(const clap_ostream_t* stream, const void* buffer, uint64_t size) noexcept
{
    auto* s = static_cast<VectorStream*>(stream->ctx);
    if (s->failed || (size && !buffer) || size > session::PluginState::kStreamLimit - s->out->size()) {
        s->failed = true; return -1;
    }
    if (!size) return 0;
    const auto* bytes = static_cast<const std::uint8_t*>(buffer);
    try { s->out->insert(s->out->end(), bytes, bytes + static_cast<std::size_t>(size)); }
    catch (...) { s->failed = true; return -1; }
    return static_cast<int64_t>(size);
}
int64_t readBytes(const clap_istream_t* stream, void* buffer, uint64_t size) noexcept
{
    auto* s = static_cast<VectorStream*>(stream->ctx);
    if (s->failed || (size && !buffer)) { s->failed = true; return -1; }
    const std::size_t left = s->in.size() - s->position;
    const std::size_t n = static_cast<std::size_t>(std::min<uint64_t>(size, left));
    if (!n) return 0;
    std::memcpy(buffer, s->in.data() + s->position, n);
    s->position += n;
    return static_cast<int64_t>(n);
}
} // namespace

bool Instance::saveState(std::vector<std::uint8_t>& out) const noexcept
{
    if (!host_.onMainThread() || !hasState() || plugin_ == nullptr) return false;
    std::vector<std::uint8_t> candidate;
    VectorStream s;
    s.out = &candidate;
    clap_ostream_t stream{};
    stream.ctx = &s;
    stream.write = &writeBytes;
    try {
        const bool ok = stateContext_ && stateContext_->save
            ? stateContext_->save(plugin_, &stream, CLAP_STATE_CONTEXT_FOR_PROJECT) : state_->save(plugin_, &stream);
        if (!ok || s.failed) return false;
        out = std::move(candidate); return true;
    } catch (...) { return false; }
}

bool Instance::loadState(std::span<const std::uint8_t> in) noexcept
{
    if (!host_.onMainThread() || !hasState() || plugin_ == nullptr || in.size() > session::PluginState::kStreamLimit) return false;
    VectorStream s;
    s.in = in;
    clap_istream_t stream{};
    stream.ctx = &s;
    stream.read = &readBytes;
    invalidateFeedbackValues();
    try {
    const bool ok = stateContext_ && stateContext_->load
        ? stateContext_->load(plugin_, &stream, CLAP_STATE_CONTEXT_FOR_PROJECT) : state_->load(plugin_, &stream);
    if (!ok || s.failed) return false;
    // Stage all IDs before publishing, including hidden feedback baselines.
    // During initial creation metadata is still empty and is read afterward.
    std::vector<double> current;
    if (!readNativeValues(current)) return false;
    publishNativeValues(current);
    return true;
    } catch (...) { return false; }
}

std::optional<session::PluginState> Instance::captureState(std::string& error)
{
    error.clear();
    if (!host_.onMainThread()) { error = "CLAP state capture requires its control thread"; return {}; }
    if (!hasState()) { error = "CLAP plugin has no complete state interface"; return {}; }
    if (!flushParameterChanges(error)) return {};
    try {
        std::vector<std::uint8_t> bytes;
        if (!saveState(bytes)) {
            invalidateFeedbackValues();
            error = "CLAP plugin state could not be captured within the size limit"; return {};
        }
        // A main-thread callback or preset can dirty opaque state without
        // emitting PARAM_VALUE. The event cache alone is not a coherent UI
        // snapshot. All producers are suspended by capture's ownership contract.
        std::vector<double> current;
        if (!readNativeValues(current)) {
            invalidateFeedbackValues();
            error = "CLAP parameter values could not be captured coherently"; return {};
        }
        auto captured = session::PluginState::create(uri(), std::move(bytes), {}, error);
        if (!captured) return {};
        publishNativeValues(current);
        return captured;
    } catch (const std::exception& exception) { error = exception.what(); }
      catch (...) { error = "exception while capturing CLAP plugin state"; }
    invalidateFeedbackValues(); return {};
}

bool Instance::refreshRescannedValues(std::string& error)
{
    if (!host_.takeValueRescanRequest()) return true;
    try {
        // Read all native IDs, including hidden parameters. Publish the cache
        // only after the whole set validates; the exclusive caller has already
        // delivered earlier host commands. Rescans raised DURING a query remain
        // pending for the next call, rather than forming an unbounded loop here.
        std::vector<double> current;
        if (!readNativeValues(current)) {
            invalidateFeedbackValues();
            error = "CLAP rescanned parameter values could not be read coherently";
            host_.requestValueRescan(); return false;
        }
        publishNativeValues(current);
        return true;
    } catch (const std::exception& exception) { error = exception.what(); }
      catch (...) { error = "exception while reading rescanned CLAP parameter values"; }
    invalidateFeedbackValues(); host_.requestValueRescan();
    return false;
}

bool Instance::flushParameterChanges(std::string& error)
{
    error.clear();
    if (!host_.onMainThread()) { error = "CLAP parameter flush requires its control thread"; return false; }
    if (eventCount_) { error = "CLAP parameter flush requires scheduled automation to finish"; return false; }
    try {
        const bool requested = host_.takeFlushRequest();
        flushParameters();
        if (params_ && (eventCount_ || requested)) {
            publishInputValues();
            const auto time = parameterTime(); // main/exclusive owner, before audio scope
            std::optional<ProbeHost::AudioThreadScope> audio;
            if (active_) audio.emplace(host_);
            OutputScope output(*this, 0, time);
            params_->flush(plugin_, &inEvents_, &outEvents_);
        }
        eventCount_ = 0; sorted_ = true;
        return refreshRescannedValues(error);
    } catch (const std::exception& exception) { error = exception.what(); }
      catch (...) { error = "exception while flushing CLAP parameters"; }
    invalidateFeedbackValues();
    // These are only untimed host edits: scheduled events were refused before
    // entry. Keep delivery pending on failure without misclassifying the leftover
    // input list as future automation on every subsequent Save.
    for (std::uint32_t i = 0; i < eventCount_; ++i) {
        const auto id = events_[i].param_id;
        const auto it = std::lower_bound(nativeParameters_.begin(), nativeParameters_.end(), id,
            [](const auto& parameter, clap_id key) { return parameter.id < key; });
        if (it != nativeParameters_.end() && it->id == id && it->control != SIZE_MAX)
            dirty_[it->control].store(true, std::memory_order_release);
    }
    eventCount_ = 0; sorted_ = true;
    host_.requestParameterFlush();
    return false;
}

} // namespace omawave::clap
