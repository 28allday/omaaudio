#include "repair.h"
#include "heal_runs.h"
#include "mask_rows.h"
#include "render/audio_store.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace omawave::spectral {
namespace {
bool stopped(const std::atomic<bool>* cancel) {
    return cancel && cancel->load(std::memory_order_relaxed);
}
void release(const session::AudioBuffer& audio) {
    if (audio.diskStorage()) audio.diskStorage()->releaseMappedPages();
}

bool collectRuns(HealRuns& runs, Stft& stft, std::span<const float> slice, MaskRows& mask,
    std::size_t first, std::size_t last, std::uint32_t context,
    const session::AudioBuffer& audio, const std::atomic<bool>* cancel)
{
    struct State {
        HealRuns::Run run;
        bool covered = false, pending = false;
        std::size_t uncovered = 0, leftCount = 0, rightCount = 0;
    };
    const auto columns = columnCount(slice.size(), stft.config().hop);
    const auto bins = stft.bins();
    // Asking for more context than exists is equivalent to asking for the whole
    // signal. The usual repair uses four columns.
    const auto historySize = std::min<std::size_t>(context, columns);
    std::vector<float> history(historySize * bins);
    std::vector<State> states(bins);
    std::vector<std::complex<float>> column(bins);
    const auto finish = [&](std::uint32_t k) {
        auto& s = states[k];
        if (s.leftCount || s.rightCount) {
            auto r = s.run;
            if (s.leftCount) r.left /= float(s.leftCount);
            if (s.rightCount) r.right /= float(s.rightCount);
            if (!s.leftCount) r.left = r.right;
            if (!s.rightCount) r.right = r.left;
            runs.add(k, r);
        }
        s.pending = false;
    };
    for (std::size_t c = 0; c <= columns; ++c) {
        if (stopped(cancel)) return false;
        if (c < columns) stft.analyse(slice, c, column.data());
        const float* weights = c >= first && c <= last ? mask.row(c - first) : nullptr;
        if (stopped(cancel)) return false;
        for (std::uint32_t k = 0; k < bins; ++k) {
            auto& s = states[k];
            const bool covered = weights && weights[k] > 0;
            if (covered && !s.covered) {
                if (s.pending) finish(k);
                s.run = {.start = c};
                s.leftCount = std::min<std::size_t>(context, s.uncovered);
                s.rightCount = 0;
                // Nearest to furthest, as in the original bin-by-bin algorithm.
                for (std::size_t j = 1; j <= s.leftCount; ++j)
                    s.run.left += history[((c - j) % historySize) * bins + k];
            } else if (!covered && s.covered) {
                s.run.end = c;
                s.pending = true;
            }
            if (!covered && s.pending) {
                if (c < columns && s.rightCount < context) {
                    s.run.right += std::abs(column[k]);
                    ++s.rightCount;
                }
                if (s.rightCount == context || c == columns) finish(k);
            }
            s.covered = covered;
            s.uncovered = covered ? 0 : std::min(historySize, s.uncovered + 1);
            if (historySize && c < columns) history[(c % historySize) * bins + k] = std::abs(column[k]);
        }
        if ((c & 127) == 0) release(audio);
    }
    runs.rewind();
    return !stopped(cancel);
}

enum class Operation { Attenuate, Heal, Denoise };
void process(session::AudioBuffer& audio, std::uint32_t rate, Region region,
    const MaskSource& mask, const StftConfig& config, Operation operation,
    float amount, float strength, const NoisePrint* print, std::uint32_t context,
    const std::atomic<bool>* cancel)
{
    if (stopped(cancel) || !mask || !rate || audio.empty() || !config.valid()
        || region.end <= region.start || region.start >= audio.frames()) return;
    region.end = std::min(region.end, audio.frames());
    const auto from = region.start > config.fftSize ? region.start - config.fftSize : 0;
    const auto to = region.end + std::min<std::size_t>(config.fftSize, audio.frames() - region.end);
    const auto start = region.start - from, end = region.end - from;
    const auto columns = columnCount(to - from, config.hop);
    const auto last = (end - 1) / config.hop;
    // A selection shorter than a hop can fall between two analysis columns.
    // The column before it still covers it, and the edge fades keep the change
    // inside the selection: the repair happens, rather than quietly not.
    const auto first = std::min(start / config.hop + (start % config.hop != 0), last);
    const auto bins = config.bins();
    MaskRows weights(mask, {.firstFrame = first * config.hop, .columns = last - first + 1,
        .hop = config.hop, .bins = bins, .sampleRate = rate, .originSeconds = double(from) / rate});
    if (stopped(cancel) || !weights.valid()) return;
    render::AudioStore output(audio.channels(), audio.frames());
    const auto& input = std::as_const(audio);
    Stft stft(config);
    const float gain = std::pow(10.0f, amount / 20.0f);
    const float floor = std::pow(10.0f, -std::max(0.0f, amount) / 20.0f);
    const float over = std::max(0.0f, strength);
    const auto fade = std::min<std::size_t>(config.hop, (end - start) / 2);
    std::array<float, render::AudioStore::blockFrames> block;
    for (std::uint32_t ch = 0; ch < audio.channels(); ++ch) {
        if (stopped(cancel)) return;
        const auto plane = input.plane(ch);
        const auto slice = plane.subspan(from, to - from);
        // Copy untouched context and audio outside the padded selection in blocks.
        const auto copy = [&](std::size_t a, std::size_t b) {
            for (auto at = a; at < b;) {
                if (stopped(cancel)) return;
                const auto count = std::min(block.size(), b - at);
                output.write(ch, at, plane.subspan(at, count));
                at += count;
                if ((at & 65535) < block.size()) release(input);
            }
        };
        copy(0, region.start);
        copy(region.end, audio.frames());
        if (stopped(cancel)) return;
        HealRuns runs(bins, last - first + 1);
        if (operation == Operation::Heal
            && !collectRuns(runs, stft, slice, weights, first, last, context, input, cancel)) return;
        std::vector<float> noise(bins), gains(3 * bins, 1);
        std::vector<std::complex<float>> lookahead(bins);
        if (print && !print->magnitude.empty() && print->bins > 0) {
            const auto& printed = print->magnitude[std::min<std::size_t>(ch, print->magnitude.size() - 1)];
            if (print->bins == bins) noise = printed;
            else for (std::uint32_t k = 0; k < bins; ++k) {
                const float at = float(k) * float(print->bins - 1) / float(std::max(1u, bins - 1));
                const auto lo = std::uint32_t(at), hi = std::min(lo + 1, print->bins - 1);
                const float t = at - float(lo);
                noise[k] = printed[lo] * (1 - t) + printed[hi] * t;
            }
            // A bin's noise magnitude grows with the energy of the window that
            // measured it. A print from another frame length or window is the same
            // noise on another scale; bring it to this analysis's.
            if (print->fftSize != config.fftSize || print->window != config.window) {
                const auto energy = [](Window window, std::uint32_t size) {
                    double sum = 0;
                    for (const float w : makeWindow(window, size)) sum += double(w) * w;
                    return sum;
                };
                const double theirs = print->fftSize ? energy(print->window, print->fftSize) : 0;
                if (theirs > 0) {
                    const auto scale = float(std::sqrt(energy(config.window, config.fftSize) / theirs));
                    for (float& value : noise) value *= scale;
                }
            }
        }
        const auto prepareGain = [&](std::size_t c) {
            stft.analyse(slice, c, lookahead.data());
            for (std::uint32_t k = 0; k < bins; ++k) {
                const float mag = std::abs(lookahead[k]);
                gains[(c % 3) * bins + k] = mag > 1e-12f ? std::clamp((mag - over * noise[k]) / mag, floor, 1.0f) : 1;
            }
        };
        const auto read = [&](std::size_t c, std::complex<float>* col) {
            stft.analyse(slice, c, col);
            if (c >= first && c <= last) {
                const auto* w = weights.row(c - first);
                if (operation == Operation::Denoise) {
                    if (c == first) prepareGain(c);
                    if (c < last) prepareGain(c + 1);
                }
                for (std::uint32_t k = 0; k < bins; ++k) {
                    if (operation == Operation::Attenuate) col[k] *= 1.0f + (gain - 1.0f) * w[k];
                    else if (operation == Operation::Heal) {
                        const auto& run = runs.at(k, c);
                        if (c < run.start || c >= run.end) continue;
                        const float t = float(c - run.start + 1) / float(run.end - run.start + 1);
                        const float target = run.left * (1.0f - t) + run.right * t;
                        const float mag = std::abs(col[k]);
                        const float wanted = mag * (1.0f - w[k]) + target * w[k];
                        if (mag > 1e-12f) col[k] *= wanted / mag;
                        else col[k] = {wanted, 0};
                    } else if (w[k] > 0) {
                        float g = gains[(c % 3) * bins + k];
                        int n = 1;
                        if (c > first) { g += gains[((c - 1) % 3) * bins + k]; ++n; }
                        if (c < last) { g += gains[((c + 1) % 3) * bins + k]; ++n; }
                        g /= float(n);
                        col[k] *= 1.0f + (g - 1.0f) * w[k];
                    }
                }
            }
            if ((c & 127) == 0) release(input);
        };
        const auto write = [&](std::size_t at, std::span<const float> samples) {
            const auto a = std::max(at, start), b = std::min(at + samples.size(), end);
            if (a >= b) return;
            for (auto i = a; i < b; ++i) {
                float t = 1;
                if (fade) {
                    if (i - start < fade) t = float(i - start + 1) / float(fade + 1);
                    else if (end - i <= fade) t = float(end - i) / float(fade + 1);
                }
                block[i - a] = plane[from + i] * (1.0f - t) + samples[i - at] * t;
            }
            output.write(ch, from + a, std::span<const float>(block).first(b - a));
        };
        if (!stft.inverseBlocks(columns, slice.size(), read, write, cancel)) return;
        release(input);
    }
    if (!stopped(cancel)) output.finishInto(audio, cancel);
}
} // namespace

