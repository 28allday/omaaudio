#pragma once

#include "session/audio_buffer.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace omawave::engine {

// "Nothing measured yet" -- silence, or a meter that has not completed a block. The
// UI draws it as -inf; it is a finite float so it survives a QVariant and a TOML file.
inline constexpr float kLoudnessSilence = -200.0f;

// How many channels the meter weighs. The mix is stereo (kOutputChannels); anything
// past the second plane is not measured. A single plane is measured as one channel,
// which is what BS.1770 says a mono programme is.
inline constexpr std::uint32_t kLoudnessChannels = 2;

// What a whole-programme measurement comes to. Every value is in LUFS (or LU for
// the range, dBTP for the peak); kLoudnessSilence where nothing was above the gate.
struct LoudnessReport {
    float integrated = kLoudnessSilence;    // gated, BS.1770-4
    float range = 0.0f;                     // LU, EBU Tech 3342
    float truePeak = kLoudnessSilence;      // dBTP, 4x oversampled, max hold
    float momentaryMax = kLoudnessSilence;  // loudest 400 ms block
    float shortTermMax = kLoudnessSilence;  // loudest 3 s block
    std::uint64_t frames = 0;               // how much audio went in
};

// One point of the loudness history an offline analysis keeps: the momentary and
// short-term readings as they stood at the end of a 100 ms hop.
struct LoudnessPoint {
    std::int64_t frame = 0;
    float momentary = kLoudnessSilence;
    float shortTerm = kLoudnessSilence;
};

// An EBU R128 / ITU-R BS.1770-4 loudness meter: momentary (400 ms), short-term
// (3 s), gated integrated loudness, loudness range and a 4x-oversampled true-peak
// reading, all from the same stream of samples.
//
// It obeys the audio thread's rules (BRIEF.md section 2): every buffer is a fixed
// array sized at compile time, process() allocates nothing, locks nothing and calls
// nothing that does. The readings are published through atomics, so the GUI thread
// can read them while the audio thread writes -- each on its own is consistent; a
// momentary read next to an integrated one may be a hop apart, which is fine for a
// meter and would cost a lock to fix.
//
// The gate needs the loudness of every block the programme has produced, and a
// growing list would allocate. Blocks go into a fixed histogram instead (0.1 LU bins
// from -70 to +10 LUFS, carrying the exact power sum per bin), which is how every
// real-time R128 meter does it: the gate threshold lands on a bin edge, and the
// integrated mean is exact within the bin.
class LoudnessMeter {
public:
    explicit LoudnessMeter(std::uint32_t sampleRate = 48000) noexcept;

    // Main thread, while nothing is being processed. Recomputes the K-weighting for
    // the rate and resets everything.
    void configure(std::uint32_t sampleRate) noexcept;

    // Clears every reading. Safe from the audio thread, which is where a "reset"
    // command lands, because it touches nothing that allocates.
    void reset() noexcept;

    // Audio thread. `planes` are non-interleaved; channels beyond kLoudnessChannels
    // are ignored, a null plane is silence.
    void process(const float* const* planes, std::uint32_t channels,
                 std::uint32_t frames) noexcept;

