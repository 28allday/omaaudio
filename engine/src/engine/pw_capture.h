#pragma once

#include "recording.h"
#include <atomic>
#include <string>

struct pw_thread_loop;
struct pw_stream;
struct spa_pod;

namespace omawave::engine {
class PwCapture {
public:
    PwCapture(Recording& sink, std::uint32_t rate, std::uint32_t channels)
        : sink_(sink), rate_(rate), channels_(channels) {}
    ~PwCapture();
    PwCapture(const PwCapture&) = delete;
    PwCapture& operator=(const PwCapture&) = delete;
    // autoconnect=false leaves the stream unlinked (paused) for the caller to wire
    // port by port; a JACK client is not a target the session manager will
    // resolve, and falling back to another input would record the wrong thing.
    bool start(const std::string& target, std::string& error, bool autoconnect = true);
    void stop() noexcept;
    [[nodiscard]] std::string error() const;
private:
    static void process(void* userdata) noexcept;
    static void stateChanged(void* userdata, int oldState, int state, const char* error) noexcept;
    static void formatChanged(void* userdata, std::uint32_t id, const spa_pod* param) noexcept;
    Recording& sink_;
    std::uint32_t rate_, channels_;
    pw_thread_loop* loop_ = nullptr;
    pw_stream* stream_ = nullptr;
    bool running_ = false;
    std::atomic<bool> formatReady_{false};
    std::atomic<int> error_{0};
};
} // namespace omawave::engine
