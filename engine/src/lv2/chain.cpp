#include "lv2/chain.h"
#include "clap/instance.h"
#include "vst3/instance.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <limits>
#include <cstring>

namespace omawave::lv2 {

Chain::Chain(std::uint32_t channels, std::uint32_t maxBlock, host::ProcessingMode mode)
    : mode_(mode), channels_(channels == 0 ? 1 : channels),
      maxBlock_(maxBlock == 0 ? kMaxBlock : maxBlock)
{
    // Two buffers, `channels` planes each, maxBlock frames each. Allocated once and
    // never grown: every connect_port below hands out an address inside this vector.
    scratch_.assign(static_cast<std::size_t>(2) * channels_ * maxBlock_, 0.0f);
}

Chain::~Chain() = default;

float* Chain::plane(std::size_t buffer, std::uint32_t channel) noexcept
{
    const std::size_t offset =
        ((buffer * channels_) + channel) * static_cast<std::size_t>(maxBlock_);
    return scratch_.data() + offset;
}

float* Chain::input(std::uint32_t channel) noexcept
{
    return plane(0, channel < channels_ ? channel : 0);
}

const float* Chain::output(std::uint32_t channel) const noexcept
{
    const std::size_t offset =
        ((activeBuffer_ * channels_) + (channel < channels_ ? channel : 0))
        * static_cast<std::size_t>(maxBlock_);
    return scratch_.data() + offset;
}

bool Chain::append(const World& world, HostFeatures& features,
                   const session::Insert& insert, std::string& error)
{
    const PluginInfo* info = world.find(insert.uri);
    if (info == nullptr) {
        error = "plugin not installed: " + insert.uri;
        return false;
    }

    // How many instances this plugin needs to cover the track, and refuse rather than
    // guess. `fits` already accounts for optional ports -- LSP's sidechain compressors
    // advertise four inputs of which two are optional, and counting raw ports would
    // refuse about fifty perfectly ordinary stereo inserts on this box.
    bool dualMono = false;
    std::uint32_t instanceCount = 1;
    if (channels_ == 2 && insert.instanceState.size() == 2 && info->fits(1)) {
        // Preserve independent saved mono processors even when a newer scan or
        // plug-in version also offers stereo. Never collapse two opaque states.
        dualMono = true;
        instanceCount = 2;
    } else if (info->fits(channels_)) {
        instanceCount = 1;
    } else if (channels_ == 2 && info->fits(1)) {
        dualMono = true;
        instanceCount = 2;
    } else {
        error = info->name + " does not fit a " + std::to_string(channels_)
              + "-channel track: it needs " + std::to_string(info->requiredIn)
              + " to " + std::to_string(info->totalIn) + " in and "
              + std::to_string(info->requiredOut) + " to "
              + std::to_string(info->totalOut) + " out";
        return false;
    }

    if (!insert.instanceState.empty() && insert.instanceState.size() != instanceCount) {
        error = "saved plugin state does not match the number of channel instances"; return false;
    }
    if (insert.instanceState.empty() && std::any_of(insert.controls.begin(), insert.controls.end(),
            [](const auto& control) { return control.fromState; })) {
        error = "state-authoritative control has no plugin state reference"; return false;
    }

    // The slot this would become, and so which buffer it reads and which it writes.
    const std::size_t index = slots_.size();
    const std::size_t inBuffer = index % 2;
    const std::size_t outBuffer = (index + 1) % 2;

    Slot slot(insert.identity);
    slot.capturedState = insert.instanceState;
    slot.bypass = insert.bypass;
    slot.dualMono = dualMono;
    if (info->sidechainIn > 0) slot.sidechain.assign(static_cast<std::size_t>(channels_ + 1) * maxBlock_, 0.0f);

    for (std::uint32_t i = 0; i < instanceCount; ++i) {
        std::unique_ptr<host::Instance> instance = host::createInstance(world, insert.uri, features, error, insert.legacyClapControls,
            insert.instanceState.empty() ? nullptr : &insert.instanceState[i], {dualMono ? 1u : channels_, mode_});
        if (instance == nullptr)
            return false;           // `error` is already what a person needs to read

        // Dual-mono drives one instance per channel; otherwise one instance takes the
        // whole track, channel for channel.
        const std::uint32_t first = dualMono ? i : 0;
        const std::uint32_t count = dualMono ? 1 : channels_;

        for (std::uint32_t c = 0; c < count; ++c) {
            const std::uint32_t track = first + c;
            if (!instance->connectMainAudioIn(c, plane(inBuffer, track))
                || !instance->connectAudioOut(c, plane(outBuffer, track))) {
                error = info->name + ": could not connect audio port "
                      + std::to_string(c);
                return false;
            }
        }

        // Optional main channels beyond this track's layout are explicitly null.
        // Key ports get their own buffers below, independently of port order.
        for (std::uint32_t c = count; c < instance->mainAudioInCount(); ++c) {
            if (!instance->connectMainAudioIn(c, nullptr)) {
                error = info->name + ": audio input " + std::to_string(c)
                      + " is required but this track has nothing to feed it";
                return false;
            }
        }
        for (std::uint32_t c = 0; c < instance->sidechainInCount(); ++c) {
            const auto keyChannel = dualMono ? i : instance->sidechainInCount() == 1 && channels_ > 1
                ? channels_ : std::min(c, channels_ - 1);
            if (slot.sidechain.empty() || !instance->connectSidechainIn(c, slot.sidechain.data() + keyChannel * maxBlock_)) {
                error = info->name + ": could not connect sidechain input"; return false;
            }
        }
        for (std::uint32_t c = count; c < instance->audioOutCount(); ++c) {
            if (!instance->connectAudioOut(c, nullptr)) {
                error = info->name + ": audio output " + std::to_string(c)
                      + " is required and has nowhere to go";
                return false;
            }
        }

        if (!instance->ready()) {
            error = info->name + " has ports left unconnected and will not be run";
            return false;
        }

        // Attached before activate(): a plugin is entitled to schedule work from its
        // first run(), and the x42 convolvers will not even instantiate without the
        // feature -- which every instance now gets; this decides who executes it.
        if (instance->wantsWorker()) {
            if (worker_ == nullptr)
                worker_ = std::make_unique<Worker>();
            instance->setWorker(worker_.get());
        }

        // Apply only overrides newer than the opaque snapshot. A shared rack
        // value must not replace independent dual-mono values restored above.
        // Apply overrides before activate to avoid a startup jump.
        for (const session::InsertControl& control : insert.controls)
            if (!control.fromState) instance->setParameter(control.port, control.value);

        instance->activate();
        if (!instance->active()) {
            // An inactive instance never writes its output: the track would go silent.
            error = info->name + " could not activate at this sample rate and block size";
            return false;
        }
        slot.instances.push_back(std::move(instance));
    }

    slots_.push_back(std::move(slot));
    activeBuffer_ = slots_.size() % 2;
    return true;
}

void Chain::warmUp() noexcept
{
    (void)warmUp(nullptr);
}

bool Chain::warmUp(const std::atomic<bool>* cancel) noexcept
{
    const auto cancelled = [&] { return cancel && cancel->load(std::memory_order_relaxed); };
    if (cancelled()) return false;
    std::fill(scratch_.begin(), scratch_.end(), 0.0f);
    bool needsProcessingThread = false;
    for (auto& slot : slots_) {
        std::fill(slot.sidechain.begin(), slot.sidechain.end(), 0.0f);
        for (auto& instance : slot.instances)
            needsProcessingThread |= instance->warmUpNeedsProcessingThread();
    }
    warmUpNotificationsPending_ = true;
    const auto processWarmUp = [&] {
        std::size_t begun = 0;
        bool complete = true;
        for (auto& slot : slots_) {
            for (auto& instance : slot.instances) {
                if (cancelled() || !instance->beginWarmUp()) { complete = false; break; }
                ++begun;
            }
            if (!complete) break;
        }
        // Preserve LV2 worker response delivery between blocks. Lazy setup runs
        // at the largest callback size, in one owned phase for the whole chain.
        constexpr std::uint32_t kWarmBlocks = 100;
        for (std::uint32_t block = 0; complete && block < kWarmBlocks; ++block) {
            if (cancelled()) { complete = false; break; }
            for (auto& slot : slots_) {
                for (auto& instance : slot.instances) {
                    if (cancelled() || !instance->warmUpBlock(maxBlock_)) { complete = false; break; }
                }
                if (!complete) break;
            }
            if (worker_) worker_->deliverResponses();
        }
        for (auto& slot : slots_) for (auto& instance : slot.instances) {
            if (begun) { complete = instance->endWarmUp() && complete; --begun; }
        }
        return complete && !cancelled();
    };
    bool complete = false;
    if (needsProcessingThread && std::this_thread::get_id() == controlThread_) {
        try {
            std::thread processing([&] { complete = processWarmUp(); });
            processing.join();
        } catch (...) { complete = false; }
    } else {
        complete = processWarmUp();
    }
    std::fill(scratch_.begin(), scratch_.end(), 0.0f);
    latency_.store(measureLatency(), std::memory_order_relaxed);
    if (std::this_thread::get_id() == controlThread_) (void)finishWarmUp();
    return complete && !cancelled();
}

bool Chain::finishWarmUp() noexcept
{
    if (std::this_thread::get_id() != controlThread_) return false;
    if (!warmUpNotificationsPending_) return true;
    // Construction/load/warmup output predates publication and is not a user
    // edit. Sources must be quiescent; notification APIs belong to this owner,
    // even when the preceding processing phase belonged to an offline worker.
    for (auto& slot : slots_) for (auto& pending : slot.pendingEvents) pending.reset();
    for (auto& slot : slots_) for (auto& instance : slot.instances) {
        host::ParameterEvent event;
        for (std::size_t i = 0; i < kNotificationPollLimit && instance->nextParameterEvent(event); ++i) {}
        (void)instance->takeStateDirty();
        (void)instance->takeNotificationOverflow();
        (void)instance->takeParameterTextChanged();
    }
    warmUpNotificationsPending_ = false;
    return true;
}

void Chain::serviceMainThread() noexcept
{
    for (auto& slot : slots_) for (auto& instance : slot.instances)
        (void)instance->serviceMainThread();
}

void Chain::setParameterClock(host::ParameterClock clock, void* context) noexcept
{
    for (auto& slot : slots_) for (auto& instance : slot.instances)
        instance->setParameterClock(clock, context);
}

Chain::Notifications Chain::consumeNotifications(ParameterListener listener, void* context,
                                                 StateListener stateListener, StateListener overflowListener,
                                                 StateListener readoutListener, bool parameterEditsHandled,
                                                 bool drainInFlight) noexcept
{
    Notifications result;
    if (std::this_thread::get_id() != controlThread_) {
        result.newDiagnostics = host::WrongNotificationThread;
        return result;
    }
    // All instances of this completed segment have published their output.
    // Freeze once: a newer left event cannot outrun a still-processing right.
    const auto completed = processedTime();
    std::uint32_t diagnostics = 0;
    for (std::size_t slot = 0; slot < slots_.size(); ++slot) {
        auto& current = slots_[slot];
        std::array<std::size_t, 2> consumed{};
        std::array<bool, 2> exhausted{};
        for (;;) {
            std::size_t selected = SIZE_MAX;
            for (std::size_t actual = 0; actual < current.instances.size(); ++actual) {
                auto& pending = current.pendingEvents[actual];
                if (consumed[actual] >= kNotificationPollLimit) continue;
                if (!pending && !exhausted[actual]) {
                    host::ParameterEvent event;
                    if (current.instances[actual]->nextParameterEvent(event)) pending = event;
                    else exhausted[actual] = true;
                }
                if (!pending) continue;
                const auto& time = pending->time;
                if (current.dualMono && !drainInFlight && time.valid
                    && time.processSequence > completed.processSequence) continue;
                if (selected == SIZE_MAX) { selected = actual; continue; }
                const auto& previous = current.pendingEvents[selected]->time;
                // Main-thread groups and invalid/private timestamps keep source
                // order. The chain's sequence spans loop wraps and Play epochs;
                // every actual instance shares it, even across a restart.
                if (time.valid && previous.valid && time.processSequence && previous.processSequence
                    && (time.processSequence < previous.processSequence
                        || (time.processSequence == previous.processSequence && time.frame < previous.frame)))
                    selected = actual;
            }
            if (selected == SIZE_MAX) break;
            const auto event = *current.pendingEvents[selected];
            current.pendingEvents[selected].reset();
            ++consumed[selected]; ++result.events;
            if (listener) listener(context, slot, selected, event);
        }
        for (std::size_t actual = 0; actual < slots_[slot].instances.size(); ++actual) {
            auto& instance = *slots_[slot].instances[actual];
            diagnostics |= instance.notificationDiagnostics();
            // Its retained event must reach the owner before the coalesced dirty
            // flag can close history or trigger a state/overflow capture.
            if (current.pendingEvents[actual]) continue;
            // Always consume EVERY instance's flag; short-circuiting after the
            // first dirty one leaves a second mono instance dirty after a Save.
            const auto changes = instance.takeStateChanges();
            const bool overflow = instance.takeNotificationOverflow();
            // Owners that do not explicitly opt into deferred verification
            // retain the conservative legacy dirty notification.
            const bool dirty = (changes.edited && (!parameterEditsHandled || !changes.parametersOnly || overflow))
                || (changes.readout && !readoutListener);
            if (dirty && stateListener) stateListener(context, slot, actual);
            result.dirty = result.dirty || dirty;
            if (changes.readout && readoutListener) readoutListener(context, slot, actual);
            result.readout = result.readout || changes.readout;
            if (overflow && overflowListener) overflowListener(context, slot, actual);
            result.overflow = result.overflow || overflow;
            const bool textChanged = instance.takeParameterTextChanged();
            result.textChanged = result.textChanged || textChanged;
        }
    }
    result.newDiagnostics = diagnostics & ~reportedNotificationDiagnostics_;
    reportedNotificationDiagnostics_ |= diagnostics;
    return result;
}

bool Chain::parameterFlushRequested() const noexcept
{
    for (const auto& slot : slots_) for (const auto& instance : slot.instances)
        if (instance->parameterFlushRequested()) return true;
    return false;
}

bool Chain::flushParameterChanges(std::string& error)
{
    error.clear();
    for (auto& slot : slots_) for (auto& instance : slot.instances) {
        if (!instance->parameterFlushRequested()) continue;
        std::string reason;
        if (!instance->flushParameterChanges(reason) && error.empty())
            error = instance->uri() + ": " + reason;
    }
    return error.empty();
}

std::optional<host::UnrenderedParameter> Chain::unrenderedParameter(
    std::size_t slot, std::uint32_t port, std::size_t actual) const noexcept
{
    if (slot >= slots_.size() || actual >= slots_[slot].instances.size()) return {};
    return slots_[slot].instances[actual]->unrenderedParameter(port);
}

host::ParameterTime Chain::nextInputTime(std::size_t slot, std::size_t actual) const noexcept
{
    if (slot >= slots_.size() || actual >= slots_[slot].instances.size()) return {};
    return slots_[slot].instances[actual]->nextInputTime();
}

float* Chain::sidechainInput(std::size_t slot, std::uint32_t channel) noexcept
{
    if (!hasSidechain(slot) || channel >= channels_) return nullptr;
    return slots_[slot].sidechain.data() + channel * maxBlock_;
}

bool Chain::hasSidechain(std::size_t slot) const noexcept
{
    return slot < slots_.size() && !slots_[slot].sidechain.empty();
}

std::uint32_t Chain::slotLatency(std::size_t slot) const noexcept
{
    if (slot >= slots_.size() || slots_[slot].bypass) return 0;
    std::uint32_t latency = 0;
    for (const auto& instance : slots_[slot].instances) latency = std::max(latency, instance->latencyFrames());
    return latency;
}

host::ParameterTime Chain::processedTime() const noexcept
{
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        const auto before = processPublication_.load(std::memory_order_acquire);
        if (before & 1) continue;
        host::ParameterTime time;
        time.frame = processedFrame_.load(std::memory_order_acquire);
        time.document = processedDocument_.load(std::memory_order_acquire);
        const auto playback = processedPlayback_.load(std::memory_order_acquire);
        time.playing = playback & 1; time.valid = playback & 2;
        time.playbackEpoch = static_cast<std::uint32_t>(playback >> 2);
        time.processSequence = processedSequence_.load(std::memory_order_acquire);
        time.loopIteration = processedLoopIteration_.load(std::memory_order_acquire);
        if (before == processPublication_.load(std::memory_order_acquire)) return time;
    }
    return {};
}