    // The readings, in LUFS / LU / dBTP, kLoudnessSilence when there is nothing.
    [[nodiscard]] float momentary() const noexcept { return momentary_.load(std::memory_order_relaxed); }
    [[nodiscard]] float shortTerm() const noexcept { return shortTerm_.load(std::memory_order_relaxed); }
    [[nodiscard]] float integrated() const noexcept { return integrated_.load(std::memory_order_relaxed); }
    [[nodiscard]] float range() const noexcept { return range_.load(std::memory_order_relaxed); }
    [[nodiscard]] float truePeak() const noexcept { return truePeak_.load(std::memory_order_relaxed); }
    [[nodiscard]] float momentaryMax() const noexcept { return momentaryMax_.load(std::memory_order_relaxed); }
    [[nodiscard]] float shortTermMax() const noexcept { return shortTermMax_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t frames() const noexcept { return frames_.load(std::memory_order_relaxed); }

    [[nodiscard]] LoudnessReport report() const noexcept;

    // 100 ms at the configured rate: the momentary hop, and the granularity of an
    // offline history.
    [[nodiscard]] std::uint32_t hopFrames() const noexcept { return hopFrames_; }
    [[nodiscard]] std::uint32_t sampleRate() const noexcept { return sampleRate_; }
    // How many hops have completed since the last reset.
    [[nodiscard]] std::uint64_t hops() const noexcept { return hops_.load(std::memory_order_relaxed); }

    // The polyphase true-peak filter: 4 phases of this many taps.
    static constexpr std::uint32_t kTruePeakPhases = 4;
    static constexpr std::uint32_t kTruePeakTaps = 12;

private:
    // One biquad per channel per stage, direct form I.
    struct Biquad {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    };
    struct BiquadState {
        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
    };

    static constexpr std::uint32_t kMomentaryBlocks = 4;    // 400 ms of 100 ms hops
    static constexpr std::uint32_t kShortTermBlocks = 30;   // 3 s
    static constexpr float kHistogramFloor = -70.0f;
    static constexpr float kHistogramCeiling = 10.0f;
    static constexpr float kHistogramStep = 0.1f;
    static constexpr std::size_t kHistogramBins = 801;

    void completeHop() noexcept;
    void publishGated() noexcept;
    [[nodiscard]] static float loudnessOfPower(double power) noexcept;
    [[nodiscard]] static double powerOfLoudness(float lufs) noexcept;
    [[nodiscard]] static std::size_t binOf(float lufs) noexcept;

    std::uint32_t sampleRate_ = 48000;
    std::uint32_t hopFrames_ = 4800;

    Biquad shelf_;
    Biquad highPass_;
    std::array<BiquadState, kLoudnessChannels> shelfState_{};
    std::array<BiquadState, kLoudnessChannels> highPassState_{};

    // The 100 ms sub-block being accumulated: sum of squares per channel.
    std::array<double, kLoudnessChannels> hopSum_{};
    std::uint32_t hopFilled_ = 0;

    // The last 30 completed sub-blocks, per channel, as a ring.
    std::array<std::array<double, kShortTermBlocks>, kLoudnessChannels> ring_{};
    std::uint32_t ringHead_ = 0;
    std::uint32_t ringFilled_ = 0;

    // Integrated: every 400 ms block above the absolute gate, binned.
    std::array<double, kHistogramBins> blockPower_{};
    std::array<std::uint32_t, kHistogramBins> blockCount_{};
    // Range: every 3 s block above the absolute gate, at 1 s hops, binned.
    std::array<double, kHistogramBins> shortPower_{};
    std::array<std::uint32_t, kHistogramBins> shortCount_{};
    std::uint32_t hopsSinceShort_ = 0;

    // True peak: polyphase upsampler state per channel.
    std::array<std::array<float, kTruePeakTaps * kTruePeakPhases>, kTruePeakPhases> phase_{};
    std::array<std::array<float, kTruePeakTaps>, kLoudnessChannels> peakHistory_{};
    std::array<std::uint32_t, kLoudnessChannels> peakHead_{};

    std::atomic<float> momentary_{kLoudnessSilence};
    std::atomic<float> shortTerm_{kLoudnessSilence};
    std::atomic<float> integrated_{kLoudnessSilence};
    std::atomic<float> range_{0.0f};
    std::atomic<float> truePeak_{kLoudnessSilence};
    std::atomic<float> momentaryMax_{kLoudnessSilence};
    std::atomic<float> shortTermMax_{kLoudnessSilence};
    std::atomic<std::uint64_t> frames_{0};
    std::atomic<std::uint64_t> hops_{0};
    float truePeakLinear_ = 0.0f;
};

// Offline: measures a whole buffer, and optionally keeps the momentary and
// short-term readings at every hop for a loudness history. Allocates; not for the
// audio thread.
[[nodiscard]] LoudnessReport analyseLoudness(const session::AudioBuffer& audio,
                                             std::uint32_t sampleRate,
                                             std::vector<LoudnessPoint>* history = nullptr);

// dB helpers shared by the exporter and the tests.
[[nodiscard]] float linearToDb(float linear) noexcept;
[[nodiscard]] float dbToLinear(float db) noexcept;

} // namespace omawave::engine
