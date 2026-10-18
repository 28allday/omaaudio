#pragma once

#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace omawave::vst3 {
class ParameterQueue;
struct ParameterPoint { Steinberg::int32 offset = 0; double value = 0; };

// The process interfaces are borrowed, valid only for the owning instance's
// process call. Construction allocates on the control thread. Every other
// operation below, including sorting, is bounded and allocation-free.
class ParameterInput final : public Steinberg::Vst::IParameterChanges {
public:
    ParameterInput(std::span<const Steinberg::Vst::ParamID> ids, std::uint32_t eventCapacity,
                   std::uint32_t maxFrames);
    ~ParameterInput();
    ParameterInput(const ParameterInput&) = delete;
    ParameterInput& operator=(const ParameterInput&) = delete;

    // Baselines are the values actually held by the processor before this
    // block, in constructor ID order. Zero-frame flushes allow offset zero.
    [[nodiscard]] bool begin(std::uint32_t frames, std::span<const double> baselines) noexcept;
    // Hold semantics: value changes at this sample. Submission order breaks
    // ties; events may arrive out of timestamp/parameter order. Invalid/overflow
    // events are refused without replacing any accepted event.
    [[nodiscard]] bool push(Steinberg::Vst::ParamID, double value, std::uint32_t offset) noexcept;
    void seal() noexcept;
    [[nodiscard]] double finalValue(std::size_t slot) const noexcept;
    // Last accepted host change at/before this sample, or the block baseline
    // (offset -1). Synthetic hold points are not new host changes. Used to merge
    // processor feedback without moving an earlier output past later automation.
    [[nodiscard]] ParameterPoint valueAt(std::size_t slot, std::uint32_t offset) const noexcept;
    [[nodiscard]] bool rejected() const noexcept { return rejected_; }
    [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID, void**) override;
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }
    Steinberg::int32 PLUGIN_API getParameterCount() override;
    Steinberg::Vst::IParamValueQueue* PLUGIN_API getParameterData(Steinberg::int32) override;
    Steinberg::Vst::IParamValueQueue* PLUGIN_API addParameterData(const Steinberg::Vst::ParamID&, Steinberg::int32&) override;
private:
    struct Event { std::uint32_t slot = 0, offset = 0, order = 0; double value = 0; };
    std::vector<std::pair<Steinberg::Vst::ParamID, std::uint32_t>> slots_;
    std::unique_ptr<ParameterQueue[]> queues_;
    std::vector<Event> events_;
    std::vector<ParameterPoint> points_;
    std::vector<double> final_, baseline_;
    struct Range { std::uint32_t begin = 0, end = 0; };
    std::vector<Range> ranges_;
    std::vector<std::uint32_t> active_;
    std::uint32_t capacity_ = 0, maxFrames_ = 0, frames_ = 0, count_ = 0, activeCount_ = 0;
    bool begun_ = false, sealed_ = false, rejected_ = false;
};

class ParameterOutput final : public Steinberg::Vst::IParameterChanges {
public:
    // At most 16 feedback points per parameter. The plugin receives failure
    // on overflow; the owner can report it via rejected(), never a silent loss.
    static constexpr std::size_t kPointsPerParameter = 16;
    explicit ParameterOutput(std::span<const Steinberg::Vst::ParamID> ids, std::uint32_t maxFrames);
    ~ParameterOutput();
    ParameterOutput(const ParameterOutput&) = delete;
    ParameterOutput& operator=(const ParameterOutput&) = delete;
    [[nodiscard]] bool begin(std::uint32_t frames) noexcept;
    [[nodiscard]] bool rejected() const noexcept { return rejected_; }
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID, void**) override;
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }
    Steinberg::int32 PLUGIN_API getParameterCount() override;
    Steinberg::Vst::IParamValueQueue* PLUGIN_API getParameterData(Steinberg::int32) override;
    Steinberg::Vst::IParamValueQueue* PLUGIN_API addParameterData(const Steinberg::Vst::ParamID&, Steinberg::int32&) override;
private:
    std::vector<std::pair<Steinberg::Vst::ParamID, std::uint32_t>> slots_;
    std::unique_ptr<ParameterQueue[]> queues_;
    std::vector<ParameterPoint> points_;
    std::vector<std::uint32_t> active_;
    std::vector<Steinberg::int32> indices_;
    std::uint32_t maxFrames_ = 0, frames_ = 0, count_ = 0;
    bool begun_ = false, rejected_ = false;
};
}
