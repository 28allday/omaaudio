#include "pw_output.h"

#include <ctime>
#include "stream_timing.h"

#include <pipewire/pipewire.h>
#include <spa/node/io.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>

#include <algorithm>
#include <array>
#include <cstdio>

namespace omawave::engine {
namespace {

// PipeWire's event structs are C aggregates; building them once at namespace scope
// keeps the setup path free of designated-initialiser warnings under -Wextra.
pw_stream_events makeStreamEvents(void (*process)(void*),
                                  void (*stateChanged)(void*, pw_stream_state,
                                                       pw_stream_state, const char*),
                                  void (*ioChanged)(void*, uint32_t, void*, uint32_t))
{
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.process = process;
    e.state_changed = stateChanged;
    e.io_changed = ioChanged;
    return e;
}

} // namespace

PwOutput::PwOutput(Transport& transport, std::uint32_t sampleRate, std::uint32_t channels,
                   std::uint32_t quantum, CaptureClock* capture) noexcept
    : transport_(transport), sampleRate_(sampleRate),
      channels_(std::min(channels, kMaxChannels)), quantum_(quantum), capture_(capture)
{
}

PwOutput::~PwOutput() { stop(); }

bool PwOutput::setSampleRateStopped(std::uint32_t rate) noexcept
{
    if (running_ || stream_ || rate < 8000 || rate > 192000) return false;
    sampleRate_ = rate;
    return true;
}

void PwOutput::onStateChanged(void* userdata, int oldState, int newState,
                              const char* error) noexcept
{
    (void)userdata;
    (void)oldState;
    (void)newState;
    if (error != nullptr)
        std::fprintf(stderr, "[pw] stream error: %s\n", error);
}

// Data thread. PipeWire hands the stream its clock area; the graph rate lives in it.
void PwOutput::onIoChanged(void* userdata, std::uint32_t id, void* area, std::uint32_t size) noexcept
{
    auto* self = static_cast<PwOutput*>(userdata);
    if (id != SPA_IO_Clock) return;
    self->clock_ = (area != nullptr && size >= sizeof(spa_io_clock)) ? static_cast<spa_io_clock*>(area) : nullptr;
    if (self->clock_ == nullptr) self->graphRate_.store(0, std::memory_order_relaxed);
}

// --- realtime callback -------------------------------------------------------------
// Audio thread. No allocation, no locks, no I/O, no logging (BRIEF.md section 2).

void PwOutput::onProcess(void* userdata) noexcept
{
    auto* self = static_cast<PwOutput*>(userdata);
    if (self->clock_ != nullptr && self->clock_->rate.num == 1)
        self->graphRate_.store(self->clock_->rate.denom, std::memory_order_relaxed);

    pw_buffer* b = pw_stream_dequeue_buffer(self->stream_);
    if (b == nullptr)
        return;

    spa_buffer* buf = b->buffer;

    // Non-interleaved: one plane per channel. Fixed-size array, so collecting the
    // pointers cannot allocate.
    std::array<float*, kMaxChannels> planes{};
    const std::uint32_t channelCount = std::min<std::uint32_t>(buf->n_datas, kMaxChannels);

    std::uint32_t frames = b->requested != 0
                         ? static_cast<std::uint32_t>(b->requested)
                         : self->quantum_;

    for (std::uint32_t c = 0; c < channelCount; ++c) {
        planes[c] = static_cast<float*>(buf->datas[c].data);
        const std::uint32_t capacity = buf->datas[c].maxsize / sizeof(float);
        if (capacity < frames)
            frames = capacity;
    }

    // Audio load: the callback's own time against the block's deadline, and whether
    // PipeWire says the previous cycle missed it. clock_gettime is a vDSO read.
    const bool dropout = self->clock_ != nullptr && (self->clock_->flags & SPA_IO_CLOCK_FLAG_XRUN_RECOVER);
    timespec started{};
    ::clock_gettime(CLOCK_MONOTONIC, &started);
    self->transport_.process(planes.data(), channelCount, frames);
    timespec finished{};
    ::clock_gettime(CLOCK_MONOTONIC, &finished);
    const auto elapsed = std::int64_t(finished.tv_sec - started.tv_sec) * 1000000000 + (finished.tv_nsec - started.tv_nsec);
    self->lastFrames_.store(frames, std::memory_order_relaxed);
    if (self->sampleRate_ > 0 && frames > 0)
        self->transport_.recordLoad(std::uint64_t(std::max<std::int64_t>(0, elapsed)),
                                    std::uint64_t(frames) * 1000000000ULL / self->sampleRate_, dropout);
    if (self->capture_ && self->transport_.captureEpoch() != self->captureEpoch_) {
        self->captureEpoch_ = self->transport_.captureEpoch();
        self->captureFrames_ = 0;
    }
    if (self->capture_ && self->captureEpoch_ > 0 && self->transport_.playing()
        && self->capture_->firstOutputNs.load(std::memory_order_relaxed) == 0) {
        pw_time timing{};
        if (pw_stream_get_time_n(self->stream_, &timing, sizeof(timing)) == 0) {
            const auto delay = streamDelayNs(timing.delay, timing.rate.num, timing.rate.denom,
                timing.queued, timing.buffered, self->sampleRate_);
            if (delay >= 0 && timing.now > 0 && timing.now <= std::numeric_limits<std::int64_t>::max() - delay) {
                const auto elapsed = static_cast<long double>(self->captureFrames_) * 1000000000.0L / self->sampleRate_;
                if (elapsed < timing.now + delay)
                    self->capture_->firstOutputNs.store(timing.now + delay - static_cast<std::int64_t>(elapsed), std::memory_order_release);
            }
        }
        self->captureFrames_ += frames;
    }
    b->size = frames;

    for (std::uint32_t c = 0; c < channelCount; ++c) {
        buf->datas[c].chunk->offset = 0;
        buf->datas[c].chunk->stride = sizeof(float);
        buf->datas[c].chunk->size = frames * static_cast<std::uint32_t>(sizeof(float));
    }

    pw_stream_queue_buffer(self->stream_, b);
}

bool PwOutput::start(std::string& error)
{
    if (running_)
        return true;

    loop_ = pw_thread_loop_new("omawave-engine", nullptr);
    if (loop_ == nullptr) {
        error = "pw_thread_loop_new failed";
        return false;
    }

    const std::string latency = std::to_string(quantum_) + "/" + std::to_string(sampleRate_);
    const std::string rate = "1/" + std::to_string(sampleRate_);

    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Production",
        PW_KEY_APP_NAME, "OmaAudio",
        PW_KEY_NODE_NAME, "omaaudio",
        PW_KEY_NODE_DESCRIPTION, "OmaAudio",
        PW_KEY_NODE_LATENCY, latency.c_str(),
        PW_KEY_NODE_RATE, rate.c_str(),
        nullptr);

