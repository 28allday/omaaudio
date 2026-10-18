#pragma once

#include "mask.h"
#include "stft.h"
#include "session/audio_buffer.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace omawave::spectral {

// The spectral repairs (brief section 8): attenuate part of a region, heal it from
// what surrounds it, reduce noise in it against a print. All rewrite samples -- the
// one thing nothing else in the editor does -- and all are written to leave
// everything outside the region and outside the mask alone: the STFT is run over
// the region plus one frame either side, and only [start, end) is written back,
// crossfaded over a hop at each edge.
//
// Each takes a MaskSource for what, inside the region, it touches: a band (one
// weight per bin, the same in every column) or drawn shapes (a weight per column
// and bin). The Band overloads are the same repairs with a band mask.
// Cancellation can leave a working buffer partly processed; the caller must
// discard it when cancelled, rather than publish it as a project source.

struct Region {
    std::size_t start = 0;  // first frame changed
    std::size_t end = 0;    // one past the last
};

// Multiplies what the mask covers inside the region by gainDb, every channel.
void attenuate(session::AudioBuffer& audio, std::uint32_t sampleRate, Region region,
               const MaskSource& mask, float gainDb, const StftConfig& config,
               const std::atomic<bool>* cancel = nullptr);
void attenuateBand(session::AudioBuffer& audio, std::uint32_t sampleRate, Region region,
                   Band band, float gainDb, const StftConfig& config,
                   const std::atomic<bool>* cancel = nullptr);

// Replaces what the mask covers with magnitudes interpolated across it, per bin,
// from `contextColumns` uncovered STFT columns either side of each covered run,
// keeping the original phase. A click, a cough or a whistle goes; a tone that
// continues through stays, because both sides agree on it.
void heal(session::AudioBuffer& audio, std::uint32_t sampleRate, Region region,
          const MaskSource& mask, const StftConfig& config, std::uint32_t contextColumns = 4,
          const std::atomic<bool>* cancel = nullptr);
void healBand(session::AudioBuffer& audio, std::uint32_t sampleRate, Region region, Band band,
              const StftConfig& config, std::uint32_t contextColumns = 4,
              const std::atomic<bool>* cancel = nullptr);

// A noise print: the mean magnitude of every bin over a stretch the user says is
// nothing but the noise (room tone, hiss, hum). One vector per channel of the
// capture; reduction on a take with more channels than the print uses the last.
struct NoisePrint {
    std::string channelScope = "all";
    std::uint32_t fftSize = 0;
    std::uint32_t bins = 0;
    Window window = Window::Hann;
    std::size_t columns = 0;                    // how many STFT columns were averaged
    std::vector<std::vector<float>> magnitude;  // [channel][bin], the Stft's own scale

    [[nodiscard]] bool empty() const noexcept { return bins == 0 || magnitude.empty(); }
};

[[nodiscard]] NoisePrint captureNoisePrint(const session::AudioBuffer& audio, Region region,
                                           const StftConfig& config, const std::atomic<bool>* cancel = nullptr);

// Spectral subtraction against the print, where the mask covers the region: each
// bin's magnitude loses `strength` times the print's, but never more than
// `amountDb` of it (the floor that keeps a voice from turning to water), and the
// gains are smoothed across neighbouring columns so what is left does not twinkle.
// Phase is kept. A print made at another FFT size is interpolated across bins.
void reduceNoise(session::AudioBuffer& audio, std::uint32_t sampleRate, Region region,
                 const MaskSource& mask, const NoisePrint& print, float amountDb, float strength,
                 const StftConfig& config, const std::atomic<bool>* cancel = nullptr);
void reduceNoise(session::AudioBuffer& audio, std::uint32_t sampleRate, Region region, Band band,
                 const NoisePrint& print, float amountDb, float strength, const StftConfig& config,
                 const std::atomic<bool>* cancel = nullptr);

} // namespace omawave::spectral
