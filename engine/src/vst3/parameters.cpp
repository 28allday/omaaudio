#include "parameters.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace omawave::vst3 {
using namespace Steinberg;
using namespace Steinberg::Vst;
namespace {
constexpr std::size_t kMaxParameters = 16384, kMaxEvents = 1024 * 1024;
bool valid(double value) noexcept { return std::isfinite(value) && value >= 0 && value <= 1; }
template<class Interface> tresult query(Interface* self, const TUID id, void** output) noexcept {
    if (!output) return kInvalidArgument;
    *output = nullptr;
    if (!id || (std::memcmp(id, Interface::iid.toTUID(), 16) && std::memcmp(id, FUnknown::iid.toTUID(), 16))) return kNoInterface;
    *output = self; self->addRef(); return kResultOk;
}
auto identities(std::span<const ParamID> ids, std::uint32_t frames) {
    if (ids.size() > kMaxParameters || !frames || frames > 65536) throw std::invalid_argument("invalid VST3 parameter queue limits");
    std::vector<std::pair<ParamID, std::uint32_t>> slots;
    slots.reserve(ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (ids[i] > kMaxParamId) throw std::invalid_argument("VST3 plugin parameter uses a host-reserved identity");
        slots.emplace_back(ids[i], i);
    }
    std::sort(slots.begin(), slots.end());
    for (std::size_t i = 1; i < slots.size(); ++i)
        if (slots[i - 1].first == slots[i].first) throw std::invalid_argument("duplicate VST3 parameter identity");
    return slots;
}
std::size_t slotOf(const std::vector<std::pair<ParamID, std::uint32_t>>& slots, ParamID id) noexcept {
    const auto found = std::lower_bound(slots.begin(), slots.end(), id,
        [](const auto& entry, ParamID key) { return entry.first < key; });
    return found != slots.end() && found->first == id ? found->second : slots.size();
}
}

class ParameterQueue final : public IParamValueQueue {
public:
    ParamID id = kNoParamId;
    ParameterPoint* points = nullptr;
    std::uint32_t count = 0, capacity = 0, frames = 0;
    bool* rejected = nullptr; // writable feedback queue; null for sealed input
    tresult PLUGIN_API queryInterface(const TUID uid, void** output) override { return query<IParamValueQueue>(this, uid, output); }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    ParamID PLUGIN_API getParameterId() override { return id; }
    int32 PLUGIN_API getPointCount() override { return count; }
    tresult PLUGIN_API getPoint(int32 index, int32& offset, ParamValue& value) override {
        if (index < 0 || static_cast<std::uint32_t>(index) >= count) return kInvalidArgument;
        offset = points[index].offset; value = points[index].value; return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32 offset, ParamValue value, int32& index) override {
        index = -1;
        if (!rejected) return kNotImplemented;
        if (offset < 0 || static_cast<std::uint32_t>(offset) >= std::max(1u, frames) || !valid(value)) {
            *rejected = true; return kInvalidArgument;
        }
        std::uint32_t at = 0;
        while (at < count && points[at].offset < offset) ++at;
        if (at < count && points[at].offset == offset) { points[at].value = value; index = at; return kResultOk; }
        if (count == capacity) { *rejected = true; return kOutOfMemory; }
        for (auto i = count; i > at; --i) points[i] = points[i - 1];
        points[at] = {offset, value}; ++count; index = at; return kResultOk;
    }
};

