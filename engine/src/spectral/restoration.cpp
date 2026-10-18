#include "restoration.h"
#include "render/audio_store.h"
#include "mask_rows.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace omawave::spectral {
namespace {
bool stopped(const std::atomic<bool>* cancel)
{
    return cancel && cancel->load(std::memory_order_relaxed);
}
void validate(const session::AudioBuffer& audio, std::uint32_t rate, Region region,
              const MaskSource& mask, const StftConfig& config)
{
    if (audio.empty() || !rate || !config.valid() || !mask || region.end <= region.start
        || region.end > audio.frames())
        throw std::invalid_argument("invalid restoration region or analysis settings");
}
void release(const session::AudioBuffer& audio) {
    if (audio.diskStorage()) audio.diskStorage()->releaseMappedPages();
}
void copyInto(const session::AudioBuffer& audio, render::AudioStore& out,
              const std::atomic<bool>* cancel)
{
    for (std::uint32_t ch = 0; ch < audio.channels(); ++ch) {
        const auto src = audio.plane(ch);
        for (std::size_t at = 0; at < src.size();) {
            if (stopped(cancel)) return;
            const auto count = std::min(render::AudioStore::blockFrames, src.size() - at);
            out.write(ch, at, src.subspan(at, count));
            at += count;
            release(audio);
        }
    }
}
enum class MaskKind { Empty, Full, Partial };
MaskKind inspectMask(MaskRows& rows, std::size_t columns, std::uint32_t bins,
                     const std::atomic<bool>* cancel)
{
    if (!rows.valid(true)) throw std::invalid_argument("invalid restoration mask");
    bool empty = true, full = rows.uniform();
    for (std::size_t c = 0; c < columns; ++c) {
        if (stopped(cancel)) return MaskKind::Empty;
        const auto* row = rows.row(c);
        for (std::uint32_t k = 0; k < bins; ++k) {
            const float w = row[k];
            if (!std::isfinite(w) || w < 0 || w > 1) throw std::invalid_argument("invalid restoration mask");
            empty &= w == 0;
            full &= w == 1;
        }
        if (rows.uniform()) break;
    }
    return empty ? MaskKind::Empty : full ? MaskKind::Full : MaskKind::Partial;
}

// Filter only the candidate's difference: an unchanged channel remains bit exact,
// and a full band bypasses resynthesis so unclipped samples stay bit exact too.
bool blendCandidate(session::AudioBuffer& audio, const session::AudioBuffer& candidate,
                    Region region, MaskRows& grid, MaskKind kind, const StftConfig& config,
                    const std::atomic<bool>* cancel)
{
    if (stopped(cancel)) return false;
    render::AudioStore result(audio.channels(), audio.frames());
    copyInto(audio, result, cancel);
    bool changed = false;
    Stft stft(config);
    const auto& input = std::as_const(audio);
    std::array<float, render::AudioStore::blockFrames> block;
    for (std::uint32_t ch = 0; ch < audio.channels(); ++ch) {
        if (stopped(cancel)) return false;
        const auto a = input.plane(ch), b = candidate.plane(ch);
        bool equal = true;
        for (std::size_t at = 0; at < a.size();) {
            if (stopped(cancel)) return false;
            const auto count = std::min(block.size(), a.size() - at);
            equal &= std::equal(a.begin() + at, a.begin() + at + count, b.begin() + at);
            at += count;
            release(input); release(candidate);
            if (!equal) break;
        }
        if (equal) continue;
        if (kind == MaskKind::Full) {
            for (auto at = region.start; at < region.end;) {
                if (stopped(cancel)) return false;
                const auto count = std::min(block.size(), region.end - at);
                for (std::size_t i = 0; i < count; ++i) changed |= a[at + i] != b[at + i];
                result.write(ch, at, b.subspan(at, count));
                at += count;
                release(input); release(candidate);
            }
            continue;
        }
        render::AudioStore difference(1, audio.frames());
        for (auto at = region.start; at < region.end;) {
            if (stopped(cancel)) return false;
            const auto count = std::min(block.size(), region.end - at);
            for (std::size_t i = 0; i < count; ++i) block[i] = b[at + i] - a[at + i];
            difference.write(0, at, std::span<const float>(block).first(count));
            at += count;
            release(input); release(candidate);
        }
        const auto delta = difference.finish();
        const auto src = delta.plane(0);
        const auto fade = std::min<std::size_t>(config.hop, (region.end - region.start) / 2);
        const auto read = [&](std::size_t c, std::complex<float>* col) {
            stft.analyse(src, c, col);
            const auto* w = grid.row(c);
            for (std::uint32_t k = 0; k < config.bins(); ++k) col[k] *= w[k];
            if ((c & 127) == 0) release(delta);
        };
        const auto write = [&](std::size_t at, std::span<const float> samples) {
            const auto first = std::max(at, region.start), end = std::min(at + samples.size(), region.end);
            if (first >= end) return;
            for (auto i = first; i < end; ++i) {
                const float edge = std::min(1.0f, float(std::min(i - region.start + 1, region.end - i)) / float(fade + 1));
                block[i - first] = a[i] + samples[i - at] * edge;
                changed |= block[i - first] != a[i];
            }
            result.write(ch, first, std::span<const float>(block).first(end - first));
            release(input);
        };
        if (!stft.inverseBlocks(columnCount(audio.frames(), config.hop), audio.frames(), read, write, cancel)) return false;
    }
    if (!stopped(cancel) && changed) audio = result.finish();
    return changed && !stopped(cancel);
}

struct Model { std::vector<double> a; double mean = 0; };

// Small regularised least-squares AR model. A training row is admitted only when
// its target AND every predictor sample are below clipping. Increasing orders
// are tried until a model explains the clean samples; noise is refused.
Model fit(std::span<const float> x, float ceiling, bool reverse,
          const std::atomic<bool>* cancel)
{
    auto sample = [&](std::size_t i) { return double(x[reverse ? x.size() - 1 - i : i]); };
    double mean = 0; std::size_t clean = 0;
    for (float v : x) if (std::isfinite(v) && std::abs(v) < ceiling) { mean += v; ++clean; }
    if (clean < 32) return {};
    mean /= double(clean);
    for (const std::size_t p : {2u, 4u, 8u, 12u, 16u, 24u, 32u}) {
        if (stopped(cancel)) return {};
        std::vector<double> matrix(p * (p + 1), 0), row(p);
        double energy = 0; std::size_t rows = 0, consecutive = 0;
        for (std::size_t i = 0; i < x.size(); ++i) {
            const double v = sample(i);
            consecutive = std::isfinite(v) && std::abs(v) < ceiling ? consecutive + 1 : 0;
            if (consecutive <= p) continue;
            const double y = v - mean;
            for (std::size_t j = 0; j < p; ++j) row[j] = sample(i - j - 1) - mean;
            for (std::size_t j = 0; j < p; ++j) {
                for (std::size_t k = 0; k < p; ++k) matrix[j * (p + 1) + k] += row[j] * row[k];
                matrix[j * (p + 1) + p] += row[j] * y;
            }
            energy += y * y; ++rows;
        }
        if (rows < std::max<std::size_t>(24, p * 4) || energy < 1e-10) continue;
        const auto original = matrix;
        for (std::size_t j = 0; j < p; ++j) matrix[j * (p + 1) + j] += energy * 1e-10;
        bool valid = true;
        for (std::size_t j = 0; j < p; ++j) {
            std::size_t pivot = j;
            for (std::size_t k = j + 1; k < p; ++k)
                if (std::abs(matrix[k * (p + 1) + j]) > std::abs(matrix[pivot * (p + 1) + j])) pivot = k;
            if (std::abs(matrix[pivot * (p + 1) + j]) < energy * 1e-13) { valid = false; break; }
            for (std::size_t k = j; k <= p; ++k) std::swap(matrix[j * (p + 1) + k], matrix[pivot * (p + 1) + k]);
            const double divisor = matrix[j * (p + 1) + j];
            for (std::size_t k = j; k <= p; ++k) matrix[j * (p + 1) + k] /= divisor;
            for (std::size_t r = 0; r < p; ++r) if (r != j) {
                const double scale = matrix[r * (p + 1) + j];
                for (std::size_t k = j; k <= p; ++k) matrix[r * (p + 1) + k] -= scale * matrix[j * (p + 1) + k];
            }
        }
        if (!valid) continue;
        Model model; model.mean = mean; model.a.resize(p);
        double residual = energy;
        for (std::size_t j = 0; j < p; ++j) {
            model.a[j] = matrix[j * (p + 1) + p];
            residual -= 2 * model.a[j] * original[j * (p + 1) + p];
            for (std::size_t k = 0; k < p; ++k)
                residual += model.a[j] * matrix[k * (p + 1) + p] * original[j * (p + 1) + k];
        }
        if (std::all_of(model.a.begin(), model.a.end(), [](double a) { return std::isfinite(a) && std::abs(a) < 64; })
            && residual / energy < 0.0001) return model;
    }
    return {};
}

std::vector<float> predict(const Model& model, std::span<const float> x, std::size_t first,
                           std::size_t end, bool reverse, float bound)
{
    const auto p = model.a.size();
    if (!p || (!reverse && first < p) || (reverse && x.size() - end < p)) return {};
    std::vector<double> history(p);
    for (std::size_t i = 0; i < p; ++i) history[i] = x[reverse ? end + i : first - 1 - i];
    std::vector<float> out(end - first);
    for (std::size_t i = 0; i < out.size(); ++i) {
        double v = model.mean;
        for (std::size_t j = 0; j < p; ++j) v += model.a[j] * (history[j] - model.mean);
        if (!std::isfinite(v) || std::abs(v) > bound) return {};
        out[reverse ? out.size() - 1 - i : i] = float(v);
        for (std::size_t j = p - 1; j > 0; --j) history[j] = history[j - 1];
        history[0] = v;
    }
    return out;
}
} // namespace

