#include "mask.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace omawave::spectral {
namespace {

// Half the width of the smaller falloff, so a hard 0/1 edge becomes a raised
// cosine outward from the shape over `feather` cells: the shape itself is never
// eaten into, which is what a person who drew round a click expects.
std::vector<float> falloff(std::uint32_t feather)
{
    std::vector<float> f(feather + 1, 1.0f);
    for (std::uint32_t d = 1; d <= feather; ++d) {
        const float t = static_cast<float>(d) / static_cast<float>(feather + 1);
        f[d] = 0.5f + 0.5f * std::cos(t * std::numbers::pi_v<float>);
    }
    return f;
}

// One pass of soft dilation along an axis: out[i] = max over d of in[i ± d] * f[d].
void dilate(const float* in, float* out, std::size_t n, std::ptrdiff_t stride,
            const std::vector<float>& f)
{
    const auto feather = static_cast<std::ptrdiff_t>(f.size() - 1);
    for (std::ptrdiff_t i = 0; i < static_cast<std::ptrdiff_t>(n); ++i) {
        float best = in[i * stride];
        for (std::ptrdiff_t d = 1; d <= feather && best < 1.0f; ++d) {
            if (i - d >= 0)
                best = std::max(best, in[(i - d) * stride] * f[static_cast<std::size_t>(d)]);
            if (i + d < static_cast<std::ptrdiff_t>(n))
                best = std::max(best, in[(i + d) * stride] * f[static_cast<std::size_t>(d)]);
        }
        out[i * stride] = best;
    }
}

// The bins [lo, hi] of a Hz interval, or nothing if it misses the grid.
bool binRange(float hzLow, float hzHigh, float binHz, std::uint32_t bins, std::uint32_t& lo,
              std::uint32_t& hi)
{
    if (hzHigh < hzLow)
        std::swap(hzLow, hzHigh);
    const float top = binHz * static_cast<float>(bins - 1);
    if (hzHigh < 0.0f || hzLow > top)
        return false;
    const float a = std::clamp(hzLow, 0.0f, top);
    const float b = std::clamp(hzHigh, 0.0f, top);
    lo = static_cast<std::uint32_t>(std::lround(a / binHz));
    hi = static_cast<std::uint32_t>(std::lround(b / binHz));
    lo = std::min(lo, bins - 1);
    hi = std::min(hi, bins - 1);
    return hi >= lo;
}

void fill(float* row, std::uint32_t lo, std::uint32_t hi, float value)
{
    std::fill(row + lo, row + hi + 1, value);
}

// A lasso against the vertical line at `t`: the Hz where its edges cross, in
// pairs (even-odd), which is what a scanline fill wants. Edges are straight in
// Hz; a lasso drawn with the mouse has points every few pixels, so the
// difference from the screen's line on a logarithmic display is under a bin.
void polygonCrossings(const std::vector<MaskPoint>& poly, float t, std::vector<float>& out)
{
    out.clear();
    const std::size_t n = poly.size();
    for (std::size_t i = 0; i < n; ++i) {
        const MaskPoint& p = poly[i];
        const MaskPoint& q = poly[(i + 1) % n];
        if (p.seconds == q.seconds)
            continue;
        const float lo = std::min(p.seconds, q.seconds);
        const float hi = std::max(p.seconds, q.seconds);
        if (t < lo || t >= hi)
            continue;
        const float u = (t - p.seconds) / (q.seconds - p.seconds);
        out.push_back(p.hz + (q.hz - p.hz) * u);
    }
    std::sort(out.begin(), out.end());
}

} // namespace

bool HarmonicComb::validPitch() const noexcept
{
    if (pitch.empty()) return true;
    if (pitch.size() < 2 || pitch.size() > 4097 || !std::isfinite(fromSeconds)
        || !std::isfinite(toSeconds) || toSeconds <= fromSeconds) return false;
    float previous = -std::numeric_limits<float>::infinity();
    for (const MaskPoint& p : pitch) {
        if (!std::isfinite(p.seconds) || !std::isfinite(p.hz) || p.hz <= 0
            || p.seconds <= previous || p.seconds < fromSeconds || p.seconds > toSeconds)
            return false;
        previous = p.seconds;
    }
    return true;
}

