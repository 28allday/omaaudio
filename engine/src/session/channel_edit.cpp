#include "channel_edit.h"
#include "edits.h"
#include "clip_envelope.h"
#include "render.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace omawave::session {
namespace {
struct Cancelled {};
void check(const std::atomic<bool>* cancel)
{
    if (cancel && cancel->load(std::memory_order_relaxed)) throw Cancelled{};
}
template<class Work> SampleEditResult run(Project project, const std::atomic<bool>* cancel, Work work)
{
    try {
        check(cancel);
        if (!work(project)) return {};
        check(cancel);
        if (const auto error = validateClipEnvelopes(project); !error.empty()) throw std::runtime_error(error);
        return {std::move(project), false, {}};
    } catch (const Cancelled&) { return {std::nullopt, true, {}}; }
    catch (const std::exception& e) { return {std::nullopt, false, e.what()}; }
}
unsigned layout(const Project& project, const std::vector<Clip>& clips, bool uniform = false)
{
    if (!edits::isWellFormed(clips)) throw std::runtime_error("invalid clip geometry");
    unsigned channels = 0;
    for (const auto& clip : clips) {
        const auto* source = project.source(clip.source);
        if (!source || source->audio.channels() < 1 || source->audio.channels() > 8
            || !clip.validSource(FramePos(source->audio.frames())))
            throw std::runtime_error("invalid audio source or channel layout");
        if (uniform && channels && channels != source->audio.channels())
            throw std::runtime_error("map mixed source layouts to consistent channels before this edit");
        channels = std::max(channels, source->audio.channels());
    }
    return std::max(1u, channels);
}
AudioBuffer renderRange(const Project& project, const std::vector<Clip>& clips, FramePos from,
                       FramePos to, unsigned channels, const std::atomic<bool>* cancel)
{
    if (from < 0 || to <= from || std::uint64_t(to - from) > std::numeric_limits<std::size_t>::max() / (sizeof(float) * channels))
        throw std::runtime_error("invalid audio edit range");
    auto out = renderClipRange(project,clips,from,to,channels,cancel);
    check(cancel);
    return out;
}
bool equal(const AudioBuffer& a, const AudioBuffer& b)
{
    if (a.channels() != b.channels() || a.frames() != b.frames()) return false;
    for (unsigned c = 0; c < a.channels(); ++c)
        if (!std::equal(a.plane(c).begin(), a.plane(c).end(), b.plane(c).begin())) return false;
    return true;
}
void layAudio(Project& project, TrackId id, FramePos start, AudioBuffer audio, bool insert,
              const std::string& name, const std::vector<Clip>& ownership, const std::atomic<bool>* cancel)
{
    const auto length = FramePos(audio.frames());
    audio.storeOnDisk(AudioBuffer::kDiskThreshold, cancel); check(cancel);
    const auto source = project.addSource(name, "", std::move(audio));
    Clip clip;
    clip.source = source; clip.length = length;
    auto& clips = project.track(id)->clips;
    if (insert) edits::insertAt(clips, {clip}, start);
    else edits::overwriteRendered(clips, Clip{.source = source, .timelineStart = start, .length = length}, ownership);
}
} // namespace