void attenuate(session::AudioBuffer& audio, std::uint32_t rate, Region region,
    const MaskSource& mask, float gainDb, const StftConfig& config, const std::atomic<bool>* cancel)
{
    process(audio, rate, region, mask, config, Operation::Attenuate, gainDb, 0, nullptr, 0, cancel);
}
void attenuateBand(session::AudioBuffer& audio, std::uint32_t rate, Region region,
    Band band, float gainDb, const StftConfig& config, const std::atomic<bool>* cancel)
{
    attenuate(audio, rate, region, bandMask(band), gainDb, config, cancel);
}
void heal(session::AudioBuffer& audio, std::uint32_t rate, Region region,
    const MaskSource& mask, const StftConfig& config, std::uint32_t context, const std::atomic<bool>* cancel)
{
    process(audio, rate, region, mask, config, Operation::Heal, 0, 0, nullptr, context, cancel);
}
void healBand(session::AudioBuffer& audio, std::uint32_t rate, Region region,
    Band band, const StftConfig& config, std::uint32_t context, const std::atomic<bool>* cancel)
{
    heal(audio, rate, region, bandMask(band), config, context, cancel);
}
NoisePrint captureNoisePrint(const session::AudioBuffer& audio, Region region,
                             const StftConfig& config, const std::atomic<bool>* cancel)
{
    NoisePrint print;
    if (audio.empty() || !config.valid() || region.end <= region.start
        || region.start >= audio.frames())
        return print;
    region.end = std::min(region.end, audio.frames());
    print.fftSize = config.fftSize;
    print.bins = config.bins();
    print.window = config.window;
    Stft stft(config);
    std::vector<std::complex<float>> column(print.bins);

    for (std::uint32_t ch = 0; ch < audio.channels(); ++ch) {
        const std::span<const float> plane = audio.plane(ch);
        const std::span<const float> slice(plane.data() + region.start, region.end - region.start);
        const std::size_t columns = columnCount(slice.size(), config.hop);
        std::vector<float> mean(print.bins, 0.0f);
        for (std::size_t c = 0; c < columns; ++c) {
            if (cancel && cancel->load(std::memory_order_relaxed)) return {};
            stft.analyse(slice, c, column.data());
            if ((c & 127) == 0 && audio.diskStorage()) audio.diskStorage()->releaseMappedPages();
            for (std::uint32_t k = 0; k < print.bins; ++k)
                mean[k] += std::abs(column[k]);
        }
        if (columns > 0) {
            for (float& v : mean)
                v /= static_cast<float>(columns);
        }
        print.columns = columns;
        print.magnitude.push_back(std::move(mean));
    }
    return print;
}

void reduceNoise(session::AudioBuffer& audio, std::uint32_t rate, Region region,
    const MaskSource& mask, const NoisePrint& print, float amount, float strength,
    const StftConfig& config, const std::atomic<bool>* cancel)
{
    if (print.empty()) return;
    process(audio, rate, region, mask, config, Operation::Denoise, amount, strength, &print, 0, cancel);
}
void reduceNoise(session::AudioBuffer& audio, std::uint32_t rate, Region region,
    Band band, const NoisePrint& print, float amount, float strength,
    const StftConfig& config, const std::atomic<bool>* cancel)
{
    reduceNoise(audio, rate, region, bandMask(band), print, amount, strength, config, cancel);
}
} // namespace omawave::spectral
