#include "automation.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace omawave::engine {
RecordedSpan AutomationRecorder::begin(const RecordingLane& lane, const session::AutomationCurve& curve,
                                       const host::ProcessContext& context, std::uint32_t frames,
                                       std::uint32_t gridFrames) noexcept
{
    gridFrames = std::max(1u, gridFrames);
    struct Playback { const session::AutomationCurve& curve; double base; std::uint32_t grid; } playback{curve, lane.base, gridFrames};
    return begin(lane, host::AutomationPlayback{&playback, [](const void* opaque, session::FramePos frame) noexcept {
        const auto& p = *static_cast<const Playback*>(opaque);
        return p.curve.preciseValueAt(p.grid > 1 ? p.curve.controlFrame(frame, p.grid) : frame, p.base);
    }}, context, frames, gridFrames);
}

RecordedSpan AutomationRecorder::begin(const RecordingLane& lane, const host::AutomationPlayback& playback,
                                       const host::ProcessContext& context, std::uint32_t frames,
                                       std::uint32_t gridFrames) noexcept
{
    RecordedSpan result;
    const auto& policy = lane.policy;
    gridFrames = std::max(1u, gridFrames);
    const auto readAt = [&](session::FramePos frame) {
        return playback.valueAt ? playback.valueAt(playback.context, frame) : lane.base;
    };
    auto time = context.recordTime();
    if (!policy.active() || !time.valid || !time.playing || !frames
        || time.frame > std::numeric_limits<std::int64_t>::max() - frames) {
        previous_ = {}; known_ = returning_ = latched_ = false; return result;
    }
    const auto mapped = [&](session::FramePos frame) {
        return context.hasLoop() && frame >= context.loopEnd
            ? context.loopStart + (frame - context.loopStart) % (context.loopEnd - context.loopStart) : frame;
    };
    const bool newPass = policy.pass != previous_.pass || policy.control != previous_.control || policy.mode != previous_.mode;
    const bool continuous = !newPass && previousTime_.valid && time.document == previousTime_.document
        && time.playbackEpoch == previousTime_.playbackEpoch && time.frame == mapped(nextFrame_);
    if (!continuous) { known_ = returning_ = latched_ = false; loopIteration_ = 0; }
    else if (time.frame < previousTime_.frame) { ++loopIteration_; gridAnchor_ = time.frame; }
    if (sequence_ != UINT64_MAX) ++sequence_; else return result;
    time.processSequence = sequence_; time.loopIteration = loopIteration_;
    const bool changed = newPass
        ? policy.mode == host::AutomationMode::Write || policy.manualRevision != lane.armRevision
        : policy.manualRevision != previous_.manualRevision;
    const bool started = policy.manualActive && (!continuous || !previous_.manualActive);
    const bool ended = continuous && previous_.manualActive && !policy.manualActive;
    const bool pending = policy.manualPending && changed;
    if (policy.mode == host::AutomationMode::Latch && (policy.manualActive || pending)) latched_ = true;
    const bool writing = policy.mode == host::AutomationMode::Write || policy.manualActive || pending || latched_;
    if (writing) {
        if (changed) held_ = policy.manualValue;
        else if (started) held_ = known_ ? last_ : readAt(time.frame);
        returning_ = false;
    } else if (ended && policy.mode == host::AutomationMode::Touch && policy.automatchFrames
               && time.frame <= std::numeric_limits<std::int64_t>::max() - policy.automatchFrames) {
        from_ = known_ ? last_ : held_;
        to_ = readAt(mapped(time.frame + policy.automatchFrames));
        duration_ = policy.automatchFrames; elapsed_ = 0; returning_ = true; gridAnchor_ = time.frame;
    }
    previous_ = policy; previousTime_ = time; nextFrame_ = time.frame + frames;
    if (pending) previous_.manualActive = true; // release after this applied segment, even without a GUI poll
    auto count = writing ? frames : returning_ ? std::min(frames, duration_ - elapsed_) : 0;
    if (count && (blockedPass_ == policy.pass || events_.full())) {
        blockedPass_ = policy.pass; overflow_.store(policy.pass, std::memory_order_release);
        returning_ = false; count = 0; // never render a write which cannot be acknowledged
    }
    if (count) {
        auto& event = result.event;
        event.kind = host::ParameterEventKind::Value; event.control = policy.control;
        event.time = time; event.automationPass = policy.pass; event.recordable = true;
        event.automationRevision = policy.manualRevision;
        event.automationFeedback = host::AutomationFeedback::Ramp; event.automationSpanFrames = count;
        event.automationGridFrames = gridFrames;
        event.value = writing ? held_ : std::lerp(from_, to_, double(elapsed_) / duration_);
        event.automationEndValue = writing ? held_ : std::lerp(from_, to_, double(elapsed_ + count - 1) / duration_);
        event.automationStep = writing ? 0 : (to_ - from_) / duration_;
        const auto sampled = std::max(gridAnchor_, time.frame - time.frame % gridFrames);
        result.appliedValue = writing ? held_ : event.value + (sampled - time.frame) * event.automationStep;
        if (returning_) { elapsed_ += count; returning_ = elapsed_ < duration_; }
    }
    last_ = count == frames ? (gridFrames > 1 ? result.appliedValue : result.event.automationEndValue)
        : readAt(time.frame + frames - 1);
    known_ = true;
    return result;
}

