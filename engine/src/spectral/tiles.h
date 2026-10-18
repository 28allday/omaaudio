#pragma once

#include "stft.h"
#include "session/audio_buffer.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace omawave::spectral {

// The spectral tile cache: a take's short-time spectrum as greyscale PNGs the page
// uploads as GPU textures, built off the GUI thread (brief section 8).
//
// Layout of one tile: width = columns (time), height = bins (row 0 is DC, the last
// row Nyquist -- the shader turns it the right way up), one byte per texel holding
// the bin's magnitude in dB from kFloorDb (0) to 0 dBFS (255). A level is the run of
// tiles across the whole take at one time decimation: level 0 pools analysis hops
// when the take would exceed the cache budget (short takes keep every column),
// level 1 keeps the maximum of every kLevelStep columns, and so on until a level
// fits in one tile -- the same powers-of-8 pyramid the peaks use, and for the same
// reason: the shader takes at most 8 taps per pixel and must never undersample.

constexpr float kFloorDb = -120.0f;
constexpr std::uint32_t kTileColumns = 1024;
constexpr std::uint32_t kLevelStep = 8;
// Level 0 across all channels; the remaining pyramid adds at most about 1/7.
// PNG headers/filter bytes add a small overhead. This limits stored texels, not
// the audio buffer or the time spent analysing the take.
constexpr std::size_t kLevel0ByteBudget = 192u * 1024u * 1024u;

// Power-of-two pooling needed to fit level 0 in the budget, retaining every bin.
// A budget smaller than one column still keeps one column per channel.
[[nodiscard]] std::uint32_t displayDecimation(std::size_t frames, std::uint32_t channels,
                                              const StftConfig& config,
                                              std::size_t byteBudget = kLevel0ByteBudget) noexcept;

[[nodiscard]] std::uint8_t encodeDb(float db) noexcept;
[[nodiscard]] float decodeDb(std::uint8_t texel) noexcept;

// Magnitude in dBFS of one bin, given the STFT's magnitude scale.
[[nodiscard]] float binDb(std::complex<float> bin, float magnitudeScale) noexcept;

struct SpectralTile {
    bool ok = false;
    std::string path;
    std::size_t firstColumn = 0;    // in this level's columns
    std::uint32_t columns = 0;      // = width
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct SpectralLevel {
    std::uint32_t decimation = 1;   // level-0 columns per column here
    std::size_t columns = 0;
    std::vector<SpectralTile> tiles;
};

struct SpectralChannel {
    std::vector<SpectralLevel> levels;
};

struct SpectralSet {
    bool ok = false;
    bool cancelled = false;
    std::string error;
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    std::uint32_t fftSize = 0;
    std::uint32_t analysisHop = 0;  // the chosen overlap; also used by repairs
    std::uint32_t hop = 0;          // display spacing, after pooling
    std::uint32_t bins = 0;
    std::size_t columns = 0;        // level 0
    std::uint32_t tileColumns = kTileColumns;
    std::vector<SpectralChannel> perChannel;

    // Every file this set wrote, for whoever deletes it.
    [[nodiscard]] std::vector<std::string> files() const;
};

// The encoded texels of columns [first, first + count) of one channel, column-major
// (a column is `bins` bytes, DC first). What a tile is made of, exposed so a test can
// read a tile back without a PNG decoder. With decimation > 1, each output column
// keeps the strongest value of that many analysis columns, including short peaks
// between display columns. first/count address the pooled columns. Cancellation
// returns an empty vector and is checked between analysis frames.
[[nodiscard]] std::vector<std::uint8_t> analyseColumns(std::span<const float> signal,
                                                       const StftConfig& config,
                                                       std::size_t first, std::size_t count,
                                                       std::uint32_t decimation = 1,
                                                       const std::atomic<bool>* cancel = nullptr);

// Max-pools `factor` columns into one. `src` is column-major with `columns` columns
// of `bins` bytes; the result has ceil(columns / factor) columns.
[[nodiscard]] std::vector<std::uint8_t> decimateColumns(const std::vector<std::uint8_t>& src,
                                                        std::uint32_t bins, std::size_t columns,
                                                        std::uint32_t factor);

using ProgressFn = std::function<void(float)>;

// Builds every tile of every channel of `audio` into `dir` as `<stem>-c<channel>-l<level>-t<tile>.png`.
// Streams: level 0 is analysed one tile at a time and written, so the whole
// spectrum is never in memory; the coarser levels are small enough to keep.
// Cancellable between tiles; a cancelled build removes what it wrote.
[[nodiscard]] SpectralSet buildSpectralTiles(const session::AudioBuffer& audio,
                                             std::uint32_t sampleRate,
                                             const StftConfig& config,
                                             const std::string& dir, const std::string& stem,
                                             const std::atomic<bool>* cancel = nullptr,
                                             const ProgressFn& progress = {},
                                             std::uint32_t tileColumns = kTileColumns,
                                             std::size_t byteBudget = kLevel0ByteBudget);

} // namespace omawave::spectral
