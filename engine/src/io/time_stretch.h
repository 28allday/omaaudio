#pragma once
#include "session/audio_buffer.h"
#include <atomic>
#include <cstdint>
#include <string>
namespace omawave::io {
struct StretchResult {
    session::AudioBuffer audio;
    bool cancelled=false;
    std::string error;
};
// Worker-only. FFmpeg is spawned as a separate process, never linked or run via a shell.
[[nodiscard]] StretchResult stretchAudio(const session::AudioBuffer& input, std::uint32_t rate,
    std::size_t targetFrames, const std::atomic<bool>* cancel=nullptr);
}