void Chain::publishProcessedTime(const host::ParameterTime& time) noexcept
{
    processPublication_.fetch_add(1, std::memory_order_relaxed);
    processedFrame_.store(time.frame, std::memory_order_release);
    processedDocument_.store(time.document, std::memory_order_release);
    processedPlayback_.store((std::uint64_t(time.playbackEpoch) << 2) | std::uint64_t(time.playing)
        | (std::uint64_t(time.valid) << 1), std::memory_order_release);
    processedSequence_.store(time.processSequence, std::memory_order_release);
    processedLoopIteration_.store(time.loopIteration, std::memory_order_release);
    processPublication_.fetch_add(1, std::memory_order_release);
}

bool Chain::processParameterEvents(std::size_t slot) const noexcept
{
    return slot < slots_.size() && !slots_[slot].instances.empty()
        && slots_[slot].instances.front()->processParameterEvents();
}

bool Chain::processAutomationSpans(std::size_t slot) const noexcept
{
    return slot < slots_.size() && !slots_[slot].instances.empty()
        && slots_[slot].instances.front()->processAutomationSpans();
}

void Chain::setAutomationRecording(std::size_t slot, const host::AutomationRecording& recording,
                                   const host::AutomationPlayback& playback) noexcept
{
    setInstanceAutomationRecording(slot, {recording, recording}, {playback, playback});
}

