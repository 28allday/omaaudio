#include "file_batch.h"
#include "audio_store.h"
#include "io/audio_file.h"
#include "io/resampler.h"

#include <array>
#include <limits>
#include <stdexcept>

namespace omawave::render {
namespace {
std::int64_t rescaleMetadataFrame(std::int64_t value, std::uint32_t from, std::uint32_t to)
{
    if (value < 0 || !from || !to) throw std::runtime_error("invalid file metadata position");
    const auto limit = std::uint64_t(std::numeric_limits<std::int64_t>::max());
    const auto whole = std::uint64_t(value) / from;
    const auto fraction = ((std::uint64_t(value) % from) * to + from / 2) / from;
    if (whole > limit / to || whole * to > limit - fraction)
        throw std::runtime_error("file metadata position exceeds the output range");
    return std::int64_t(whole * to + fraction);
}
}

PreparedFile prepareBatchFile(const std::string& path, ExportSpec output,
                             const std::atomic<bool>* cancel, const ProgressFn& progress)
{
    PreparedFile result; result.output = std::move(output); result.output.replaceExisting = false;
    const auto stopped = [&] { return cancel && cancel->load(std::memory_order_relaxed); };
    const auto checkCancel = [&] { if (stopped()) throw std::runtime_error("file processing cancelled"); };
    try {
        checkCancel();
        auto file = io::readAudioFile(path, cancel, [&](float fraction) { if (progress) progress(.65f * fraction); });
        checkCancel();
        if (!file.ok) throw std::runtime_error(file.error);
        if (file.audio.empty()) throw std::runtime_error("the input contains no audio");
        result.warning = file.error;
        const auto rate = result.output.sampleRate ? result.output.sampleRate : file.sampleRate;
        const auto channels = result.output.channels ? result.output.channels : file.channels;
        if (rate < 8000 || rate > 192000) throw std::runtime_error("choose a processing rate from 8 to 192 kHz");
        if (!channels || channels > 8) throw std::runtime_error("batch output supports up to eight channels; choose Mono to fold this file");
        if (channels != file.channels && channels != 1 && !(channels == 2 && file.channels == 1))
            throw std::runtime_error("Stereo accepts mono or stereo sources; preserve channels or choose Mono for this layout");
        // Check after channel conversion has been resolved: mono-to-stereo is an
        // explicit preparation operation, not an exporter channel-count guess.
        const auto capability = checkSpec(result.output, rate, channels);
        if (!capability.ok) throw std::runtime_error(capability.reason);
        result.output.metadata = (result.output.writeMetadata || result.output.writeMarkers)
            ? std::move(file.metadata) : session::AudioMetadata{};
        auto& metadata = result.output.metadata;
        if (metadata.bext.present)
            metadata.bext.timeReference = rescaleMetadataFrame(metadata.bext.timeReference, file.sampleRate, rate);
        for (auto& marker : metadata.markers) {
            if (marker.length < 0 || marker.position < 0
                || marker.position > std::numeric_limits<std::int64_t>::max() - marker.length)
                throw std::runtime_error("invalid file marker interval");
            const auto end = rescaleMetadataFrame(marker.position + marker.length, file.sampleRate, rate);
            marker.position = rescaleMetadataFrame(marker.position, file.sampleRate, rate);
            marker.length = end - marker.position;
        }
        metadata.bext.loudnessValue.reset(); metadata.bext.loudnessRange.reset(); metadata.bext.maxTruePeak.reset();
        metadata.bext.maxMomentaryLoudness.reset(); metadata.bext.maxShortTermLoudness.reset();
        if (rate != file.sampleRate) file.audio = io::Resampler(file.sampleRate, rate).process(file.audio, cancel);
        checkCancel(); if (progress) progress(.8f);
        if (channels != file.channels) {
            AudioStore mapped(channels, file.audio.frames());
            std::array<float, AudioStore::blockFrames> block;
            const auto& source = file.audio;
            for (std::size_t first = 0; first < source.frames(); first += block.size()) {
                checkCancel(); const auto count = std::min(block.size(), source.frames() - first);
                if (channels == 1) {
                    std::fill_n(block.begin(), count, 0.f);
                    for (std::uint32_t c = 0; c < source.channels(); ++c) {
                        const auto plane = source.plane(c);
                        for (std::size_t f = 0; f < count; ++f) block[f] += plane[first + f] / float(source.channels());
                    }
                    mapped.write(0, first, std::span<const float>(block).first(count));
                } else {
                    const auto plane = source.plane(0).subspan(first, count);
                    mapped.write(0, first, plane); mapped.write(1, first, plane);
                }
                if (source.diskStorage()) source.diskStorage()->releaseMappedPages();
                if (progress) progress(.8f + .2f * float(first + count) / source.frames());
            }
            file.audio = mapped.finish();
        }
        checkCancel();
        result.audio = std::move(file.audio); result.sampleRate = rate;
        result.output.sampleRate = rate; result.output.channels = channels;
        result.ok = true; if (progress) progress(1);
    } catch (const std::exception& error) {
        result.audio = {}; result.cancelled = stopped();
        result.error = result.cancelled ? "file processing cancelled" : error.what();
    }
    return result;
}
} // namespace omawave::render
