#include "remix.h"

#include "spectral/fft.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include <numeric>

namespace omawave::render {
namespace {

constexpr int kChroma = 12, kTimbre = 12, kDims = kChroma + kTimbre + 1;
using Feature = std::array<float, kDims>;

bool stop(const std::atomic<bool>* cancel) { return cancel && cancel->load(std::memory_order_relaxed); }

std::vector<float> monoOf(const session::AudioBuffer& audio)
{
    std::vector<float> mono(audio.frames(), 0.0f);
    const float scale = 1.0f / float(std::max<std::uint32_t>(1, audio.channels()));
    for (std::uint32_t c = 0; c < audio.channels(); ++c) {
        const auto plane = audio.plane(c);
        for (std::size_t f = 0; f < mono.size(); ++f) mono[f] += plane[f] * scale;
    }
    return mono;
}

// Harmony (pitch-class profile), tone colour (log bands) and level per hop.
std::vector<Feature> features(const std::vector<float>& mono, std::uint32_t rate, std::size_t hop,
                              const std::atomic<bool>* cancel)
{
    std::uint32_t size = 1024;
    while (size < 2 * hop && size < 16384) size *= 2;
    const spectral::Fft fft(size);
    std::vector<float> window(size);
    for (std::uint32_t i = 0; i < size; ++i) window[i] = float(0.5 - 0.5 * std::cos(2 * M_PI * i / size));
    std::vector<int> pitchClass(size / 2 + 1, -1), band(size / 2 + 1, -1);
    for (std::size_t bin = 1; bin <= size / 2; ++bin) {
        const double hz = double(bin) * rate / size;
        if (hz >= 55 && hz <= 5000)
            pitchClass[bin] = int(std::lround(12 * std::log2(hz / 440.0)) % 12 + 12) % 12;
        if (hz >= 60 && hz <= std::min(14000.0, rate * 0.45))
            band[bin] = std::min(kTimbre - 1, int(kTimbre * std::log(hz / 60) / std::log(std::min(14000.0, rate * 0.45) / 60)));
    }
    const std::size_t count = mono.size() / hop + 1;
    std::vector<Feature> out(count);
    std::vector<std::complex<float>> data(size);
    for (std::size_t k = 0; k < count; ++k) {
        if ((k & 127) == 0 && stop(cancel)) return {};
        const auto centre = std::int64_t(k * hop);
        double energy = 0;
        for (std::uint32_t i = 0; i < size; ++i) {
            const auto at = centre - std::int64_t(size / 2) + std::int64_t(i);
            const float v = at >= 0 && at < std::int64_t(mono.size()) ? mono[std::size_t(at)] : 0.0f;
            energy += double(v) * v;
            data[i] = {v * window[i], 0.0f};
        }
        fft.forward(data.data());
        std::array<double, kChroma> chroma{};
        std::array<double, kTimbre> timbre{};
        for (std::size_t bin = 1; bin <= size / 2; ++bin) {
            const double power = std::norm(data[bin]);
            if (pitchClass[bin] >= 0) chroma[std::size_t(pitchClass[bin])] += power;
            if (band[bin] >= 0) timbre[std::size_t(band[bin])] += power;
        }
        const double norm = std::sqrt(std::inner_product(chroma.begin(), chroma.end(), chroma.begin(), 0.0)) + 1e-12;
        for (int c = 0; c < kChroma; ++c) out[k][std::size_t(c)] = float(chroma[std::size_t(c)] / norm);
        for (int b = 0; b < kTimbre; ++b) out[k][std::size_t(kChroma + b)] = float(std::log(timbre[std::size_t(b)] + 1e-9));
        out[k][kDims - 1] = float(std::log(energy / size + 1e-10));
    }
    // Tone colour and level are compared relative to the whole piece.
    for (int d = kChroma; d < kDims; ++d) {
        double sum = 0, squares = 0;
        for (const auto& f : out) { sum += f[std::size_t(d)]; squares += double(f[std::size_t(d)]) * f[std::size_t(d)]; }
        const double mean = sum / count, deviation = std::sqrt(std::max(1e-6, squares / count - mean * mean));
        const double weight = d == kDims - 1 ? 0.6 : 0.35;
        for (auto& f : out) f[std::size_t(d)] = float((f[std::size_t(d)] - mean) / deviation * weight);
    }
    return out;
}

float distance(const Feature& a, const Feature& b)
{
    float sum = 0;
    for (int d = 0; d < kDims; ++d) { const float x = a[std::size_t(d)] - b[std::size_t(d)]; sum += x * x; }
    return std::sqrt(sum);
}

// A join: leave the music at hop `exit` and continue from hop `entry`.
struct Jump { std::size_t exit = 0, entry = 0; float score = 0; };

} // namespace

Remix remixToDuration(const session::AudioBuffer& audio, std::uint32_t rate, const RemixSettings& settings,
                      const std::atomic<bool>* cancel)
{
    Remix result;
    const auto length = session::FramePos(audio.frames());
    if (!rate || audio.empty()) { result.error = "Choose a clip with music to remix."; return result; }
    if (length > session::FramePos(rate) * 60 * 20) { result.error = "Remix works on clips up to 20 minutes long."; return result; }
    if (settings.targetFrames < session::FramePos(rate) * 5 || settings.targetFrames > length * 4) {
        result.error = "Choose a duration from 5 seconds up to four times the clip's length."; return result;
    }
    const auto keepStart = session::FramePos(std::max(0.0, settings.keepStartSeconds) * rate);
    const auto keepEnd = session::FramePos(std::max(0.0, settings.keepEndSeconds) * rate);
    const auto fade = session::FramePos(std::clamp(settings.crossfadeSeconds, 0.005, 0.5) * rate);
    if (length < keepStart + keepEnd + session::FramePos(settings.minimumSkipSeconds * rate) + 2 * fade) {
        result.error = "This clip is too short to remix while keeping its opening and ending."; return result;
    }
    const double tolerance = settings.toleranceSeconds > 0 ? settings.toleranceSeconds * rate
        : std::max(0.5 * rate, 0.02 * double(settings.targetFrames));
    const auto difference = length - settings.targetFrames;   // > 0: shorten
    if (std::abs(double(difference)) <= tolerance) {
        result.audio = audio; result.segments = {{0, length}}; result.matchQuality = 1; result.ok = true;
        return result;
    }

    // Hops of 50 ms, coarser for long pieces so the search stays bounded.
    const auto hop = std::size_t(std::max<double>(rate * 0.05, double(length) / 4000.0));
    const auto mono = monoOf(audio);
    const auto f = features(mono, rate, hop, cancel);
    if (f.empty()) { result.cancelled = true; result.error = "Remix cancelled."; return result; }
    const std::size_t n = f.size();
    const auto context = std::max<std::size_t>(2, std::size_t(1.5 * rate / hop));        // compared either side of a join
    const auto minimumGap = std::max<std::size_t>(context, std::size_t(settings.minimumSkipSeconds * rate / hop));
    const auto firstJoin = std::max<std::size_t>(context, std::size_t(keepStart / session::FramePos(hop)) + 1);
    const auto lastJoin = std::size_t(std::max<std::int64_t>(0, std::int64_t(n) - std::int64_t(context)
        - std::int64_t(keepEnd / session::FramePos(hop)) - 1));
    if (lastJoin <= firstJoin + minimumGap) { result.error = "This clip is too short to remix while keeping its opening and ending."; return result; }

    // Every pair of moments at least `minimumGap` apart, scored by how alike the
    // music is around both (mean feature distance over the context window, along
    // the diagonal). Keep the best few per distance.
    std::vector<Jump> candidates;
    std::vector<double> along(n + 1);
    double typical = 0; std::size_t sampled = 0;
    for (std::size_t gap = minimumGap; gap + firstJoin <= lastJoin; ++gap) {
        if ((gap & 31) == 0 && stop(cancel)) { result.cancelled = true; result.error = "Remix cancelled."; return result; }
        along[0] = 0;
        for (std::size_t i = 0; i + gap < n; ++i) {
            const double d = distance(f[i], f[i + gap]);
            along[i + 1] = along[i] + d;
            if ((i % 97) == 0) { typical += d; ++sampled; }
        }
        std::array<Jump, 3> best{};
        for (auto& b : best) b.score = std::numeric_limits<float>::infinity();
        for (std::size_t i = firstJoin; i + gap <= lastJoin; ++i) {
            const auto low = i - context, high = std::min(i + context, n - gap - 1);
            const float score = float((along[high + 1] - along[low]) / double(high + 1 - low));
            // Keep three separated minima per gap: a nearby kept moment is
            // improved in place rather than a second one added beside it.
            bool near = false;
            for (auto& b : best) {
                if (std::isfinite(b.score) && (i > b.exit ? i - b.exit : b.exit - i) < context) {
                    if (score < b.score) b = {i, i + gap, score};
                    near = true;
                    break;
                }
            }
            if (!near) {
                auto worst = std::max_element(best.begin(), best.end(), [](const Jump& a, const Jump& b) { return a.score < b.score; });
                if (score < worst->score) *worst = {i, i + gap, score};
            }
        }
        for (const auto& b : best) if (std::isfinite(b.score)) candidates.push_back(b);
    }
    typical = sampled ? typical / double(sampled) : 1.0;
    std::sort(candidates.begin(), candidates.end(), [](const Jump& a, const Jump& b) { return a.score < b.score; });
    if (candidates.size() > 400) candidates.resize(400);

    // Choose up to four joins whose lengths add up to the change, preferring the
    // best-matched passages and fewer joins. Shortening skips forward over
    // passages; lengthening repeats passages (play to `entry`, jump back to `exit`).
    const bool shorten = difference > 0;
    const double wanted = std::abs(double(difference)) / double(hop);
    const double slack = tolerance / double(hop);
    struct Choice { std::vector<std::pair<std::size_t, int>> picks; double cost = std::numeric_limits<double>::infinity(); };
    Choice bestChoice, current;
    std::size_t visited = 0;
    const auto overlaps = [&](const Jump& a, const Jump& b) {
        return !(a.entry + context <= b.exit || b.entry + context <= a.exit);
    };
    const auto search = [&](auto&& self, std::size_t from, double total, double cost) -> void {
        if (++visited > 300000) return;
        if (std::abs(total - wanted) <= slack) {
            const double withJoins = cost + 0.15 * typical * double(current.picks.size());
            if (withJoins < bestChoice.cost) { bestChoice = current; bestChoice.cost = withJoins; }
            return;
        }
        if (current.picks.size() >= 4 || total > wanted + slack) return;
        for (std::size_t c = from; c < candidates.size(); ++c) {
            const auto& jump = candidates[c];
            if (cost + jump.score >= bestChoice.cost) break;   // sorted by score
            if (std::any_of(current.picks.begin(), current.picks.end(), [&](const auto& p) { return overlaps(candidates[p.first], jump); }))
                continue;
            const double gap = double(jump.entry - jump.exit);
            for (int repeat = 1; repeat <= (shorten ? 1 : 8); ++repeat) {
                if (total + gap * repeat > wanted + slack) break;
                current.picks.emplace_back(c, repeat);
                self(self, c + 1, total + gap * repeat, cost + jump.score * repeat);
                current.picks.pop_back();
            }
        }
    };
    search(search, 0, 0.0, 0.0);
    if (bestChoice.picks.empty()) {
        result.error = "No musical passages could be joined to reach that duration. Try a duration closer to the original, or a longer clip.";
        return result;
    }

    // Segments in source frames, in play order.
    std::vector<Jump> chosen;
    std::vector<int> repeats;
    for (const auto& [index, repeat] : bestChoice.picks) { chosen.push_back(candidates[index]); repeats.push_back(repeat); }
    std::vector<std::size_t> order(chosen.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](auto a, auto b) { return chosen[a].exit < chosen[b].exit; });
    // Each join is moved by up to half a hop so the two waveforms line up.
    const auto align = [&](session::FramePos exit, session::FramePos entry) {
        const auto radius = session::FramePos(hop / 2), span = std::max<session::FramePos>(fade, 256);
        const auto sample = [&](session::FramePos at) { return at >= 0 && at < length ? mono[std::size_t(at)] : 0.0f; };
        double bestScore = -std::numeric_limits<double>::infinity(); session::FramePos best = 0;
        for (session::FramePos d = -radius; d <= radius; d += 2) {
            double score = 0;
            for (session::FramePos i = -span / 2; i < span / 2; i += 2) score += double(sample(exit + i)) * sample(entry + d + i);
            if (score > bestScore) { bestScore = score; best = d; }
        }
        return std::clamp<session::FramePos>(entry + best, fade, length - fade);
    };
    session::FramePos cursor = 0;
    double worst = 0;
    for (const auto k : order) {
        const auto exit = session::FramePos(chosen[k].exit * hop), entry = session::FramePos(chosen[k].entry * hop);
        worst = std::max(worst, double(chosen[k].score));
        if (shorten) {
            result.segments.push_back({cursor, exit});
            cursor = align(exit, entry);
        } else {
            // Play through to `entry`, then go back to `exit` as many times as chosen.
            const auto back = align(entry, exit);
            result.segments.push_back({cursor, entry});
            for (int r = 1; r < repeats[k]; ++r) result.segments.push_back({back, entry});
            cursor = back;
        }
    }
    result.segments.push_back({cursor, length});
    result.seams = result.segments.size() - 1;
    result.matchQuality = std::clamp(1.0 - worst / std::max(1e-6, typical), 0.0, 1.0);