void Chain::setInstanceAutomationRecording(std::size_t slot,
    const std::array<host::AutomationRecording, 2>& recording,
    const std::array<host::AutomationPlayback, 2>& playback) noexcept
{
    if ((!recording[0].active() && !recording[1].active()) || slot >= slots_.size()) slot = SIZE_MAX;
    if (recordingSlot_ < slots_.size() && recordingSlot_ != slot)
        for (const auto& instance : slots_[recordingSlot_].instances) instance->setAutomationRecording({});
    if (slot < slots_.size()) for (std::size_t actual = 0; actual < slots_[slot].instances.size(); ++actual) {
        const auto& instance = slots_[slot].instances[actual];
        instance->setAutomationPlayback(playback[actual]);
        if (slot != recordingSlot_ || recording[actual] != recording_[actual]) instance->setAutomationRecording(recording[actual]);
    }
    recordingSlot_ = slot; recording_ = recording;
}

std::size_t Chain::instanceCount(std::size_t slot) const noexcept
{
    return slot < slots_.size() ? slots_[slot].instances.size() : 0;
}

void Chain::setAutomationControl(std::size_t slot, std::uint32_t port, double value, std::size_t actual) noexcept
{
    if (slot < slots_.size()) for (std::size_t i = 0; i < slots_[slot].instances.size(); ++i) {
        auto& instance = slots_[slot].instances[i];
        if ((actual == SIZE_MAX || actual == i) && instance->automationPlaybackAllowed(port)) instance->setParameter(port, value);
    }
}