ParameterInput::ParameterInput(std::span<const ParamID> ids, std::uint32_t capacity, std::uint32_t maxFrames)
    : slots_(identities(ids, maxFrames)), capacity_(capacity), maxFrames_(maxFrames)
{
    if (!capacity || capacity > kMaxEvents) throw std::invalid_argument("invalid VST3 input event capacity");
    queues_ = std::make_unique<ParameterQueue[]>(ids.size());
    events_.resize(capacity); points_.resize(std::size_t{capacity} * 2);
    final_.resize(ids.size()); baseline_.resize(ids.size()); ranges_.resize(ids.size()); active_.resize(ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i) queues_[i].id = ids[i];
}
ParameterInput::~ParameterInput() = default;
bool ParameterInput::begin(std::uint32_t frames, std::span<const double> values) noexcept
{
    count_ = activeCount_ = 0; begun_ = sealed_ = rejected_ = false;
    if (frames > maxFrames_ || values.size() != final_.size()
        || !std::all_of(values.begin(), values.end(), valid)) { rejected_ = true; return false; }
    std::copy(values.begin(), values.end(), final_.begin());
    std::copy(values.begin(), values.end(), baseline_.begin());
    std::fill(ranges_.begin(), ranges_.end(), Range{});
    frames_ = frames; begun_ = true; return true;
}
bool ParameterInput::push(ParamID id, double value, std::uint32_t offset) noexcept
{
    const auto slot = slotOf(slots_, id);
    if (!begun_ || sealed_ || slot == slots_.size() || offset >= std::max(1u, frames_) || !valid(value) || count_ == capacity_) {
        rejected_ = true; return false;
    }
    events_[count_] = {static_cast<std::uint32_t>(slot), offset, count_, value}; ++count_; return true;
}
void ParameterInput::seal() noexcept
{
    if (!begun_ || sealed_) return;
    std::sort(events_.begin(), events_.begin() + count_, [](const Event& a, const Event& b) {
        if (a.slot != b.slot) return a.slot < b.slot;
        if (a.offset != b.offset) return a.offset < b.offset;
        return a.order < b.order;
    });
    std::size_t position = 0;
    for (std::uint32_t i = 0; i < count_;) {
        const auto slot = events_[i].slot;
        ranges_[slot].begin = i;
        auto& queue = queues_[slot]; queue.points = points_.data() + position; queue.count = 0;
        auto previous = int32{-1}; auto value = final_[slot];
        while (i < count_ && events_[i].slot == slot) {
            const auto offset = events_[i].offset;
            while (i + 1 < count_ && events_[i + 1].slot == slot && events_[i + 1].offset == offset) ++i;
            // VST3 curves interpolate from the previous point (or implicit -1).
            // A hold/jump therefore needs the old value at offset-1 as well.
            if (offset > 0 && previous < static_cast<int32>(offset) - 1) {
                points_[position++] = {static_cast<int32>(offset) - 1, value}; ++queue.count;
            }
            value = events_[i].value; previous = offset;
            points_[position++] = {previous, value}; ++queue.count; ++i;
        }
        ranges_[slot].end = i;
        final_[slot] = value; active_[activeCount_++] = slot;
    }
    sealed_ = true;
}
double ParameterInput::finalValue(std::size_t slot) const noexcept { return slot < final_.size() ? final_[slot] : 0; }
ParameterPoint ParameterInput::valueAt(std::size_t slot, std::uint32_t offset) const noexcept {
    if (!sealed_ || slot >= ranges_.size()) return {-1, 0};
    const auto range = ranges_[slot];
    const auto first = events_.begin() + range.begin;
    const auto last = std::upper_bound(first, events_.begin() + range.end, offset,
        [](std::uint32_t sample, const Event& event) { return sample < event.offset; });
    if (last == first) return {-1, baseline_[slot]};
    const auto& event = *std::prev(last);
    return {static_cast<Steinberg::int32>(event.offset), event.value};
}
tresult PLUGIN_API ParameterInput::queryInterface(const TUID id, void** output) { return query<IParameterChanges>(this, id, output); }
int32 PLUGIN_API ParameterInput::getParameterCount() { return sealed_ ? activeCount_ : 0; }
IParamValueQueue* PLUGIN_API ParameterInput::getParameterData(int32 index) {
    return sealed_ && index >= 0 && static_cast<std::uint32_t>(index) < activeCount_ ? &queues_[active_[index]] : nullptr;
}
IParamValueQueue* PLUGIN_API ParameterInput::addParameterData(const ParamID&, int32& index) { index = -1; return nullptr; }

ParameterOutput::ParameterOutput(std::span<const ParamID> ids, std::uint32_t maxFrames)
    : slots_(identities(ids, maxFrames)), maxFrames_(maxFrames)
{
    queues_ = std::make_unique<ParameterQueue[]>(ids.size()); points_.resize(ids.size() * kPointsPerParameter);
    active_.resize(ids.size()); indices_.assign(ids.size(), -1);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        queues_[i].id = ids[i]; queues_[i].points = points_.data() + i * kPointsPerParameter;
        queues_[i].capacity = kPointsPerParameter; queues_[i].rejected = &rejected_;
    }
}
ParameterOutput::~ParameterOutput() = default;
bool ParameterOutput::begin(std::uint32_t frames) noexcept
{
    for (std::uint32_t i = 0; i < count_; ++i) {
        indices_[active_[i]] = -1; queues_[active_[i]].count = 0;
    }
    count_ = 0; rejected_ = false; begun_ = frames <= maxFrames_;
    if (!begun_) { rejected_ = true; return false; }
    frames_ = frames; return true;
}
tresult PLUGIN_API ParameterOutput::queryInterface(const TUID id, void** output) { return query<IParameterChanges>(this, id, output); }
int32 PLUGIN_API ParameterOutput::getParameterCount() { return count_; }
IParamValueQueue* PLUGIN_API ParameterOutput::getParameterData(int32 index) {
    return index >= 0 && static_cast<std::uint32_t>(index) < count_ ? &queues_[active_[index]] : nullptr;
}
IParamValueQueue* PLUGIN_API ParameterOutput::addParameterData(const ParamID& id, int32& index)
{
    index = -1;
    const auto slot = slotOf(slots_, id);
    if (!begun_ || slot == slots_.size()) { rejected_ = true; return nullptr; }
    if (indices_[slot] < 0) { indices_[slot] = count_; active_[count_++] = slot; queues_[slot].frames = frames_; }
    index = indices_[slot]; return &queues_[slot];
}
}