    // Render with equal-power crossfades centred on each join.
    session::FramePos total = 0;
    for (const auto& s : result.segments) total += s.to - s.from;
    session::AudioBuffer out(audio.channels(), std::size_t(total));
    session::FramePos at = 0;
    for (std::size_t s = 0; s < result.segments.size(); ++s) {
        if (stop(cancel)) { result.cancelled = true; result.error = "Remix cancelled."; return result; }
        const auto& seg = result.segments[s];
        const bool fadeIn = s > 0, fadeOut = s + 1 < result.segments.size();
        const auto half = fade / 2;
        const auto from = fadeIn ? seg.from - half : seg.from;
        const auto to = fadeOut ? seg.to + half : seg.to;
        for (std::uint32_t c = 0; c < audio.channels(); ++c) {
            const auto input = audio.plane(c);
            auto output = out.plane(c);
            for (auto src = std::max<session::FramePos>(0, from); src < std::min(to, length); ++src) {
                const auto dst = at + (src - seg.from);
                if (dst < 0 || dst >= total) continue;
                double gain = 1;
                if (fadeIn && src < seg.from + half) gain *= std::sin(M_PI / 2 * double(src - (seg.from - half)) / double(fade));
                if (fadeOut && src >= seg.to - half) gain *= std::cos(M_PI / 2 * double(src - (seg.to - half)) / double(fade));
                output[std::size_t(dst)] += float(input[std::size_t(src)] * gain);
            }
        }
        at += seg.to - seg.from;
    }
    result.audio = std::move(out);
    result.ok = true;
    return result;
}

} // namespace omawave::render