    static const pw_stream_events events = makeStreamEvents(&PwOutput::onProcess, nullptr,
                                                            &PwOutput::onIoChanged);
    (void)&PwOutput::onStateChanged;   // kept for diagnostics; not wired in Phase 1

    pw_thread_loop_lock(loop_);

    stream_ = pw_stream_new_simple(pw_thread_loop_get_loop(loop_), "omaaudio", props,
                                   &events, this);
    if (stream_ == nullptr) {
        pw_thread_loop_unlock(loop_);
        error = "pw_stream_new_simple failed";
        return false;
    }

    std::array<std::uint8_t, 1024> podBuffer{};
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(
        podBuffer.data(), static_cast<std::uint32_t>(podBuffer.size()));

    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32P;      // section 2: float, non-interleaved
    info.rate = sampleRate_;
    info.channels = channels_;
    if (channels_ >= 2) {
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
    } else {
        info.position[0] = SPA_AUDIO_CHANNEL_MONO;
    }

    std::array<const spa_pod*, 1> params{
        spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info)};

    const int rc = pw_stream_connect(
        stream_, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT
                                     | PW_STREAM_FLAG_MAP_BUFFERS
                                     | PW_STREAM_FLAG_RT_PROCESS),
        params.data(), static_cast<std::uint32_t>(params.size()));

    pw_thread_loop_unlock(loop_);

    if (rc < 0) {
        error = std::string("pw_stream_connect failed: ") + spa_strerror(rc);
        return false;
    }

    if (pw_thread_loop_start(loop_) < 0) {
        error = "pw_thread_loop_start failed";
        return false;
    }

    running_ = true;
    return true;
}

void PwOutput::stop() noexcept
{
    if (loop_ != nullptr && running_)
        pw_thread_loop_stop(loop_);
    running_ = false;

    if (stream_ != nullptr) {
        pw_stream_destroy(stream_);
        stream_ = nullptr;
    }
    clock_ = nullptr;
    graphRate_.store(0, std::memory_order_relaxed);
    if (loop_ != nullptr) {
        pw_thread_loop_destroy(loop_);
        loop_ = nullptr;
    }
}

} // namespace omawave::engine