session::AudioBuffer repairChannels(const session::AudioBuffer& audio, ChannelScope scope,
    const std::atomic<bool>* cancel)
{
    if (scope == ChannelScope::All) return audio;
    if (audio.channels() != 2 && !(audio.channels() == 1 && scope == ChannelScope::Left))
        throw std::invalid_argument("left/right and mid/side repair scopes require a stereo take");
    if (audio.empty() || stopped(cancel)) return {};
    render::AudioStore out(1, audio.frames());
    std::array<float, render::AudioStore::blockFrames> block;
    const auto left = audio.plane(0), right = audio.plane(audio.channels() == 2 ? 1 : 0);
    for (std::size_t at = 0; at < audio.frames();) {
        if (stopped(cancel)) return {};
        const auto count = std::min(block.size(), audio.frames() - at);
        for (std::size_t i = 0; i < count; ++i) {
            const float l = left[at + i], r = right[at + i];
            block[i] = scope == ChannelScope::Left ? l : scope == ChannelScope::Right ? r
                     : scope == ChannelScope::Mid ? (l + r) * 0.5f : (l - r) * 0.5f;
        }
        out.write(0, at, std::span<const float>(block).first(count));
        at += count;
        if (audio.diskStorage()) audio.diskStorage()->releaseMappedPages();
    }
    return out.finish();
}
void mergeRepairChannels(session::AudioBuffer& audio, const session::AudioBuffer& selected,
    ChannelScope scope, const std::atomic<bool>* cancel)
{
    if (selected.frames() != audio.frames()) throw std::invalid_argument("repair length changed");
    if (scope == ChannelScope::All) {
        if (selected.channels() != audio.channels()) throw std::invalid_argument("repair channels changed");
        if (!stopped(cancel)) audio = selected;
        return;
    }
    if (selected.channels() != 1 || (audio.channels() != 2 && !(audio.channels() == 1 && scope == ChannelScope::Left)))
        throw std::invalid_argument("invalid selected repair channel");
    if (audio.empty() || stopped(cancel)) return;
    const auto& input = std::as_const(audio);
    const auto left = input.plane(0), right = input.plane(audio.channels() == 2 ? 1 : 0), repaired = selected.plane(0);
    render::AudioStore out(audio.channels(), audio.frames());
    std::array<float, render::AudioStore::blockFrames> lblock, rblock;
    for (std::size_t at = 0; at < audio.frames();) {
        if (stopped(cancel)) return;
        const auto count = std::min(lblock.size(), audio.frames() - at);
        for (std::size_t i = 0; i < count; ++i) {
            const float l = left[at + i], r = right[at + i], value = repaired[at + i];
            const float original = scope == ChannelScope::Left ? l : scope == ChannelScope::Right ? r
                : scope == ChannelScope::Mid ? (l + r) * 0.5f : (l - r) * 0.5f;
            const float delta = value - original;
            lblock[i] = l; rblock[i] = r;
            if (delta == 0) continue;
            if (scope == ChannelScope::Left) lblock[i] = value;
            else if (scope == ChannelScope::Right) rblock[i] = value;
            else { lblock[i] += delta; rblock[i] += scope == ChannelScope::Mid ? delta : -delta; }
        }
        out.write(0, at, std::span<const float>(lblock).first(count));
        if (audio.channels() == 2) out.write(1, at, std::span<const float>(rblock).first(count));
        at += count;
        if (input.diskStorage()) input.diskStorage()->releaseMappedPages();
        if (selected.diskStorage()) selected.diskStorage()->releaseMappedPages();
    }
    if (!stopped(cancel)) audio = out.finish();
}

