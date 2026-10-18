#include "clip_effect_runtime.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace omawave::engine {
using session::FramePos;

ClipEffectRuntime::ClipEffectRuntime(std::shared_ptr<const session::ClipRack> rack,
    lv2::Chain& chain, FramePos timelineOrigin)
    : rack_(std::move(rack)), chain_(chain), channels_(chain.channels()),
      maxBlock_(chain.maxBlock()), latency_(chain.latencyFrames()),
      timelineOrigin_(timelineOrigin), endLocal_(0)
{
    if (!rack_ || !rack_->validOrigin() || rack_->inserts.size() > maxSlots
        || !channels_ || channels_ > maxChannels || !maxBlock_ || maxBlock_ > lv2::kMaxBlock)
        throw std::invalid_argument("invalid clip effect runtime configuration");
    constexpr auto maximum = std::numeric_limits<FramePos>::max();
    if (rack_->length > maximum - latency_)
        throw std::invalid_argument("clip effect latency exceeds the supported timeline");
    endLocal_ = rack_->length + latency_;
    if (timelineOrigin_ > maximum - endLocal_)
        throw std::invalid_argument("clip effect processing origin overflows the timeline");
    if (programStatus() != Status::Ready)
        throw std::invalid_argument("clip effect chain does not match its prepared rack");
    std::size_t storedControls = 0;
    for (const auto& insert : rack_->inserts) {
        if (insert.controls.size() > maxControls - storedControls)
            throw std::invalid_argument("too many clip effect controls");
        storedControls += insert.controls.size();
    }
    controls_ = buildControlAutomation(rack_->inserts);
    if (controls_.size() > maxControls)
        throw std::invalid_argument("too many clip effect automation controls");
    output_.resize(std::size_t(channels_) * maxBlock_);
}

ClipEffectRuntime::Status ClipEffectRuntime::programStatus() const noexcept
{
    if (chain_.channels() != channels_ || chain_.maxBlock() != maxBlock_
        || chain_.size() != rack_->inserts.size()) return Status::ProgramChanged;
    for (std::size_t slot = 0; slot < rack_->inserts.size(); ++slot) {
        const auto& insert = rack_->inserts[slot];
        const auto* identity = chain_.insertIdentity(slot);
        if (!identity || *identity != insert.identity || chain_.bypassed(slot) != insert.bypass
            || (!insert.bypass && !chain_.hasProcessor(slot))) return Status::ProgramChanged;
    }
    return chain_.latencyFrames() == latency_ ? Status::Ready : Status::LatencyChanged;
}

bool ClipEffectRuntime::samePlayback(const host::ProcessContext& context) const noexcept
{
    return !started_ || (previous_.time.document == context.time.document
        && previous_.time.playbackEpoch == context.time.playbackEpoch
        && previous_.time.playing == context.time.playing
        && previous_.looping == context.looping
        && (!context.looping || (previous_.loopStart == context.loopStart
            && previous_.loopEnd == context.loopEnd)));
}

ClipEffectRuntime::Status ClipEffectRuntime::preflight(FramePos local, std::uint32_t frames,
    const host::ProcessContext& global) const noexcept
{
    if (failed_) return Status::NeedsPreparation;
    if (!frames || frames > maxBlock_ || local < 0 || local > endLocal_
        || FramePos(frames) > endLocal_ - local) return Status::InvalidRange;
    if (!global.time.valid || global.time.frame != timelineOrigin_ + local
        || (global.looping && (!global.hasLoop() || (!global.preroll && global.time.frame < global.loopStart)
            || global.time.frame >= global.loopEnd
            || global.time.frame + frames > global.loopEnd))) return Status::InvalidContext;
    if (local != nextLocal_ || !samePlayback(global)) return Status::NeedsPreparation;
    return programStatus();
}

ClipEffectRuntime::Status ClipEffectRuntime::process(FramePos local,
    std::span<const float* const> input, std::uint32_t frames,
    const host::ProcessContext& global) noexcept
{
    if (input.size() != channels_) return Status::InvalidRange;
    const auto ready = preflight(local, frames, global);
    if (ready != Status::Ready) return ready;
    std::uint32_t offset = 0;
    while (offset < frames) {
        const auto count = automationBlock(chain_, controls_, local + offset, frames - offset);
        if (!count) { failed_ = true; outputFrames_ = 0; return Status::NeedsPreparation; }
        for (std::uint32_t c = 0; c < channels_; ++c) {
            if (input[c]) std::copy_n(input[c] + offset, count, chain_.input(c));
            else std::fill_n(chain_.input(c), count, 0.0f);
        }
        auto context = global;
        context.time.frame += offset;
        // This kernel plays immutable curves; it does not arm recording lanes.
        // Curve/grid lookup is local while all plugin transport context is global.
        processControlAutomation(chain_, controls_, local + offset, count, false, context);
        const auto status = programStatus();
        if (status != Status::Ready) {
            failed_ = true; outputFrames_ = 0;
            return status; // DSP advanced: only a fresh preparation can retry.
        }
        for (std::uint32_t c = 0; c < channels_; ++c)
            std::copy_n(chain_.output(c), count, output_.data() + std::size_t(c) * maxBlock_ + offset);
        offset += count;
    }
    nextLocal_ = local + frames;
    previous_ = global;
    started_ = true;
    outputFrames_ = frames;
    return Status::Ready;
}

ClipEffectRuntime::Status ClipEffectRuntime::continueLoop(FramePos local,
    const host::ProcessContext& global) noexcept
{
    if (failed_ || !started_) return Status::NeedsPreparation;
    if (local < 0 || local >= endLocal_ || !global.time.valid || !global.hasLoop()
        || !previous_.hasLoop() || !samePlayback(global)
        || timelineOrigin_ + nextLocal_ != global.loopEnd
        || global.time.frame != global.loopStart || timelineOrigin_ + local != global.loopStart)
        return Status::InvalidContext;
    const auto ready = programStatus();
    if (ready != Status::Ready) return ready;
    nextLocal_ = local;
    previous_ = global;
    return Status::Ready;
}

std::span<const float> ClipEffectRuntime::output(std::uint32_t channel) const noexcept
{
    return channel < channels_
        ? std::span<const float>(output_.data() + std::size_t(channel) * maxBlock_, outputFrames_)
        : std::span<const float>{};
}

} // namespace omawave::engine
