#include "lv2/instance.h"

#include "lv2/features.h"

#include <lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/time/time.h>
#include <cmath>
#include <lv2/core/lv2.h>
#include <lv2/resize-port/resize-port.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstring>

namespace omawave::lv2 {
namespace {

// Everything a port is asked about while the instance is being built.
struct PortVocabulary {
    explicit PortVocabulary(LilvWorld* world)
        : world(world), audio(lilv_new_uri(world, LV2_CORE__AudioPort)),
          control(lilv_new_uri(world, LV2_CORE__ControlPort)),
          cv(lilv_new_uri(world, LV2_CORE__CVPort)),
          input(lilv_new_uri(world, LV2_CORE__InputPort)),
          atom(lilv_new_uri(world, LV2_ATOM__AtomPort)),
          optional(lilv_new_uri(world, LV2_CORE__connectionOptional)),
          toggled(lilv_new_uri(world, LV2_CORE__toggled)),
          integer(lilv_new_uri(world, LV2_CORE__integer)),
          latency(lilv_new_uri(world, LV2_CORE__latency)),
          designation(lilv_new_uri(world, LV2_CORE__designation)),
          minimumSize(lilv_new_uri(world, LV2_RESIZE_PORT__minimumSize)),
          portGroup(lilv_new_uri(world, "http://lv2plug.in/ns/ext/port-groups#group")),
          sideChainOf(lilv_new_uri(world, "http://lv2plug.in/ns/ext/port-groups#sideChainOf")),
          timePosition(lilv_new_uri(world, LV2_TIME__Position))
    {
    }

    ~PortVocabulary()
    {
        lilv_node_free(audio);
        lilv_node_free(control);
        lilv_node_free(cv);
        lilv_node_free(input);
        lilv_node_free(atom);
        lilv_node_free(optional);
        lilv_node_free(toggled);
        lilv_node_free(integer);
        lilv_node_free(latency);
        lilv_node_free(designation);
        lilv_node_free(minimumSize);
        lilv_node_free(portGroup); lilv_node_free(sideChainOf); lilv_node_free(timePosition);
    }

    [[nodiscard]] bool isSidechain(const LilvPlugin* plugin, const LilvPort* port) const {
        LilvNodes* groups = lilv_port_get_value(plugin, port, portGroup); bool found = false;
        LILV_FOREACH(nodes, i, groups) {
            LilvNodes* of = lilv_world_find_nodes(world, lilv_nodes_get(groups, i), sideChainOf, nullptr);
            found = found || lilv_nodes_size(of) > 0; lilv_nodes_free(of);
        }
        lilv_nodes_free(groups); return found;
    }

    LilvWorld* world;

    PortVocabulary(const PortVocabulary&) = delete;
    PortVocabulary& operator=(const PortVocabulary&) = delete;
    PortVocabulary(PortVocabulary&&) = delete;
    PortVocabulary& operator=(PortVocabulary&&) = delete;

