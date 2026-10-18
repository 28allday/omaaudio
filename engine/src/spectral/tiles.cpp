#include "tiles.h"

#include "io/png_writer.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>

namespace omawave::spectral {

std::uint32_t displayDecimation(std::size_t frames, std::uint32_t channels,
                                 const StftConfig& config, std::size_t byteBudget) noexcept
{
    if (!config.valid() || channels == 0 || frames == 0)
        return 1;
    const std::size_t maxColumns = std::max<std::size_t>(1, byteBudget / channels / config.bins());
    const std::size_t columns = columnCount(frames, config.hop);
    std::uint32_t factor = 1;
    // Bound hop * factor as well as factor itself. Division avoids overflowing
    // the estimate even for a length supplied without an allocated audio buffer.
    const std::uint32_t maxFactor = std::numeric_limits<std::uint32_t>::max() / config.hop;
    while ((columns - 1) / factor + 1 > maxColumns && factor <= maxFactor / 2)
        factor *= 2;
    return factor;
}

std::uint8_t encodeDb(float db) noexcept
{
    const float t = std::clamp((db - kFloorDb) / -kFloorDb, 0.0f, 1.0f);
    return static_cast<std::uint8_t>(std::lround(t * 255.0f));
}

float decodeDb(std::uint8_t texel) noexcept
{
    return kFloorDb + (static_cast<float>(texel) / 255.0f) * -kFloorDb;
}

float binDb(std::complex<float> bin, float magnitudeScale) noexcept
{
    const float mag = std::abs(bin) * magnitudeScale;
    if (mag <= 1e-9f)
        return kFloorDb;
    return 20.0f * std::log10(mag);
}

std::vector<std::string> SpectralSet::files() const
{
    std::vector<std::string> out;
    for (const SpectralChannel& ch : perChannel) {
        for (const SpectralLevel& level : ch.levels) {
            for (const SpectralTile& tile : level.tiles) {
                if (tile.ok)
                    out.push_back(tile.path);
            }
        }
    }
    return out;
}

std::vector<std::uint8_t> analyseColumns(std::span<const float> signal, const StftConfig& config,
                                         std::size_t first, std::size_t count,
                                         std::uint32_t decimation, const std::atomic<bool>* cancel)
{
    if (!config.valid() || decimation == 0)
        return {};
    Stft stft(config);
    const std::uint32_t bins = config.bins();
    std::vector<std::uint8_t> out(count * bins, 0);
    std::vector<std::complex<float>> column(bins);
    const std::size_t columns = columnCount(signal.size(), config.hop);
    for (std::size_t c = 0; c < count; ++c) {
        std::uint8_t* dst = out.data() + c * bins;
        const std::size_t from = (first + c) * decimation;
        if (from >= columns)
            break;
        const std::size_t to = from + std::min<std::size_t>(decimation, columns - from);
        for (std::size_t i = from; i < to; ++i) {
            if (cancel != nullptr && cancel->load(std::memory_order_relaxed))
                return {};
            stft.analyse(signal, i, column.data());
            for (std::uint32_t k = 0; k < bins; ++k)
                dst[k] = std::max(dst[k], encodeDb(binDb(column[k], stft.magnitudeScale())));
        }
    }
    return out;
}

std::vector<std::uint8_t> decimateColumns(const std::vector<std::uint8_t>& src, std::uint32_t bins,
                                          std::size_t columns, std::uint32_t factor)
{
    if (factor == 0 || bins == 0)
        return {};
    const std::size_t outColumns = (columns + factor - 1) / factor;
    std::vector<std::uint8_t> out(outColumns * bins, 0);
    for (std::size_t o = 0; o < outColumns; ++o) {
        std::uint8_t* dst = out.data() + o * bins;
        const std::size_t from = o * factor;
        const std::size_t to = std::min(columns, from + factor);
        for (std::size_t c = from; c < to; ++c) {
            const std::uint8_t* s = src.data() + c * bins;
            for (std::uint32_t k = 0; k < bins; ++k)
                dst[k] = std::max(dst[k], s[k]);
        }
    }
    return out;
}

namespace {

// Column-major texels (a column is `bins` bytes) to the row-major image a PNG wants
// (row = bin). A transpose, in other words.
std::vector<std::uint8_t> toRows(const std::uint8_t* columns, std::size_t count, std::uint32_t bins)
{
    std::vector<std::uint8_t> rows(count * bins, 0);
    for (std::size_t c = 0; c < count; ++c) {
        const std::uint8_t* src = columns + c * bins;
        for (std::uint32_t k = 0; k < bins; ++k)
            rows[static_cast<std::size_t>(k) * count + c] = src[k];
    }
    return rows;
}

SpectralTile writeTile(const std::string& path, const std::uint8_t* columns, std::size_t count,
                       std::uint32_t bins, std::size_t firstColumn)
{
    SpectralTile tile;
    tile.path = path;
    tile.firstColumn = firstColumn;
    tile.columns = static_cast<std::uint32_t>(count);
    tile.width = tile.columns;
    tile.height = bins;
    const std::string error = io::writePng(path, tile.width, tile.height, io::PngColour::Gray,
                                           toRows(columns, count, bins));
    tile.ok = error.empty();
    if (!tile.ok) {
        // A short write can leave an incomplete PNG. Do not remove a directory
        // that blocked the write, or anything outside this tile's own path.
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec))
            std::filesystem::remove(path, ec);
    }
    return tile;
}

