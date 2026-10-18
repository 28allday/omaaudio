#include "harmonics.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <optional>
#include <vector>

namespace omawave::spectral {
namespace {

std::optional<float> nearbyPeak(const std::vector<float>& magnitude,
                                float binHz, float hz, float spanRatio)
{
    const auto bins = static_cast<std::uint32_t>(magnitude.size());
    if (!std::isfinite(hz) || !(hz > 0) || hz >= binHz * static_cast<float>(bins - 1))
        return std::nullopt;
    const float centre = hz / binHz;
    const float span = std::max(1.0f, hz * spanRatio / binHz);
    const auto lo = static_cast<std::uint32_t>(std::max(1.0f, std::floor(centre - span) - 1.0f));
    const auto hi = static_cast<std::uint32_t>(
        std::min(static_cast<float>(bins - 2), std::ceil(centre + span) + 1.0f));
    if (hi <= lo + 1)
        return std::nullopt;
    std::uint32_t best = lo;
    for (std::uint32_t k = lo + 1; k <= hi; ++k) {
        if (magnitude[k] > magnitude[best])
            best = k;
    }
    if (best == lo || best == hi)
        return std::nullopt;
    const float a = magnitude[best - 1], b = magnitude[best], c = magnitude[best + 1];
    if (!(b > a && b > c) || b <= 0)
        return std::nullopt;
    const float loudest = *std::max_element(magnitude.begin(), magnitude.end());
    if (b < loudest * 1e-3f)
        return std::nullopt;
    const float la = std::log(std::max(a, 1e-12f));
    const float lb = std::log(b);
    const float lc = std::log(std::max(c, 1e-12f));
    const float denom = la - 2 * lb + lc;
    const float delta = denom != 0 ? std::clamp(0.5f * (la - lc) / denom, -0.5f, 0.5f) : 0;
    return (static_cast<float>(best) + delta) * binHz;
}

// hop=1 makes analyse's column argument an exact sample centre, including picks
// close to the start. All FFT and magnitude scratch is reused.
class PeakReader {
public:
    PeakReader(const session::AudioBuffer& audio, std::uint32_t rate, StftConfig config, bool gate = false)
        : audio_(audio), stft_(sampleHop(config)), magnitude_(config.bins()),
          column_(config.bins()), binHz_(static_cast<float>(rate) / config.fftSize), gate_(gate) {}

    std::optional<float> at(std::size_t frame, float hz, float span = 0.03f)
    {
        std::fill(magnitude_.begin(), magnitude_.end(), 0.0f);
        for (std::uint32_t ch = 0; ch < audio_.channels(); ++ch) {
            stft_.analyse(audio_.plane(ch), frame, column_.data());
            for (std::size_t k = 0; k < magnitude_.size(); ++k)
                magnitude_[k] += std::abs(column_[k]);
        }
        const float level = *std::max_element(magnitude_.begin(), magnitude_.end());
        reference_ = std::max(reference_, level);
        // A nearly vanished window has unstable lobes: hold through the tail
        // instead of following those lobes away from the last measured pitch.
        if (gate_ && level < reference_ * 0.2f)
            return std::nullopt;
        return nearbyPeak(magnitude_, binHz_, hz, span);
    }
private:
    static StftConfig sampleHop(StftConfig config) { config.hop = 1; return config; }
    const session::AudioBuffer& audio_;
    Stft stft_;
    std::vector<float> magnitude_;
    std::vector<std::complex<float>> column_;
    float binHz_;
    bool gate_;
    float reference_ = 0;
};

} // namespace

float refinePartial(const session::AudioBuffer& audio, std::uint32_t sampleRate,
                    const StftConfig& config, std::size_t centreFrame, float hz, float spanRatio)
{
    if (!config.valid() || audio.empty() || sampleRate == 0 || centreFrame >= audio.frames()
        || !std::isfinite(spanRatio))
        return hz;
    PeakReader reader(audio, sampleRate, config);
    return reader.at(centreFrame, hz, std::clamp(spanRatio, 0.0f, 1.0f)).value_or(hz);
}

std::vector<MaskPoint> followPartial(const session::AudioBuffer& audio,
    std::uint32_t sampleRate, const StftConfig& config, std::size_t from, std::size_t to,
    std::size_t seed, float hz, const std::atomic<bool>* cancel)
{
    const auto cancelled = [cancel] { return cancel && cancel->load(std::memory_order_relaxed); };
    to = std::min(to, audio.frames());
    if (!config.valid() || audio.empty() || sampleRate == 0 || from >= to || to - from < 2
        || !std::isfinite(hz) || hz <= 0 || hz >= static_cast<float>(sampleRate) / 2 || cancelled())
        return {};
    seed = std::clamp(seed, from, to - 1);
    const std::size_t span = to - from - 1;
    const std::size_t segments = std::min<std::size_t>(4095, (span + config.hop - 1) / config.hop);
    std::vector<std::size_t> frames;
    frames.reserve(segments + 2);
    for (std::size_t i = 0; i <= segments; ++i)
        frames.push_back(from + (span / segments) * i + (span % segments) * i / segments);
    auto at = std::lower_bound(frames.begin(), frames.end(), seed);
    if (at == frames.end() || *at != seed)
        at = frames.insert(at, seed);
    const auto anchor = static_cast<std::size_t>(at - frames.begin());
    PeakReader reader(audio, sampleRate, config, true);
    const auto picked = reader.at(seed, hz);
    if (!picked)
        return {};
    std::vector<MaskPoint> points(frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i)
        points[i].seconds = static_cast<float>(static_cast<double>(frames[i]) / sampleRate);
    points[anchor].hz = *picked;
    for (std::size_t i = anchor + 1; i < frames.size(); ++i) {
        if (cancelled()) return {};
        points[i].hz = reader.at(frames[i], points[i - 1].hz).value_or(points[i - 1].hz);
    }
    for (std::size_t i = anchor; i > 0; --i) {
        if (cancelled()) return {};
        points[i - 1].hz = reader.at(frames[i - 1], points[i].hz).value_or(points[i].hz);
    }
    return cancelled() ? std::vector<MaskPoint>{} : points;
}

} // namespace omawave::spectral
