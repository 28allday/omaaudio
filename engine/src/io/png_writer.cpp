#include "png_writer.h"

#include <array>
#include <cstdio>

namespace omawave::io {
namespace {

std::uint32_t crc32(const std::uint8_t* data, std::size_t size, std::uint32_t crc = 0)
{
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[n] = c;
        }
        return t;
    }();

    crc ^= 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i)
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

std::uint32_t adler32(const std::uint8_t* data, std::size_t size)
{
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (std::size_t i = 0; i < size; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

void appendBE32(std::vector<std::uint8_t>& out, std::uint32_t v)
{
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
}

void appendChunk(std::vector<std::uint8_t>& out, const char (&tag)[5],
                 const std::vector<std::uint8_t>& data)
{
    appendBE32(out, static_cast<std::uint32_t>(data.size()));
    std::vector<std::uint8_t> tagged;
    tagged.reserve(4 + data.size());
    for (int i = 0; i < 4; ++i)
        tagged.push_back(static_cast<std::uint8_t>(tag[i]));
    tagged.insert(tagged.end(), data.begin(), data.end());
    out.insert(out.end(), tagged.begin(), tagged.end());
    appendBE32(out, crc32(tagged.data(), tagged.size()));
}

// A zlib stream of stored blocks: no compression, no compressor linked. 65535 bytes
// per block is the format's maximum.
std::vector<std::uint8_t> storedZlib(const std::vector<std::uint8_t>& raw)
{
    std::vector<std::uint8_t> out;
    out.reserve(raw.size() + raw.size() / 65535 * 5 + 16);
    out.push_back(0x78);
    out.push_back(0x01);

    std::size_t offset = 0;
    do {
        const std::size_t chunk = std::min<std::size_t>(65535, raw.size() - offset);
        const bool last = offset + chunk >= raw.size();
        out.push_back(last ? 1 : 0);
        const auto len = static_cast<std::uint16_t>(chunk);
        const std::uint16_t nlen = static_cast<std::uint16_t>(~len);
        out.push_back(static_cast<std::uint8_t>(len & 0xFFu));
        out.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFFu));
        out.push_back(static_cast<std::uint8_t>(nlen & 0xFFu));
        out.push_back(static_cast<std::uint8_t>((nlen >> 8) & 0xFFu));
        out.insert(out.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
                   raw.begin() + static_cast<std::ptrdiff_t>(offset + chunk));
        offset += chunk;
    } while (offset < raw.size());

    appendBE32(out, adler32(raw.data(), raw.size()));
    return out;
}

} // namespace

std::string writePng(const std::string& path, std::uint32_t width, std::uint32_t height,
                     PngColour colour, const std::vector<std::uint8_t>& pixels)
{
    const std::size_t bpp = bytesPerPixel(colour);
    const std::size_t rowBytes = static_cast<std::size_t>(width) * bpp;
    if (width == 0 || height == 0)
        return "empty image";
    if (pixels.size() != rowBytes * height)
        return "pixel buffer is the wrong size";

    // Filter byte 0 in front of every row.
    std::vector<std::uint8_t> raw;
    raw.resize(static_cast<std::size_t>(height) * (1 + rowBytes), 0);
    for (std::uint32_t row = 0; row < height; ++row) {
        const std::size_t dst = static_cast<std::size_t>(row) * (1 + rowBytes) + 1;
        const std::size_t src = static_cast<std::size_t>(row) * rowBytes;
        std::copy(pixels.begin() + static_cast<std::ptrdiff_t>(src),
                  pixels.begin() + static_cast<std::ptrdiff_t>(src + rowBytes),
                  raw.begin() + static_cast<std::ptrdiff_t>(dst));
    }

    std::vector<std::uint8_t> png;
    const std::array<std::uint8_t, 8> signature{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    png.insert(png.end(), signature.begin(), signature.end());

    std::vector<std::uint8_t> ihdr;
    appendBE32(ihdr, width);
    appendBE32(ihdr, height);
    ihdr.push_back(8);                                          // bit depth
    ihdr.push_back(colour == PngColour::Rgba ? 6 : 0);          // colour type
    ihdr.push_back(0);                                          // compression: deflate
    ihdr.push_back(0);                                          // filter method
    ihdr.push_back(0);                                          // no interlace
    appendChunk(png, "IHDR", ihdr);
    appendChunk(png, "IDAT", storedZlib(raw));
    appendChunk(png, "IEND", {});

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr)
        return "could not open " + path + " for writing";
    const std::size_t written = std::fwrite(png.data(), 1, png.size(), f);
    const int closed = std::fclose(f);
    if (written != png.size() || closed != 0)
        return "short write to " + path;
    return {};
}

} // namespace omawave::io