RestorationResult restoreClippedPeaks(session::AudioBuffer& audio, std::uint32_t rate,
    Region region, const MaskSource& mask, float threshold, const StftConfig& config,
    const std::atomic<bool>* cancel)
{
    validate(audio, rate, region, mask, config);
    if (!std::isfinite(threshold) || threshold < 0.01f || threshold > 4)
        throw std::invalid_argument("clipping threshold must be between 0.01 and 4");
    RestorationResult result;
    if (stopped(cancel)) { result.cancelled = true; return result; }
    MaskRows grid(mask, {.columns = columnCount(audio.frames(), config.hop), .hop = config.hop,
        .bins = config.bins(), .sampleRate = rate});
    const auto kind = inspectMask(grid, columnCount(audio.frames(), config.hop), config.bins(), cancel);
    if (stopped(cancel)) { result.cancelled = true; return result; }
    if (kind == MaskKind::Empty) return result;
    render::AudioStore candidate(audio.channels(), audio.frames());
    copyInto(audio, candidate, cancel);
    for (std::uint32_t ch = 0; ch < audio.channels(); ++ch) {
        const auto x = std::as_const(audio).plane(ch);
        std::size_t cachedBlock = std::numeric_limits<std::size_t>::max();
        float cachedLevel = 0; Model forward, backward;
        for (std::size_t i = region.start; i < region.end;) {
            if (stopped(cancel)) { result.cancelled = true; return result; }
            if ((i & 65535) == 0) release(audio);
            const auto first = i++;
            const float level = std::abs(x[first]);
            if (!std::isfinite(level) || level < threshold) continue;
            const float tolerance = 4 * std::numeric_limits<float>::epsilon() * level;
            // Exact equal samples are deliberate: a float-rounded low tone
            // has a very shallow top, but is not a hard clipping plateau.
            while (i < region.end && x[i] == x[first]) {
                if ((i & 65535) == 0) {
                    if (stopped(cancel)) { result.cancelled = true; return result; }
                    release(audio);
                }
                ++i;
            }
            if (i - first < 3) continue;
            // A selection bisecting a plateau cannot supply its missing boundary.
            if (!first || i == x.size() || i - first > 1024
                || std::abs(x[first - 1]) >= level || std::abs(x[i]) >= level) { ++result.skipped; continue; }
            const auto block = first / 2048;
            if (block != cachedBlock || level != cachedLevel) {
                const auto begin = block * 2048 > 2048 ? block * 2048 - 2048 : 0;
                const auto end = std::min(x.size(), block * 2048 + 4096);
                const auto context = x.subspan(begin, end - begin);
                forward = fit(context, level - tolerance, false, cancel);
                backward = fit(context, level - tolerance, true, cancel);
                cachedBlock = block; cachedLevel = level;
            }
            auto f = predict(forward, x, first, i, false, level * 4);
            auto b = predict(backward, x, first, i, true, level * 4);
            if (f.empty() || b.empty()) { ++result.skipped; continue; }
            bool valid = true, raised = false;
            for (std::size_t j = 0; j < f.size(); ++j) {
                if (f[j] * x[first] <= 0 || b[j] * x[first] <= 0 || std::abs(f[j] - b[j]) > level * 0.5f) { valid = false; break; }
                const float t = float(j + 1) / float(f.size() + 1), w = t * t * (3 - 2 * t);
                f[j] = f[j] * (1 - w) + b[j] * w;
                raised |= std::abs(f[j]) > level + tolerance;
            }
            if (!valid || !raised) { ++result.skipped; continue; }
            for (std::size_t j = 0; j < f.size(); ++j)
                if (std::abs(f[j]) <= std::abs(x[first + j])) f[j] = x[first + j];
            candidate.write(ch, first, f);
            ++result.restored;
        }
    }
    result.changed = blendCandidate(audio, candidate.finish(), region, grid, kind, config, cancel);
    result.cancelled = stopped(cancel);
    return result;
}

