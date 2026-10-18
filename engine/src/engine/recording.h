#pragma once

#include "audio_ring.h"
#include "session/audio_buffer.h"
#include "session/types.h"

#include <atomic>
#include <string>
#include <thread>

namespace omawave::engine {

// Stable for the lifetime of Transport, including while a take is finalized.
struct CaptureClock {
    std::atomic<std::uint64_t> epoch{0};
    std::atomic<session::FramePos> start{0}, frames{0};
    std::atomic<bool> active{false}, monitor{false};
    // A group can outlive its first input while delayed inputs finish draining.
    std::atomic<bool> groupActive{false};
    std::atomic<std::uint32_t> channels{1};
    std::atomic<std::int64_t> firstInputNs{0}, firstOutputNs{0};
    std::atomic<session::FramePos> countInFrames{0}, beatFrames{0};
    AudioRing<8192> monitoring;
};

struct RecordingResult {
    session::AudioBuffer audio;
    std::string path, error;
};

// Allocate on the main/worker thread, then feed on ONE capture callback thread.
// Stop the input producer before requestStop() or destruction. The writer is
// independent of the app's display/save worker so those jobs cannot starve capture.
class Recording {
public:
    Recording(CaptureClock& clock, std::string path, std::uint32_t sampleRate,
              std::uint32_t channels, session::FramePos frameLimit = 0);
    ~Recording();
    Recording(const Recording&) = delete;
    Recording& operator=(const Recording&) = delete;
    void ingest(const float* const* planes, std::uint32_t channels, std::uint32_t frames) noexcept;
    void requestStop() noexcept;
    void setFrameLimit(session::FramePos frames) noexcept { limit_.store(frames, std::memory_order_release); }
    void firstInputTime(std::int64_t ns) noexcept {
        if (clock_.firstInputNs.load(std::memory_order_relaxed) != 0) return;
        std::int64_t expected = 0;
        // A timing report can first become available after earlier buffers. Anchor
        // sample zero, not the start of whichever callback supplied the report.
        const auto elapsed = static_cast<long double>(captured_) * 1000000000.0L / rate_;
        if (elapsed >= ns) return;
        ns -= static_cast<std::int64_t>(elapsed);
        if (ns > 0) (void)clock_.firstInputNs.compare_exchange_strong(expected, ns, std::memory_order_release);
    }
    [[nodiscard]] bool ready() const noexcept { return ready_.load(std::memory_order_acquire); }
    [[nodiscard]] bool inputDone() const noexcept { return inputDone_.load(std::memory_order_acquire); }
    [[nodiscard]] bool finished() const noexcept { return finished_.load(std::memory_order_acquire); }
    [[nodiscard]] float peak() const noexcept { return peak_.load(std::memory_order_relaxed); }
    RecordingResult takeResult(); // only once finished()
    void wait(); // shutdown only; the UI normally polls finished()
private:
    void write();
    CaptureClock& clock_;
    std::string path_;
    std::uint32_t rate_, channels_;
    std::atomic<session::FramePos> limit_;
    session::FramePos captured_ = 0;
    AudioRing<262144> ring_;
    std::atomic<bool> ready_{false}, inputDone_{false}, finished_{false}, overflow_{false};
    std::atomic<bool> writerFailed_{false};
    std::atomic<float> peak_{0};
    RecordingResult result_;
    std::thread writer_;
};
} // namespace omawave::engine