void Chain::adoptProcessingThread() noexcept
{
    for (auto& slot : slots_)
        for (auto& instance : slot.instances) instance->adoptProcessingThread();
}

void Chain::process(std::uint32_t frames, bool useSidechain, const host::ProcessContext& supplied) noexcept
{
    if (frames == 0)
        return;
    timespec started{};
    ::clock_gettime(CLOCK_MONOTONIC, &started);
    struct Cost {
        Chain& chain; timespec started; std::uint32_t frames;
        ~Cost() {
            timespec finished{};
            ::clock_gettime(CLOCK_MONOTONIC, &finished);
            const double ns = double(finished.tv_sec - started.tv_sec) * 1e9 + double(finished.tv_nsec - started.tv_nsec);
            const double perFrame = ns / double(frames);
            const double previous = chain.costPerFrame_.load(std::memory_order_relaxed);
            chain.costPerFrame_.store(previous + (perFrame - previous) * 0.02, std::memory_order_relaxed);
        }
    } cost{*this, started, frames};
    if (frames > maxBlock_)
        frames = maxBlock_;         // the transport sub-blocks; this is the backstop

    auto context = supplied;
    if (processSequence_ != UINT64_MAX) ++processSequence_;
    else context.time.valid = false;
    context.time.processSequence = processSequence_;
    const bool samePlayback = previousProcess_.time.valid && context.time.valid
        && previousProcess_.time.document == context.time.document
        && previousProcess_.time.playbackEpoch == context.time.playbackEpoch;
    if (!samePlayback || !context.hasLoop() || !previousProcess_.hasLoop()
        || context.loopStart != previousProcess_.loopStart || context.loopEnd != previousProcess_.loopEnd)
        processLoopIteration_ = 0;
    else if (context.time.frame <= previousProcess_.time.frame && processLoopIteration_ != UINT64_MAX)
        ++processLoopIteration_;
    context.time.loopIteration = processLoopIteration_;

    // Deliver what the worker finished since last cycle before anything runs, so a
    // response is in the plugin before the run() that would want it. Allocates
    // nothing: the rings are preallocated and delivery is two loads and a call.
    if (worker_ != nullptr)
        worker_->deliverResponses();

    for (std::size_t i = 0; i < slots_.size(); ++i) {
        Slot& slot = slots_[i];
        if (!slot.sidechain.empty()) {
            if (!useSidechain) {
                for (std::uint32_t c = 0; c <= channels_; ++c)
                    std::fill_n(slot.sidechain.data() + c * maxBlock_, frames, 0.0f);
            } else if (!slot.dualMono && channels_ > 1) {
                auto* mono = slot.sidechain.data() + channels_ * maxBlock_;
                for (std::uint32_t f = 0; f < frames; ++f) {
                    float sum = 0;
                    for (std::uint32_t c = 0; c < channels_; ++c) sum += slot.sidechain[c * maxBlock_ + f];
                    mono[f] = sum / static_cast<float>(channels_);
                }
            }
        }
        const std::size_t inBuffer = i % 2;
        const std::size_t outBuffer = (i + 1) % 2;

        if (slot.bypass || slot.instances.empty()) {
            // Still has to move the audio: the next slot reads the other buffer, and a
            // bypassed plugin must be bit-exact passthrough, not silence.
            for (std::uint32_t c = 0; c < channels_; ++c) {
                std::memcpy(plane(outBuffer, c), plane(inBuffer, c),
                            static_cast<std::size_t>(frames) * sizeof(float));
            }
            continue;
        }

        for (std::unique_ptr<host::Instance>& instance : slot.instances) {
            instance->setProcessContext(context);
            instance->run(frames);
        }
    }
    latency_.store(measureLatency(), std::memory_order_relaxed);
    auto end = context.recordTime();
    if (end.playing) {
        if (end.frame <= INT64_MAX - (frames - 1)) end.frame += frames - 1;
        else end.valid = false;
    }
    previousProcess_ = context; previousProcess_.time = end;
    publishProcessedTime(end);
}

