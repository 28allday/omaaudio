#include "pw_capture.h"
#include "stream_timing.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>

#include <algorithm>
#include <array>
#include <limits>

namespace omawave::engine {
PwCapture::~PwCapture() { stop(); }

void PwCapture::stateChanged(void* userdata, int oldState, int state, const char* error) noexcept
{
    auto& self = *static_cast<PwCapture*>(userdata);
    if (state == PW_STREAM_STATE_ERROR || error
        || (oldState == PW_STREAM_STATE_STREAMING && state == PW_STREAM_STATE_UNCONNECTED))
        self.error_.store(1, std::memory_order_relaxed);
}

void PwCapture::formatChanged(void* userdata, std::uint32_t id, const spa_pod* param) noexcept
{
    auto& self = *static_cast<PwCapture*>(userdata);
    if (id != SPA_PARAM_Format || !param) return;
    spa_audio_info_raw format{};
    const bool valid = spa_format_audio_raw_parse(param, &format) >= 0
        && format.format == SPA_AUDIO_FORMAT_F32P && format.rate == self.rate_
        && format.channels == self.channels_;
    self.formatReady_.store(valid, std::memory_order_release);
    if (!valid) self.error_.store(2, std::memory_order_relaxed);
}

void PwCapture::process(void* userdata) noexcept
{
    auto& self = *static_cast<PwCapture*>(userdata);
    pw_buffer* buffer = pw_stream_dequeue_buffer(self.stream_);
    if (!buffer) return;
    spa_buffer* data = buffer->buffer;
    if (!data || !self.formatReady_.load(std::memory_order_acquire)
        || self.error_.load(std::memory_order_relaxed) != 0 || self.sink_.inputDone()) {
        pw_stream_queue_buffer(self.stream_, buffer);
        return;
    }
    std::array<const float*, 2> planes{};
    std::uint32_t frames = std::numeric_limits<std::uint32_t>::max();
    bool valid = data->n_datas >= self.channels_;
    for (std::uint32_t c = 0; valid && c < self.channels_; ++c) {
        const auto& plane = data->datas[c];
        const auto* chunk = plane.chunk;
        if (!chunk || !plane.data || chunk->offset > plane.maxsize
            || chunk->size > plane.maxsize - chunk->offset
            || chunk->offset % sizeof(float) != 0 || chunk->size % sizeof(float) != 0
            || (chunk->stride != 0 && chunk->stride != sizeof(float))
            || (chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)) {
            valid = false;
            break;
        }
        frames = std::min(frames, chunk->size / static_cast<std::uint32_t>(sizeof(float)));
        if (!(chunk->flags & SPA_CHUNK_FLAG_EMPTY))
            planes[c] = reinterpret_cast<const float*>(static_cast<const std::uint8_t*>(plane.data) + chunk->offset);
    }
    if (valid) {
        pw_time timing{};
        if (frames > 0 && pw_stream_get_time_n(self.stream_, &timing, sizeof(timing)) == 0) {
            const auto delay = streamDelayNs(timing.delay, timing.rate.num, timing.rate.denom, 0, timing.buffered, self.rate_);
            const auto at = buffer->time > 0 && buffer->time <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                ? static_cast<std::int64_t>(buffer->time) : timing.now;
            if (delay >= 0 && at > delay) self.sink_.firstInputTime(at - delay);
        }
        self.sink_.ingest(planes.data(), self.channels_, frames);
    }
    else self.error_.store(3, std::memory_order_relaxed);
    pw_stream_queue_buffer(self.stream_, buffer);
}

bool PwCapture::start(const std::string& target, std::string& error, bool autoconnect)
{
    if (running_) return true;
    loop_ = pw_thread_loop_new("omaaudio-capture", nullptr);
    if (!loop_) { error = "could not create the PipeWire capture loop"; return false; }
    pw_stream_events events{};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.process = &PwCapture::process;
    events.param_changed = &PwCapture::formatChanged;
    events.state_changed = [](void* data, pw_stream_state oldState, pw_stream_state state, const char* message) {
        stateChanged(data, oldState, state, message);
    };
    // Listener storage must outlive the stream; pw_stream copies the listener
    // pointer, not the event table. The functions are identical for all instances.
    static const pw_stream_events callbacks = events;
    auto* properties = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Production",
        PW_KEY_NODE_NAME, "omaaudio-capture", PW_KEY_NODE_DONT_RECONNECT, "true", nullptr);
    if (autoconnect) pw_properties_set(properties, "node.dont-fallback", "true");
    else pw_properties_set(properties, PW_KEY_NODE_AUTOCONNECT, "false");
    if (autoconnect && !target.empty()) pw_properties_set(properties, PW_KEY_TARGET_OBJECT, target.c_str());
    pw_thread_loop_lock(loop_);
    stream_ = pw_stream_new_simple(pw_thread_loop_get_loop(loop_), "OmaAudio recording", properties, &callbacks, this);
    if (!stream_) {
        pw_thread_loop_unlock(loop_);
        error = "could not create the PipeWire capture stream";
        stop();
        return false;
    }
    std::array<std::uint8_t, 1024> storage{};
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
    spa_audio_info_raw format{};
    format.format = SPA_AUDIO_FORMAT_F32P;
    format.rate = rate_;
    format.channels = channels_;
    format.position[0] = channels_ == 1 ? SPA_AUDIO_CHANNEL_MONO : SPA_AUDIO_CHANNEL_FL;
    if (channels_ == 2) format.position[1] = SPA_AUDIO_CHANNEL_FR;
    const spa_pod* params[]{spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format)};
    const int result = pw_stream_connect(stream_, PW_DIRECTION_INPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>((autoconnect ? PW_STREAM_FLAG_AUTOCONNECT : 0) | PW_STREAM_FLAG_MAP_BUFFERS
            | PW_STREAM_FLAG_RT_PROCESS | PW_STREAM_FLAG_DONT_RECONNECT), params, 1);
    pw_thread_loop_unlock(loop_);
    if (result < 0) {
        error = std::string("PipeWire capture connection failed: ") + spa_strerror(result);
        stop();
        return false;
    }
    if (pw_thread_loop_start(loop_) < 0) {
        error = "could not start the PipeWire capture loop";
        stop();
        return false;
    }
    running_ = true;
    return true;
}

void PwCapture::stop() noexcept
{
    if (loop_ && running_) pw_thread_loop_stop(loop_);
    running_ = false;
    if (stream_) { pw_stream_destroy(stream_); stream_ = nullptr; }
    if (loop_) { pw_thread_loop_destroy(loop_); loop_ = nullptr; }
}

std::string PwCapture::error() const
{
    switch (error_.load(std::memory_order_relaxed)) {
    case 1: return "PipeWire recording input disconnected";
    case 2: return "PipeWire could not provide the requested recording format";
    case 3: return "PipeWire delivered an invalid recording buffer";
    default: return {};
    }
}
} // namespace omawave::engine
