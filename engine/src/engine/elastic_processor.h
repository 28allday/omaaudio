#pragma once
#include "session/audio_buffer.h"
#include "session/clip.h"
#include <memory>
#include <atomic>

namespace omawave::engine {
// Worker only. One processor stream, bounded scratch, immutable original input.
// Fixed internal blocks make sequential output independent of reader block size.
class ElasticProcessor {
public:
    ElasticProcessor(const session::AudioBuffer& input, session::ElasticSpec spec, std::uint32_t rate);
    ~ElasticProcessor();
    ElasticProcessor(const ElasticProcessor&) = delete;
    ElasticProcessor& operator=(const ElasticProcessor&) = delete;
    void render(session::FramePos start, std::uint32_t frames, float* const* output,
                const std::atomic<bool>* cancel = nullptr);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace omawave::engine
