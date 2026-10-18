#include "render/audio_store.h"
#include "detect.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <span>

namespace omawave::spectral {
namespace {

constexpr double kWindowSeconds = 0.020;     // the mean the ratio is against
constexpr double kMergeSeconds = 0.001;      // flagged samples this close are one event
constexpr double kLongestSeconds = 0.015;    // longer than a pop is not a click
constexpr float kFloor = 1.0e-4f;            // a mean below this is silence: never divide by it

struct Flag {
    std::size_t frame;
    float ratio;
};

// A small Burg model, learned independently on each side of a missing interval.
// All scratch is bounded by 512 context samples and order 32, on the repair worker.
bool predict(std::span<const float> context, bool reverse, std::span<double> prediction)
{
    constexpr std::size_t maxOrder = 32;
    const std::size_t n = context.size();
    if (n < 32) return false;
    std::array<double, 512> forward{}, backward{}, samples{};
    const double mean = std::accumulate(context.begin(), context.end(), 0.0) / static_cast<double>(n);
    double energy = 0, peak = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double value = context[reverse ? n - 1 - i : i] - mean;
        if (!std::isfinite(value)) return false;
        samples[i] = forward[i] = backward[i] = value;
        energy += value * value;
        peak = std::max(peak, std::abs(value));
    }
    if (energy < 1e-16) {
        std::fill(prediction.begin(), prediction.end(), mean);
        return true;
    }
    std::array<double, maxOrder + 1> a{};
    a[0] = 1;
    double residual = 1;
    std::size_t order = 0;
    for (std::size_t m = 1; m <= std::min(maxOrder, n / 4); ++m) {
        double numerator = 0, denominator = 0;
        for (std::size_t j = m; j < n; ++j) {
            numerator += forward[j] * backward[j - 1];
            denominator += forward[j] * forward[j] + backward[j - 1] * backward[j - 1];
        }
        if (denominator < energy * 1e-12) break;
        const double reflection = std::clamp(-2 * numerator / denominator, -0.9999999, 0.9999999);
        const auto previous = a;
        for (std::size_t j = 1; j < m; ++j) a[j] = previous[j] + reflection * previous[m - j];
        a[m] = reflection;
        for (std::size_t j = n; j-- > m;) {
            const double f = forward[j], b = backward[j - 1];
            forward[j] = f + reflection * b;
            backward[j] = b + reflection * f;
        }
        order = m;
        residual *= 1 - reflection * reflection;
        if (residual < 1e-8) break;
    }
    // White noise offers no reliable continuation. Prefer the local interpolation
    // to a fitted noise sequence; likewise refuse unstable/nonfinite predictions.
    if (order == 0 || residual > 0.3) return false;
    std::array<double, maxOrder> history{};
    for (std::size_t i = 0; i < order; ++i) history[i] = samples[n - 1 - i];
    for (double& value : prediction) {
        double next = 0;
        for (std::size_t k = 1; k <= order; ++k) next -= a[k] * history[k - 1];
        if (!std::isfinite(next) || std::abs(next) > 4 * peak
            || std::abs(next + mean) > std::numeric_limits<float>::max()) return false;
        for (std::size_t k = order - 1; k > 0; --k) history[k] = history[k - 1];
        history[0] = next;
        value = next + mean;
    }
    return true;
}

// One channel's flagged samples, in order.
std::vector<Flag> flagChannel(std::span<const float> x, std::size_t start, std::size_t end,
                              std::size_t window, float threshold, const std::atomic<bool>* cancel)
{
    std::vector<Flag> out;
    const std::size_t n = x.size();
    if (n < 3 || end <= start || (cancel && cancel->load(std::memory_order_relaxed)))
        return out;

    // |second difference| of a sample, zero at the edges. Computed where it is
    // needed rather than kept for the whole take: two arrays the length of the
    // audio were twelve bytes a frame, gigabytes for a long recording whose
    // audio itself stays on disk.
    const auto difference = [&](std::size_t i) -> double {
        return i == 0 || i + 1 >= n ? 0.0 : static_cast<double>(std::fabs(x[i - 1] - 2.0f * x[i] + x[i + 1]));
    };
    const std::size_t half = std::max<std::size_t>(1, window / 2);
    const auto windowSum = [&](std::size_t lo, std::size_t hi) {
        double sum = 0.0;
        for (std::size_t j = lo; j < hi; ++j) sum += difference(j);
        return sum;
    };

    // The mean over `window` samples centred on i, as a sum that slides with i
    // and is taken afresh every so often so rounding cannot build up.
    std::size_t lo = 0, hi = 0;
    double sum = 0.0;
    std::size_t sinceExact = 0;
    for (std::size_t i = std::max<std::size_t>(start, 1); i < std::min(end, n - 1); ++i) {
        if ((i & 2047U) == 0 && cancel && cancel->load(std::memory_order_relaxed)) return {};
        const std::size_t wantLo = i > half ? i - half : 0;
        const std::size_t wantHi = std::min(n, i + half + 1);
        if (hi == 0 || ++sinceExact >= 65536 || wantLo >= hi) {
            lo = wantLo; hi = wantHi; sum = windowSum(lo, hi); sinceExact = 0;
        } else {
            while (hi < wantHi) sum += difference(hi++);
            while (lo < wantLo) sum -= difference(lo++);
        }
        const auto mean = static_cast<float>(sum / static_cast<double>(hi - lo));
        const float scale = std::max(mean, kFloor);
        const float ratio = static_cast<float>(difference(i)) / scale;
        if (ratio >= threshold)
            out.push_back({i, ratio});
    }
    return out;
}

} // namespace

