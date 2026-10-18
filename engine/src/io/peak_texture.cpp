#include "peak_texture.h"
#include "png_writer.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

namespace omawave::io {
namespace {

constexpr std::uint32_t kTextureWidth = 2048;   // stays inside every GL max-texture limit

void split16(float value, std::uint8_t& hi, std::uint8_t& lo)
{
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    const auto q = static_cast<std::uint32_t>((clamped * 0.5f + 0.5f) * 65535.0f + 0.5f);
    const std::uint32_t limited = std::min<std::uint32_t>(q, 65535u);
    hi = static_cast<std::uint8_t>((limited >> 8) & 0xFFu);
    lo = static_cast<std::uint8_t>(limited & 0xFFu);
}

} // namespace

float PeakTexels::decode(std::size_t texelIndex) const noexcept
{
    const std::size_t base = texelIndex * 4;
    if (base + 1 >= rgba.size())
        return 0.0f;
    const float q = static_cast<float>(rgba[base]) * 256.0f + static_cast<float>(rgba[base + 1]);
    return (q / 65535.0f) * 2.0f - 1.0f;
}

PeakTexels buildPeakTexels(const PeakLevel& peaks)
{
    PeakTexels out;
    if (peaks.empty())
        return out;

    const std::size_t count = peaks.count;
    const std::uint32_t width = kTextureWidth;
    const std::uint32_t blocks = 1 + peaks.channels;

    // Two texels per peak, `blocks` blocks of `count` peaks: the combined envelope
    // first, then each channel. See the premultiplied-alpha note in the header for
    // why nothing lives in A and every texel is opaque.
    const std::size_t texels = count * 2 * blocks;
    const auto height = static_cast<std::uint32_t>((texels + width - 1) / width);

    out.width = width;
    out.height = height;
    out.peakCount = count;
    out.channels = peaks.channels;
    out.rgba.assign(static_cast<std::size_t>(width) * height * 4, 0);
    for (std::size_t t = 0; t < static_cast<std::size_t>(width) * height; ++t)
        out.rgba[t * 4 + 3] = 255;

    const auto writeTexel = [&](std::size_t texelIndex, float value) {
        const std::size_t base = texelIndex * 4;
        split16(value, out.rgba[base + 0], out.rgba[base + 1]);
        out.rgba[base + 2] = 0;
        out.rgba[base + 3] = 255;
    };

    for (std::size_t i = 0; i < count; ++i) {
        float lo = peaks.minAt(0, i);
        float hi = peaks.maxAt(0, i);
        for (std::uint32_t c = 1; c < peaks.channels; ++c) {
            lo = std::min(lo, peaks.minAt(c, i));
            hi = std::max(hi, peaks.maxAt(c, i));
        }
        writeTexel(i * 2, hi);
        writeTexel(i * 2 + 1, lo);

        for (std::uint32_t c = 0; c < peaks.channels; ++c) {
            const std::size_t blockBase = (static_cast<std::size_t>(c) + 1) * count * 2;
            writeTexel(blockBase + i * 2, peaks.maxAt(c, i));
            writeTexel(blockBase + i * 2 + 1, peaks.minAt(c, i));
        }
    }
    return out;
}

PeakTexture writePeakTexture(const PeakLevel& peaks, const std::string& path)
{
    PeakTexture out;
    out.path = path;

    if (peaks.empty()) {
        out.error = "no peak data";
        return out;
    }

    const PeakTexels texels = buildPeakTexels(peaks);
    const std::uint32_t width = texels.width;
    const std::uint32_t height = texels.height;

    const std::string error = writePng(path, width, height, PngColour::Rgba, texels.rgba);
    if (!error.empty()) {
        out.error = error;
        return out;
    }

    out.ok = true;
    out.width = width;
    out.height = height;
    out.peakCount = texels.peakCount;
    out.channels = texels.channels;
    return out;
}

} // namespace omawave::io
