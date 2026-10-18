#pragma once

#include "stft.h"
#include "mask.h"
#include "session/audio_buffer.h"

#include <cstddef>
#include <cstdint>
#include <atomic>

namespace omawave::spectral {

// Harmonic selection (amendment A24) starts from a pick on the display, and a
// pick is only as good as the pointer: a few pixels off on a logarithmic axis is
// a percent or two of frequency, which by the twentieth partial is a whole
// partial's spacing. So the pick is refined from the spectrum itself before the
// comb is built.
//
// One STFT frame centred on `centreFrame` (every channel summed), the loudest bin
// within `spanRatio` of `hz` -- never less than a bin either side -- and a
// parabolic fit through that bin's neighbours for the fraction. A peak on the
// edge of the search window belongs to something outside it, and a window with
// no peak in it (noise, silence, the space between partials -- anything more than
// 60 dB under the loudest bin of the frame) has nothing to refine towards: both
// leave `hz` as it was. Frames past either end of the buffer
// read as silence, as Stft::analyse has them.
[[nodiscard]] float refinePartial(const session::AudioBuffer& audio, std::uint32_t sampleRate,
                                  const StftConfig& config, std::size_t centreFrame, float hz,
                                  float spanRatio = 0.03f);

// Track a picked spectral peak both ways over [from, to), in buffer frames.
// At most 4097 points, in buffer seconds; long spans use a wider analysis step.
// Missing peaks hold the previous frequency. Invalid/unmeasurable seeds and
// cancellation return no points. FFT scratch is reused between frames.
[[nodiscard]] std::vector<MaskPoint> followPartial(const session::AudioBuffer& audio,
    std::uint32_t sampleRate, const StftConfig& config, std::size_t from, std::size_t to,
    std::size_t seed, float hz, const std::atomic<bool>* cancel = nullptr);

} // namespace omawave::spectral
