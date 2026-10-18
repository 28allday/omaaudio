#include "speech_align.h"

#include "spectral/fft.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>

namespace omawave::render {
namespace {

constexpr int kBands = 26;
constexpr double kMaxSeconds = 10 * 60;
constexpr std::size_t kMaxCells = 60'000'000;   // DTW band cells (cost float + step byte each)

bool cancelled(const std::atomic<bool>* cancel)
{
    return cancel && cancel->load(std::memory_order_relaxed);
}

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

// Log band energies every `hop` frames on a mel-like scale, each band normalised
// over the whole take so level and microphone differences between takes matter
// less than what is said.
std::vector<std::array<float, kBands>> features(const std::vector<float>& mono, std::uint32_t rate,
                                                std::uint32_t hop, const std::atomic<bool>* cancel)
{
    std::uint32_t size = 256;
    while (size < hop * 5 / 2) size *= 2;
    const spectral::Fft fft(size);
    std::vector<float> window(size);
    for (std::uint32_t i = 0; i < size; ++i) window[i] = float(0.5 - 0.5 * std::cos(2 * M_PI * i / size));
    // Band edges from 80 Hz to 7.5 kHz (or Nyquist), evenly spaced in mel.
    const auto mel = [](double hz) { return 2595 * std::log10(1 + hz / 700); };
    const auto hz = [](double m) { return 700 * (std::pow(10, m / 2595) - 1); };
    const double top = std::min(7500.0, rate * 0.45);
    std::array<std::size_t, kBands + 1> edges{};
    for (int b = 0; b <= kBands; ++b)
        edges[std::size_t(b)] = std::size_t(std::lround(hz(mel(80) + (mel(top) - mel(80)) * b / kBands) * size / rate));
    for (int b = 1; b <= kBands; ++b) edges[std::size_t(b)] = std::max(edges[std::size_t(b)], edges[std::size_t(b - 1)] + 1);

    const std::size_t count = mono.size() / hop + 1;
    std::vector<std::array<float, kBands>> out(count);
    std::vector<std::complex<float>> data(size);
    for (std::size_t k = 0; k < count; ++k) {
        if ((k & 255) == 0 && cancelled(cancel)) return {};
        const auto centre = std::int64_t(k * hop);
        for (std::uint32_t i = 0; i < size; ++i) {
            const auto at = centre - std::int64_t(size / 2) + std::int64_t(i);
            const float v = at >= 0 && at < std::int64_t(mono.size()) ? mono[std::size_t(at)] : 0.0f;
            data[i] = {v * window[i], 0.0f};
        }
        fft.forward(data.data());
        for (int b = 0; b < kBands; ++b) {
            double energy = 0;
            for (auto bin = edges[std::size_t(b)]; bin < edges[std::size_t(b + 1)] && bin <= size / 2; ++bin)
                energy += std::norm(data[bin]);
            out[k][std::size_t(b)] = float(std::log(energy + 1e-9));
        }
    }
    for (int b = 0; b < kBands; ++b) {
        double sum = 0, squares = 0;
        for (const auto& frame : out) { sum += frame[std::size_t(b)]; squares += double(frame[std::size_t(b)]) * frame[std::size_t(b)]; }
        const double mean = sum / count, deviation = std::sqrt(std::max(1e-6, squares / count - mean * mean));
        for (auto& frame : out) frame[std::size_t(b)] = float((frame[std::size_t(b)] - mean) / deviation);
    }
    return out;
}

} // namespace

SpeechAlignment alignSpeech(const session::AudioBuffer& reference, const session::AudioBuffer& unaligned,
                            std::uint32_t rate, double tightness, const std::atomic<bool>* cancel)
{
    SpeechAlignment result;
    if (!rate || reference.empty() || unaligned.empty()) { result.error = "Choose two clips with audio to align."; return result; }
    if (reference.frames() > kMaxSeconds * rate || unaligned.frames() > kMaxSeconds * rate) {
        result.error = "Each clip can be at most 10 minutes long for alignment."; return result;
    }
    if (reference.frames() < rate / 4 || unaligned.frames() < rate / 4) {
        result.error = "Each clip needs at least a quarter of a second of audio to align."; return result;
    }
    tightness = std::clamp(std::isfinite(tightness) ? tightness : 0.7, 0.0, 1.0);
    const std::uint32_t hop = std::max<std::uint32_t>(1, rate / 100);   // 10 ms
    result.hop = hop;
    const auto referenceMono = monoOf(reference), unalignedMono = monoOf(unaligned);
    const auto a = features(referenceMono, rate, hop, cancel);
    const auto b = features(unalignedMono, rate, hop, cancel);
    if (a.empty() || b.empty()) { result.cancelled = true; result.error = "Alignment cancelled."; return result; }

    // Dynamic time warping inside a band around the straight line between the two
    // takes, wide enough for two seconds of drift plus their length difference.
    const std::size_t n = a.size(), m = b.size();
    const std::size_t half = std::min<std::size_t>(std::max(n, m), 200 + (n > m ? n - m : m - n));
    if (n * (2 * half + 1) > kMaxCells) { result.error = "These clips are too long or too different in length to align."; return result; }
    const std::size_t width = 2 * half + 1;
    const auto centreOf = [&](std::size_t i) { return std::int64_t(n > 1 ? i * (m - 1) / (n - 1) : 0); };
    constexpr float inf = std::numeric_limits<float>::infinity();
    std::vector<float> cost(n * width, inf);
    std::vector<std::uint8_t> step(n * width, 0);   // 1 diagonal, 2 from i-1 (hold source), 3 from j-1 (skip source)
    // Off-diagonal steps cost extra; a looser alignment makes them dearer.
    const float penalty = float(0.15 + (1.0 - tightness) * 1.5);
    const auto distance = [&](std::size_t i, std::size_t j) {
        float sum = 0;
        for (int k = 0; k < kBands; ++k) { const float d = a[i][std::size_t(k)] - b[j][std::size_t(k)]; sum += d * d; }
        return std::sqrt(sum / kBands);
    };
    const auto at = [&](std::size_t i, std::int64_t j) -> std::int64_t {
        const auto offset = j - centreOf(i) + std::int64_t(half);
        return offset >= 0 && offset < std::int64_t(width) && j >= 0 && j < std::int64_t(m)
            ? std::int64_t(i * width) + offset : -1;
    };
    for (std::size_t i = 0; i < n; ++i) {
        if ((i & 127) == 0 && cancelled(cancel)) { result.cancelled = true; result.error = "Alignment cancelled."; return result; }
        const auto low = std::max<std::int64_t>(0, centreOf(i) - std::int64_t(half));
        const auto high = std::min<std::int64_t>(std::int64_t(m) - 1, centreOf(i) + std::int64_t(half));
        for (auto j = low; j <= high; ++j) {
            const auto here = at(i, j);
            const float d = distance(i, std::size_t(j));
            if (i == 0 && j == 0) { cost[std::size_t(here)] = d; step[std::size_t(here)] = 1; continue; }
            float best = inf; std::uint8_t from = 0;
            if (i > 0 && j > 0) if (const auto p = at(i - 1, j - 1); p >= 0 && cost[std::size_t(p)] + d < best) { best = cost[std::size_t(p)] + d; from = 1; }
            if (i > 0) if (const auto p = at(i - 1, j); p >= 0 && cost[std::size_t(p)] + d + penalty < best) { best = cost[std::size_t(p)] + d + penalty; from = 2; }
            if (j > 0) if (const auto p = at(i, j - 1); p >= 0 && cost[std::size_t(p)] + d + penalty < best) { best = cost[std::size_t(p)] + d + penalty; from = 3; }
            cost[std::size_t(here)] = best; step[std::size_t(here)] = from;
        }
    }
    if (at(n - 1, std::int64_t(m) - 1) < 0 || !std::isfinite(cost[std::size_t(at(n - 1, std::int64_t(m) - 1))])) {
        result.error = "These clips could not be matched. Check they contain the same words."; return result;
    }
    // Walk back, collecting the source frames each reference frame matched.
    std::vector<double> sum(n, 0.0);
    std::vector<int> count(n, 0);
    for (std::int64_t i = std::int64_t(n) - 1, j = std::int64_t(m) - 1; i >= 0 && j >= 0;) {
        sum[std::size_t(i)] += double(j); ++count[std::size_t(i)];
        const auto s = step[std::size_t(at(std::size_t(i), j))];
        if (s == 1) { --i; --j; } else if (s == 2) --i; else if (s == 3) --j; else break;
    }
    std::vector<double> map(n);
    for (std::size_t i = 0; i < n; ++i) map[i] = count[i] ? sum[i] / count[i] : (i ? map[i - 1] : 0);
    // Smooth: a looser alignment averages over a longer span, keeping the
    // performance's own rhythm inside it.
    const int radius = int(std::lround(2 + (1.0 - tightness) * 18));
    std::vector<double> smooth(n);
    for (std::size_t i = 0; i < n; ++i) {
        double total = 0; int used = 0;
        for (auto k = std::max<std::int64_t>(0, std::int64_t(i) - radius); k <= std::min<std::int64_t>(std::int64_t(n) - 1, std::int64_t(i) + radius); ++k) {
            total += map[std::size_t(k)]; ++used;
        }
        smooth[i] = total / used;
    }
    for (std::size_t i = 1; i < n; ++i) smooth[i] = std::max(smooth[i], smooth[i - 1]);
    result.sourceAt.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto source = session::FramePos(std::llround(smooth[i] * hop));
        result.sourceAt[i] = std::clamp<session::FramePos>(source, 0, session::FramePos(unaligned.frames()) - 1);
        result.largestShiftMs = std::max(result.largestShiftMs,
            std::abs(double(result.sourceAt[i]) - double(i) * hop) * 1000.0 / rate);
    }