std::string tilePath(const std::string& dir, const std::string& stem, std::uint32_t channel,
                     std::size_t level, std::size_t tile)
{
    return dir + "/" + stem + "-c" + std::to_string(channel) + "-l" + std::to_string(level)
         + "-t" + std::to_string(tile) + ".png";
}

void removeAll(const SpectralSet& set)
{
    std::error_code ec;
    for (const std::string& path : set.files())
        std::filesystem::remove(path, ec);
}

} // namespace

SpectralSet buildSpectralTiles(const session::AudioBuffer& audio, std::uint32_t sampleRate,
                               const StftConfig& config, const std::string& dir,
                               const std::string& stem, const std::atomic<bool>* cancel,
                               const ProgressFn& progress, std::uint32_t tileColumns,
                               std::size_t byteBudget)
{
    SpectralSet set;
    set.sampleRate = sampleRate;
    // A multiple of the level step, so pooling each tile on its own gives the same
    // columns as pooling the whole run would.
    set.tileColumns = std::max<std::uint32_t>(kLevelStep,
                                              (tileColumns + kLevelStep - 1) / kLevelStep * kLevelStep);
    if (!config.valid()) {
        set.error = "invalid STFT configuration";
        return set;
    }
    if (audio.empty()) {
        set.error = "no audio";
        return set;
    }
    set.channels = audio.channels();
    set.fftSize = config.fftSize;
    const std::uint32_t pooling = displayDecimation(audio.frames(), audio.channels(), config, byteBudget);
    set.analysisHop = config.hop;
    set.hop = config.hop * pooling;
    set.bins = config.bins();
    set.columns = columnCount(audio.frames(), set.hop);

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    const std::uint32_t bins = set.bins;
    const std::size_t level0Tiles = (set.columns + set.tileColumns - 1) / set.tileColumns;
    // Work units for the progress: level-0 tiles dominate; the coarser levels are
    // counted as one each.
    const float perTile = 1.0f / static_cast<float>(std::max<std::size_t>(1, level0Tiles * set.channels + set.channels));
    float done = 0.0f;

    const auto cancelled = [&] { return cancel != nullptr && cancel->load(std::memory_order_relaxed); };
    const auto fail = [&](const std::string& error, bool wasCancelled = false) {
        set.cancelled = wasCancelled;
        set.error = error;
        removeAll(set);
        set.perChannel.clear();
        return std::move(set);
    };

    set.perChannel.resize(set.channels);
    for (std::uint32_t ch = 0; ch < set.channels; ++ch) {
        const std::span<const float> plane = audio.plane(ch);
        SpectralChannel& channel = set.perChannel[ch];

        // Level 0, one tile at a time; the level-1 columns accumulate as we go.
        // Register each level before writing it, so cancellation/error cleanup
        // includes tiles from the channel currently being built.
        SpectralLevel& level0 = channel.levels.emplace_back();
        level0.decimation = 1;
        level0.columns = set.columns;
        std::vector<std::uint8_t> coarse;      // level 1, column-major, built up per tile
        std::size_t coarseColumns = 0;
        for (std::size_t t = 0; t < level0Tiles; ++t) {
            if (cancelled())
                return fail("cancelled", true);
            const std::size_t first = t * set.tileColumns;
            const std::size_t count = std::min<std::size_t>(set.tileColumns, set.columns - first);
            const std::vector<std::uint8_t> texels = analyseColumns(plane, config, first, count, pooling, cancel);
            if (cancelled())
                return fail("cancelled", true);
            level0.tiles.push_back(writeTile(tilePath(dir, stem, ch, 0, t), texels.data(), count,
                                             bins, first));
            if (!level0.tiles.back().ok)
                return fail("could not write " + level0.tiles.back().path);
            if (level0Tiles > 1) {
                // A tile's columns are a multiple of kLevelStep except the last, so
                // pooling per tile gives the same columns as pooling the whole run.
                const std::vector<std::uint8_t> pooled = decimateColumns(texels, bins, count, kLevelStep);
                coarse.insert(coarse.end(), pooled.begin(), pooled.end());
                coarseColumns += pooled.size() / bins;
            }
            done += perTile;
            if (progress)
                progress(std::min(1.0f, done));
        }

        // The coarser levels, until one fits in a single tile.
        std::uint32_t decimation = kLevelStep;
        std::size_t levelIndex = 1;
        while (coarseColumns > 0) {
            if (cancelled())
                return fail("cancelled", true);
            SpectralLevel& level = channel.levels.emplace_back();
            level.decimation = decimation;
            level.columns = coarseColumns;
            const std::size_t tiles = (coarseColumns + set.tileColumns - 1) / set.tileColumns;
            for (std::size_t t = 0; t < tiles; ++t) {
                if (cancelled())
                    return fail("cancelled", true);
                const std::size_t first = t * set.tileColumns;
                const std::size_t count = std::min<std::size_t>(set.tileColumns, coarseColumns - first);
                level.tiles.push_back(writeTile(tilePath(dir, stem, ch, levelIndex, t),
                                                coarse.data() + first * bins, count, bins, first));
                if (!level.tiles.back().ok)
                    return fail("could not write " + level.tiles.back().path);
            }
            if (coarseColumns <= set.tileColumns)
                break;
            coarse = decimateColumns(coarse, bins, coarseColumns, kLevelStep);
            coarseColumns = coarse.size() / bins;
            decimation *= kLevelStep;
            ++levelIndex;
        }
        done += perTile;
        if (progress)
            progress(std::min(1.0f, done));
    }

    if (cancelled())
        return fail("cancelled", true);
    set.ok = true;
    if (progress)
        progress(1.0f);
    return set;
}

} // namespace omawave::spectral
