#pragma once
#include "session/audio_buffer.h"
#include <atomic>
#include <functional>
#include <string>

namespace omawave::io {
struct AacSupport { bool available=false; std::string executable, reason; };
// Non-realtime. Probes are cached by the resolved executable's file identity.
[[nodiscard]] AacSupport aacSupport(const std::atomic<bool>* cancel = nullptr);
struct AacResult { bool ok=false, cancelled=false; std::string error; };
// Worker only. output must be a private staging file, not the user's destination.
[[nodiscard]] AacResult encodeAac(const session::AudioBuffer&, std::uint32_t rate,
    unsigned bitrateKbps, const std::string& output, const std::atomic<bool>* cancel,
    const std::function<void(float)>& progress = {});

// Decoding through the same boundary: AAC/M4A (and the other MPEG-4 audio
// containers) are demuxed and decoded by an FFmpeg subprocess into a private
// float WAV in the cache, which the ordinary reader then loads. FFmpeg applies
// the container's edit list and the codec's priming/padding trim, so a file
// with gapless metadata comes back at its true length; one without it keeps
// the encoder's leading delay, and the result says so.
struct DecodeSupport { bool available=false; std::string executable, probeExecutable, reason; };
[[nodiscard]] DecodeSupport aacDecodeSupport(const std::atomic<bool>* cancel = nullptr);
[[nodiscard]] bool isCompressedAudioPath(const std::string& path);   // by extension: m4a, aac, mp4, m4b
struct DecodedInfo { bool ok=false; std::string error, codec; std::uint32_t sampleRate=0, channels=0;
                     double seconds=0; std::int64_t frames=0; };
[[nodiscard]] DecodedInfo probeCompressedAudio(const std::string& path, const std::atomic<bool>* cancel = nullptr);
struct DecodedAudio { bool ok=false, cancelled=false; std::string error, warning, stagedWav, codec;
                      std::uint32_t sampleRate=0, channels=0; std::int64_t declaredFrames=0; };
// Worker only. Writes the staged WAV under the cache and returns its path; the
// caller reads it with readAudioFile and removes it. Refuses a file whose decoded
// size would not fit the cache's free space (with a margin) or exceeds six hours.
[[nodiscard]] DecodedAudio decodeCompressedAudio(const std::string& path, const std::atomic<bool>* cancel,
    const std::function<void(float)>& progress = {});
} // namespace omawave::io
