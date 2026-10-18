#pragma once
#include "session/track.h"
#include "lv2/chain.h"
#include "command_queue.h"
#include <vector>

namespace omawave::engine {
class AutomationRecorder;
// Immutable policy plus an audio-owned runtime, retained across plan swaps.
// Private/offline render plans leave the runtime empty.
struct RecordingLane {
    host::AutomationRecording policy{};
    std::shared_ptr<AutomationRecorder> runtime{};
    double base = 1;
    // Revisions from an earlier arm are not a new gesture. A first no-motion
    // grab follows the processing position, which can lead the UI by latency.
    std::uint64_t armRevision = 0;
};

struct RecordedSpan {
    host::ParameterEvent event{};
    double appliedValue = 0; // first value after sampling the block-control grid
    [[nodiscard]] double valueAt(std::uint32_t offset) const noexcept {
        return event.automationSpanFrames > 1 ? std::lerp(event.value, event.automationEndValue,
            double(offset) / (event.automationSpanFrames - 1)) : event.value;
    }
};

class AutomationRecorder {
public:
    // Audio owner only. A segment is committed after rendering, never per sample.
    [[nodiscard]] RecordedSpan begin(const RecordingLane&, const session::AutomationCurve&,
                                    const host::ProcessContext&, std::uint32_t frames,
                                    std::uint32_t gridFrames = 1) noexcept;
    [[nodiscard]] RecordedSpan begin(const RecordingLane&, const host::AutomationPlayback&,
                                    const host::ProcessContext&, std::uint32_t frames,
                                    std::uint32_t gridFrames = 1) noexcept;
    [[nodiscard]] std::uint32_t blockLimit(const host::AutomationRecording&, std::uint32_t limit) const noexcept;
    void appliedValue(double value) noexcept { last_ = value; }
    // Preflight multiple spans before rendering any of them. The main-thread
    // consumer can only free space between this check and their commits.
    [[nodiscard]] bool prepareCapacity(std::uint64_t pass, std::size_t count) noexcept;
    void commit(const RecordedSpan&) noexcept;
    // Control owner only; bounded SPSC feedback, including explicit backpressure.
    [[nodiscard]] bool next(host::ParameterEvent& event) noexcept { return events_.pop(event); }
    [[nodiscard]] std::uint64_t takeOverflow() noexcept { return overflow_.exchange(0, std::memory_order_acq_rel); }
private:
    CommandQueue<host::ParameterEvent, 1024> events_;
    std::atomic<std::uint64_t> overflow_{0};
    host::AutomationRecording previous_{};
    host::ParameterTime previousTime_{};
    session::FramePos nextFrame_ = 0, gridAnchor_ = 0;
    std::uint64_t sequence_ = 0, loopIteration_ = 0, blockedPass_ = 0;
    double held_ = 0, last_ = 0, from_ = 0, to_ = 0;
    std::uint32_t elapsed_ = 0, duration_ = 0;
    bool known_ = false, latched_ = false, returning_ = false;
};

// The same lookup as a normal curve, with the audio owner's current write/return.
// One cursor per rendered lane/segment; destruction acknowledges that segment.
class RecordingCursor {
public:
    RecordingCursor(const session::AutomationCurve&, double base, const RecordingLane&,
                    const host::ProcessContext&, std::uint32_t frames) noexcept;
    ~RecordingCursor() { if (runtime_) runtime_->commit(span_); }
    RecordingCursor(const RecordingCursor&) = delete;
    RecordingCursor& operator=(const RecordingCursor&) = delete;
    [[nodiscard]] bool active() const noexcept { return active_ || span_.event.automationSpanFrames; }
    [[nodiscard]] float valueAt(session::FramePos frame, double base) noexcept;
private:
    session::AutomationCursor cursor_;
    AutomationRecorder* runtime_ = nullptr;
    RecordedSpan span_{};
    bool active_ = false;
    double base_ = 0;
};

// One automated plugin control, as the audio thread drives it (W10).
//
// How a value reaches the plugin depends on the slot's host::Timing:
//   * with sample offsets (CLAP), applyControlAutomation schedules the value at
//     the block start, every point inside the block on its exact frame, and a
//     ramp one value per rampStep frames (1 for CLAP), skipping values that
//     did not change (so a discrete parameter gets one event per step of its
//     staircase and a flat segment none). When more values would be needed
//     than the instance can take in one block, the ramp step widens for that
//     block; points stay exact.
//   * without (LV2 control ports), the value is one per block: automationBlock
//     ends a block at the next point and on the rampStep grid, and the value is
//     read at the grid frame, so points are exact and ramps step every 64 frames.
// Live gestures (Write/Touch/Latch) are timestamped by the callback that sees
// them and stored as points; their playback is then as exact as the above.
struct ControlAutomation {
    std::size_t slot = 0;
    std::uint32_t port = 0;
    double base = 0;
    session::AutomationCurve curve{};
    host::AutomationRecording recording{};
    std::shared_ptr<AutomationRecorder> runtime{};
    std::uint64_t armRevision = 0;
    // SIZE_MAX preserves legacy shared automation; otherwise only this actual
    // processor receives this lane's input (0/1 are left/right for dual mono).
    std::size_t actual = SIZE_MAX;
};
using ControlAutomations = std::vector<ControlAutomation>;
[[nodiscard]] ControlAutomations buildControlAutomation(const std::vector<session::Insert>& inserts);
// The most frames the next process() may cover from `start` so every
// block-constant control sees its points and grid; slots with sample offsets
// impose nothing. `limit` is the caller's own bound.
[[nodiscard]] std::uint32_t automationBlock(const lv2::Chain& chain, const ControlAutomations& controls,
    session::FramePos start, std::uint32_t limit) noexcept;
// Audio thread, before process(frames): hands every control its values for
// [start, start + frames). Allocates nothing.
void applyControlAutomation(lv2::Chain& chain, const ControlAutomations& controls,
    session::FramePos start, std::uint32_t frames) noexcept;
// Live processing also acknowledges block-control recording after the run.
void processControlAutomation(lv2::Chain&, const ControlAutomations&, session::FramePos start,
    std::uint32_t frames, bool useSidechain, const host::ProcessContext&) noexcept;
void resetControlAutomation(lv2::Chain& chain, const ControlAutomations& controls,
    const ControlAutomations* replacement = nullptr) noexcept;
} // namespace omawave::engine
