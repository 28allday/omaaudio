#pragma once

#include "repair.h"
#include "session/audio_buffer.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace omawave::spectral {

// Click and pop detection (amendment A23): the detector Auto Heal runs on, and
// the list the page marks.
//
// A click is a few samples that do not belong to the waveform around them. The
// second difference of the signal (x[i-1] - 2x[i] + x[i+1]) is small for anything
// smooth -- a tone's is its amplitude times omega squared, a fraction of a percent
// at speech frequencies -- and as large as the click itself for a click. Each
// sample's second difference is compared with the mean of it over the 20 ms around
// it; where the ratio passes a threshold, the sample is flagged, and flagged samples
// within a millisecond of each other are one event. Events longer than a pop can be
// (15 ms) are not clicks and are dropped. Channels are detected separately and
// merged, so a click on the left alone is still one event.
//
// `sensitivity` runs 0..1: 0 flags only what stands thirty times above its
// surroundings, 1 flags eight times. Noise peaks sit four or five times above their
// mean, a tone's under two, so 0.5 (nineteen) is a safe default. The mean is of the
// second difference, which bright material (cymbals, a lot of 5 kHz) makes large,
// so a quiet click in a bright mix wants the sensitivity up; and a drum hit can
// pass at 1, which is why the list is shown before it is healed.

struct Click {
    std::size_t frame = 0;      // first frame of the event
    std::size_t length = 1;     // frames, at least 1
    float strength = 0.0f;      // the largest ratio inside it
};

[[nodiscard]] std::vector<Click> detectClicks(const session::AudioBuffer& audio,
                                              std::uint32_t sampleRate, Region region,
                                              float sensitivity, const std::atomic<bool>* cancel = nullptr);

// The threshold `sensitivity` stands for, so the panel can say it.
[[nodiscard]] float clickThreshold(float sensitivity) noexcept;

// Time-domain repair (A33): predict inward from up to 512 clean samples per side
// with independent Burg models, then blend. Order <= 32, predictive gaps <= 1024
// samples. Short/unreliable context falls back to bounded cubic/linear interpolation.
// Overlapping events are merged, neighbouring gaps are excluded from training, and
// only the event plus one sample per side changes (two endpoint anchors are kept).
// Frames are the buffer's own. Runs on the repair worker, never the audio callback.
void repairClicks(session::AudioBuffer& audio, const std::vector<Click>& clicks,
                  const std::atomic<bool>* cancel = nullptr);

} // namespace omawave::spectral