std::uint32_t AutomationRecorder::blockLimit(const host::AutomationRecording& policy, std::uint32_t limit) const noexcept
{
    if (policy.pass == previous_.pass && policy.control == previous_.control
        && policy.mode == host::AutomationMode::Touch && !policy.manualActive) {
        if (previous_.manualActive && policy.automatchFrames) return std::min(limit, policy.automatchFrames);
        if (returning_) return std::min(limit, duration_ - elapsed_);
    }
    return limit;
}

void AutomationRecorder::commit(const RecordedSpan& span) noexcept
{
    if (span.event.automationSpanFrames && !events_.push(span.event)) {
        blockedPass_ = span.event.automationPass;
        overflow_.store(blockedPass_, std::memory_order_release);
    }
}

bool AutomationRecorder::prepareCapacity(std::uint64_t pass, std::size_t count) noexcept
{
    if (blockedPass_ == pass || !events_.hasRoom(count)) {
        blockedPass_ = pass; overflow_.store(pass, std::memory_order_release); return false;
    }
    return true;
}

RecordingCursor::RecordingCursor(const session::AutomationCurve& curve, double base, const RecordingLane& lane,
                                 const host::ProcessContext& context, std::uint32_t frames) noexcept
    : cursor_(curve), runtime_(lane.runtime.get()), active_(curve.active()), base_(runtime_ ? lane.base : base)
{
    if (runtime_) span_ = runtime_->begin(lane, curve, context, frames);
}

float RecordingCursor::valueAt(session::FramePos frame, double base) noexcept
{
    const auto& event = span_.event;
    if (event.automationSpanFrames && frame >= event.time.frame
        && frame - event.time.frame < event.automationSpanFrames)
        return static_cast<float>(span_.valueAt(static_cast<std::uint32_t>(frame - event.time.frame)));
    return cursor_.valueAt(frame, static_cast<float>(runtime_ ? base_ : base));
}

namespace {
using session::FramePos;

// Integer and switch controls take whole values, whatever the curve says.
double quantise(const lv2::Chain& chain, const ControlAutomation& c, double value) noexcept
{
    if (const auto* ports = chain.controls(c.slot))
        for (const auto& port : *ports) if (port.index == c.port) {
            if (port.toggled) return value >= 0.5f ? 1.0f : 0.0f;
            if (port.integer) return std::round(value);
            break;
        }
    return value;
}

// Does this control move inside [start, end)? A flat curve costs no events.
bool ramping(const ControlAutomation& c, FramePos start, FramePos end) noexcept
{
    return c.curve.hasRamp(start, end);
}
} // namespace

