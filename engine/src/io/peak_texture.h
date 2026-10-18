#pragma once

#include "peak_builder.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace omawave::io {

struct PeakTexture {
    bool ok = false;
    std::string error;
    std::string path;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t peakCount = 0;          // per block
    std::uint32_t channels = 0;         // blocks 1..channels are the channels
};

// The texels before they are a PNG, for the tests and for anything that wants
// to read them back without a decoder. RGBA8, row-major, no filter bytes.
struct PeakTexels {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t peakCount = 0;
    std::uint32_t channels = 0;
    std::vector<std::uint8_t> rgba;

    // The 16-bit value in a texel's R/G, back to -1..1.
    [[nodiscard]] float decode(std::size_t texelIndex) const noexcept;
};

[[nodiscard]] PeakTexels buildPeakTexels(const PeakLevel& peaks);

// Writes peak data as a PNG for the UI to upload as a GPU texture.
//
// Why a file and not the socket: Phase 0 established that the waveform must be drawn
// with a ShaderEffect (docs/phase0-results.md), and QML cannot turn received bytes
// into a texture without a C++ image provider -- which amendment A3 rules out, since
// Omarchy plugins are QML entry points. An Image pointed at a path is the one route
// from engine-side data to a GPU texture that stays inside the plugin contract.
//
// The PNG is written by hand using stored (uncompressed) deflate blocks, so nothing is
// linked to produce it. That keeps the licence table in BRIEF.md section 2 unchanged.
//
// Layout matches what wave.frag expects: RGBA8, two texels per peak -- the first
// holding the 16-bit max in R/G, the second the 16-bit min. Nothing is stored in the
// alpha channel and every texel is opaque, because Qt premultiplies alpha on upload
// and would otherwise scale the data by it. Channels are combined into a single
// envelope for Phase 1.
[[nodiscard]] PeakTexture writePeakTexture(const PeakLevel& peaks, const std::string& path);

} // namespace omawave::io