float HarmonicComb::fundamentalAt(float seconds) const noexcept
{
    if (pitch.empty()) return fundamentalHz;
    const auto next = std::lower_bound(pitch.begin(), pitch.end(), seconds,
        [](const MaskPoint& p, float t) { return p.seconds < t; });
    if (next == pitch.begin()) return pitch.front().hz;
    if (next == pitch.end()) return pitch.back().hz;
    const MaskPoint& prev = *(next - 1);
    const float fraction = (seconds - prev.seconds) / (next->seconds - prev.seconds);
    return prev.hz + (next->hz - prev.hz) * fraction;
}

std::vector<std::pair<float, float>> HarmonicComb::bands(float nyquist) const
{
    return bandsAt(nyquist, fromSeconds);
}

std::vector<std::pair<float, float>> HarmonicComb::bandsAt(float nyquist, float seconds) const
{
    std::vector<std::pair<float, float>> out;
    if (empty())
        return out;
    // Cents to a ratio once: 2^(c/1200) - 1 of the partial either side.
    const float ratio = widthInHz ? 0.0f : std::exp2(width / 1200.0f) - 1.0f;
    const float fundamental = fundamentalAt(seconds);
    for (std::uint32_t n = includeFundamental ? 1 : 2; n <= partials; ++n) {
        const float f = fundamental * static_cast<float>(n);
        const float half = widthInHz ? width : f * ratio;
        if (f - half > nyquist)
            break;
        out.emplace_back(std::max(0.0f, f - half), f + half);
    }
    return out;
}

bool MaskShapes::empty() const noexcept
{
    return std::none_of(shapes.begin(), shapes.end(), [](const MaskShape& s) {
        return s.op == MaskOp::Add && !s.empty();
    });
}

std::optional<std::pair<float, float>> MaskShapes::timeBounds() const
{
    std::optional<std::pair<float, float>> out;
    const auto grow = [&out](float a, float b) {
        if (!out) {
            out = {a, b};
        } else {
            out->first = std::min(out->first, a);
            out->second = std::max(out->second, b);
        }
    };
    for (const MaskShape& s : shapes) {
        if (s.op != MaskOp::Add || s.empty())
            continue;
        for (const MaskPoint& p : s.polygon)
            grow(p.seconds, p.seconds);
        for (const MaskStamp& st : s.stamps)
            grow(st.seconds - st.rSeconds, st.seconds + st.rSeconds);
        if (s.comb.has_value())
            grow(s.comb->fromSeconds, s.comb->toSeconds);
    }
    return out;
}

std::optional<std::pair<float, float>> MaskShapes::hzBounds() const
{
    std::optional<std::pair<float, float>> out;
    const auto grow = [&out](float a, float b) {
        if (!out) {
            out = {a, b};
        } else {
            out->first = std::min(out->first, a);
            out->second = std::max(out->second, b);
        }
    };
    for (const MaskShape& s : shapes) {
        if (s.op != MaskOp::Add || s.empty())
            continue;
        for (const MaskPoint& p : s.polygon)
            grow(p.hz, p.hz);
        for (const MaskStamp& st : s.stamps)
            grow(st.hzLow, st.hzHigh);
        if (s.comb.has_value()) {
            // Unbounded above: the caller clamps to its own Nyquist.
            for (const auto& [lo, hi] : s.comb->bands(std::numeric_limits<float>::infinity()))
                grow(lo, hi);
            if (!s.comb->pitch.empty()) {
                const auto [low, high] = std::minmax_element(s.comb->pitch.begin(), s.comb->pitch.end(),
                    [](const MaskPoint& a, const MaskPoint& b) { return a.hz < b.hz; });
                for (const auto* p : {&*low, &*high}) {
                    for (const auto& [lo, hi] : s.comb->bandsAt(std::numeric_limits<float>::infinity(), p->seconds))
                        grow(lo, hi);
                }
            }
        }
    }
    return out;
}