    LilvNode* audio;
    LilvNode* control;
    LilvNode* cv;
    LilvNode* input;
    LilvNode* atom;
    LilvNode* optional;
    LilvNode* toggled;
    LilvNode* integer;
    LilvNode* latency;
    LilvNode* designation;
    LilvNode* minimumSize;
    LilvNode* portGroup;
    LilvNode* sideChainOf;
    LilvNode* timePosition;
};

std::string portName(const LilvPlugin* plugin, const LilvPort* port)
{
    LilvNode* node = lilv_port_get_name(plugin, port);
    std::string out;
    if (node != nullptr) {
        const char* str = lilv_node_as_string(node);
        if (str != nullptr)
            out = str;
        lilv_node_free(node);
    }
    return out;
}

// An atom port's buffer, as the plugin will find it at the top of run().
//
// An input sequence must be a well-formed EMPTY sequence: a header whose size covers
// the body and nothing more. Zeroed memory is not that -- its type is 0, which is not
// atom:Sequence, and a plugin that iterates it is reading a garbage type.
//
// An output buffer is the other way round: it is handed over as an atom:Chunk whose
// size is the capacity, which is how the plugin is told how much room it has to write.
void resetAtomBuffer(std::uint8_t* buffer, std::uint32_t capacity, bool isInput,
                     LV2_URID sequenceUrid, LV2_URID chunkUrid) noexcept
{
    auto* atom = reinterpret_cast<LV2_Atom*>(buffer);
    if (isInput) {
        auto* sequence = reinterpret_cast<LV2_Atom_Sequence*>(buffer);
        sequence->atom.size = sizeof(LV2_Atom_Sequence_Body);
        sequence->atom.type = sequenceUrid;
        sequence->body.unit = 0;
        sequence->body.pad = 0;
    } else {
        atom->size = capacity - static_cast<std::uint32_t>(sizeof(LV2_Atom));
        atom->type = chunkUrid;
    }
}

} // namespace

std::unique_ptr<Instance> Instance::create(const World& world, const std::string& uri,
                                           HostFeatures& features, std::string& error)
{
    const PluginInfo* info = world.find(uri);
    const LilvPlugin* plugin = world.lilvPlugin(uri);
    if (info == nullptr || plugin == nullptr) {
        error = "no such plugin: " + uri;
        return nullptr;
    }
    if (!info->usable) {
        error = info->unusableReason;
        return nullptr;
    }

    const PortVocabulary vocab(world.raw());

    auto self = std::unique_ptr<Instance>(new Instance());
    self->uri_ = uri;
    self->info_ = *info;
    self->portCount_ = lilv_plugin_get_num_ports(plugin);
    self->sequenceCapacity_ = HostFeatures::kSequenceSize;
    self->atomSequenceUrid_ = features.ids().atomSequence;
    self->atomChunkUrid_ = features.ids().atomChunk;
    // Host transport for plug-ins that follow it (time:Position), mapped here
    // so run() only writes. Tempo-synced effects then follow the project.
    self->sampleRate_ = features.sampleRate();
    if (const auto* map = features.urids().mapFeature())
        lv2_atom_forge_init(&self->forge_, static_cast<LV2_URID_Map*>(map->data));
    {
        auto& urids = features.urids();
        self->time_ = {urids.map(LV2_TIME__Position), urids.map(LV2_TIME__frame), urids.map(LV2_TIME__speed),
            urids.map(LV2_TIME__bar), urids.map(LV2_TIME__barBeat), urids.map(LV2_TIME__beatUnit),
            urids.map(LV2_TIME__beatsPerBar), urids.map(LV2_TIME__beatsPerMinute)};
    }

    self->controlValues_.assign(self->portCount_, 0.0f);
    self->atomBuffers_.resize(self->portCount_);
    self->connected_.assign(self->portCount_, false);
    self->optional_.assign(self->portCount_, false);

    // Defaults, minima and maxima come back as three parallel arrays indexed by port.
    std::vector<float> minima(self->portCount_, 0.0f);
    std::vector<float> maxima(self->portCount_, 0.0f);
    std::vector<float> defaults(self->portCount_, 0.0f);
    lilv_plugin_get_port_ranges_float(plugin, minima.data(), maxima.data(),
                                      defaults.data());

    for (std::uint32_t i = 0; i < self->portCount_; ++i) {
        const LilvPort* port = lilv_plugin_get_port_by_index(plugin, i);
        const bool isInput = lilv_port_is_a(plugin, port, vocab.input);
        self->optional_[i] = lilv_port_has_property(plugin, port, vocab.optional);

        if (lilv_port_is_a(plugin, port, vocab.audio)) {
            if (isInput) (vocab.isSidechain(plugin, port) ? self->sidechainIn_ : self->mainAudioIn_)
                .push_back(static_cast<std::uint32_t>(self->audioIn_.size()));
            (isInput ? self->audioIn_ : self->audioOut_).push_back(i);
            continue;
        }

        if (lilv_port_is_a(plugin, port, vocab.control)) {
            ControlPort control;
            control.index = i;
            control.name = portName(plugin, port);
            control.output = !isInput;
            if (const LilvNode* symbol = lilv_port_get_symbol(plugin, port);
                symbol != nullptr) {
                if (const char* str = lilv_node_as_string(symbol); str != nullptr)
                    control.symbol = str;
            }
            control.minimum = std::isfinite(minima[i]) ? minima[i] : 0.0f;
            control.maximum = std::isfinite(maxima[i]) ? maxima[i] : 1.0f;
            control.defaultValue = std::isfinite(defaults[i]) ? defaults[i] : 0.0f;
            if (control.maximum < control.minimum)
                std::swap(control.minimum, control.maximum);
            control.toggled = lilv_port_has_property(plugin, port, vocab.toggled);
            control.integer = lilv_port_has_property(plugin, port, vocab.integer);

            self->controlValues_[i] = control.clamp(control.defaultValue);
            if (control.output) self->outputControls_.push_back(i);
            self->controls_.push_back(std::move(control));

            // Connected once the instance exists, and never again: the buffer is a
            // single float this object owns for its whole life.
            self->connected_[i] = true;

            // Latency is a designated output control, and reading it is how delay
            // compensation later learns what to compensate for.
            LilvNodes* designations =
                lilv_port_get_value(plugin, port, vocab.designation);
            LILV_FOREACH (nodes, d, designations) {
                const char* str = lilv_node_as_string(lilv_nodes_get(designations, d));
                if (str != nullptr && std::strcmp(str, LV2_CORE__latency) == 0)
                    self->latencyPort_ = i;
            }
            lilv_nodes_free(designations);
            continue;
        }

        if (lilv_port_is_a(plugin, port, vocab.atom)) {
            std::uint32_t capacity = self->sequenceCapacity_;
            LilvNodes* sizes = lilv_port_get_value(plugin, port, vocab.minimumSize);
            LILV_FOREACH (nodes, s, sizes) {
                const LilvNode* node = lilv_nodes_get(sizes, s);
                if (lilv_node_is_int(node)) {
                    const auto asked = static_cast<std::uint32_t>(lilv_node_as_int(node));
                    capacity = std::max(capacity, asked);
                }
            }
            lilv_nodes_free(sizes);

            self->atomBuffers_[i].assign(capacity, 0);
            resetAtomBuffer(self->atomBuffers_[i].data(), capacity, isInput,
                            self->atomSequenceUrid_, self->atomChunkUrid_);
            self->atomPorts_.push_back({i, capacity, isInput,
                isInput && lilv_port_supports_event(plugin, port, vocab.timePosition)});
            self->connected_[i] = true;
            continue;
        }

        // Anything else -- a CV port, a morph port, something not yet invented. If the
        // plugin can live without it, leave it unconnected; if it cannot, refuse now
        // rather than crash in run(). Nothing installed on this box reaches here.
        if (!self->optional_[i]) {
            const bool isCv = lilv_port_is_a(plugin, port, vocab.cv);
            error = info->name + ": port " + portName(plugin, port) + " is a "
                  + (isCv ? "CV port" : "port type") + " Omawave cannot connect";
            return nullptr;
        }
    }

    // The schedule feature is per instance: its handle is the instance itself, so a
    // plugin's schedule_work lands back here with enough context to know who asked.
    // The shared list is copied and extended rather than mutated -- HostFeatures is
    // shared by every instance in the process.
    self->schedule_.handle = self.get();
    self->schedule_.schedule_work = &Instance::scheduleWork;
    self->scheduleFeature_.URI = LV2_WORKER__schedule;
    self->scheduleFeature_.data = &self->schedule_;
    for (const LV2_Feature* const* f = features.array(); *f != nullptr; ++f)
        self->featureArray_.push_back(*f);
    self->featureArray_.push_back(&self->scheduleFeature_);
    self->featureArray_.push_back(nullptr);

    self->instance_ = lilv_plugin_instantiate(plugin, features.sampleRate(),
                                              self->featureArray_.data());
    if (self->instance_ == nullptr) {
        error = "could not instantiate " + info->name;
        return nullptr;
    }

    self->workerIface_ = static_cast<const LV2_Worker_Interface*>(
        lilv_instance_get_extension_data(self->instance_, LV2_WORKER__interface));
    if (info->needsWorker && self->workerIface_ == nullptr) {
        // It said it cannot run without scheduling work and then gave us nothing to
        // schedule work TO. Refusing beats calling run() on a plugin whose stated
        // contract is already broken.
        error = info->name + " requires the worker extension but exposes no worker "
                             "interface";
        return nullptr;
    }

    // Now that the instance exists, hand it the buffers. Control and atom ports are
    // connected once and never again; audio ports are connected by the chain, which
    // owns the scratch they point into.
    for (std::uint32_t i = 0; i < self->portCount_; ++i) {
        if (!self->atomBuffers_[i].empty()) {
            lilv_instance_connect_port(self->instance_, i,
                                       self->atomBuffers_[i].data());
        }
    }
    for (const ControlPort& control : self->controls_) {
        lilv_instance_connect_port(self->instance_, control.index,
                                   &self->controlValues_[control.index]);
    }
    self->outputMirror_ = std::make_unique<std::atomic<float>[]>(self->portCount_);
    for (const std::uint32_t i : self->outputControls_)
        self->outputMirror_[i].store(self->controlValues_[i], std::memory_order_relaxed);

    return self;
}

Instance::~Instance()
{
    if (instance_ == nullptr)
        return;
    if (active_)
        lilv_instance_deactivate(instance_);
    lilv_instance_free(instance_);
}

bool Instance::connectAudioIn(std::uint32_t channel, float* buffer) noexcept
{
    if (channel >= audioIn_.size())
        return false;
    const std::uint32_t port = audioIn_[channel];
    if (buffer == nullptr && !optional_[port])
        return false;
    lilv_instance_connect_port(instance_, port, buffer);
    connected_[port] = buffer != nullptr;
    return true;
}

bool Instance::connectMainAudioIn(std::uint32_t channel, float* buffer) noexcept
{
    return channel < mainAudioIn_.size() && connectAudioIn(mainAudioIn_[channel], buffer);
}

bool Instance::connectSidechainIn(std::uint32_t channel, float* buffer) noexcept
{
    return channel < sidechainIn_.size() && connectAudioIn(sidechainIn_[channel], buffer);
}

bool Instance::connectAudioOut(std::uint32_t channel, float* buffer) noexcept
{
    if (channel >= audioOut_.size())
        return false;
    const std::uint32_t port = audioOut_[channel];
    if (buffer == nullptr && !optional_[port])
        return false;
    lilv_instance_connect_port(instance_, port, buffer);
    connected_[port] = buffer != nullptr;
    return true;
}

bool Instance::ready() const noexcept
{
    if (instance_ == nullptr)
        return false;
    for (std::uint32_t i = 0; i < portCount_; ++i) {
        if (!connected_[i] && !optional_[i])
            return false;
    }
    return true;
}

void Instance::activate() noexcept
{
    if (instance_ == nullptr || active_)
        return;
    lilv_instance_activate(instance_);
    active_ = true;
}

void Instance::deactivate() noexcept
{
    if (instance_ == nullptr || !active_)
        return;
    lilv_instance_deactivate(instance_);
    active_ = false;
}

void Instance::run(std::uint32_t frames) noexcept
{
    if (instance_ == nullptr || !active_)
        return;

    // Synchronous-mode responses, collected while work() ran inline. Always empty on
    // the audio thread -- an instance without a Worker is never published to it, and
    // an instance with one queues responses through the rings instead -- so on the
    // path that matters this is one branch on a size.
    if (!syncResponses_.empty()) {
        for (const std::vector<std::uint8_t>& response : syncResponses_) {
            deliverResponse(static_cast<std::uint32_t>(response.size()),
                            response.data());
        }
        syncResponses_.clear();
    }

    // Every input sequence is presented empty again, and every output chunk gets its
    // capacity back. A plugin that wrote to an output buffer last block left its size
    // saying how much it wrote, and leaving that in place tells the next run it has
    // almost no room. Two stores per port, no allocation, no branch that touches the
    // heap -- this is the audio thread.
    for (const AtomPort& port : atomPorts_) {
        resetAtomBuffer(atomBuffers_[port.index].data(), port.capacity, port.input,
                        atomSequenceUrid_, atomChunkUrid_);
    }
    if (positionDue(frames))
        for (const AtomPort& port : atomPorts_)
            if (port.timePosition) writePosition(atomBuffers_[port.index].data(), port.capacity);

    lilv_instance_run(instance_, frames);
    // Meters: one relaxed store per output control, no allocation.
    for (const std::uint32_t index : outputControls_)
        outputMirror_[index].store(controlValues_[index], std::memory_order_relaxed);

    // The spec puts end_run in the run() context, after the cycle's responses have
    // been delivered. Called every cycle it exists rather than only on cycles that
    // delivered something -- the interface documents it as a cycle-end hook and the
    // plugins that implement it treat it as one.
    if (workerIface_ != nullptr && workerIface_->end_run != nullptr)
        workerIface_->end_run(lilv_instance_get_handle(instance_));
}

bool Instance::positionDue(std::uint32_t frames) noexcept
{
    // Sent when the transport starts, stops, jumps or changes tempo or meter;
    // a plug-in advances its own position between those.
    if (!context_.time.valid) { positionSent_ = false; return false; }
    const auto& c = context_;
    const bool due = !positionSent_ || c.time.playing != sentPlaying_ || c.tempo != sentTempo_
        || c.numerator != sentNumerator_ || c.denominator != sentDenominator_
        || c.time.frame != expectedFrame_;
    positionSent_ = true; sentPlaying_ = c.time.playing; sentTempo_ = c.tempo;
    sentNumerator_ = c.numerator; sentDenominator_ = c.denominator;
    expectedFrame_ = c.time.playing ? c.time.frame + std::int64_t(frames) : c.time.frame;
    return due;
}

void Instance::writePosition(std::uint8_t* buffer, std::uint32_t capacity) noexcept
{
    if (!forge_.Object || !time_.position) return;
    const auto& c = context_;
    lv2_atom_forge_set_buffer(&forge_, buffer, capacity);
    LV2_Atom_Forge_Frame sequence, object;
    if (!lv2_atom_forge_sequence_head(&forge_, &sequence, 0)) return;
    lv2_atom_forge_frame_time(&forge_, 0);
    lv2_atom_forge_object(&forge_, &object, 0, time_.position);
    lv2_atom_forge_key(&forge_, time_.frame); lv2_atom_forge_long(&forge_, c.time.frame);
    lv2_atom_forge_key(&forge_, time_.speed); lv2_atom_forge_float(&forge_, c.time.playing ? 1.0f : 0.0f);
    if (c.hasMusic() && sampleRate_ > 0) {
        // time: beats are the meter's note value; ProcessContext tempo is in quarters.
        const double unitTempo = c.tempo * c.denominator / 4.0;
        const double beats = double(c.time.frame) / sampleRate_ * unitTempo / 60.0;
        const double bar = std::floor(beats / c.numerator);
        lv2_atom_forge_key(&forge_, time_.bar); lv2_atom_forge_long(&forge_, std::int64_t(bar));
        lv2_atom_forge_key(&forge_, time_.barBeat); lv2_atom_forge_float(&forge_, float(beats - bar * c.numerator));
        lv2_atom_forge_key(&forge_, time_.beatUnit); lv2_atom_forge_int(&forge_, c.denominator);
        lv2_atom_forge_key(&forge_, time_.beatsPerBar); lv2_atom_forge_float(&forge_, float(c.numerator));
        lv2_atom_forge_key(&forge_, time_.beatsPerMinute); lv2_atom_forge_float(&forge_, float(unitTempo));
    }
    lv2_atom_forge_pop(&forge_, &object);
    lv2_atom_forge_pop(&forge_, &sequence);
}

void Instance::warmUp(std::uint32_t blocks, std::uint32_t frames) noexcept
{
    if (!active_ || !ready())
        return;
    for (std::uint32_t i = 0; i < blocks; ++i)
        run(frames);
}

void Instance::setControl(std::uint32_t portIndex, float value) noexcept
{
    if (portIndex >= portCount_)
        return;
    for (const ControlPort& control : controls_) {
        if (control.index != portIndex)
            continue;
        if (control.output)
            return;                     // a meter is not a knob
        controlValues_[portIndex] = control.clamp(value);
        return;
    }
}

float Instance::outputValue(std::uint32_t portIndex) const noexcept
{
    if (portIndex >= portCount_ || !outputMirror_)
        return 0.0f;
    return outputMirror_[portIndex].load(std::memory_order_relaxed);
}

float Instance::controlValue(std::uint32_t portIndex) const noexcept
{
    if (portIndex >= portCount_)
        return 0.0f;
    return controlValues_[portIndex];
}

LV2_Worker_Status Instance::scheduleWork(LV2_Worker_Schedule_Handle handle,
                                         std::uint32_t size, const void* data)
{
    auto* self = static_cast<Instance*>(handle);
    if (self == nullptr || self->workerIface_ == nullptr)
        return LV2_WORKER_ERR_UNKNOWN;

    if (self->worker_ != nullptr)
        return self->worker_->schedule(self, size, data);

    // No worker attached: a standalone instance on a thread where blocking is legal.
    // Run the work right here, synchronously; whatever it responds with is delivered
    // at the top of the next run(), which is the earliest moment the spec allows.
    self->performWork(size, data);
    return LV2_WORKER_SUCCESS;
}

LV2_Worker_Status Instance::respondTrampoline(LV2_Worker_Respond_Handle handle,
                                              std::uint32_t size, const void* data)
{
    auto* self = static_cast<Instance*>(handle);
    if (self == nullptr || (data == nullptr && size > 0))
        return LV2_WORKER_ERR_UNKNOWN;

    if (self->worker_ != nullptr)
        return self->worker_->respond(self, size, data);

    if (size > Worker::kMaxPayload)
        return LV2_WORKER_ERR_NO_SPACE;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    self->syncResponses_.emplace_back(bytes, bytes + size);
    return LV2_WORKER_SUCCESS;
}

void Instance::performWork(std::uint32_t size, const void* data) noexcept
{
    if (instance_ == nullptr || workerIface_ == nullptr
        || workerIface_->work == nullptr)
        return;
    workerIface_->work(lilv_instance_get_handle(instance_),
                       &Instance::respondTrampoline, this, size, data);
}

void Instance::deliverResponse(std::uint32_t size, const void* data) noexcept
{
    if (instance_ == nullptr || workerIface_ == nullptr
        || workerIface_->work_response == nullptr)
        return;
    workerIface_->work_response(lilv_instance_get_handle(instance_), size, data);
}

std::uint32_t Instance::latencyFrames() const noexcept
{
    if (latencyPort_ == kNoPort)
        return 0;
    const float frames = controlValues_[latencyPort_];
    if (!std::isfinite(frames) || frames < 0 || static_cast<double>(frames) >= std::numeric_limits<std::uint32_t>::max())
        return std::numeric_limits<std::uint32_t>::max();
    return static_cast<std::uint32_t>(frames);
}

} // namespace omawave::lv2