float clickThreshold(float sensitivity) noexcept
{
    const float s = std::clamp(sensitivity, 0.0f, 1.0f);
    return 30.0f - 22.0f * s;
}

std::vector<Click> detectClicks(const session::AudioBuffer& audio, std::uint32_t sampleRate,
                                Region region, float sensitivity, const std::atomic<bool>* cancel)
{
    std::vector<Click> out;
    if (audio.channels() == 0 || audio.frames() < 3 || sampleRate == 0 || (cancel && cancel->load(std::memory_order_relaxed)))
        return out;
    const std::size_t end = std::min(region.end, audio.frames());
    if (end <= region.start)
        return out;

    const auto window = static_cast<std::size_t>(kWindowSeconds * sampleRate);
    const auto merge = static_cast<std::size_t>(std::max(1.0, kMergeSeconds * sampleRate));
    const auto longest = static_cast<std::size_t>(kLongestSeconds * sampleRate);
    const float threshold = clickThreshold(sensitivity);

    // Every channel's flags, then sorted together: a click on one channel is a
    // click, and one on both is still one.
    std::vector<Flag> flags;
    for (std::uint32_t c = 0; c < audio.channels(); ++c) {
        std::vector<Flag> mine = flagChannel(audio.plane(c), region.start, end, window, threshold, cancel);
        if (cancel && cancel->load(std::memory_order_relaxed)) return {};
        flags.insert(flags.end(), mine.begin(), mine.end());
    }
    std::sort(flags.begin(), flags.end(),
              [](const Flag& a, const Flag& b) { return a.frame < b.frame; });

    for (const Flag& f : flags) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return {};
        if (!out.empty()) {
            Click& last = out.back();
            const std::size_t lastEnd = last.frame + last.length;    // one past
            if (f.frame < lastEnd + merge) {
                last.length = std::max(lastEnd, f.frame + 1) - last.frame;
                last.strength = std::max(last.strength, f.ratio);
                continue;
            }
        }
        out.push_back(Click{.frame = f.frame, .length = 1, .strength = f.ratio});
    }

    // A pop is a few milliseconds; anything longer is a burst, a drop-out or the
    // music, and healing it as a click would be wrong.
    std::erase_if(out, [longest](const Click& c) { return c.length > longest; });
    return out;
}

