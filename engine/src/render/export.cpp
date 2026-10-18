#include "export.h"
#include "audio_store.h"

#include "io/resampler.h"
#include "io/ffmpeg_audio.h"
#include "io/metadata.h"

#include <sndfile.h>

#include <algorithm>
#include <iterator>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <span>
#include <system_error>
#include <memory>
#include <fcntl.h>
#include <unistd.h>

namespace omawave::render {

namespace {

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
        const int result = file_ ? sf_close(file_) : 0;
        file_ = nullptr; return result;
    }

private:
    SNDFILE* file_ = nullptr;
};

// Keep the previous destination intact until a complete encoder output is ready.
class ExportDestination {
public:
    explicit ExportDestination(const std::string& target) : target_(std::filesystem::absolute(target)) {
        auto pattern=(target_.parent_path()/".omaaudio-export-XXXXXX").string();
        if(!::mkdtemp(pattern.data()))throw std::system_error(errno,std::generic_category(),"create export staging directory");
        directory_=pattern;file_=(directory_/"encoded-audio").string();
    }
    ~ExportDestination(){std::error_code ec;std::filesystem::remove_all(directory_,ec);}
    const std::string& file() const {return file_;}
    bool install(bool replaceExisting, std::string& error) {
        const int fd=::open(file_.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
        if(fd<0){error="cannot open completed export";return false;}
        int synced;do{synced=::fsync(fd);}while(synced!=0 && errno==EINTR);
        const int closed=::close(fd);
        if(synced!=0 || closed!=0){error="cannot sync completed export";return false;}
        if (!replaceExisting) {
            // A same-filesystem hard link installs the completed inode only if
            // the name is absent. Unlike an existence check followed by rename,
            // a concurrent writer or a symlink can never be replaced here.
            if (::link(file_.c_str(), target_.c_str()) != 0) {
                error = "cannot install completed export without replacing an existing file: "
                    + std::error_code(errno, std::generic_category()).message();
                return false;
            }
            return true; // the private staging link is removed by the destructor
        }
        std::error_code ec;std::filesystem::rename(file_,target_,ec);
        if(ec){error="cannot install completed export: "+ec.message();return false;}
        return true;
    }
private:
    std::filesystem::path target_,directory_;
    std::string file_;
};

// What each container can hold. The fixed lists are the codecs' own: Opus runs at
// 48 kHz and its divisors, MPEG layer III at the MPEG-1/2/2.5 rates, FFmpeg's AAC
// encoder at the standard rates up to 96 kHz. The ranges are what libsndfile
// writes for the uncompressed and FLAC containers, capped at the 192 kHz this
// application offers anywhere.
constexpr std::uint32_t kAnyRateMin = 8000;
constexpr std::uint32_t kAnyRateMax = 192000;
constexpr std::uint32_t kOpusRates[] = {8000, 12000, 16000, 24000, 48000};
constexpr std::uint32_t kMp3Rates[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
constexpr std::uint32_t kAacRates[] = {7350, 8000, 11025, 12000, 16000, 22050, 24000, 32000,
                                       44100, 48000, 64000, 88200, 96000};

struct ContainerDesc {
    Container container;
    const char* id;
    const char* name;
    const char* extension;
    bool lossy;
    int majorFormat;
    const std::uint32_t* fixedRates;    // nullptr: any rate in [minRate, maxRate]
    std::size_t fixedCount;
    std::uint32_t minRate;
    std::uint32_t maxRate;
    std::uint32_t maxChannels;
};

constexpr ContainerDesc kContainers[] = {
    {Container::Wav, "wav", "WAV", "wav", false, SF_FORMAT_WAV, nullptr, 0, kAnyRateMin, kAnyRateMax, 8},
    {Container::Aiff, "aiff", "AIFF", "aiff", false, SF_FORMAT_AIFF, nullptr, 0, kAnyRateMin, kAnyRateMax, 8},
    {Container::Flac, "flac", "FLAC", "flac", false, SF_FORMAT_FLAC, nullptr, 0, kAnyRateMin, kAnyRateMax, 8},
    {Container::OggVorbis, "ogg", "Ogg Vorbis", "ogg", true, SF_FORMAT_OGG | SF_FORMAT_VORBIS, nullptr, 0, kAnyRateMin, kAnyRateMax, 8},
    {Container::Opus, "opus", "Opus", "opus", true, SF_FORMAT_OGG | SF_FORMAT_OPUS, kOpusRates, std::size(kOpusRates), 8000, 48000, 8},
    {Container::Aac, "aac", "AAC (M4A)", "m4a", true, 0, kAacRates, std::size(kAacRates), 7350, 96000, 2},
    {Container::Mp3, "mp3", "MP3", "mp3", true, SF_FORMAT_MPEG | SF_FORMAT_MPEG_LAYER_III, kMp3Rates, std::size(kMp3Rates), 8000, 48000, 2},
    {Container::Bwf, "bwf", "BWF (WAV + broadcast info)", "wav", false, SF_FORMAT_WAV, nullptr, 0, kAnyRateMin, kAnyRateMax, 8},
    {Container::Rf64, "rf64", "RF64 (large WAV)", "wav", false, SF_FORMAT_RF64, nullptr, 0, kAnyRateMin, kAnyRateMax, 8},
};

bool wavFamily(Container c) noexcept { return c == Container::Wav || c == Container::Bwf || c == Container::Rf64; }

// "8, 12, 16, 24 or 48 kHz", for a reason a person reads.
std::string rateList(const ContainerDesc& d)
{
    std::string out;
    for (std::size_t i = 0; i < d.fixedCount; ++i) {
        if (i > 0) out += (i + 1 == d.fixedCount) ? " or " : ", ";
        const std::uint32_t r = d.fixedRates[i];
        if (r % 1000 == 0) out += std::to_string(r / 1000);
        else {
            char buf[16];
            std::snprintf(buf, sizeof buf, "%.3g", r / 1000.0);
            out += buf;
        }
    }
    return out + " kHz";
}

bool rateSupported(const ContainerDesc& d, std::uint32_t rate) noexcept
{
    if (d.fixedRates != nullptr)
        return std::find(d.fixedRates, d.fixedRates + d.fixedCount, rate) != d.fixedRates + d.fixedCount;
    return rate >= d.minRate && rate <= d.maxRate;
}

// The nearest rate the container takes: the highest supported rate at or below
// the request, else the lowest above it -- so an over-ambitious choice loses the
// least and an impossible low one still works.
std::uint32_t nearestRate(const ContainerDesc& d, std::uint32_t rate) noexcept
{
    if (d.fixedRates == nullptr)
        return std::clamp(rate, d.minRate, d.maxRate);
    std::uint32_t best = 0;
    for (std::size_t i = 0; i < d.fixedCount; ++i)
        if (d.fixedRates[i] <= rate) best = std::max(best, d.fixedRates[i]);
    if (best != 0) return best;
    best = d.fixedRates[0];
    for (std::size_t i = 0; i < d.fixedCount; ++i) best = std::min(best, d.fixedRates[i]);
    return best;
}

const ContainerDesc& describe(Container container) noexcept
{
    for (const ContainerDesc& d : kContainers) {
        if (d.container == container)
            return d;
    }
    return kContainers[0];
}

int subtypeOf(SampleFormat format) noexcept
{
    switch (format) {
    case SampleFormat::Pcm16: return SF_FORMAT_PCM_16;
    case SampleFormat::Pcm24: return SF_FORMAT_PCM_24;
    case SampleFormat::Float32: return SF_FORMAT_FLOAT;
    }
    return SF_FORMAT_PCM_24;
}

// The full libsndfile format word for a spec, or 0 when this build cannot write it.
int formatWord(Container container, SampleFormat sampleFormat) noexcept
{
    const ContainerDesc& d = describe(container);
    if (d.majorFormat == 0)
        return 0;
    if (d.lossy)
        return d.majorFormat;       // the codec decides the sample format
    return d.majorFormat | subtypeOf(sampleFormat);
}

bool canWrite(int format, std::uint32_t rate, std::uint32_t channels) noexcept
{
    if (format == 0)
        return false;
    SF_INFO info{};
    info.samplerate = static_cast<int>(rate);
    info.channels = static_cast<int>(channels);
    info.format = format;
    return sf_format_check(&info) != 0;
}

// Triangular dither, +-1 LSB at the target depth. A tiny xorshift is all the
// randomness a dither needs, and it is deterministic, which a test appreciates.
class Tpdf {
public:
    explicit Tpdf(float lsb) noexcept : lsb_(lsb) {}

    [[nodiscard]] float next() noexcept
    {
        return (uniform() - uniform()) * lsb_;
    }

private:
    [[nodiscard]] float uniform() noexcept
    {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return static_cast<float>(state_ & 0xFFFFFFu) / 16777216.0f;
    }

    std::uint32_t state_ = 0x9E3779B9u;
    float lsb_;
};

} // namespace

std::vector<FormatInfo> availableFormats()
{
    std::vector<FormatInfo> out;
    for (const ContainerDesc& d : kContainers) {
        FormatInfo info;
        info.container = d.container;
        info.id = d.id;
        info.name = d.name;
        info.extension = d.extension;
        info.lossy = d.lossy;
        if (d.container == Container::Aac) {
            const auto support=io::aacSupport();info.available=support.available;info.reason=support.reason;
        } else if (d.lossy) {
            // Opus only speaks 48 kHz (and its divisors); probe at that.
            const std::uint32_t rate = d.container == Container::Opus ? 48000 : 44100;
            info.available = canWrite(formatWord(d.container, SampleFormat::Pcm16), rate, 2);
        } else {
            for (const SampleFormat f : {SampleFormat::Pcm16, SampleFormat::Pcm24,
                                         SampleFormat::Float32}) {
                if (canWrite(formatWord(d.container, f), 48000, 2))
                    info.sampleFormats.push_back(f);
            }
            info.available = !info.sampleFormats.empty();
        }
        if (!info.available && info.reason.empty())
            info.reason = "The installed libsndfile cannot encode this format.";
        if (d.fixedCount) info.sampleRates.assign(d.fixedRates, d.fixedRates + d.fixedCount);
        info.minSampleRate = d.minRate;
        info.maxSampleRate = d.maxRate;
        info.maxChannels = d.maxChannels;
        out.push_back(std::move(info));
    }
    return out;
}

SpecCheck checkSpec(const ExportSpec& spec, std::uint32_t mixRate, std::uint32_t mixChannels)
{
    SpecCheck c;
    const ContainerDesc& d = describe(spec.container);
    const std::uint32_t rate = spec.sampleRate == 0 ? mixRate : spec.sampleRate;
    const std::uint32_t channels = spec.channels == 0 ? mixChannels : spec.channels;
    const auto refuse = [&](std::string reason) { c.ok = false; c.reason = std::move(reason); };

    if (mixChannels != 0 && (channels == 0 || channels > mixChannels)) {
        refuse("the mix has " + std::to_string(mixChannels) + " channels; cannot deliver "
               + std::to_string(channels));
        c.suggestedChannels = mixChannels;
        return c;
    }
    if (channels > d.maxChannels) {
        refuse(std::string(d.name) + " holds at most " + std::to_string(d.maxChannels)
               + " channels; choose " + (d.maxChannels == 1 ? "mono" : "mono or stereo"));
        c.suggestedChannels = d.maxChannels;
        return c;
    }
    if (!rateSupported(d, rate)) {
        c.suggestedSampleRate = nearestRate(d, rate);
        if (spec.container == Container::Opus)
            refuse("Opus takes 8, 12, 16, 24 or 48 kHz; " + std::to_string(rate) + " Hz is not one");
        else if (d.fixedRates != nullptr)
            refuse(std::string(d.name) + " takes " + rateList(d) + "; " + std::to_string(rate)
                   + " Hz is not one");
        else
            refuse(std::string(d.name) + " is written from " + std::to_string(d.minRate / 1000)
                   + " to " + std::to_string(d.maxRate / 1000) + " kHz; " + std::to_string(rate)
                   + " Hz is outside that");
        return c;
    }
    if (spec.container == Container::Flac && spec.sampleFormat == SampleFormat::Float32) {
        refuse("FLAC is 16 or 24 bit; it has no float form");
        c.suggestedSampleFormat = SampleFormat::Pcm24;
        return c;
    }
    if (spec.container == Container::Aac) {
        if (spec.bitrateKbps != 0 && (spec.bitrateKbps < 64 || spec.bitrateKbps > 320)) {
            refuse("AAC bitrate must be 64–320 kb/s, or automatic");
            c.suggestedBitrateKbps = 0;
            return c;
        }
        if (spec.bitrateKbps > 6ull * rate * channels / 1000) {
            refuse("AAC bitrate exceeds the selected rate's capacity; choose Automatic or a higher sample rate");
            c.suggestedBitrateKbps = 0;
            return c;
        }
        const auto support = io::aacSupport();
        if (!support.available) { refuse(support.reason); return c; }
        return c;
    }
    const int format = formatWord(spec.container, spec.sampleFormat);
    if (!canWrite(format, rate, channels)) {
        refuse(std::string(d.name) + " cannot be written by this libsndfile"
               + (d.lossy ? "" : " at " + std::to_string(bitsOf(spec.sampleFormat)) + " bit")
               + ", " + std::to_string(rate) + " Hz, " + std::to_string(channels) + " channels");
        if (!d.lossy && spec.sampleFormat == SampleFormat::Float32) c.suggestedSampleFormat = SampleFormat::Pcm24;
    }
    return c;
}

Container containerForSize(Container requested, std::uint64_t frames, std::uint32_t channels,
                           SampleFormat format) noexcept
{
    if (requested != Container::Wav && requested != Container::Bwf) return requested;
    const std::uint64_t bytes = frames * channels * static_cast<std::uint64_t>(bitsOf(format) / 8);
    // The RIFF size field is 32-bit; keep a margin for the header chunks.
    constexpr std::uint64_t limit = (std::uint64_t{4} << 30) - (1u << 20);
    return bytes >= limit ? Container::Rf64 : requested;
}

std::optional<Container> containerFromId(std::string_view id) noexcept
{
    for (const ContainerDesc& d : kContainers) {
        if (id == d.id)
            return d.container;
    }
    return std::nullopt;
}

std::string_view containerId(Container container) noexcept
{
    return describe(container).id;
}

std::string_view containerExtension(Container container) noexcept
{
    return describe(container).extension;
}

std::optional<SampleFormat> sampleFormatFromBits(int bits) noexcept
{
    switch (bits) {
    case 16: return SampleFormat::Pcm16;
    case 24: return SampleFormat::Pcm24;
    case 32: return SampleFormat::Float32;
    default: return std::nullopt;
    }
}

int bitsOf(SampleFormat format) noexcept
{
    switch (format) {
    case SampleFormat::Pcm16: return 16;
    case SampleFormat::Pcm24: return 24;
    case SampleFormat::Float32: return 32;
    }
    return 24;
}

void applyGain(session::AudioBuffer& audio, float linear)
{
    if (linear == 1.0f)
        return;
    for (std::uint32_t c = 0; c < audio.channels(); ++c) {
        for (float& s : audio.plane(c))
            s *= linear;
    }
}

session::AudioBuffer foldToMono(const session::AudioBuffer& audio)
{
    if (audio.channels() <= 1)
        return audio;
    session::AudioBuffer out(1, audio.frames());
    const float scale = 1.0f / static_cast<float>(audio.channels());
    const std::span<float> dst = out.plane(0);
    for (std::uint32_t c = 0; c < audio.channels(); ++c) {
        const std::span<const float> src = audio.plane(c);
        for (std::size_t i = 0; i < audio.frames(); ++i)
            dst[i] += src[i] * scale;
    }
    return out;
}

ExportResult exportMix(const session::AudioBuffer& mix, std::uint32_t mixRate,
                       const ExportSpec& spec, const std::atomic<bool>* cancel,
                       const ProgressFn& progress)
{
    ExportResult out;
    out.path = spec.path;

    if (mix.empty()) {
        out.error = "nothing to export: the mix is empty";
        return out;
    }
    if (cancel && cancel->load(std::memory_order_relaxed)) { out.cancelled=true;out.error="export cancelled";return out; }
    if (spec.path.empty()) {
        out.error = "no output path";
        return out;
    }

    const std::uint32_t rate = spec.sampleRate == 0 ? mixRate : spec.sampleRate;
    const std::uint32_t channels = spec.channels == 0 ? mix.channels() : spec.channels;
    const ContainerDesc& desc = describe(spec.container);
    // One capability model decides what can be written; the writer never second-guesses it.
    if (const SpecCheck check = checkSpec(spec, mixRate, mix.channels()); !check.ok) {
        out.error = check.reason;
        return out;
    }
    const bool aac = spec.container == Container::Aac;
    const int format = formatWord(spec.container, spec.sampleFormat);

    const auto report = [&](float f) {
        if (progress)
            progress(f);
    };
    const auto cancelled = [&] {
        return cancel != nullptr && cancel->load(std::memory_order_relaxed);
    };

    // A mono delivery is measured and normalised as the fold it will be: the
    // sum of two channels is not as loud as the pair, and its peak is not theirs.
    session::AudioBuffer folded;
    if (channels == 1 && mix.channels() > 1) {
        try {
            AudioStore output(1, mix.frames());
            std::array<float, AudioStore::blockFrames> block;
            const float scale = 1.0f / mix.channels();
            for (std::size_t first = 0; first < mix.frames(); first += AudioStore::blockFrames) {
                if (cancelled()) { out.cancelled = true; out.error = "export cancelled"; return out; }
                const auto count = std::min(AudioStore::blockFrames, mix.frames() - first);
                std::fill_n(block.begin(), count, 0);
                for (std::uint32_t source = 0; source < mix.channels(); ++source) {
                    const auto input = mix.plane(source);
                    for (std::size_t f = 0; f < count; ++f) block[f] += input[first + f] * scale;
                }
                output.write(0, first, std::span<const float>(block).first(count));
                if (mix.diskStorage()) mix.diskStorage()->releaseMappedPages();
            }
            folded = output.finish();
        } catch (const std::exception& e) {
            out.cancelled = cancelled();
            out.error = out.cancelled ? "export cancelled" : e.what();
            return out;
        }
    }
    const session::AudioBuffer& mixed = folded.empty() ? mix : folded;

    bool finite = true;
    const auto measure = [&](const session::AudioBuffer& audio, std::uint32_t sampleRate,
                             float start, float end) {
        engine::LoudnessMeter meter(sampleRate);
        std::array<const float*, engine::kLoudnessChannels> planes{};
        const auto measuredChannels = std::min(audio.channels(), engine::kLoudnessChannels);
        for (std::size_t first = 0; first < audio.frames();) {
            if (cancelled()) break;
            const auto count = std::min(AudioStore::blockFrames, audio.frames() - first);
            for (std::uint32_t c = 0; c < measuredChannels; ++c) planes[c] = audio.plane(c).data() + first;
            // Every channel, not only the ones the meter takes.
            for (std::uint32_t c = 0; c < audio.channels() && finite; ++c) {
                const float* samples = audio.plane(c).data() + first;
                for (std::size_t f = 0; f < count; ++f) if (!std::isfinite(samples[f])) { finite = false; break; }
            }
            meter.process(planes.data(), measuredChannels, static_cast<std::uint32_t>(count));
            first += count;
            if (audio.diskStorage()) audio.diskStorage()->releaseMappedPages();
            report(start + (end - start) * static_cast<float>(first) / audio.frames());
        }
        return meter.report();
    };

    // Measure the immutable render in blocks; the effects ran exactly once.
    out.report.measured = measure(mixed, mixRate, 0, 0.25f);
    report(0.25f);
    if (!finite) {
        // One NaN from an effect would otherwise be written out, or become the gain.
        out.error = "the mix contains a non-finite sample; check the source audio and active effects";
        return out;
    }
    if (cancelled()) {
        out.cancelled = true;
        out.error = "export cancelled";
        return out;
    }

    // Resampling changes both loudness and intersample peaks. Keep the original
    // measurement in the report, but choose gain from the delivery-rate audio.
    session::AudioBuffer converted;
    const session::AudioBuffer* work = &mixed;
    auto normaliseMeasurement = out.report.measured;
    if (rate != mixRate) {
        try {
            converted = io::Resampler(mixRate, rate).process(mixed, cancel);
            work = &converted;
            if (spec.normalise != Normalise::None)
                normaliseMeasurement = measure(*work, rate, 0.25f, 0.35f);
        } catch (const std::exception& e) {
            out.cancelled = cancelled();
            out.error = out.cancelled ? "export cancelled" : e.what();
            return out;
        }
    }
    report(0.35f);
    if (cancelled()) { out.cancelled = true; out.error = "export cancelled"; return out; }
    if (!finite) { out.error = "sample-rate conversion produced a non-finite sample"; return out; }

    // --- normalise -----------------------------------------------------------------
    float gainDb = 0.0f;
    switch (spec.normalise) {
    case Normalise::None:
        break;
    case Normalise::Peak:
        if (normaliseMeasurement.truePeak <= engine::kLoudnessSilence + 1.0f) {
            out.error = "the mixed is silent; there is no peak to normalise";
            return out;
        }
        gainDb = spec.peakTargetDb - normaliseMeasurement.truePeak;
        break;
    case Normalise::Loudness:
        if (normaliseMeasurement.integrated <= engine::kLoudnessSilence + 1.0f) {
            out.error = "the mixed is silent; there is no loudness to normalise";
            return out;
        }
        gainDb = spec.targetLufs - normaliseMeasurement.integrated;
        if (normaliseMeasurement.truePeak + gainDb > spec.ceilingDbTp) {
            gainDb = spec.ceilingDbTp - normaliseMeasurement.truePeak;
            out.report.ceilingLimited = true;
            out.warning = "held at the " + std::to_string(spec.ceilingDbTp).substr(0, 5)
                        + " dBTP ceiling, short of the loudness target";
        }
        break;
    }
    out.report.appliedGainDb = gainDb;

    try {
        const auto& prepared = *work;
        const float linear = engine::dbToLinear(gainDb);
        if (channels != prepared.channels() || linear != 1) {
            AudioStore output(channels, prepared.frames());
            std::array<float, AudioStore::blockFrames> block;
            for (std::size_t first = 0; first < prepared.frames(); first += AudioStore::blockFrames) {
                if (cancelled()) { out.cancelled = true; out.error = "export cancelled"; return out; }
                const auto count = std::min(AudioStore::blockFrames, prepared.frames() - first);
                for (std::uint32_t c = 0; c < channels; ++c) {
                    if (channels == 1 && prepared.channels() > 1) {
                        std::fill_n(block.begin(), count, 0);
                        const float scale = 1.0f / prepared.channels();
                        for (std::uint32_t source = 0; source < prepared.channels(); ++source) {
                            const auto input = prepared.plane(source);
                            for (std::size_t f = 0; f < count; ++f) block[f] += input[first + f] * scale;
                        }
                    } else {
                        std::copy_n(prepared.plane(c).data() + first, count, block.data());
                    }
                    if (linear != 1) for (std::size_t f = 0; f < count; ++f) block[f] *= linear;
                    output.write(c, first, std::span<const float>(block).first(count));
                }
                if (prepared.diskStorage()) prepared.diskStorage()->releaseMappedPages();
                report(0.35f + 0.15f * static_cast<float>(first + count) / prepared.frames());
            }
            converted = output.finish();
            work = &converted;
        }
    } catch (const std::exception& e) {
        out.cancelled = cancelled();
        out.error = out.cancelled ? "export cancelled" : e.what();
        return out;
    }
    report(0.5f);
    if (cancelled()) {
        out.cancelled = true;
        out.error = "export cancelled";
        return out;
    }

    out.report.delivered = measure(*work, rate, 0.5f, 0.6f);
    out.report.frames = static_cast<std::int64_t>(work->frames());
    out.report.sampleRate = rate;
    out.report.channels = channels;
    report(0.6f);
    if (cancelled()) { out.cancelled = true; out.error = "export cancelled"; return out; }
    if (!finite) { out.error = "audio conversion produced a non-finite sample"; return out; }

    // --- write ---------------------------------------------------------------------
    std::unique_ptr<ExportDestination> destination;
    try { destination=std::make_unique<ExportDestination>(spec.path); }
    catch(const std::exception& e){out.error=e.what();return out;}
    if (aac) {
        const auto encoded=io::encodeAac(*work,rate,spec.bitrateKbps?spec.bitrateKbps:std::min(unsigned(channels==1?128:256),unsigned(6ull*rate*channels/1000)),
            destination->file(),cancel,[&](float fraction){report(0.6f+0.39f*fraction);});
        if(!encoded.ok || cancelled()) {out.cancelled=encoded.cancelled || cancelled();out.error=encoded.error;return out;}
        if(!destination->install(spec.replaceExisting,out.error))return out;
        if(!out.warning.empty())out.warning+="; ";
        out.warning+="AAC is lossy: decoded peaks can change and playback duration can include codec padding.";
        out.ok=true;report(1);return out;
    }
    // A WAV that would pass 4 GiB is written as RF64 instead, and says so.
    const Container written = containerForSize(spec.container, work->frames(), channels, spec.sampleFormat);
    const int writtenFormat = written == spec.container ? format : formatWord(written, spec.sampleFormat);
    if (written != spec.container) {
        if (!out.warning.empty()) out.warning += "; ";
        out.warning += "written as RF64: the data exceeds what a WAV can hold (4 GiB)";
    }
    out.report.container = std::string(describe(written).id);

    SF_INFO info{};
    info.samplerate = static_cast<int>(rate);
    info.channels = static_cast<int>(channels);
    info.format = writtenFormat;

    SndFile file(sf_open(destination->file().c_str(), SFM_WRITE, &info));
    if (!file.valid()) {
        out.error = "could not write " + spec.path + ": " + sf_strerror(nullptr);
        return out;
    }
    if (written == Container::Rf64) {
        // RF64 chosen is RF64 written, whatever the size.
        sf_command(file.get(), SFC_RF64_AUTO_DOWNGRADE, nullptr, SF_FALSE);
    }
    std::vector<session::FileMarker> markersToAppend;
    if (wavFamily(written) && (spec.writeMetadata || spec.writeMarkers || written == Container::Bwf)) {
        session::AudioMetadata meta;
        if (spec.writeMetadata || written == Container::Bwf) {
            meta.bext = spec.metadata.bext;
            meta.bext.present = true;
            meta.ixml = spec.metadata.ixml;
            meta.strings = spec.metadata.strings;
            // The delivered measurement fills the version-2 loudness fields unless the
            // caller set them; a value of silence stays unset.
            const engine::LoudnessReport& d = out.report.delivered;
            if (!meta.bext.loudnessValue && d.integrated > -100.0f) meta.bext.loudnessValue = d.integrated;
            if (!meta.bext.loudnessRange && d.integrated > -100.0f) meta.bext.loudnessRange = d.range;
            if (!meta.bext.maxTruePeak && d.truePeak > -100.0f) meta.bext.maxTruePeak = d.truePeak;
            if (!meta.bext.maxMomentaryLoudness && d.momentaryMax > -100.0f) meta.bext.maxMomentaryLoudness = d.momentaryMax;
            if (!meta.bext.maxShortTermLoudness && d.shortTermMax > -100.0f) meta.bext.maxShortTermLoudness = d.shortTermMax;
            if (meta.bext.originationDate.empty()) meta.bext.originationDate = io::todayForBext();
            if (meta.bext.originationTime.empty()) meta.bext.originationTime = io::nowForBext();
            meta.bext.codingHistory += io::codingHistoryLine(rate, bitsOf(spec.sampleFormat), channels, "OmaAudio");
            out.report.metadataWritten = true;
        }
        if (spec.writeMarkers) {
            // Appended as standard chunks once libsndfile has closed the file.
            markersToAppend = spec.metadata.markers;
            out.report.markersWritten = static_cast<std::uint32_t>(markersToAppend.size());
        }
        if (const std::string problem = io::writeMetadata(file.get(), meta); !problem.empty()) {
            out.error = problem;
            return out;
        }
    }

    // Anything over full scale clips rather than wrapping, which is libsndfile's
    // default for float-to-integer and is never what anyone means.
    sf_command(file.get(), SFC_SET_CLIPPING, nullptr, SF_TRUE);
    if (desc.lossy) {
        double quality = 0.6;
        sf_command(file.get(), SFC_SET_VBR_ENCODING_QUALITY, &quality, sizeof(quality));
#ifdef SFC_SET_BITRATE_MODE
        if (spec.container == Container::Mp3) {
            int mode = SF_BITRATE_MODE_VARIABLE;
            sf_command(file.get(), SFC_SET_BITRATE_MODE, &mode, sizeof(mode));
            double level = 0.25;    // lame's V2-ish: ~190 kb/s, transparent for speech
            sf_command(file.get(), SFC_SET_COMPRESSION_LEVEL, &level, sizeof(level));
        }
#endif
    }

    const bool dither = spec.dither && !desc.lossy && spec.sampleFormat != SampleFormat::Float32;
    Tpdf tpdf(spec.sampleFormat == SampleFormat::Pcm16 ? 1.0f / 32768.0f : 1.0f / 8388608.0f);

    constexpr std::size_t kBlockFrames = 8192;
    std::vector<float> block(kBlockFrames * channels);
    std::size_t done = 0;
    while (done < work->frames()) {
        if (cancelled()) {
            file.close();
            std::error_code ec;
            std::filesystem::remove(destination->file(), ec);
            out.cancelled = true;
            out.error = "export cancelled";
            return out;
        }
        const std::size_t want = std::min(kBlockFrames, work->frames() - done);
        for (std::uint32_t c = 0; c < channels; ++c) {
            const std::span<const float> plane = work->plane(c);
            for (std::size_t i = 0; i < want; ++i) {
                float s = plane[done + i];
                if (dither)
                    s += tpdf.next();
                block[i * channels + c] = s;
            }
        }
        const sf_count_t wrote =
            sf_writef_float(file.get(), block.data(), static_cast<sf_count_t>(want));
        if (wrote != static_cast<sf_count_t>(want)) {
            out.error = "short write to " + spec.path + ": " + sf_strerror(file.get());
            file.close();
            std::error_code ec;
            std::filesystem::remove(destination->file(), ec);
            return out;
        }
        done += want;
        if (work->diskStorage()) work->diskStorage()->releaseMappedPages();
        report(0.6f + 0.39f * static_cast<float>(done) / static_cast<float>(work->frames()));
    }

    const int closed=file.close();
    if(closed!=0){out.error=std::string("could not close export: ")+sf_error_number(closed);return out;}
    if(cancelled()){out.cancelled=true;out.error="export cancelled";return out;}
    if (!markersToAppend.empty()) {
        if (const std::string problem = io::appendMarkers(destination->file(), markersToAppend); !problem.empty()) {
            out.error = problem;
            std::error_code ec;
            std::filesystem::remove(destination->file(), ec);
            return out;
        }
    }
    if(!destination->install(spec.replaceExisting,out.error))return out;
    out.ok = true;
    report(1.0f);
    return out;
}

} // namespace omawave::render