std::uint32_t Chain::measureLatency() const noexcept
{
    std::uint32_t total = 0;
    for (const Slot& slot : slots_) {
        if (slot.bypass)
            continue;
        // Dual-mono runs the same plugin twice with the same settings, so the pair
        // delays by what one of them delays by, not by both added together.
        std::uint32_t worst = 0;
        for (const std::unique_ptr<host::Instance>& instance : slot.instances)
            worst = std::max(worst, instance->latencyFrames());
        if (worst > std::numeric_limits<std::uint32_t>::max() - total) return std::numeric_limits<std::uint32_t>::max();
        total += worst;
    }
    return total;
}

void Chain::setBypass(std::size_t slot, bool bypass) noexcept
{
    if (slot < slots_.size())
        slots_[slot].bypass = bypass;
    latency_.store(measureLatency(), std::memory_order_relaxed);
}

bool Chain::bypassed(std::size_t slot) const noexcept
{
    return slot < slots_.size() && slots_[slot].bypass;
}

std::uint64_t Chain::parameterVersion(std::size_t slot) const noexcept
{
    std::uint64_t version = 0;
    if (slot < slots_.size()) for (const auto& instance : slots_[slot].instances)
        version = std::max(version, instance->parameterVersion());
    return version;
}

void Chain::setControl(std::size_t slot, std::uint32_t port, double value, std::uint64_t version, std::size_t actual) noexcept
{
    if (slot >= slots_.size())
        return;
    // Ordinary host knob moves remain shared. Independent automation resets
    // target only the processor whose lane was removed or disabled.
    for (std::size_t i = 0; i < slots_[slot].instances.size(); ++i)
        if (actual == SIZE_MAX || actual == i) slots_[slot].instances[i]->setParameterAtVersion(port, value, version);
}

