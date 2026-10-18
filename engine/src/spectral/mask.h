#pragma once

#include "stft.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace omawave::spectral {

// A mask is what a repair works on when a rectangle is not enough: the user draws
// in the time-frequency plane (a lasso, brush strokes, a spot) and the repairs
// take a weight per (column, bin) instead of a weight per bin. The shapes are kept
// in the take's own units -- seconds and Hz -- so they survive a zoom, a scale
// change and a different FFT size, and are rasterised onto the STFT grid when a
// repair runs. Painter's order: a later Subtract shape cuts a hole in what came
// before; a later Intersect keeps only what it covers of it; a later Add fills
// it again.

struct MaskPoint {
    float seconds = 0.0f;
    float hz = 0.0f;
};

// One dab of the brush: an ellipse in time, and in frequency an interval [hzLow,
// hzHigh] around `hz` that the ellipse's curve scales -- asymmetric on purpose, so a
// round brush on a logarithmic display is exactly what the engine treats.
struct MaskStamp {
    float seconds = 0.0f;
    float hz = 0.0f;
    float rSeconds = 0.0f;
    float hzLow = 0.0f;
    float hzHigh = 0.0f;
};

// Painter's order: Add covers, Subtract cuts a hole in what came before, and
// Intersect keeps only what came before that it also covers (A25).
enum class MaskOp : std::uint8_t { Add, Subtract, Intersect };

// A harmonic series (amendment A24): the partials of a fundamental, each a band
// of its own, over a stretch of time. What a picked note, a whistle's overtones
// or mains hum are on the display -- a comb -- so the user draws it with one
// click rather than a dozen rectangles. Each partial's band is `width` wide
// either side of n × fundamental: in cents (proportional, so vibrato that wanders
// n times as far at the n-th partial is still inside) or in Hz (fixed, which is
// what hum wants -- every partial of a 50 Hz hum is as narrow as the first).
// `partials` is the highest harmonic number; with the fundamental left out the
// series is the overtones alone, from the second partial up.
struct HarmonicComb {
    float fromSeconds = 0.0f;
    float toSeconds = 0.0f;
    float fundamentalHz = 0.0f;
    std::uint32_t partials = 16;
    float width = 30.0f;
    bool widthInHz = false;
    bool includeFundamental = true;
    std::vector<MaskPoint> pitch{}; // optional, strictly increasing time/fundamental points

    [[nodiscard]] bool validPitch() const noexcept;
    [[nodiscard]] float fundamentalAt(float seconds) const noexcept;

    [[nodiscard]] bool empty() const noexcept
    {
        return fundamentalHz <= 0.0f || partials == 0 || toSeconds <= fromSeconds
               || (!includeFundamental && partials < 2);
    }
    // The bands, lowest first, as [lowHz, highHz]; a partial whose band starts
    // above `nyquist` is left out, and those after it.
    [[nodiscard]] std::vector<std::pair<float, float>> bands(float nyquist) const;
    [[nodiscard]] std::vector<std::pair<float, float>> bandsAt(float nyquist, float seconds) const;
};

struct MaskShape {
    MaskOp op = MaskOp::Add;
    std::vector<MaskPoint> polygon;  // a closed lasso: three points or more, or
    std::vector<MaskStamp> stamps;   // a brush stroke: one stamp or more, or
    std::optional<HarmonicComb> comb; // a harmonic series

    [[nodiscard]] bool empty() const noexcept
    {
        return polygon.size() < 3 && stamps.empty() && (!comb.has_value() || comb->empty());
    }

    [[nodiscard]] static MaskShape lasso(std::vector<MaskPoint> points, MaskOp op = MaskOp::Add)
    {
        MaskShape s;
        s.op = op;
        s.polygon = std::move(points);
        return s;
    }
    [[nodiscard]] static MaskShape stroke(std::vector<MaskStamp> stamps, MaskOp op = MaskOp::Add)
    {
        MaskShape s;
        s.op = op;
        s.stamps = std::move(stamps);
        return s;
    }
    [[nodiscard]] static MaskShape harmonics(HarmonicComb comb, MaskOp op = MaskOp::Add)
    {
        MaskShape s;
        s.op = op;
        s.comb = std::move(comb);
        return s;
    }
};

struct MaskShapes {
    std::vector<MaskShape> shapes;
    // The soft edge, outward from the drawn shape, as a raised cosine over this many
    // bins and columns. Zero is a hard edge.
    std::uint32_t featherBins = 2;
    std::uint32_t featherColumns = 2;

    [[nodiscard]] bool empty() const noexcept;
    // The extent of what is added, in seconds and in Hz; nothing when the mask adds
    // nothing.
    [[nodiscard]] std::optional<std::pair<float, float>> timeBounds() const;
    [[nodiscard]] std::optional<std::pair<float, float>> hzBounds() const;
    // The same shapes with every time moved by `seconds` (a repair's buffer starts
    // part-way into the take).
    [[nodiscard]] MaskShapes shifted(double seconds) const;
};

// The weights a repair reads: one per (column, bin) over the columns of a region,
// column-major. A grid with one column and `uniform` set is the same row for every
// column (a band).
struct MaskGrid {
    std::size_t columns = 0;
    std::uint32_t bins = 0;
    bool uniform = false;
    std::vector<float> weight;

    [[nodiscard]] float at(std::size_t column, std::uint32_t bin) const noexcept
    {
        return weight[(uniform ? 0 : column) * bins + bin];
    }
    [[nodiscard]] const float* row(std::size_t column) const noexcept
    {
        return weight.data() + (uniform ? 0 : column) * bins;
    }
};

// Where a grid's columns sit: column c is the frame centred on buffer frame
// `firstFrame + c * hop`, and buffer frame 0 is `originSeconds` into the shapes'
// time base (the take).
struct MaskFrame {
    std::size_t firstFrame = 0;
    std::size_t columns = 0;
    std::uint32_t hop = 512;
    std::uint32_t bins = 1025;
    std::uint32_t sampleRate = 48000;
    double originSeconds = 0.0;
};

// A repair asks its mask for the grid once it knows the region's columns.
using MaskSource = std::function<MaskGrid(const MaskFrame&)>;

// Bins over which a band's edge ramps from untouched to fully processed, so a
// treated band has no hard spectral edge to ring.
constexpr std::uint32_t kFeatherBins = 2;

// Frequencies in Hz. `highHz <= 0` means Nyquist; a band of 0..Nyquist is the whole
// spectrum, which makes attenuate a plain gain and heal a time interpolation.
struct Band {
    float lowHz = 0.0f;
    float highHz = 0.0f;
};

// Per-bin weight of the band, 0 outside, 1 inside, a raised cosine over kFeatherBins
// at each edge.
[[nodiscard]] std::vector<float> bandWeights(Band band, std::uint32_t bins,
                                             std::uint32_t sampleRate);

[[nodiscard]] MaskSource bandMask(Band band);
[[nodiscard]] MaskSource shapeMask(MaskShapes shapes);
// Built-in masks support blocks with this many neighbouring columns. Arbitrary
// callbacks still receive the complete grid, preserving their existing contract.
[[nodiscard]] std::optional<std::uint32_t> maskColumnHalo(const MaskSource& mask);

// The rasteriser itself, for tests: coverage from the shapes in painter's order,
// then the feather as a soft dilation (the drawn shape stays fully inside).
[[nodiscard]] MaskGrid rasterise(const MaskShapes& shapes, const MaskFrame& frame);

} // namespace omawave::spectral