    // WSOLA: overlap-add 40 ms Hann grains at a 20 ms hop, each taken from where
    // the map points, nudged up to 8 ms so it continues the previous grain's waveform.
    const std::size_t grain = std::size_t(std::max<std::uint32_t>(64, rate / 25)) & ~std::size_t(1);
    const std::size_t synthesisHop = grain / 2;
    const std::int64_t tolerance = std::int64_t(rate / 125);
    std::vector<float> window(grain);
    for (std::size_t i = 0; i < grain; ++i) window[i] = float(0.5 - 0.5 * std::cos(2 * M_PI * double(i) / double(grain)));
    const std::size_t outFrames = reference.frames();
    session::AudioBuffer out(unaligned.channels(), outFrames);
    const auto sourceFor = [&](std::size_t outputFrame) {
        const double position = double(outputFrame) / hop;
        const auto index = std::min<std::size_t>(std::size_t(position), n - 1);
        const auto next = std::min(index + 1, n - 1);
        const double t = std::clamp(position - double(index), 0.0, 1.0);
        return std::int64_t(std::llround(double(result.sourceAt[index]) * (1 - t) + double(result.sourceAt[next]) * t));
    };
    const auto sample = [&](std::int64_t index) {
        return index >= 0 && index < std::int64_t(unalignedMono.size()) ? unalignedMono[std::size_t(index)] : 0.0f;
    };
    std::int64_t previous = -1;
    for (std::size_t start = 0; start < outFrames + synthesisHop; start += synthesisHop) {
        if ((start / synthesisHop & 63) == 0 && cancelled(cancel)) { result.cancelled = true; result.error = "Alignment cancelled."; return result; }
        // The grain is centred on its output position.
        const auto outputCentre = std::int64_t(start) - std::int64_t(synthesisHop);
        std::int64_t source = sourceFor(std::size_t(std::max<std::int64_t>(0, outputCentre + std::int64_t(synthesisHop)))) - std::int64_t(synthesisHop);
        if (previous >= 0) {
            // The waveform that would naturally follow the previous grain.
            const auto natural = previous + std::int64_t(synthesisHop);
            const auto score = [&](std::int64_t candidate, int stride) {
                double total = 0;
                for (std::size_t i = 0; i < grain; i += std::size_t(stride)) total += double(sample(candidate + std::int64_t(i))) * sample(natural + std::int64_t(i));
                return total;
            };
            std::int64_t best = source; double bestScore = -std::numeric_limits<double>::infinity();
            for (std::int64_t d = -tolerance; d <= tolerance; d += 4)
                if (const double s = score(source + d, 4); s > bestScore) { bestScore = s; best = source + d; }
            const auto coarse = best;
            bestScore = -std::numeric_limits<double>::infinity();
            for (std::int64_t d = -3; d <= 3; ++d)
                if (const double s = score(coarse + d, 1); s > bestScore) { bestScore = s; best = coarse + d; }
            source = best;
        }
        previous = source;
        for (std::uint32_t c = 0; c < unaligned.channels(); ++c) {
            const auto input = unaligned.plane(c);
            auto output = out.plane(c);
            for (std::size_t i = 0; i < grain; ++i) {
                const auto o = outputCentre + std::int64_t(i);
                const auto s = source + std::int64_t(i);
                if (o < 0 || o >= std::int64_t(outFrames) || s < 0 || s >= std::int64_t(input.size())) continue;
                output[std::size_t(o)] += input[std::size_t(s)] * window[i];
            }
        }
    }
    result.audio = std::move(out);
    result.ok = true;
    return result;
}

} // namespace omawave::render