void Chain::scheduleControl(std::size_t slot, std::uint32_t port, double value, std::uint32_t offset, std::size_t actual) noexcept
{
    if (slot >= slots_.size())
        return;
    // A bypassed slot is not run, so nothing would take timed events off its
    // list: it remembers the latest value and hears it when it next runs.
    if (slots_[slot].bypass) {
        for (std::size_t i = 0; i < slots_[slot].instances.size(); ++i)
            if (actual == SIZE_MAX || actual == i) slots_[slot].instances[i]->setParameter(port, value);
        return;
    }
    for (std::size_t i = 0; i < slots_[slot].instances.size(); ++i) {
        auto& instance = slots_[slot].instances[i];
        if ((actual == SIZE_MAX || actual == i) && instance->automationPlaybackAllowed(port)) (void)instance->scheduleParameter(port, value, offset);
    }
}

double Chain::controlValue(std::size_t slot, std::uint32_t port, std::size_t actual) const noexcept
{
    return slot < slots_.size() && actual < slots_[slot].instances.size() ? slots_[slot].instances[actual]->parameterValue(port) : 0.0f;
}

double Chain::outputValue(std::size_t slot, std::uint32_t port) const noexcept
{
    if (slot >= slots_.size()) return 0.0;
    double best = 0.0;
    bool first = true;
    for (const auto& instance : slots_[slot].instances) {
        const double v = instance->outputValue(port);
        if (first || std::abs(v) > std::abs(best)) best = v;
        first = false;
    }
    return best;
}

