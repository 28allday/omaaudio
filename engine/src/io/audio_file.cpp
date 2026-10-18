#include "audio_file.h"
#include "ffmpeg_audio.h"
#include "metadata.h"

#include <sndfile.h>
#include <cctype>
#include <filesystem>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <span>
#include <vector>

namespace omawave::io {
namespace {

// RAII for the libsndfile handle. Section 8: no raw owning pointers.
class SndFile {
public:
    explicit SndFile(SNDFILE* f) noexcept : file_(f) {}
    ~SndFile()
    {
        if (file_ != nullptr)
            sf_close(file_);
    }

    SndFile(const SndFile&) = delete;
    SndFile& operator=(const SndFile&) = delete;
    SndFile(SndFile&&) = delete;
    SndFile& operator=(SndFile&&) = delete;

    [[nodiscard]] SNDFILE* get() const noexcept { return file_; }
    [[nodiscard]] bool valid() const noexcept { return file_ != nullptr; }
    int close() noexcept
    {
        SNDFILE* file = file_;
        file_ = nullptr;
        return sf_close(file);
    }

private:
    SNDFILE* file_ = nullptr;
};

// "AAC (MPEG-4)", "PCM (MXF)", "PCM (QuickTime)": the codec, then its container.
std::string containerLabel(const std::string& path, const std::string& codec)
{
    auto extension = std::filesystem::path(path).extension().string();
    for (auto& c : extension) c = char(std::tolower(static_cast<unsigned char>(c)));
    const std::string container = extension == ".mxf" ? "MXF" : (extension == ".mov" || extension == ".qt") ? "QuickTime" : "MPEG-4";
    const std::string name = codec == "aac" ? "AAC" : codec.starts_with("pcm_") ? "PCM" : codec;
    return name + " (" + container + ")";
}

std::string describeFormat(int format)
{
    SF_FORMAT_INFO info{};
    info.format = format;
    if (sf_command(nullptr, SFC_GET_FORMAT_INFO, &info, sizeof(info)) == 0
        && info.name != nullptr) {
        return info.name;
    }
    return "unknown";
}

} // namespace

AudioFileInfo probeAudioFile(const std::string& path)
{
    AudioFileInfo out;

    SF_INFO info{};
    const SndFile file(sf_open(path.c_str(), SFM_READ, &info));
    if (!file.valid()) {
        if (isCompressedAudioPath(path)) {
            const DecodedInfo probed = probeCompressedAudio(path);
            if (!probed.ok) { out.error = probed.error; return out; }
            out.ok = true;
            out.sampleRate = probed.sampleRate;
            out.channels = probed.channels;
            out.frames = probed.frames;
            out.formatName = containerLabel(path, probed.codec);
            return out;
        }
        out.error = sf_strerror(nullptr);
        return out;
    }

    out.ok = true;
    out.sampleRate = static_cast<std::uint32_t>(info.samplerate);
    out.channels = static_cast<std::uint32_t>(info.channels);
    out.frames = static_cast<std::int64_t>(info.frames);
    out.formatName = describeFormat(info.format & SF_FORMAT_TYPEMASK);
    return out;
}

AudioFile readAudioFile(const std::string& path)
{
    return readAudioFile(path, nullptr, {});
}

ImportSupport importSupport()
{
    const DecodeSupport d = aacDecodeSupport();
    return {d.available, d.reason};
}

AudioFile readAudioFile(const std::string& path, const std::atomic<bool>* cancel,
                        const std::function<void(float)>& progress)
{
    AudioFile out;
    const auto cancelled = [&] {
        if (!cancel || !cancel->load(std::memory_order_relaxed)) return false;
        out.audio = {}; out.ok = false; out.error = "Audio loading cancelled.";
        return true;
    };
    try {
    if (cancelled()) return out;

    SF_INFO info{};
    const SndFile file(sf_open(path.c_str(), SFM_READ, &info));
    if (!file.valid()) {
        if (!isCompressedAudioPath(path)) {
            out.error = sf_strerror(nullptr);
            return out;
        }
        // An MPEG-4 audio file: FFmpeg decodes it into a private float WAV in the
        // cache, which is read like any other file and then removed.
        const DecodedAudio decoded = decodeCompressedAudio(path, cancel,
            [&](float value) { if (progress) progress(0.9f * value); });
        if (!decoded.ok) {
            out.error = decoded.error;
            return out;
        }
        struct Staged { std::string path; ~Staged() { std::error_code ec; std::filesystem::remove(path, ec); } } staged{decoded.stagedWav};
        out = readAudioFile(decoded.stagedWav, cancel,
            [&](float value) { if (progress) progress(0.9f + 0.1f * value); });
        out.formatName = containerLabel(path, decoded.codec);
        if (out.ok && out.error.empty() && !decoded.warning.empty())
            out.error = decoded.warning;
        if (out.ok && decoded.declaredFrames > 0) {
            const auto got = static_cast<std::int64_t>(out.audio.frames());
            // A whole second short of what the container declares is a damaged or
            // truncated file, not codec padding; say so and keep what decoded.
            // The count outranks whatever the decoder remarked on the way.
            if (decoded.declaredFrames - got > static_cast<std::int64_t>(out.sampleRate))
                out.error = "the file declares " + std::to_string(decoded.declaredFrames) + " frames but only "
                          + std::to_string(got) + " decoded; it may be truncated or damaged";
        }
        return out;
    }

    if (info.channels <= 0 || info.samplerate <= 0) {
        out.error = "file reports no channels or no sample rate";
        return out;
    }
    if (info.frames < 0) {
        out.error = "file reports a negative frame count";
        return out;
    }

    const auto channels = static_cast<std::uint32_t>(info.channels);
    const auto frames = static_cast<std::size_t>(info.frames);

    if (frames > std::numeric_limits<std::size_t>::max() / sizeof(float) / channels)
        throw std::length_error("audio file exceeds addressable storage");
    std::shared_ptr<session::DiskAudio> backing;
    if (frames * channels * sizeof(float) >= session::AudioBuffer::kDiskThreshold)
        backing = session::DiskAudio::create(channels, frames);
    else out.audio = session::AudioBuffer(channels, frames);
    out.sampleRate = static_cast<std::uint32_t>(info.samplerate);
    out.channels = channels;
    out.formatName = describeFormat(info.format & SF_FORMAT_TYPEMASK);
    out.metadata = readMetadata(file.get(), path);

    // libsndfile only hands back interleaved data, so it is read in blocks and fanned
    // out into planes. The block is sized to stay comfortably inside L2 rather than
    // to minimise call count.
    constexpr std::size_t kBlockFrames = 8192;
    std::vector<float> block(kBlockFrames * channels);
    std::vector<float> planar(backing ? kBlockFrames : 0);

    std::size_t done = 0;
    while (done < frames) {
        if (cancelled()) return out;
        const auto want = static_cast<sf_count_t>(
            std::min(kBlockFrames, frames - done));
        const sf_count_t got = sf_readf_float(file.get(), block.data(), want);
        if (got <= 0)
            break;

        const auto gotFrames = static_cast<std::size_t>(got);
        for (std::uint32_t c = 0; c < channels; ++c) {
            const std::span<float> plane = backing ? std::span<float>(planar).first(gotFrames)
                                                   : out.audio.plane(c).subspan(done, gotFrames);
            for (std::size_t i = 0; i < gotFrames; ++i)
                plane[i] = block[i * channels + c];
            if (backing) backing->writePlane(c, done, plane);
        }
        done += gotFrames;
        if (progress) progress(static_cast<float>(done) / static_cast<float>(frames));
    }
    if (cancelled()) return out;

    if (backing && done) {
        backing->seal(done);
        out.audio = session::AudioBuffer::fromDisk(std::move(backing));
    }
    if (done != frames) {
        // Truncated file: keep what was decoded rather than throwing it away, but say
        // so, because silently shortening someone's take is worse than an error.
        if (!out.audio.diskStorage()) out.audio.resize(channels, done);
        out.error = "file ended early: expected " + std::to_string(frames)
                  + " frames, read " + std::to_string(done);
        out.ok = done > 0;
        return out;
    }

    out.ok = true;
    return out;
    } catch (const std::exception& e) {
        out.audio = {}; out.ok = false; out.error = e.what(); return out;
    }
}

std::string writeFloatWav(const std::string& path, const session::AudioBuffer& audio,
                          std::uint32_t sampleRate, const std::atomic<bool>* cancel,
                          const std::function<void(float)>& progress)
{
    if (audio.channels() == 0)
        return "refusing to write a file with no channels";
    if (sampleRate == 0)
        return "refusing to write a file with no sample rate";

    SF_INFO info{};
    info.samplerate = static_cast<int>(sampleRate);
    info.channels = static_cast<int>(audio.channels());
    // RF64 keeps long-session sources valid beyond RIFF's 32-bit chunk length.
    const bool rf64 = audio.frames() > (0xffffffffull - 4096) / (sizeof(float) * audio.channels());
    info.format = (rf64 ? SF_FORMAT_RF64 : SF_FORMAT_WAV) | SF_FORMAT_FLOAT;

    if (sf_format_check(&info) == 0)
        return "libsndfile rejected float32 WAV at " + std::to_string(sampleRate) + " Hz";

    SndFile file(sf_open(path.c_str(), SFM_WRITE, &info));
    if (!file.valid())
        return std::string("could not write ") + path + ": " + sf_strerror(nullptr);

    // Interleave in blocks rather than all at once: a 30-minute stereo take would
    // otherwise need a second 691 MB buffer alongside the one already in memory.
    constexpr std::size_t kBlockFrames = 8192;
    const std::uint32_t channels = audio.channels();
    std::vector<float> block(kBlockFrames * channels);

    std::size_t done = 0;
    while (done < audio.frames()) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return "audio write cancelled";
        const std::size_t want = std::min(kBlockFrames, audio.frames() - done);
        for (std::uint32_t c = 0; c < channels; ++c) {
            const std::span<const float> plane = audio.plane(c);
            for (std::size_t i = 0; i < want; ++i)
                block[i * channels + c] = plane[done + i];
        }

        const sf_count_t wrote =
            sf_writef_float(file.get(), block.data(), static_cast<sf_count_t>(want));
        if (wrote != static_cast<sf_count_t>(want)) {
            return std::string("short write to ") + path + ": " + sf_strerror(file.get());
        }
        done += want;
        if (progress) progress(static_cast<float>(double(done) / double(audio.frames())));
        if (audio.diskStorage()) audio.diskStorage()->releaseMappedPages();
    }

    // Closing finishes the WAV header and can report delayed write failures.
    const int closed = file.close();
    if (closed != 0)
        return "could not close " + path + ": " + sf_error_number(closed);
    return {};
}

} // namespace omawave::io