ControlAutomations buildControlAutomation(const std::vector<session::Insert>& inserts)
{
    if(const auto error=session::validateAutomation(inserts);!error.empty()) throw std::runtime_error(error);
    ControlAutomations result;
    for (std::size_t slot = 0; slot < inserts.size(); ++slot)
        for (const auto& c : inserts[slot].controls) {
            if (!c.hasInstanceAutomation()) {
                if (c.automation.hasData()) result.push_back({slot, c.port, c.value, c.automation});
            } else for (std::size_t actual = 0; actual < c.instanceAutomation.size(); ++actual) {
                if (!c.instanceAutomation[actual] && !c.automation.hasData()) continue;
                result.push_back({slot, c.port, c.automationBase(actual), c.automationAt(actual)});
                result.back().actual = actual;
            }
        }
    return result;
}

std::uint32_t automationBlock(const lv2::Chain& chain, const ControlAutomations& controls,
    FramePos start, std::uint32_t limit) noexcept
{
    for (const auto& c : controls) {
        if (!c.curve.active() && !c.recording.active()) continue;
        const host::Timing timing = chain.timing(c.slot);
        if (timing.sampleOffsets) continue;        // events carry their own frames
        const auto step = static_cast<FramePos>(std::max<std::uint32_t>(1, timing.rampStep));
        const FramePos from = std::max(FramePos{0}, start);
        limit = static_cast<std::uint32_t>(std::min<FramePos>(limit, step - from % step));
        const auto next = c.curve.nextPoint(start);
        if (next > start) limit = static_cast<std::uint32_t>(std::min<FramePos>(limit, next - start));
        if (c.runtime) limit = c.runtime->blockLimit(c.recording, limit);
    }
    return limit;
}

static void applyControlAutomationImpl(lv2::Chain& chain, const ControlAutomations& controls,
    FramePos start, std::uint32_t frames, const ControlAutomation* manual, const RecordedSpan& span) noexcept
{
    const FramePos end = start + static_cast<FramePos>(frames);
    const auto recording = std::find_if(controls.begin(), controls.end(), [](const auto& c) { return c.recording.active(); });
    std::array<host::AutomationRecording, 2> policies{};
    std::array<host::AutomationPlayback, 2> playback{};
    if (recording != controls.end()) for (const auto& c : controls) {
        if (!c.recording.active() || c.slot != recording->slot) continue;
        for (std::size_t actual = 0; actual < policies.size(); ++actual) if (c.actual == SIZE_MAX || c.actual == actual) {
            policies[actual] = c.recording;
            playback[actual] = {&c, [](const void* data, std::int64_t frame) noexcept {
                const auto& control = *static_cast<const ControlAutomation*>(data);
                return control.curve.preciseValueAt(frame, control.base);
            }};
        }
    }
    chain.setInstanceAutomationRecording(recording == controls.end() ? SIZE_MAX : recording->slot, policies, playback);
    for (const auto& c : controls) {
        if (&c == manual && span.event.automationSpanFrames) {
            chain.setAutomationControl(c.slot, c.port, quantise(chain, c, span.appliedValue), c.actual);
            continue;
        }
        const host::Timing timing = chain.timing(c.slot);
        if (!timing.sampleOffsets || !c.curve.active() || frames == 0) {
            const auto step = static_cast<FramePos>(std::max<std::uint32_t>(1, timing.rampStep));
            chain.setAutomationControl(c.slot, c.port, quantise(chain, c, c.curve.preciseValueAt(c.curve.controlFrame(start, step), c.base)), c.actual);
            continue;
        }

        // The budget for this slot in this block: one value per control at the
        // start, the points, and the ramps at the finest step that fits.
        std::uint32_t slotControls = 0, rampingControls = 0;
        for (const auto& d : controls) if (d.slot == c.slot && (d.actual == c.actual || d.actual == SIZE_MAX || c.actual == SIZE_MAX)) {
            ++slotControls;
            if (ramping(d, start, end)) ++rampingControls;
        }
        FramePos step = static_cast<FramePos>(std::max<std::uint32_t>(1, timing.rampStep));
        const std::uint32_t reserved = slotControls * 2 + static_cast<std::uint32_t>(std::min<std::size_t>(c.curve.points.size(), 64));
        const std::uint32_t budget = timing.eventCapacity > reserved ? timing.eventCapacity - reserved : 1;
        const auto needed = static_cast<std::uint64_t>(frames) * rampingControls;
        if (needed > static_cast<std::uint64_t>(budget) * static_cast<std::uint64_t>(step))
            step = static_cast<FramePos>((needed + budget - 1) / budget);

        session::AutomationCursor cursor(c.curve);
        double last = quantise(chain, c, cursor.preciseValueAt(start, c.base));
        chain.scheduleControl(c.slot, c.port, last, 0, c.actual);
        FramePos at = start;
        while (true) {
            const FramePos nextPoint = cursor.nextPoint(at);
            const FramePos nextGrid = cursor.holdingAt(at) || step >= end - at ? end : at + step;
            const FramePos next = std::min(nextGrid, nextPoint);
            if (next >= end || next <= at) break;
            const double value = quantise(chain, c, cursor.preciseValueAt(next, c.base));
            if (value != last) {
                chain.scheduleControl(c.slot, c.port, value, static_cast<std::uint32_t>(next - start), c.actual);
                last = value;
            }
            at = next;
        }
    }
}

