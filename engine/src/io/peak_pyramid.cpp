#include "peak_pyramid.h"

#include <algorithm>
#include <cmath>

namespace omawave::io {
namespace {

// Once a level is this short there is nothing left to gain: it already fits across a
// window many times over.
constexpr std::size_t kSmallestUsefulLevel = 8;

} // namespace

PeakLevel reducePeaks(const PeakLevel& source, std::uint32_t ratio)
{
    PeakLevel out;
    if (source.empty() || ratio < 2)
        return out;

    out.samplesPerPeak = source.samplesPerPeak * ratio;
    out.channels = source.channels;
    out.count = (source.count + ratio - 1) / ratio;      // ceil, same rule as level 0
    out.data.resize(out.count * out.channels * 2);

    for (std::uint32_t c = 0; c < source.channels; ++c) {
        const std::size_t srcBase = static_cast<std::size_t>(c) * source.count;
        const std::size_t dstBase = static_cast<std::size_t>(c) * out.count;

        for (std::size_t p = 0; p < out.count; ++p) {
            const std::size_t begin = p * ratio;
            const std::size_t end = std::min(begin + ratio, source.count);

            float lo = source.data[(srcBase + begin) * 2];
            float hi = source.data[(srcBase + begin) * 2 + 1];
            for (std::size_t i = begin + 1; i < end; ++i) {
                lo = std::min(lo, source.data[(srcBase + i) * 2]);
                hi = std::max(hi, source.data[(srcBase + i) * 2 + 1]);
            }

            out.data[(dstBase + p) * 2] = lo;
            out.data[(dstBase + p) * 2 + 1] = hi;
        }
    }

    return out;
}

PeakPyramid pyramidFromLevel(PeakLevel base)
{
    PeakPyramid pyramid;
    if (base.empty())
        return pyramid;

    pyramid.levels.push_back(std::move(base));

    while (pyramid.levels.back().count > kSmallestUsefulLevel) {
        PeakLevel next = reducePeaks(pyramid.levels.back(), kLevelRatio);
        if (next.empty() || next.count >= pyramid.levels.back().count)
            break;                                        // no progress; stop rather than loop
        pyramid.levels.push_back(std::move(next));
    }

    return pyramid;
}

PeakPyramid buildPyramid(const session::AudioBuffer& audio, std::uint32_t basePeakSamples)
{
    if (audio.empty() || basePeakSamples == 0)
        return {};
    return pyramidFromLevel(buildPeaks(audio, basePeakSamples));
}

std::size_t PeakPyramid::levelForPeaksPerPixel(double basePeaksPerPixel) const noexcept
{
    if (levels.empty())
        return 0;
    if (basePeaksPerPixel <= 1.0)
        return 0;

    // Find the level where peaks-per-pixel drops below the ratio, so the renderer's
    // bounded tap count is always enough.
    double ppp = basePeaksPerPixel;
    std::size_t index = 0;
    while (ppp >= static_cast<double>(kLevelRatio) && index + 1 < levels.size()) {
        ppp /= static_cast<double>(kLevelRatio);
        ++index;
    }
    return index;
}

} // namespace omawave::io