SampleEditResult editChannels(Project snapshot, TrackId id, const ChannelEditSpec& spec,
                              const std::atomic<bool>* cancel)
{
    return run(std::move(snapshot), cancel, [&](Project& project) {
        if (spec.newTrack) id = project.addTrack("Pasted audio");
        const auto* track = project.track(id);
        if (!track) throw std::runtime_error("choose a destination track");
        // A track with nothing on it has no layout of its own: a paste gives it
        // the clipboard's, exactly as a paste to a new track does.
        const bool pasting = spec.operation == ChannelEdit::Paste || spec.operation == ChannelEdit::MixPaste;
        const auto channels = spec.newTrack || (pasting && track->clips.empty())
            ? (spec.clipboardChannel >= 0 ? 1 : layout(project, spec.clipboard))
            : layout(project, track->clips, true);
        if (spec.channel < -1 || spec.channel >= int(channels)
            || !std::isfinite(spec.gain) || spec.gain < 0 || spec.gain > 1000
            || !std::isfinite(spec.incomingGain) || std::abs(spec.incomingGain) > 1000)
            throw std::runtime_error("invalid channel or mix gain");
        const bool paste = spec.operation == ChannelEdit::Paste || spec.operation == ChannelEdit::MixPaste;
        FramePos end = spec.end;
        if (paste) {
            if (spec.clipboardFrames <= 0 || spec.start < 0
                || spec.start > std::numeric_limits<FramePos>::max() - spec.clipboardFrames)
                throw std::runtime_error("invalid clipboard duration or paste position");
            end = spec.start + spec.clipboardFrames;
        } else if (end > track->length()) throw std::runtime_error("selection is past the end of the take");
        const bool insert = paste && spec.operation == ChannelEdit::Paste && spec.ripple && spec.channel < 0;
        if (insert && track->length() > std::numeric_limits<FramePos>::max() - spec.clipboardFrames)
            throw std::runtime_error("paste would exceed the timeline");
        auto original = renderRange(project, track->clips, spec.start, end, channels, cancel);
        auto audio = original;
        AudioBuffer incoming;
        if (paste) {
            const auto sourceChannels = layout(project, spec.clipboard);
            if (spec.clipboardChannel < -1 || spec.clipboardChannel >= int(sourceChannels))
                throw std::runtime_error("invalid clipboard channel");
            incoming = renderRange(project, spec.clipboard, 0, spec.clipboardFrames, sourceChannels, cancel);
            if (spec.clipboardChannel >= 0) {
                AudioBuffer mono(1, incoming.frames());
                std::copy(incoming.plane(unsigned(spec.clipboardChannel)).begin(), incoming.plane(unsigned(spec.clipboardChannel)).end(), mono.plane(0).begin());
                incoming = std::move(mono);
            }
            if (incoming.channels() > 1 && channels > 1 && incoming.channels() != channels)
                throw std::runtime_error("map the clipboard and destination to matching channel counts first");
        }
        float gain = spec.gain;
        if (spec.operation == ChannelEdit::Normalize) {
            float peak = 0;
            for (unsigned c = 0; c < channels; ++c) if (spec.channel < 0 || c == unsigned(spec.channel))
                for (float v : audio.plane(c)) peak = std::max(peak, std::abs(v));
            if (peak == 0) return false;
            gain /= peak;
        }
        for (unsigned c = 0; c < channels; ++c) {
            if (spec.channel >= 0 && c != unsigned(spec.channel)) continue;
            auto out = audio.plane(c); const auto before = original.plane(c);
            for (std::size_t i = 0; i < audio.frames(); ++i) {
                if ((i & 4095) == 0) check(cancel);
                switch (spec.operation) {
                case ChannelEdit::Gain: case ChannelEdit::Normalize: out[i] *= gain; break;
                case ChannelEdit::Silence: out[i] = 0; break;
                case ChannelEdit::FadeIn: out[i] *= float(i) / float(audio.frames()); break;
                case ChannelEdit::FadeOut: out[i] *= float(audio.frames() - i) / float(audio.frames()); break;
                case ChannelEdit::Reverse: out[i] = before[audio.frames() - 1 - i]; break;
                case ChannelEdit::Paste: case ChannelEdit::MixPaste: {
                    float value = 0;
                    if (channels == 1 && incoming.channels() > 1) {
                        for (unsigned k = 0; k < incoming.channels(); ++k) value += incoming.plane(k)[i] / incoming.channels();
                    } else value = incoming.plane(incoming.channels() == 1 ? 0 : c)[i];
                    out[i] = spec.incomingGain * value + (spec.operation == ChannelEdit::MixPaste ? spec.gain * before[i] : 0);
                    break;
                }
                }
            }
        }
        if (!insert && end <= track->length() && equal(audio, original)) return false;
        // A channel edit or mix keeps destination ownership. Replacing every
        // channel with unrelated pasted audio does not adopt its destination's
        // group. Whole-channel reverse moves ownership with the reversed audio;
        // partial-channel reverse keeps the other channels' timeline membership.
        auto ownership = track->clips;
        if (spec.operation == ChannelEdit::Paste && spec.channel < 0) ownership.clear();
        else if (spec.operation == ChannelEdit::Reverse && spec.channel < 0) {
            ownership = edits::copyRange(track->clips, spec.start, end);
            for (auto& clip : ownership) clip.timelineStart = spec.start + (end - spec.start - clip.timelineEnd());
            std::reverse(ownership.begin(), ownership.end());
        }
        layAudio(project, id, spec.start, std::move(audio), insert, track->name + " (channel edit)", ownership, cancel);
        return true;
    });
}