void applyControlAutomation(lv2::Chain& chain, const ControlAutomations& controls,
    FramePos start, std::uint32_t frames) noexcept
{
    applyControlAutomationImpl(chain, controls, start, frames, nullptr, {});
}

void processControlAutomation(lv2::Chain& chain, const ControlAutomations& controls, FramePos start,
    std::uint32_t frames, bool useSidechain, const host::ProcessContext& context) noexcept
{
    const auto found = std::find_if(controls.begin(), controls.end(), [](const auto& c) {
        return c.runtime && c.recording.active();
    });
    const auto* manual = found == controls.end() ? nullptr : &*found;
    RecordedSpan span;
    if (manual) {
        const RecordingLane lane{chain.bypassed(manual->slot) ? host::AutomationRecording{} : manual->recording,
            manual->runtime, manual->base, manual->armRevision};
        span = manual->runtime->begin(lane, manual->curve, context, frames, chain.timing(manual->slot).rampStep);
    }
    applyControlAutomationImpl(chain, controls, start, frames, manual, span);
    chain.process(frames, useSidechain, context);
    if (manual) {
        manual->runtime->appliedValue(chain.controlValue(manual->slot, manual->port,
            manual->actual == SIZE_MAX ? 0 : manual->actual));
        manual->runtime->commit(span);
    }
}

void resetControlAutomation(lv2::Chain& chain, const ControlAutomations& controls,
                            const ControlAutomations* replacement) noexcept
{
    for (const auto& c : controls) {
        // A recording curve publication must not reset the native processor to
        // the old base value. Removed/disarmed controls retain normal restoration.
        for (std::size_t actual = 0; actual < chain.instanceCount(c.slot); ++actual) {
            if (c.actual != SIZE_MAX && c.actual != actual) continue;
            if (replacement && std::any_of(replacement->begin(), replacement->end(), [&](const auto& next) {
                return next.slot == c.slot && next.port == c.port && next.recording.active()
                    && (next.actual == SIZE_MAX || next.actual == actual);
            })) continue;
            chain.setControl(c.slot, c.port, c.base, UINT64_MAX, actual);
        }
    }
}
} // namespace omawave::engine