MaskShapes MaskShapes::shifted(double seconds) const
{
    MaskShapes out = *this;
    for (MaskShape& s : out.shapes) {
        for (MaskPoint& p : s.polygon)
            p.seconds = static_cast<float>(p.seconds + seconds);
        for (MaskStamp& st : s.stamps)
            st.seconds = static_cast<float>(st.seconds + seconds);
        if (s.comb.has_value()) {
            s.comb->fromSeconds = static_cast<float>(s.comb->fromSeconds + seconds);
            s.comb->toSeconds = static_cast<float>(s.comb->toSeconds + seconds);
            for (MaskPoint& p : s.comb->pitch)
                p.seconds = static_cast<float>(p.seconds + seconds);
        }
    }
    return out;
}

std::vector<float> bandWeights(Band band, std::uint32_t bins, std::uint32_t sampleRate)
{
    const float nyquist = static_cast<float>(sampleRate) / 2.0f;
    const float high = band.highHz <= 0.0f ? nyquist : std::min(band.highHz, nyquist);
    const float low = std::clamp(band.lowHz, 0.0f, high);
    const float binHz = nyquist / static_cast<float>(bins - 1);
    const float lowBin = low / binHz;
    const float highBin = high / binHz;
    const auto feather = static_cast<float>(kFeatherBins);

    std::vector<float> w(bins, 0.0f);
    for (std::uint32_t k = 0; k < bins; ++k) {
        const auto b = static_cast<float>(k);
        float v = 1.0f;
        if (b < lowBin - feather || b > highBin + feather) {
            v = 0.0f;
        } else if (b < lowBin) {
            const float t = (lowBin - b) / feather;               // 0 at the edge, 1 outside
            v = 0.5f + 0.5f * std::cos(t * std::numbers::pi_v<float>);
        } else if (b > highBin) {
            const float t = (b - highBin) / feather;
            v = 0.5f + 0.5f * std::cos(t * std::numbers::pi_v<float>);
        }
        // The whole spectrum: DC and Nyquist included, no feather below 0 or above
        // the top.
        if (low <= 0.0f && k == 0)
            v = 1.0f;
        if (high >= nyquist && k == bins - 1)
            v = 1.0f;
        w[k] = v;
    }
    return w;
}

namespace {
struct BandSource {
    Band band;
    MaskGrid operator()(const MaskFrame& frame) const {
        return {frame.columns, frame.bins, true, bandWeights(band, frame.bins, frame.sampleRate)};
    }
};
struct ShapeSource {
    MaskShapes shapes;
    MaskGrid operator()(const MaskFrame& frame) const { return rasterise(shapes, frame); }
};
}
MaskSource bandMask(Band band) { return BandSource{band}; }
MaskSource shapeMask(MaskShapes shapes) { return ShapeSource{std::move(shapes)}; }
std::optional<std::uint32_t> maskColumnHalo(const MaskSource& mask)
{
    if (mask.target<BandSource>()) return 0;
    if (const auto* source = mask.target<ShapeSource>()) return source->shapes.featherColumns;
    return std::nullopt;
}

