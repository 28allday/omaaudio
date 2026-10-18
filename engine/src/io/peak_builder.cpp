#include "peak_builder.h"

#include <algorithm>

namespace omawave::io {

PeakLevel buildPeaks(const session::AudioBuffer& audio, std::uint32_t samplesPerPeak)
{
    PeakLevel level;
    if (audio.empty() || samplesPerPeak == 0)
        return level;

    const std::size_t spp = samplesPerPeak;
    level.samplesPerPeak = samplesPerPeak;
    level.channels = audio.channels();
    level.count = (audio.frames() + spp - 1) / spp;   // ceil
    level.data.resize(level.count * level.channels * 2);

    for (std::uint32_t c = 0; c < audio.channels(); ++c) {
        const std::span<const float> src = audio.plane(c);
        const std::size_t channelBase = static_cast<std::size_t>(c) * level.count;

        for (std::size_t p = 0; p < level.count; ++p) {
            const std::size_t begin = p * spp;
            const std::size_t end = std::min(begin + spp, audio.frames());

            float lo = src[begin];
            float hi = src[begin];
            for (std::size_t i = begin + 1; i < end; ++i) {
                lo = std::min(lo, src[i]);
                hi = std::max(hi, src[i]);
            }

            level.data[(channelBase + p) * 2] = lo;
            level.data[(channelBase + p) * 2 + 1] = hi;
            if (audio.diskStorage() && (p & 255) == 255) audio.diskStorage()->releaseMappedPages();
        }
    }

    if (audio.diskStorage()) audio.diskStorage()->releaseMappedPages();
    return level;
}

} // namespace omawave::io
