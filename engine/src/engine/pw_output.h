#pragma once

#include "transport.h"

#include <atomic>
#include <cstdint>
#include <string>

struct pw_thread_loop;
struct pw_stream;

struct spa_io_clock;

namespace omawave::engine {

// PipeWire playback. Owns the stream and the thread it runs on.
//
// BRIEF.md section 2: PipeWire only, no ALSA and no JACK-only path. The realtime
// callback does nothing but hand planes to Transport::process, which is where the
// no-allocation rule is tested.
class PwOutput {
public:
    static constexpr std::uint32_t kMaxChannels = 8;

    PwOutput(Transport& transport, std::uint32_t sampleRate, std::uint32_t channels,
             std::uint32_t quantum, CaptureClock* capture = nullptr) noexcept;
    ~PwOutput();

    PwOutput(const PwOutput&) = delete;
    PwOutput& operator=(const PwOutput&) = delete;
    PwOutput(PwOutput&&) = delete;
    PwOutput& operator=(PwOutput&&) = delete;

    [[nodiscard]] bool start(std::string& error);
    // Main thread: whether start() succeeded and the stream is open.
    [[nodiscard]] bool running() const noexcept { return running_; }
    void stop() noexcept;
    // Main thread, stopped only. The next stream negotiates this project rate.
    bool setSampleRateStopped(std::uint32_t rate) noexcept;

    [[nodiscard]] std::uint32_t sampleRate() const noexcept { return sampleRate_; }
    [[nodiscard]] std::uint32_t channels() const noexcept { return channels_; }
    // The PipeWire graph's clock rate while the stream runs, 0 when unknown. The
    // stream itself runs at sampleRate(); PipeWire converts between the two, so a
    // difference is worth showing, not hiding.
    [[nodiscard]] std::uint32_t graphRate() const noexcept { return graphRate_.load(std::memory_order_relaxed); }
    // Frames per callback: the last block the device asked for, else the requested quantum.
    [[nodiscard]] std::uint32_t blockFrames() const noexcept {
        const auto last = lastFrames_.load(std::memory_order_relaxed);
        return last ? last : quantum_;
    }

private:
    static void onProcess(void* userdata) noexcept;
    static void onStateChanged(void* userdata, int oldState, int newState,
                               const char* error) noexcept;
    static void onIoChanged(void* userdata, std::uint32_t id, void* area, std::uint32_t size) noexcept;

    Transport& transport_;
    std::uint32_t sampleRate_;
    std::uint32_t channels_;
    std::uint32_t quantum_;

    pw_thread_loop* loop_ = nullptr;
    pw_stream* stream_ = nullptr;
    bool running_ = false;
    ::spa_io_clock* clock_ = nullptr;           // set by io_changed on the data thread
    std::atomic<std::uint32_t> graphRate_{0};
    std::atomic<std::uint32_t> lastFrames_{0};
    CaptureClock* capture_ = nullptr;
    std::uint64_t captureEpoch_ = 0;
    std::uint64_t captureFrames_ = 0;
};

} // namespace omawave::engine