bool Chain::hasOpaqueState(std::size_t slot) const noexcept
{
    return slot < slots_.size() && !slots_[slot].instances.empty()
        && std::all_of(slots_[slot].instances.begin(), slots_[slot].instances.end(),
            [](const auto& instance) { return instance->hasOpaqueState(); });
}

bool Chain::captureState(std::size_t slot, std::vector<session::PluginState>& out, std::string& error)
{
    error.clear();
    if (slot >= slots_.size() || slots_[slot].instances.empty()) { error = "cannot capture a missing plugin slot"; return false; }
    try {
        std::vector<session::PluginState> states;
        auto& saved = slots_[slot].capturedState;
        for (const auto& instance : slots_[slot].instances) {
            auto state = instance->captureState(error);
            if (!state) return false;
            const auto index = states.size();
            states.push_back(index < saved.size() && saved[index] == *state ? saved[index] : std::move(*state));
        }
        // Publish only after every actual instance succeeded. The cache holds
        // one complete capture for this live slot, with no model/Undo mutation.
        saved = states;
        out = std::move(states); return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

bool Chain::captureInsertState(std::size_t slot, session::Insert& out, std::string& error,
                              bool includeInputControls)
{
    error.clear();
    if (slot >= slots_.size() || slots_[slot].instances.empty()
        || slots_[slot].identity != out.identity || slots_[slot].instances.front()->uri() != out.uri) {
        error = "plugin state capture target does not match the live insert"; return false;
    }
    try {
        session::Insert candidate = out;
        if (!captureState(slot, candidate.instanceState, error)) return false;
        const auto& instance = *slots_[slot].instances.front();
        const auto& ports = instance.controls();
        if (includeInputControls) for (const auto& port : ports)
            if (!port.output && !candidate.control(port.index)) candidate.setControl(port.index, instance.parameterValue(port.index));
        for (auto& control : candidate.controls) {
            const auto found = std::find_if(ports.begin(), ports.end(), [&](const auto& port) {
                return port.index == control.port && !port.output;
            });
            if (found == ports.end()) continue; // Retain unknown settings for other plugin versions.
            if (!control.hasAutomationData()) control.value = instance.parameterValue(control.port);
            control.fromState = true;
        }
        out = std::move(candidate); return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

Chain::InputValues Chain::capturedInputValues(std::size_t slot) const
{
    InputValues result;
    if (slot >= slots_.size()) return result;
    for (const auto& instance : slots_[slot].instances) {
        auto& values = result.emplace_back();
        for (const auto& port : instance->controls())
            if (!port.output) values.emplace_back(port.index, instance->parameterValue(port.index));
    }
    return result;
}

bool Chain::hasProcessor(std::size_t slot) const noexcept
{
    return slot < slots_.size() && !slots_[slot].instances.empty();
}

bool Chain::restartRequested() const noexcept
{
    for (const Slot& slot : slots_)
        for (const std::unique_ptr<host::Instance>& instance : slot.instances)
            if (instance->restartRequested()) return true;
    return false;
}

bool Chain::canRefreshConfiguration() const noexcept
{
    for (const auto& slot : slots_) for (const auto& instance : slot.instances)
        if (instance->restartRequested() && !instance->canRefreshConfiguration()) return false;
    return true;
}

bool Chain::refreshConfiguration(std::string& error)
{
    error.clear();
    if (!canRefreshConfiguration()) { error = "An effect requires recreation"; return false; }
    bool ok = true;
    for (auto& slot : slots_) for (auto& instance : slot.instances) {
        if (!instance->restartRequested()) continue;
        std::string reason;
        if (!instance->refreshConfiguration(reason)) {
            ok = false;
            if (error.empty()) error = instance->info().name + ": " + reason;
        }
    }
    latency_.store(measureLatency(), std::memory_order_relaxed);
    return ok;
}

host::Timing Chain::timing(std::size_t slot) const noexcept
{
    if (slot >= slots_.size() || slots_[slot].instances.empty()) return {};
    host::Timing t = slots_[slot].instances.front()->timing();
    for (const auto& instance : slots_[slot].instances) {
        const host::Timing other = instance->timing();
        t.sampleOffsets = t.sampleOffsets && other.sampleOffsets;
        t.rampStep = std::max(t.rampStep, other.rampStep);
        t.eventCapacity = std::min(t.eventCapacity, other.eventCapacity);
    }
    return t;
}

void Chain::appendMissing(session::InsertIdentity identity)
{
    Slot slot(std::move(identity)); slot.bypass = true; slots_.push_back(std::move(slot));
}
const session::InsertIdentity* Chain::insertIdentity(std::size_t slot) const noexcept
{
    return slot < slots_.size() ? &slots_[slot].identity : nullptr;
}
const std::vector<ControlPort>* Chain::controls(std::size_t slot) const noexcept
{
    return slot<slots_.size() && !slots_[slot].instances.empty() ? &slots_[slot].instances.front()->controls() : nullptr;
}

std::string Chain::parameterText(std::size_t slot, std::uint32_t port, double value) const
{
    return slot < slots_.size() && !slots_[slot].instances.empty()
        ? slots_[slot].instances.front()->parameterText(port, value) : std::string{};
}

} // namespace omawave::lv2

namespace omawave::host {
std::unique_ptr<Instance> createInstance(const lv2::World& world, const std::string& uri,
                                       lv2::HostFeatures& features, std::string& error, bool legacyClapControls,
                                       const session::PluginState* state, InstanceOptions options)
{
    error.clear();
    const PluginInfo* info = world.find(uri);
    if (info == nullptr) {
        error = "plugin not installed: " + uri;
        return nullptr;
    }
    if (info->format == Format::Clap)
        return clap::Instance::create(world.clap(), *info, features.sampleRate(), features.maxBlock(), error, legacyClapControls, state);
    if (info->format == Format::Vst3) {
        if (!info->usable) { error = info->name + ": " + info->unusableReason; return {}; }
        if (options.mode != ProcessingMode::Realtime && options.mode != ProcessingMode::Offline) {
            error = "Invalid VST3 processing mode"; return {};
        }
        const auto channels = options.channels ? options.channels : info->audioConfigurations.empty()
            ? info->totalIn : info->audioConfigurations.front().channels;
        const auto* layout = info->configuration(channels);
        if (!layout) { error = info->name + ": requested VST3 main layout is unavailable"; return {}; }
        return vst3::Instance::create(info->path, uri.substr(5),
            {features.sampleRate(), features.maxBlock(), channels, layout->sidechainChannels,
             options.mode == ProcessingMode::Offline}, error, state);
    }
    if (info->format != Format::Lv2) { error = "unsupported plugin format: " + uri; return {}; }
    if (state) { error = "opaque state restore is unsupported for this plugin format: " + uri; return {}; }
    return lv2::Instance::create(world, uri, features, error);
}
} // namespace omawave::host