MaskGrid rasterise(const MaskShapes& shapes, const MaskFrame& frame)
{
    MaskGrid grid;
    grid.columns = frame.columns;
    grid.bins = frame.bins;
    if (frame.columns == 0 || frame.bins < 2)
        return grid;
    grid.weight.assign(frame.columns * frame.bins, 0.0f);
    const float nyquist = static_cast<float>(frame.sampleRate) / 2.0f;
    const float binHz = nyquist / static_cast<float>(frame.bins - 1);

    // A comb's bands do not change down the columns: bin ranges once per shape.
    std::vector<std::vector<std::pair<std::uint32_t, std::uint32_t>>> combBins(shapes.shapes.size());
    for (std::size_t i = 0; i < shapes.shapes.size(); ++i) {
        const MaskShape& shape = shapes.shapes[i];
        if (!shape.comb.has_value() || shape.empty() || !shape.comb->pitch.empty())
            continue;
        for (const auto& [lo, hi] : shape.comb->bands(nyquist)) {
            std::uint32_t a = 0;
            std::uint32_t b = 0;
            if (binRange(lo, hi, binHz, frame.bins, a, b))
                combBins[i].emplace_back(a, b);
        }
    }

    // Coverage, one column at a time, every shape in order. A shape paints
    // `value` over the bins it covers at time t; an Intersect paints 1 into a
    // scratch row instead, and the row keeps only what both have.
    std::vector<float> crossings;
    std::vector<float> scratch(frame.bins, 0.0f);
    const auto paint = [&](const MaskShape& shape, std::size_t i, float t, float* row, float value) {
        if (shape.comb.has_value()) {
            // Half-open in time, like a lasso's scanline.
            if (t < shape.comb->fromSeconds || t >= shape.comb->toSeconds)
                return;
            if (shape.comb->pitch.empty()) {
                for (const auto& [a, b] : combBins[i])
                    fill(row, a, b, value);
            } else {
                for (const auto& [lo, hi] : shape.comb->bandsAt(nyquist, t)) {
                    std::uint32_t a = 0, b = 0;
                    if (binRange(lo, hi, binHz, frame.bins, a, b))
                        fill(row, a, b, value);
                }
            }
            return;
        }
        if (shape.polygon.size() >= 3) {
            polygonCrossings(shape.polygon, t, crossings);
            for (std::size_t k = 0; k + 1 < crossings.size(); k += 2) {
                std::uint32_t lo = 0;
                std::uint32_t hi = 0;
                if (binRange(crossings[k], crossings[k + 1], binHz, frame.bins, lo, hi))
                    fill(row, lo, hi, value);
            }
        }
        for (const MaskStamp& st : shape.stamps) {
            if (st.rSeconds <= 0.0f) {
                if (t != st.seconds)
                    continue;
            }
            const float dt = st.rSeconds > 0.0f ? (t - st.seconds) / st.rSeconds : 0.0f;
            if (dt < -1.0f || dt > 1.0f)
                continue;
            const float s = std::sqrt(std::max(0.0f, 1.0f - dt * dt));
            const float lo = st.hz - (st.hz - st.hzLow) * s;
            const float hi = st.hz + (st.hzHigh - st.hz) * s;
            std::uint32_t a = 0;
            std::uint32_t b = 0;
            if (binRange(lo, hi, binHz, frame.bins, a, b))
                fill(row, a, b, value);
        }
    };
    for (std::size_t c = 0; c < frame.columns; ++c) {
        float* row = grid.weight.data() + c * frame.bins;
        const auto t = static_cast<float>(
            frame.originSeconds
            + static_cast<double>(frame.firstFrame + c * frame.hop) / frame.sampleRate);
        for (std::size_t i = 0; i < shapes.shapes.size(); ++i) {
            const MaskShape& shape = shapes.shapes[i];
            if (shape.empty())
                continue;
            switch (shape.op) {
            case MaskOp::Add:
                paint(shape, i, t, row, 1.0f);
                break;
            case MaskOp::Subtract:
                paint(shape, i, t, row, 0.0f);
                break;
            case MaskOp::Intersect:
                std::fill(scratch.begin(), scratch.end(), 0.0f);
                paint(shape, i, t, scratch.data(), 1.0f);
                for (std::uint32_t k = 0; k < frame.bins; ++k)
                    row[k] *= scratch[k];
                break;
            }
        }
    }

    // The feather: a soft dilation along bins, then along columns.
    if (shapes.featherBins > 0 || shapes.featherColumns > 0) {
        std::vector<float> scratch(grid.weight.size());
        if (shapes.featherBins > 0) {
            const std::vector<float> f = falloff(shapes.featherBins);
            for (std::size_t c = 0; c < frame.columns; ++c)
                dilate(grid.weight.data() + c * frame.bins, scratch.data() + c * frame.bins,
                       frame.bins, 1, f);
            grid.weight.swap(scratch);
        }
        if (shapes.featherColumns > 0) {
            const std::vector<float> f = falloff(shapes.featherColumns);
            for (std::uint32_t k = 0; k < frame.bins; ++k)
                dilate(grid.weight.data() + k, scratch.data() + k, frame.columns,
                       static_cast<std::ptrdiff_t>(frame.bins), f);
            grid.weight.swap(scratch);
        }
    }
    return grid;
}

} // namespace omawave::spectral
