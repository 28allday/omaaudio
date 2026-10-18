#pragma once

#include "engine/loudness.h"
#include "master_render.h"
#include "session/audio_buffer.h"
#include "session/metadata.h"

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace omawave::render {

// What a delivery is written as. Only what libsndfile on this box can encode is
// offered -- availableFormats() probes rather than promising (BRIEF.md section 10:
// "according to backend availability").
enum class Container : std::uint8_t {
    Wav,
    Aiff,
    Flac,
    OggVorbis,
    Opus,
    Mp3,
    Aac,
    Bwf,        // WAV with broadcast information (EBU Tech 3285)
    Rf64,       // the large-file WAV (EBU Tech 3306); never downgraded
};

enum class SampleFormat : std::uint8_t {
    Pcm16,
    Pcm24,
    Float32,
};

// How the level is set before writing. Peak puts the true peak at a target; Loudness
// puts the integrated loudness at a target and then, if that would push the true peak
// over the ceiling, backs off to the ceiling instead -- a gain, never a limiter, so
// the mix is never distorted by the exporter. A limiter belongs in the master chain.
enum class Normalise : std::uint8_t {
    None,
    Peak,
    Loudness,
};

struct FormatInfo {
    Container container = Container::Wav;
    std::string id;             // "wav", "flac", ... -- what a spec names
    std::string name;           // "WAV", "FLAC", "Ogg Vorbis", ...
    std::string extension;      // without the dot
    bool lossy = false;
    bool available = false;     // an installed encoder can write it
    std::string reason;
    std::vector<SampleFormat> sampleFormats;    // empty for a lossy container
    // What the container can hold, independent of what this box has installed.
    // A fixed list (Opus, MP3, AAC) or, when the list is empty, any rate in range.
    std::vector<std::uint32_t> sampleRates;
    std::uint32_t minSampleRate = 0;
    std::uint32_t maxSampleRate = 0;
    std::uint32_t maxChannels = 0;
};

[[nodiscard]] std::vector<FormatInfo> availableFormats();
[[nodiscard]] std::optional<Container> containerFromId(std::string_view id) noexcept;
[[nodiscard]] std::string_view containerId(Container container) noexcept;
// Static format metadata; does not probe installed codecs or run subprocesses.
[[nodiscard]] std::string_view containerExtension(Container container) noexcept;
[[nodiscard]] std::optional<SampleFormat> sampleFormatFromBits(int bits) noexcept;
[[nodiscard]] int bitsOf(SampleFormat format) noexcept;

struct ExportSpec {
    std::string path;
    bool replaceExisting = true;       // false: atomic refusal if any destination already exists
    Container container = Container::Wav;
    SampleFormat sampleFormat = SampleFormat::Pcm24;
    std::uint32_t sampleRate = 0;       // 0 = the mix's own
    std::uint32_t channels = 0;         // 0 = the mix's own; 1 folds to mono
    bool dither = true;                 // TPDF at the target depth, PCM only
    unsigned bitrateKbps = 0;           // AAC: 0 chooses128 mono/256 stereo; explicit64–320
    Normalise normalise = Normalise::None;
    float targetLufs = -16.0f;          // Loudness
    float ceilingDbTp = -1.0f;          // Loudness: never above this true peak
    float peakTargetDb = -0.3f;         // Peak
    // Written before the samples into WAV-family containers: the bext (always for
    // BWF, on request for WAV and RF64), markers as cue points, iXML and INFO.
    // Positions are in export-rate frames. bext loudness fields are filled from
    // the delivered measurement when the spec leaves them unset.
    session::AudioMetadata metadata;
    bool writeMetadata = false;         // bext, iXML, INFO
    bool writeMarkers = false;          // cue points
};

// A WAV or BWF whose data would pass 4 GiB cannot exist; it is written as RF64
// and the result says so. Explicit RF64 stays RF64. Pure, for the tests.
[[nodiscard]] Container containerForSize(Container requested, std::uint64_t frames,
                                         std::uint32_t channels, SampleFormat format) noexcept;

// One capability model for a delivery: whether a spec can be written as asked,
// why not in plain words, and the nearest choice that would work. Pure; no file
// is touched, no subprocess runs beyond the AAC availability probe.
struct SpecCheck {
    bool ok = true;
    std::string reason;
    std::optional<std::uint32_t> suggestedSampleRate;
    std::optional<std::uint32_t> suggestedChannels;
    std::optional<SampleFormat> suggestedSampleFormat;
    std::optional<unsigned> suggestedBitrateKbps;
};
[[nodiscard]] SpecCheck checkSpec(const ExportSpec& spec, std::uint32_t mixRate,
                                  std::uint32_t mixChannels);

struct ExportReport {
    engine::LoudnessReport measured;    // the mix as rendered, before any gain
    engine::LoudnessReport delivered;   // what went into the file, before dither
    float appliedGainDb = 0.0f;
    bool ceilingLimited = false;        // Loudness mode had to back off
    std::int64_t frames = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    std::string container;              // what was written: "wav", "bwf", "rf64", ...
    bool metadataWritten = false;
    std::uint32_t markersWritten = 0;
};

struct ExportResult {
    bool ok = false;
    bool cancelled = false;
    std::string error;
    std::string warning;
    ExportReport report;
    std::string path;
};

// Writes `mix` (at `mixRate`) to `spec.path`: fold, measure, resample, normalise,
// dither, encode. Measurement and conversion run in cancellable blocks; long
// intermediate audio uses private disk storage. A cancelled write removes the
// partial file and preserves any previous destination. Worker thread only.
[[nodiscard]] ExportResult exportMix(const session::AudioBuffer& mix, std::uint32_t mixRate,
                                     const ExportSpec& spec,
                                     const std::atomic<bool>* cancel = nullptr,
                                     const ProgressFn& progress = {});

// The pieces, for tests and for anyone who wants one without the file.
void applyGain(session::AudioBuffer& audio, float linear);
[[nodiscard]] session::AudioBuffer foldToMono(const session::AudioBuffer& audio);

} // namespace omawave::render
