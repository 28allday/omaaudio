#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace omawave::io {

// The one PNG writer, shared by the peak textures and the spectral tiles.
//
// Written by hand with stored (uncompressed) deflate blocks and hand-computed CRC32
// and Adler32, so nothing is linked to produce it and the licence table in BRIEF.md
// section 2 stays as it is. Qt decodes these like any other PNG (a test checks them
// with Python's zlib, which shares no code with this).
enum class PngColour : std::uint8_t {
    Rgba,   // 4 bytes per pixel; nothing may live in alpha (Qt premultiplies it)
    Gray,   // 1 byte per pixel, colour type 0 -- a spectral tile's magnitudes
};

[[nodiscard]] constexpr std::uint32_t bytesPerPixel(PngColour colour) noexcept
{
    return colour == PngColour::Rgba ? 4u : 1u;
}

// `pixels` is row-major, `height` rows of `width * bytesPerPixel(colour)` bytes, no
// filter bytes. Returns an empty string on success, otherwise what went wrong.
[[nodiscard]] std::string writePng(const std::string& path, std::uint32_t width,
                                   std::uint32_t height, PngColour colour,
                                   const std::vector<std::uint8_t>& pixels);

} // namespace omawave::io
