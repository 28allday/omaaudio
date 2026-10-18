#include "loudness.h"

#include <algorithm>
#include <cmath>

namespace omawave::engine {

namespace {

constexpr double kPi = 3.14159265358979323846;

// BS.1770-4's constant: what makes a 1 kHz sine at -23 dBFS in both channels read
// exactly -23.0 LUFS through the K-weighting.
constexpr double kLoudnessOffset = -0.691;

// Gates, in LU below the ungated mean (BS.1770-4 for the integrated, EBU Tech 3342
// for the range) and the absolute floor both share.
constexpr float kAbsoluteGate = -70.0f;
constexpr float kRelativeGateIntegrated = -10.0f;
constexpr float kRelativeGateRange = -20.0f;

} // namespace

float linearToDb(float linear) noexcept
{
    if (linear <= 0.0f)
        return kLoudnessSilence;
    return 20.0f * std::log10(linear);
}

float dbToLinear(float db) noexcept
{
    return std::pow(10.0f, db / 20.0f);
}

LoudnessMeter::LoudnessMeter(std::uint32_t sampleRate) noexcept
{
    configure(sampleRate);
}

void LoudnessMeter::configure(std::uint32_t sampleRate) noexcept
{
    sampleRate_ = sampleRate == 0 ? 48000 : sampleRate;
    hopFrames_ = std::max<std::uint32_t>(1, sampleRate_ / 10);

    // K-weighting, designed from the analogue prototype BS.1770 was fitted to, so
    // the filter is right at any rate rather than only at the 48 kHz the standard
    // tabulates. Stage one is the high shelf (+4 dB above ~1.5 kHz, the head's
    // acoustic effect); stage two is the high-pass at 38 Hz.
    {
        const double f0 = 1681.974450955533;
        const double gainDb = 3.999843853973347;
        const double q = 0.7071752369554196;
        const double k = std::tan(kPi * f0 / static_cast<double>(sampleRate_));
        const double vh = std::pow(10.0, gainDb / 20.0);
        const double vb = std::pow(vh, 0.4996667741545416);
        const double a0 = 1.0 + k / q + k * k;
        shelf_.b0 = (vh + vb * k / q + k * k) / a0;
        shelf_.b1 = 2.0 * (k * k - vh) / a0;
        shelf_.b2 = (vh - vb * k / q + k * k) / a0;
        shelf_.a1 = 2.0 * (k * k - 1.0) / a0;
        shelf_.a2 = (1.0 - k / q + k * k) / a0;
    }
    {
        const double f0 = 38.13547087602444;
        const double q = 0.5003270373238773;
        const double k = std::tan(kPi * f0 / static_cast<double>(sampleRate_));
        const double a0 = 1.0 + k / q + k * k;
        highPass_.b0 = 1.0;
        highPass_.b1 = -2.0;
        highPass_.b2 = 1.0;
        highPass_.a1 = 2.0 * (k * k - 1.0) / a0;
        highPass_.a2 = (1.0 - k / q + k * k) / a0;
    }

    // The true-peak interpolator: a 48-tap windowed sinc at four times the rate,
    // split into its four phases. Each phase is normalised to unit DC gain so a
    // constant reads as itself; the sinc's cutoff is the original Nyquist.
    {
        constexpr std::uint32_t taps = kTruePeakTaps * kTruePeakPhases;
        const double centre = (taps - 1) / 2.0;
        std::array<double, taps> h{};
        for (std::uint32_t n = 0; n < taps; ++n) {
            const double x = (static_cast<double>(n) - centre) / kTruePeakPhases;
            const double sinc = x == 0.0 ? 1.0 : std::sin(kPi * x) / (kPi * x);
            const double w = 0.42 - 0.5 * std::cos(2.0 * kPi * n / (taps - 1))
                           + 0.08 * std::cos(4.0 * kPi * n / (taps - 1));
            h[n] = sinc * w;
        }
        for (std::uint32_t p = 0; p < kTruePeakPhases; ++p) {
            double sum = 0.0;
            for (std::uint32_t k = 0; k < kTruePeakTaps; ++k)
                sum += h[k * kTruePeakPhases + p];
            for (std::uint32_t k = 0; k < kTruePeakTaps; ++k)
                phase_[p][k] = static_cast<float>(sum != 0.0 ? h[k * kTruePeakPhases + p] / sum
                                                             : 0.0);
        }
    }

    reset();
}

void LoudnessMeter::reset() noexcept
{
    for (auto& s : shelfState_)
        s = BiquadState{};
    for (auto& s : highPassState_)
        s = BiquadState{};
    hopSum_.fill(0.0);
    hopFilled_ = 0;
    for (auto& channel : ring_)
        channel.fill(0.0);
    ringHead_ = 0;
    ringFilled_ = 0;
    blockPower_.fill(0.0);
    blockCount_.fill(0);
    shortPower_.fill(0.0);
    shortCount_.fill(0);
    hopsSinceShort_ = 0;
    for (auto& h : peakHistory_)
        h.fill(0.0f);
    peakHead_.fill(0);
    truePeakLinear_ = 0.0f;

    momentary_.store(kLoudnessSilence, std::memory_order_relaxed);
    shortTerm_.store(kLoudnessSilence, std::memory_order_relaxed);
    integrated_.store(kLoudnessSilence, std::memory_order_relaxed);
    range_.store(0.0f, std::memory_order_relaxed);
    truePeak_.store(kLoudnessSilence, std::memory_order_relaxed);
    momentaryMax_.store(kLoudnessSilence, std::memory_order_relaxed);
    shortTermMax_.store(kLoudnessSilence, std::memory_order_relaxed);
    frames_.store(0, std::memory_order_relaxed);
    hops_.store(0, std::memory_order_relaxed);
}

float LoudnessMeter::loudnessOfPower(double power) noexcept
{
    if (power <= 0.0)
        return kLoudnessSilence;
    return static_cast<float>(kLoudnessOffset + 10.0 * std::log10(power));
}

double LoudnessMeter::powerOfLoudness(float lufs) noexcept
{
    return std::pow(10.0, (static_cast<double>(lufs) - kLoudnessOffset) / 10.0);
}

std::size_t LoudnessMeter::binOf(float lufs) noexcept
{
    const float clamped = std::clamp(lufs, kHistogramFloor, kHistogramCeiling);
    const auto index = static_cast<long>(std::lround((clamped - kHistogramFloor) / kHistogramStep));
    return static_cast<std::size_t>(std::clamp<long>(index, 0, static_cast<long>(kHistogramBins) - 1));
}

void LoudnessMeter::process(const float* const* planes, std::uint32_t channels,
                            std::uint32_t frames) noexcept
{
    const std::uint32_t measured = std::min(channels, kLoudnessChannels);
    if (measured == 0 || frames == 0)
        return;

    // Once its finite history is zero, the true-peak FIR produces exactly zero
    // for an all-zero input block. Keep its cursor advancing, but avoid four
    // convolutions per silent sample (especially expensive in offline checks).
    std::array<bool, kLoudnessChannels> silentPeak{};
    for (std::uint32_t c = 0; c < measured; ++c)
        silentPeak[c] = std::all_of(peakHistory_[c].begin(), peakHistory_[c].end(), [](float x) { return x == 0; })
            && (planes[c] == nullptr || std::all_of(planes[c], planes[c] + frames, [](float x) { return x == 0; }));

    for (std::uint32_t f = 0; f < frames; ++f) {
        for (std::uint32_t c = 0; c < measured; ++c) {
            const float x = planes[c] != nullptr ? planes[c][f] : 0.0f;

            // True peak: four interpolated values between this sample and the last.
            auto& history = peakHistory_[c];
            std::uint32_t& head = peakHead_[c];
            history[head] = x;
            if (!silentPeak[c]) for (std::uint32_t p = 0; p < kTruePeakPhases; ++p) {
                const auto& taps = phase_[p];
                float y = 0.0f;
                std::uint32_t index = head;
                for (std::uint32_t k = 0; k < kTruePeakTaps; ++k) {
                    y += taps[k] * history[index];
                    index = index == 0 ? kTruePeakTaps - 1 : index - 1;
                }
                const float magnitude = std::fabs(y);
                if (magnitude > truePeakLinear_)
                    truePeakLinear_ = magnitude;
            }
            head = head + 1 == kTruePeakTaps ? 0 : head + 1;

            // K-weighting: the shelf, then the high-pass.
            BiquadState& s1 = shelfState_[c];
            const double xd = static_cast<double>(x);
            const double y1 = shelf_.b0 * xd + shelf_.b1 * s1.x1 + shelf_.b2 * s1.x2
                            - shelf_.a1 * s1.y1 - shelf_.a2 * s1.y2;
            s1.x2 = s1.x1;
            s1.x1 = xd;
            s1.y2 = s1.y1;
            s1.y1 = y1;

            BiquadState& s2 = highPassState_[c];
            const double y2 = highPass_.b0 * y1 + highPass_.b1 * s2.x1 + highPass_.b2 * s2.x2
                            - highPass_.a1 * s2.y1 - highPass_.a2 * s2.y2;
            s2.x2 = s2.x1;
            s2.x1 = y1;
            s2.y2 = s2.y1;
            s2.y1 = y2;

            hopSum_[c] += y2 * y2;
        }

        if (++hopFilled_ >= hopFrames_)
            completeHop();
    }

    frames_.fetch_add(frames, std::memory_order_relaxed);
    truePeak_.store(linearToDb(truePeakLinear_), std::memory_order_relaxed);
}

void LoudnessMeter::completeHop() noexcept
{
    for (std::uint32_t c = 0; c < kLoudnessChannels; ++c) {
        ring_[c][ringHead_] = hopSum_[c];
        hopSum_[c] = 0.0;
    }
    hopFilled_ = 0;
    ringHead_ = ringHead_ + 1 == kShortTermBlocks ? 0 : ringHead_ + 1;
    ringFilled_ = std::min(ringFilled_ + 1, kShortTermBlocks);

    // Mean power over the last N hops, every channel at weight 1 (left, right and
    // centre all are; the surround weights would only matter past two channels).
    const auto windowPower = [this](std::uint32_t blocks) noexcept {
        double sum = 0.0;
        for (std::uint32_t c = 0; c < kLoudnessChannels; ++c) {
            std::uint32_t index = ringHead_;
            for (std::uint32_t b = 0; b < blocks; ++b) {
                index = index == 0 ? kShortTermBlocks - 1 : index - 1;
                sum += ring_[c][index];
            }
        }
        return sum / (static_cast<double>(blocks) * static_cast<double>(hopFrames_));
    };

    if (ringFilled_ >= kMomentaryBlocks) {
        const double power = windowPower(kMomentaryBlocks);
        const float lufs = loudnessOfPower(power);
        momentary_.store(lufs, std::memory_order_relaxed);
        if (lufs > momentaryMax_.load(std::memory_order_relaxed))
            momentaryMax_.store(lufs, std::memory_order_relaxed);
        if (lufs > kAbsoluteGate) {
            const std::size_t bin = binOf(lufs);
            blockPower_[bin] += power;
            blockCount_[bin] += 1;
        }
    }

    if (ringFilled_ >= kShortTermBlocks) {
        const double power = windowPower(kShortTermBlocks);
        const float lufs = loudnessOfPower(power);
        shortTerm_.store(lufs, std::memory_order_relaxed);
        if (lufs > shortTermMax_.load(std::memory_order_relaxed))
            shortTermMax_.store(lufs, std::memory_order_relaxed);
        // The range is measured on 3 s blocks at 1 s hops (Tech 3342), so only
        // every tenth short-term reading goes into its histogram.
        if (++hopsSinceShort_ >= 10) {
            hopsSinceShort_ = 0;
            if (lufs > kAbsoluteGate) {
                const std::size_t bin = binOf(lufs);
                shortPower_[bin] += power;
                shortCount_[bin] += 1;
            }
        }
    }

    publishGated();
    hops_.fetch_add(1, std::memory_order_relaxed);
}

void LoudnessMeter::publishGated() noexcept
{
    // Integrated: the mean of every block above the absolute gate sets a relative
    // gate 10 LU below it; the answer is the mean of the blocks above that.
    {
        double sum = 0.0;
        std::uint64_t count = 0;
        for (std::size_t i = 0; i < kHistogramBins; ++i) {
            sum += blockPower_[i];
            count += blockCount_[i];
        }
        if (count == 0) {
            integrated_.store(kLoudnessSilence, std::memory_order_relaxed);
        } else {
            const float threshold = loudnessOfPower(sum / static_cast<double>(count))
                                  + kRelativeGateIntegrated;
            double gatedSum = 0.0;
            std::uint64_t gatedCount = 0;
            for (std::size_t i = 0; i < kHistogramBins; ++i) {
                const float centre = kHistogramFloor + static_cast<float>(i) * kHistogramStep;
                if (centre > threshold) {
                    gatedSum += blockPower_[i];
                    gatedCount += blockCount_[i];
                }
            }
            integrated_.store(gatedCount == 0 ? kLoudnessSilence
                                              : loudnessOfPower(gatedSum / static_cast<double>(gatedCount)),
                              std::memory_order_relaxed);
        }
    }

    // Range: the same shape with a 20 LU gate, then the spread between the 10th
    // and 95th percentiles of what survives.
    {
        double sum = 0.0;
        std::uint64_t count = 0;
        for (std::size_t i = 0; i < kHistogramBins; ++i) {
            sum += shortPower_[i];
            count += shortCount_[i];
        }
        float range = 0.0f;
        if (count >= 2) {
            const float threshold = loudnessOfPower(sum / static_cast<double>(count))
                                  + kRelativeGateRange;
            std::uint64_t gatedCount = 0;
            std::size_t first = kHistogramBins;
            for (std::size_t i = 0; i < kHistogramBins; ++i) {
                const float centre = kHistogramFloor + static_cast<float>(i) * kHistogramStep;
                if (centre > threshold && shortCount_[i] > 0) {
                    gatedCount += shortCount_[i];
                    if (first == kHistogramBins)
                        first = i;
                }
            }
            if (gatedCount >= 2) {
                const auto low = static_cast<std::uint64_t>(std::ceil(0.10 * static_cast<double>(gatedCount)));
                const auto high = static_cast<std::uint64_t>(std::ceil(0.95 * static_cast<double>(gatedCount)));
                std::uint64_t seen = 0;
                float lowLufs = kHistogramFloor;
                float highLufs = kHistogramFloor;
                bool lowFound = false;
                for (std::size_t i = first; i < kHistogramBins; ++i) {
                    const float centre = kHistogramFloor + static_cast<float>(i) * kHistogramStep;
                    if (!(centre > threshold) || shortCount_[i] == 0)
                        continue;
                    seen += shortCount_[i];
                    if (!lowFound && seen >= low) {
                        lowLufs = centre;
                        lowFound = true;
                    }
                    if (seen >= high) {
                        highLufs = centre;
                        break;
                    }
                }
                range = std::max(0.0f, highLufs - lowLufs);
            }
        }
        range_.store(range, std::memory_order_relaxed);
    }
}

LoudnessReport LoudnessMeter::report() const noexcept
{
    LoudnessReport r;
    r.integrated = integrated();
    r.range = range();
    r.truePeak = truePeak();
    r.momentaryMax = momentaryMax();
    r.shortTermMax = shortTermMax();
    r.frames = frames();
    return r;
}

LoudnessReport analyseLoudness(const session::AudioBuffer& audio, std::uint32_t sampleRate,
                               std::vector<LoudnessPoint>* history)
{
    LoudnessMeter meter(sampleRate);
    if (audio.empty())
        return meter.report();

    const std::uint32_t channels = std::min(audio.channels(), kLoudnessChannels);
    std::array<const float*, kLoudnessChannels> planes{};
    const std::uint32_t hop = meter.hopFrames();
    const std::size_t total = audio.frames();

    if (history != nullptr) {
        history->clear();
        history->reserve(total / hop + 1);
    }

    std::size_t done = 0;
    while (done < total) {
        const std::size_t want = std::min<std::size_t>(hop, total - done);
        for (std::uint32_t c = 0; c < channels; ++c)
            planes[c] = audio.plane(c).data() + done;
        meter.process(planes.data(), channels, static_cast<std::uint32_t>(want));
        done += want;
        if (audio.diskStorage() && ((done & 65535U) < want || done == total))
            audio.diskStorage()->releaseMappedPages();
        if (history != nullptr && want == hop) {
            history->push_back(LoudnessPoint{
                .frame = static_cast<std::int64_t>(done),
                .momentary = meter.momentary(),
                .shortTerm = meter.shortTerm(),
            });
        }
    }
    return meter.report();
}

} // namespace omawave::engine
