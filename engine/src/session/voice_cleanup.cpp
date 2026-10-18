#include "voice_cleanup.h"
#include "clip_reader.h"
#include "edits.h"
#include "clip_envelope.h"
#include "io/resampler.h"
#include <rnnoise.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>

namespace omawave::session {
namespace {
constexpr std::size_t blockFrames = 8192;
constexpr std::size_t modelFrame = 480;
// RNNoise 0.2 delays both its analysis window and its spectrum, one hop each.
constexpr std::size_t modelDelay = 2 * modelFrame;
struct Cancelled {};
void check(const std::atomic<bool>* cancel)
{
    if (cancel && cancel->load(std::memory_order_relaxed)) throw Cancelled{};
}

AudioBuffer renderInput(const Project& project, const Track& track, FramePos first,
                        FramePos last, unsigned channels, float& peak,
                        const std::atomic<bool>* cancel)
{
    auto disk = DiskAudio::create(channels, std::size_t(last - first));
    ClipReader reader(project);
    AudioBuffer block(channels, blockFrames);
    for (auto at = first; at < last;) {
        check(cancel);
        const auto count = std::size_t(std::min<FramePos>(blockFrames, last - at));
        for (unsigned c = 0; c < channels; ++c) std::fill_n(block.plane(c).data(), count, 0.0f);
        for (const auto& clip : track.clips) {
            const auto from = std::max(at, clip.timelineStart);
            const auto to = std::min(at + FramePos(count), clip.timelineEnd());
            for (auto f = from; f < to;) {
                check(cancel);
                const auto n = std::uint32_t(std::min<FramePos>(ClipReader::kBlock, to - f));
                reader.read(clip, f - clip.timelineStart, n, cancel);
                for (unsigned c = 0; c < channels; ++c) {
                    auto out = block.plane(c);
                    const auto in = reader.plane(c);
                    for (unsigned i = 0; i < n; ++i)
                        out[std::size_t(f - at) + i] = in[i] * clip.channelEnvelopeAt(f - clip.timelineStart + i,c,reader.channels());
                }
                f += n;
            }
        }
        for (unsigned c = 0; c < channels; ++c) {
            const auto samples = block.plane(c).first(count);
            for (const float sample : samples) {
                if (!std::isfinite(sample)) throw std::runtime_error("voice cleanup requires finite audio samples");
                peak = std::max(peak, std::abs(sample));
            }
            disk->writePlane(c, std::size_t(at - first), samples);
        }
        at += FramePos(count);
    }
    disk->seal();
    return AudioBuffer::fromDisk(std::move(disk));
}

AudioBuffer suppress(const AudioBuffer& input, float peak, const std::atomic<bool>* cancel)
{
    if (rnnoise_get_frame_size() != int(modelFrame))
        throw std::runtime_error("the installed RNNoise has an unsupported frame size");
    if (input.frames() > std::numeric_limits<std::size_t>::max() - modelDelay - modelFrame)
        throw std::length_error("voice cleanup exceeds addressable storage");
    auto disk = DiskAudio::create(input.channels(), input.frames());
    // Preserve float headroom without sending out-of-range values into the model.
    const double scale = 32768.0 / std::max(1.0, double(peak));
    for (unsigned c = 0; c < input.channels(); ++c) {
        std::unique_ptr<DenoiseState, decltype(&rnnoise_destroy)> state(rnnoise_create(nullptr), rnnoise_destroy);
        if (!state) throw std::runtime_error("RNNoise could not initialize its built-in voice model");
        std::array<float, modelFrame> in{}, out{};
        const auto source = input.plane(c);
        for (std::size_t at = 0; at < input.frames() + modelDelay; at += modelFrame) {
            check(cancel);
            for (std::size_t i = 0; i < modelFrame; ++i) {
                const double sample = at + i < source.size() ? double(source[at + i]) * scale : 0;
                if (!std::isfinite(sample) || std::abs(sample) > std::numeric_limits<float>::max())
                    throw std::runtime_error("voice cleanup encountered invalid resampled audio");
                in[i] = float(sample);
            }
            (void)rnnoise_process_frame(state.get(), out.data(), in.data());
            if (at >= modelDelay) {
                const auto first = at - modelDelay;
                const auto count = std::min(modelFrame, input.frames() - first);
                for (std::size_t i = 0; i < count; ++i) {
                    const double value = double(out[i]) / scale;
                    if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
                        throw std::runtime_error("RNNoise returned invalid audio");
                    out[i] = float(value);
                }
                disk->writePlane(c, first, std::span(out).first(count));
            }
            if ((at / modelFrame) % 64 == 0 && input.diskStorage()) input.diskStorage()->releaseMappedPages();
        }
    }
    disk->seal();
    return AudioBuffer::fromDisk(std::move(disk));
}
} // namespace

SampleEditResult cleanupVoice(Project snapshot, TrackId id, FramePos start,
                              FramePos end, float mix, const std::atomic<bool>* cancel)
{
    try {
        check(cancel);
        auto* track = snapshot.track(id);
        if (!track || track->clips.empty() || !edits::isWellFormed(track->clips)
            || start < 0 || end <= start || end > snapshot.length()
            || !snapshot.sampleRate() || !std::isfinite(mix) || mix < 0 || mix > 1)
            throw std::runtime_error("voice cleanup needs a valid selection and amount from 0 to 100%");
        unsigned channels = 0;
        for (const auto& clip : track->clips) {
            const auto* source = snapshot.source(clip.source);
            if (!source || !clip.validSource(FramePos(source->audio.frames())) || !clip.hasValidFades()
                || !std::isfinite(clip.gain) || source->audio.channels() < 1 || source->audio.channels() > 2)
                throw std::runtime_error("voice cleanup requires valid mono or stereo clips");
            if (channels && channels != source->audio.channels())
                throw std::runtime_error("convert this mixed-layout track to mono or stereo before voice cleanup");
            channels = source->audio.channels();
        }
        if (mix == 0) return {};
        const auto rate = snapshot.sampleRate();
        const auto first = std::max<FramePos>(0, start - rate / 5);
        const auto last = end + std::min<FramePos>(rate / 20, snapshot.length() - end);
        float peak = 0;
        const auto dry = renderInput(snapshot, *track, first, last, channels, peak, cancel);
        if (peak == 0) return {};
        const auto modelInput = io::Resampler(rate, 48000).process(dry, cancel);
        check(cancel);
        const auto modelOutput = suppress(modelInput, peak, cancel);
        const auto wet = io::Resampler(48000, rate).process(modelOutput, cancel);
        check(cancel);
        const auto frames = std::size_t(end - start), offset = std::size_t(start - first);
        auto result = DiskAudio::create(channels, frames);
        std::array<float, blockFrames> block{};
        const auto edge = std::min<std::size_t>(rate / 200, frames / 2);
        bool changed = false;
        for (unsigned c = 0; c < channels; ++c) {
            const auto original = dry.plane(c), cleaned = wet.plane(c);
            for (std::size_t at = 0; at < frames; at += blockFrames) {
                check(cancel);
                const auto count = std::min(blockFrames, frames - at);
                for (std::size_t i = 0; i < count; ++i) {
                    const auto f = at + i, source = offset + f;
                    const double ramp = edge ? std::min({1.0, double(f) / edge, double(frames - 1 - f) / edge}) : 1.0;
                    const double before = original[source];
                    const double after = source < cleaned.size() ? cleaned[source] : 0;
                    const double value = before + (after - before) * double(mix) * ramp;
                    if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
                        throw std::runtime_error("voice cleanup produced an invalid mixed sample");
                    block[i] = float(value);
                    changed = changed || block[i] != original[source];
                }
                result->writePlane(c, at, std::span(block).first(count));
                dry.diskStorage()->releaseMappedPages();
                if (wet.diskStorage()) wet.diskStorage()->releaseMappedPages();
            }
        }
        check(cancel);
        if (!changed) return {};
        result->seal();
        if (snapshot.sources().size() >= std::size_t(kInvalidSource))
            throw std::runtime_error("too many audio sources");
        const auto source = snapshot.addSource("Voice Cleanup", "", AudioBuffer::fromDisk(std::move(result)));
        edits::overwriteRendered(track->clips, Clip{.source = source, .timelineStart = start, .length = FramePos(frames)}, track->clips);
        if (const auto error = validateClipEnvelopes(snapshot); !error.empty()) throw std::runtime_error(error);
        return {std::move(snapshot), false, {}};
    } catch (const Cancelled&) {
        return {std::nullopt, true, {}};
    } catch (const std::exception& error) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return {std::nullopt, true, {}};
        return {std::nullopt, false, error.what()};
    }
}
} // namespace omawave::session