void repairClicks(session::AudioBuffer& audio, const std::vector<Click>& clicks,
                  const std::atomic<bool>* cancel)
{
    const std::size_t n = audio.frames();
    const auto stopped = [&] { return cancel && cancel->load(std::memory_order_relaxed); };
    if (n < 6 || clicks.empty() || stopped()) return;
    struct Gap { std::size_t start, end; };
    std::vector<Gap> gaps;
    for (const Click& click : clicks) {
        if (click.frame >= n || click.length == 0) continue;
        const auto end = click.frame + std::min(click.length, n - click.frame);
        const auto start = std::max<std::size_t>(click.frame > 0 ? click.frame - 1 : 0, 2);
        const auto stop = std::min(end < n ? end + 1 : n, n - 2);
        if (start < stop) gaps.push_back({start, stop});
    }
    std::sort(gaps.begin(), gaps.end(), [](const Gap& a, const Gap& b) { return a.start < b.start; });
    std::vector<Gap> merged;
    for (const auto& gap : gaps) {
        if (!merged.empty() && gap.start <= merged.back().end)
            merged.back().end = std::max(merged.back().end, gap.end);
        else merged.push_back(gap);
    }
    if (merged.empty()) return;
    render::AudioStore output(audio.channels(), n);
    std::array<float, render::AudioStore::blockFrames> block;
    for (std::uint32_t c = 0; c < audio.channels(); ++c) {
        const auto x = std::as_const(audio).plane(c);
        for (std::size_t at = 0; at < n;) {
            if (stopped()) return;
            const auto count = std::min(block.size(), n - at);
            output.write(c, at, x.subspan(at, count));
            at += count;
            if (audio.diskStorage()) audio.diskStorage()->releaseMappedPages();
        }
        for (std::size_t g = 0; g < merged.size(); ++g) {
            if (stopped()) return;
            const auto [g0, g1] = merged[g];
            const auto left = std::max(g0 > 512 ? g0 - 512 : 0, g > 0 ? merged[g - 1].end : 0);
            const auto right = std::min(g1 + std::min<std::size_t>(512, n - g1),
                                        g + 1 < merged.size() ? merged[g + 1].start : n);
            std::array<double, 1024> forward{}, backward{};
            const auto length = g1 - g0;
            const bool predicted = length <= forward.size()
                && predict(x.subspan(left, g0 - left), false, std::span(forward).first(length))
                && predict(x.subspan(g1, right - g1), true, std::span(backward).first(length));
            const double p0 = x[g0 - 2];
            const double p1 = x[g0 - 1];
            const double p2 = x[g1];
            const double p3 = x[g1 + 1];
            // Guard the fallback too: a nonfinite context must not spread NaNs.
            if (!std::isfinite(p1) || !std::isfinite(p2)) continue;
            const bool cubic = std::isfinite(p0) && std::isfinite(p3)
                && (g == 0 || g0 - merged[g - 1].end >= 2)
                && (g + 1 == merged.size() || merged[g + 1].start - g1 >= 2);
            const double limit = std::max({std::abs(p1), std::abs(p2),
                                           cubic ? std::abs(p0) : 0.0, cubic ? std::abs(p3) : 0.0});
            const auto span = static_cast<double>(g1 - (g0 - 1));
            std::size_t blockStart = g0, buffered = 0;
            for (std::size_t i = g0; i < g1; ++i) {
                if (stopped()) return;
                const double t = static_cast<double>(i - (g0 - 1)) / span;
                const double t2 = t * t;
                const double t3 = t2 * t;
                if (predicted) {
                    const double blend = t2 * (3 - 2 * t);
                    block[buffered] = static_cast<float>((1 - blend) * forward[i - g0] + blend * backward[g1 - 1 - i]);
                } else if (cubic) {
                    const double value = 0.5 * ((2.0 * p1) + (-p0 + p2) * t
                               + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2
                               + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
                    block[buffered] = static_cast<float>(std::clamp(value, -limit, limit));
                } else block[buffered] = static_cast<float>((1 - t) * p1 + t * p2);
                if (++buffered == block.size() || i + 1 == g1) {
                    output.write(c, blockStart, std::span<const float>(block).first(buffered));
                    blockStart += buffered; buffered = 0;
                }
            }
            if (audio.diskStorage()) audio.diskStorage()->releaseMappedPages();
        }
    }
    if (!stopped()) output.finishInto(audio, cancel);
}

} // namespace omawave::spectral