RestorationResult reduceReverb(session::AudioBuffer& audio, std::uint32_t rate, Region region,
    const MaskSource& mask, float amountDb, float decaySeconds, float strength,
    const StftConfig& config, const std::atomic<bool>* cancel)
{
    validate(audio, rate, region, mask, config);
    if (!std::isfinite(amountDb) || amountDb < 0 || amountDb > 40
        || !std::isfinite(decaySeconds) || decaySeconds < 0.05f || decaySeconds > 3
        || !std::isfinite(strength) || strength < 0 || strength > 2)
        throw std::invalid_argument("invalid late-reverb reduction settings");
    RestorationResult result;
    if (stopped(cancel)) { result.cancelled = true; return result; }
    if (!amountDb || !strength) return result;
    MaskRows grid(mask, {.columns = columnCount(audio.frames(), config.hop), .hop = config.hop,
        .bins = config.bins(), .sampleRate = rate});
    const auto kind = inspectMask(grid, columnCount(audio.frames(), config.hop), config.bins(), cancel);
    if (stopped(cancel)) { result.cancelled = true; return result; }
    if (kind == MaskKind::Empty) return result;
    render::AudioStore candidate(audio.channels(), audio.frames());
    copyInto(audio, candidate, cancel);
    Stft stft(config);
    const float decay = std::exp(-13.8155106f * float(config.hop) / (float(rate) * decaySeconds));
    const float floor = std::pow(10.0f, -amountDb / 20);
    const auto& input = std::as_const(audio);
    std::array<float, render::AudioStore::blockFrames> block;
    for (std::uint32_t ch = 0; ch < audio.channels(); ++ch) {
        if (stopped(cancel)) { result.cancelled = true; return result; }
        const auto src = input.plane(ch);
        std::vector<float> envelope(config.bins(), 0), previous(config.bins(), 0), gains(config.bins(), 1);
        bool changed = false;
        const auto read = [&](std::size_t c, std::complex<float>* column) {
            stft.analyse(src, c, column);
            for (std::uint32_t k = 0; k < config.bins(); ++k) {
                const float power = std::norm(column[k]);
                const float tail = envelope[k] * decay;
                const bool falling = power < previous[k] * 0.99f && power > 1e-16f;
                const float evidence = falling ? std::clamp(tail / std::max(power, 1e-16f), 0.0f, 1.0f) : 0;
                const float target = std::max(floor, 1 - strength * evidence);
                gains[k] = falling ? 0.5f * gains[k] + 0.5f * target : 1;
                envelope[k] = std::max(power, tail);
                previous[k] = power;
                if (c * config.hop >= region.start && c * config.hop < region.end) {
                    changed |= gains[k] < 0.999999f;
                    column[k] *= gains[k];
                }
            }
            if ((c & 127) == 0) release(input);
        };
        const auto fade = std::min<std::size_t>(config.hop, (region.end - region.start) / 2);
        const auto write = [&](std::size_t at, std::span<const float> samples) {
            const auto first = std::max(at, region.start), end = std::min(at + samples.size(), region.end);
            if (first >= end) return;
            for (auto i = first; i < end; ++i) {
                const float edge = std::min(1.0f, float(std::min(i - region.start + 1, region.end - i)) / float(fade + 1));
                block[i - first] = src[i] + (samples[i - at] - src[i]) * edge;
            }
            candidate.write(ch, first, std::span<const float>(block).first(end - first));
        };
        if (!stft.inverseBlocks(columnCount(audio.frames(), config.hop), audio.frames(), read, write, cancel)) {
            result.cancelled = true; return result;
        }
        // The original algorithm bypasses resynthesis when no bin changed.
        if (!changed) for (auto at = region.start; at < region.end;) {
            if (stopped(cancel)) { result.cancelled = true; return result; }
            const auto count = std::min(block.size(), region.end - at);
            candidate.write(ch, at, src.subspan(at, count));
            at += count;
            release(input);
        }
        release(input);
    }
    result.changed = blendCandidate(audio, candidate.finish(), region, grid, kind, config, cancel);
    result.cancelled = stopped(cancel);
    return result;
}
} // namespace omawave::spectral
