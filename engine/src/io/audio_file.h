#pragma once

#include "session/audio_buffer.h"
#include "session/metadata.h"

#include <cstdint>
#include <string>
#include <atomic>
#include <functional>

namespace omawave::io {

// Result of reading an audio file from disk.
//
// No exceptions: BRIEF.md section 8 bans them across the audio thread boundary, and
// having one error convention rather than two is worth more than the ergonomics of
// throwing on a worker.
struct AudioFile {
    bool ok = false;
    std::string error;

    session::AudioBuffer audio;
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    std::string formatName;
    // bext, cue markers (in this file's frames), iXML and INFO strings, when present.
    session::AudioMetadata metadata;
};

// Metadata without decoding the audio. Cheap enough to call before deciding whether
// to import.
struct AudioFileInfo {
    bool ok = false;
    std::string error;

    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    std::int64_t frames = 0;
    std::string formatName;
};

[[nodiscard]] AudioFileInfo probeAudioFile(const std::string& path);

// Decodes to 32-bit float, non-interleaved (BRIEF.md section 2). Integer formats are
// scaled to [-1, 1] by libsndfile. Does not resample -- that is the importer's job,
// so that the "resample on import, never mid-graph" rule has exactly one home.
[[nodiscard]] AudioFile readAudioFile(const std::string& path);
// The same, with cancellation and progress for the formats that decode through an
// FFmpeg subprocess (AAC/M4A) and block reads through libsndfile. A file that reads
// but with reservations (truncated, damaged) comes back ok with `error` holding
// the reservation, as the libsndfile path already does for a short file.
[[nodiscard]] AudioFile readAudioFile(const std::string& path, const std::atomic<bool>* cancel,
                                      const std::function<void(float)>& progress);
// Which compressed containers this box can import, for the file chooser and the
// message when it cannot.
struct ImportSupport { bool aac = false; std::string reason; };
[[nodiscard]] ImportSupport importSupport();

// Writes float32 WAV. BRIEF.md section 4: audio inside a project is always float32
// WAV, so this deliberately offers no format choice -- a project that could hold
// several encodings is a project that has to handle several encodings.
//
// Returns an empty string on success, or the reason it failed.
[[nodiscard]] std::string writeFloatWav(const std::string& path,
                                        const session::AudioBuffer& audio,
                                        std::uint32_t sampleRate, const std::atomic<bool>* cancel = nullptr,
                                        const std::function<void(float)>& progress = {});

} // namespace omawave::io
