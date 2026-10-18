#pragma once

#include "audio_buffer.h"
#include "types.h"

#include <cstdint>
#include <string>

// Generated test and production audio: tones, sweeps and noise. Square, sawtooth
// and triangle waves are band-limited (PolyBLEP) so they do not alias.
namespace omawave::session {

enum class ToneShape { Sine, Square, Triangle, Sawtooth };
enum class NoiseColour { White, Pink, Brown };

struct ToneSpec {
    ToneShape shape = ToneShape::Sine;
    double frequency = 1000;        // Hz
    double endFrequency = 0;        // Hz; non-zero sweeps logarithmically to it
    double amplitudeDb = -18;       // peak of the ideal waveform, dBFS (band-limited edges ring slightly above it)
    FramePos frames = 0;
    std::uint32_t channels = 1;
    std::uint32_t sampleRate = 48000;
};

// Empty buffer plus a reason when the settings are out of range.
[[nodiscard]] AudioBuffer generateTone(const ToneSpec& spec, std::string& error);

struct NoiseSpec {
    NoiseColour colour = NoiseColour::White;
    double amplitudeDb = -18;       // RMS level, dBFS
    FramePos frames = 0;
    std::uint32_t channels = 1;
    std::uint64_t seed = 1;         // channels are independent; equal seeds repeat exactly
};

[[nodiscard]] AudioBuffer generateNoise(const NoiseSpec& spec, std::string& error);

} // namespace omawave::session