SampleEditResult mapTrackChannels(Project snapshot, TrackId id,
    const std::vector<std::vector<float>>& matrix, const std::atomic<bool>* cancel)
{
    return run(std::move(snapshot), cancel, [&](Project& project) {
        const auto* track = project.track(id);
        if (!track || track->clips.empty()) throw std::runtime_error("channel mapping needs a take");
        const auto inputs = layout(project, track->clips);
        if (matrix.empty() || matrix.size() > 8) throw std::runtime_error("choose 1 to 8 output channels");
        for (const auto& row : matrix)
            if (row.size() != inputs || std::any_of(row.begin(), row.end(), [](float g) { return !std::isfinite(g) || std::abs(g) > 4; }))
                throw std::runtime_error("channel matrix must match the input count and contain gains between -4 and 4");
        bool identity = matrix.size() == inputs;
        for (std::size_t o = 0; o < matrix.size(); ++o)
            for (unsigned i = 0; i < inputs; ++i) identity &= matrix[o][i] == (o == i ? 1 : 0);
        bool uniform = true;
        for (const auto& clip : track->clips) uniform &= project.source(clip.source)->audio.channels() == inputs;
        if (identity && uniform) return false;
        auto clips = track->clips;
        std::unordered_map<SourceId, SourceId> mapped;
        for (auto& clip : clips) {
            check(cancel);
            if (const auto found = mapped.find(clip.source); found != mapped.end()) { clip.source = found->second; continue; }
            const auto* source = project.source(clip.source);
            AudioBuffer audio(unsigned(matrix.size()), source->audio.frames());
            for (unsigned o = 0; o < audio.channels(); ++o) {
                auto out = audio.plane(o);
                for (std::size_t f = 0; f < audio.frames(); ++f) {
                    if ((f & 4095) == 0) check(cancel);
                    double value = 0;
                    for (unsigned i = 0; i < source->audio.channels(); ++i) value += matrix[o][i] * double(source->audio.plane(i)[f]);
                    out[f] = float(value);
                }
            }
            audio.storeOnDisk(AudioBuffer::kDiskThreshold, cancel); check(cancel);
            const auto replacement = project.addSource(source->name + " (mapped)", source->originalPath,
                std::move(audio), source->originalSampleRate);
            mapped.emplace(clip.source, replacement); clip.source = replacement;
        }
        project.track(id)->clips = std::move(clips);
        return true;
    });
}

SampleEditResult insertSilence(Project snapshot, TrackId id, FramePos at, FramePos frames,
    bool allTracks, const std::atomic<bool>* cancel)
{
    return run(std::move(snapshot), cancel, [&](Project& project) {
        if (frames <= 0 || at < 0 || at > std::numeric_limits<FramePos>::max() - frames
            || project.length() > std::numeric_limits<FramePos>::max() - frames)
            throw std::runtime_error("invalid silence duration or position");
        if (project.tracks().empty()) id = project.addTrack("Silence");
        if (!project.track(id)) throw std::runtime_error("choose a track before inserting silence");
        std::unordered_map<unsigned, SourceId> silent;
        for (auto& track : project.tracks()) if (allTracks || track.id == id) {
            check(cancel);
            const auto channels = layout(project, track.clips);
            // One source per layout shared across the affected tracks. Keeping a
            // real silent clip also preserves an insertion at/past the tail.
            if (!silent.contains(channels)) silent[channels] = project.addSource("Inserted silence", "", AudioBuffer::silentSource(channels, std::size_t(frames)));
            edits::insertAt(track.clips, {Clip{.source = silent[channels], .length = frames}}, at);
        }
        return true;
    });
}
} // namespace omawave::session
