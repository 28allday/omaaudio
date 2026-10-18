#include "sample_edit.h"

#include "edits.h"
#include "clip_envelope.h"
#include "elastic.h"
#include "effect_racks.h"
#include "render.h"
#include "clip_reader.h"
#include "io/time_stretch.h"
#include "render/audio_store.h"
#include <array>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace omawave::session {
namespace {
struct Cancelled {};
void checkCancel(const std::atomic<bool>* cancel)
{
    if (cancel && cancel->load(std::memory_order_relaxed))
        throw Cancelled{};
}

const Track& checkedTrack(const Project& project, TrackId id)
{
    const auto* track = project.track(id);
    if (!track || !edits::isWellFormed(track->clips))
        throw std::runtime_error("sample editing needs a valid track");
    for (const auto& clip : track->clips) {
        const auto* source = project.source(clip.source);
        if (!source || source->audio.channels() == 0
            || !clip.validSource(FramePos(source->audio.frames())))
            throw std::runtime_error("a clip extends beyond its audio source");
    }
    return *track;
}

SourceId addAudio(Project& project, const AudioSource* original, AudioBuffer audio,
                  const std::string& suffix, const std::atomic<bool>* cancel)
{
    audio.storeOnDisk(AudioBuffer::kDiskThreshold, cancel); checkCancel(cancel);
    if (project.sources().size() >= static_cast<std::size_t>(kInvalidSource))
        throw std::runtime_error("too many audio sources");
    return project.addSource(original ? original->name + suffix : "Reversed silence",
        original ? original->originalPath : "", std::move(audio),
        original ? original->originalSampleRate : std::nullopt);
}

template<class Edit>
SampleEditResult process(Project snapshot, const std::atomic<bool>* cancel, Edit edit)
{
    try {
        checkCancel(cancel);
        if (!edit(snapshot))
            return {};
        if (const auto error = validateClipEnvelopes(snapshot); !error.empty()) throw std::runtime_error(error);
        checkCancel(cancel);
        return {std::move(snapshot), false, {}};
    } catch (const Cancelled&) {
        return {std::nullopt, true, {}};
    } catch (const std::exception& error) {
        return {std::nullopt, false, error.what()};
    }
}
} // namespace

SampleEditResult normalizeSelection(Project snapshot, TrackId id, FramePos start, FramePos end,
                                    float target, const std::atomic<bool>* cancel)
{
    return process(std::move(snapshot), cancel, [&](Project& project) {
        const auto& track = checkedTrack(project, id);
        if (start < 0 || end <= start || !std::isfinite(target) || target <= 0 || target > 1)
            throw std::runtime_error("normalize needs a valid range and peak target");
        float peak = 0;
        ClipReader reader(project);
        for (const auto& clip : track.clips) {
            const auto from = std::max(start,clip.timelineStart), to = std::min(end,clip.timelineEnd());
            for (auto at = from; at < to;) {
                checkCancel(cancel);
                const auto count = std::uint32_t(std::min<FramePos>(ClipReader::kBlock,to-at));
                reader.read(clip,at-clip.timelineStart,count,cancel);
                for (unsigned c = 0; c < reader.channels(); ++c) for (unsigned f = 0; f < count; ++f)
                    peak = std::max(peak,std::abs(reader.plane(c)[f]*clip.channelEnvelopeAt(at-clip.timelineStart+f,c,reader.channels())));
                at += count;
            }
        }
        if (peak <= 0) throw std::runtime_error("the selection is silent; nothing to normalize");
        edits::applyGain(project.track(id)->clips, start, end, target / peak);
        return true;
    });
}

bool validCrossfadeRecipe(const Project& project, const CrossfadeRecipe& r,
                          std::size_t frames, std::uint32_t channels)
{
    if (!edits::isWellFormed({r.left, r.right}) || r.left.timelineEnd() != r.right.timelineStart
        || r.start < r.left.timelineStart || r.start >= r.right.timelineStart
        || r.end <= r.right.timelineStart || r.end > r.right.timelineEnd()
        || static_cast<std::uint64_t>(r.end - r.start) != frames || channels == 0) return false;
    for (const auto& clip : {r.left, r.right}) {
        const auto* source = project.source(clip.source);
        if (!source || source->audio.channels() != channels || !std::isfinite(clip.gain)
            || clip.gain < 0 || !clip.hasValidFades() || !clip.validSource(FramePos(source->audio.frames())))
            return false;
    }
    const auto* left = project.source(r.left.source);
    const auto available = std::uint64_t(r.left.sourceFrames(FramePos(left->audio.frames())) - r.left.sourceOffset - r.left.length);
    return r.right.sourceOffset >= r.right.timelineStart - r.start
        && static_cast<std::uint64_t>(r.end - r.right.timelineStart) <= available;
}

std::optional<CrossfadeTarget> editableCrossfade(const Project& project, TrackId id, std::size_t index)
{
    const auto* track = project.track(id);
    if (!track || index >= track->clips.size() || !edits::isWellFormed(track->clips)) return {};
    const auto& clip = track->clips[index];
    const auto* source = project.source(clip.source);
    if (!source || !source->crossfade) return {};
    auto r = *source->crossfade;
    if (!validCrossfadeRecipe(project, r, source->audio.frames(), source->audio.channels())) return {};
    // A whole pair may have moved or been pasted. Derive its current position
    // from the rendered source interval, without rewriting the immutable recipe.
    if (clip.timelineStart < clip.sourceOffset) return {};
    const auto delta = clip.timelineStart - clip.sourceOffset - r.start;
    const auto shift = [delta](FramePos& value) {
        if ((delta < 0 && value < -delta) || (delta > 0 && value > std::numeric_limits<FramePos>::max() - delta)) return false;
        value += delta; return true;
    };
    if (!shift(r.left.timelineStart) || !shift(r.right.timelineStart) || !shift(r.start) || !shift(r.end)
        || !validCrossfadeRecipe(project, r, source->audio.frames(), source->audio.channels())) return {};
    const Clip transition{.source = clip.source, .timelineStart = r.start, .length = r.end - r.start};
    auto expected = std::vector<Clip>{r.left, r.right};
    edits::overwriteAt(expected, {Clip{.source = clip.source, .length = transition.length}}, r.start);
    auto actual = std::lower_bound(track->clips.begin(), track->clips.end(), r.left.timelineStart,
        [](const Clip& c, FramePos at) { return c.timelineStart < at; });
    // Group boundaries and explicit splits may divide the same audio descriptor.
    // Compare each live fragment with its precise slice of the original recipe.
    for (const auto& whole : expected) {
        auto at = whole.timelineStart;
        while (at < whole.timelineEnd()) {
            if (actual == track->clips.end() || actual->timelineStart != at || actual->length <= 0
                || actual->length > whole.timelineEnd() - at) return {};
            auto fragment = whole;
            if (at > fragment.timelineStart) fragment = fragment.tailAfter(at);
            if (actual->timelineEnd() < fragment.timelineEnd()) fragment = fragment.headBefore(actual->timelineEnd());
            if (!fragment.sameAudio(*actual)) return {};
            at = actual->timelineEnd(); ++actual;
        }
    }
    const auto* left = project.source(r.left.source);
    const auto extra = std::uint64_t(r.left.sourceFrames(FramePos(left->audio.frames())) - r.left.sourceOffset - r.left.length);
    const auto join = r.right.timelineStart;
    return CrossfadeTarget{r, std::max(r.left.timelineStart, join - r.right.sourceOffset),
        join + static_cast<FramePos>(std::min<std::uint64_t>(extra, r.right.length))};
}

SampleEditResult adjustCrossfade(Project snapshot, TrackId id, std::size_t index,
                                 FramePos start, FramePos end, bool equalPower,
                                 const std::atomic<bool>* cancel) try
{
    checkCancel(cancel);
    const auto target = editableCrossfade(snapshot, id, index);
    if (!target) return {std::nullopt, false, "This crossfade or its neighbouring clips changed. Undo those edits before adjusting it."};
    const auto& r = target->recipe;
    if (start < target->minimum || start >= r.right.timelineStart
        || end <= r.right.timelineStart || end > target->maximum)
        return {std::nullopt, false, "Crossfade handles must straddle the join and stay within the available source audio."};
    if (start == r.start && end == r.end && equalPower == r.equalPower) return {};
    auto& clips = snapshot.track(id)->clips;
    const auto ownership = clips;
    edits::clearRange(clips, r.left.timelineStart, r.right.timelineEnd());
    auto left = r.left, right = r.right;
    left.group = right.group = kInvalidClipGroup; // recipes own audio, not live editing links
    clips.push_back(left); clips.push_back(right);
    std::sort(clips.begin(), clips.end(), [](const Clip& a, const Clip& b) { return a.timelineStart < b.timelineStart; });
    auto result = crossfadeSelection(std::move(snapshot), id, start, end, equalPower, cancel);
    if (result.project) {
        std::vector<Clip> restored;
        for (const auto& clip : result.project->track(id)->clips) {
            checkCancel(cancel);
            if (!clip.intersects(r.left.timelineStart, r.right.timelineEnd())) { restored.push_back(clip); continue; }
            auto pieces = edits::withClipGroups(clip, ownership);
            restored.insert(restored.end(), pieces.begin(), pieces.end());
        }
        result.project->track(id)->clips = std::move(restored);
    }
    return result;
} catch (const Cancelled&) {
    return {std::nullopt, true, {}};
} catch (const std::exception& error) {
    return {std::nullopt, false, error.what()};
}

SampleEditResult stretchClip(Project snapshot, TrackId id, std::size_t index,
                              FramePos target, const std::atomic<bool>* cancel)
{
    return process(std::move(snapshot), cancel, [=](Project& project) {
        const auto& track=checkedTrack(project,id);
        if(index>=track.clips.size() || target<=0) throw std::runtime_error("Choose a clip and a positive target duration.");
        const auto clip=track.clips[index];
        if(target==clip.length)return false;
        const long double ratio=static_cast<long double>(target)/clip.length;
        if(ratio<0.25L || ratio>4.0L || target>std::numeric_limits<FramePos>::max()-clip.timelineStart)
            throw std::runtime_error("Stretch duration must be between 25% and 400%.");
        if(index+1<track.clips.size() && clip.timelineStart+target>track.clips[index+1].timelineStart)
            throw std::runtime_error("The stretched clip would overlap the next clip. Move it or choose a shorter duration.");
        const auto* source=project.source(clip.source);
        auto dryClip = clip; dryClip.timelineStart = 0; dryClip.gain = 1;
        dryClip.fadeIn = dryClip.fadeOut = dryClip.fadeInOffset = dryClip.fadeOutOffset = 0;
        dryClip.volumeEnvelope = {}; dryClip.panEnvelope = {}; dryClip.envelopeOffset = 0;
        dryClip.rack.reset(); dryClip.effectOffset = 0;
        auto dry = renderTimeline(project,{dryClip},source->audio.channels(),cancel);
        checkCancel(cancel);
        auto result=io::stretchAudio(dry,project.sampleRate(),static_cast<std::size_t>(target),cancel);
        if(result.cancelled)throw Cancelled{};
        if(!result.error.empty())throw std::runtime_error(result.error);
        checkCancel(cancel);
        auto replacement=clip;
        const auto scale=[&](FramePos value) {
            const long double scaled=std::round(value*ratio);
            if(scaled>=std::numeric_limits<FramePos>::max())throw std::runtime_error("Stretched fade duration is too large.");
            return static_cast<FramePos>(scaled);
        };
        replacement.length=target;replacement.sourceOffset=0;replacement.elastic.reset();
        replacement.fadeIn=scale(clip.fadeIn);replacement.fadeOut=scale(clip.fadeOut);
        replacement.fadeInOffset=replacement.fadeIn?std::min(replacement.fadeIn-1,scale(clip.fadeInOffset)):0;
        replacement.fadeOutOffset=replacement.fadeOut?std::min(replacement.fadeOut-1,scale(clip.fadeOutOffset)):0;
        scaleClipEnvelopes(replacement,target,clip.length);
        replacement.source=addAudio(project,source,std::move(result.audio)," (stretched)",cancel);
        rescaleClipEffects(replacement,clip,target,clip.length);
        project.track(id)->clips[index]=replacement;
        if (clip.rack) {
            edits::partitionClipRacks(project.tracks());
            if (const auto error=validateClipRacks(project); !error.empty()) throw std::runtime_error(error);
            if (const auto error=validateAutomation(project); !error.empty()) throw std::runtime_error(error);
        }
        return true;
    });
}

SampleEditResult crossfadeSelection(Project snapshot, TrackId id, FramePos start,
                                    FramePos end, bool equalPower, const std::atomic<bool>* cancel)
{
    return process(std::move(snapshot), cancel, [=](Project& project) {
        const auto& track = checkedTrack(project, id);
        if (start < 0 || end <= start || end > track.length())
            throw std::runtime_error("select a range across one adjacent clip join to crossfade");
        for (std::size_t i = 1; i < track.clips.size(); ++i) {
            const auto left = track.clips[i - 1];
            const auto right = track.clips[i];
            const auto join = right.timelineStart;
            if (left.timelineEnd() != join || !(start < join && join < end)
                || start < left.timelineStart || end > right.timelineEnd()) continue;
            const auto* a = project.source(left.source);
            const auto* b = project.source(right.source);
            if (a->audio.channels() != b->audio.channels())
                throw std::runtime_error("crossfade needs matching channel layouts; convert the track first");
            const auto aOffset = left.sourceOffset + start - left.timelineStart;
            const auto lead = join - start;
            const auto length = end - start;
            if (right.sourceOffset < lead || aOffset > left.sourceFrames(FramePos(a->audio.frames()))
                || length > left.sourceFrames(FramePos(a->audio.frames()))-aOffset)
                throw std::runtime_error("crossfade needs unused audio beyond both clip edges; trim or slip the clips, or shorten the selection");
            const auto bOffset = right.sourceOffset - lead;
            auto aClip = left, bClip = right;
            aClip.sourceOffset = aOffset; bClip.sourceOffset = bOffset;
            aClip.length = bClip.length = length; aClip.timelineStart = bClip.timelineStart = 0;
            ClipReader aReader(project), bReader(project);
            render::AudioStore audio(a->audio.channels(), static_cast<std::size_t>(length));
            std::vector<std::array<float, ClipReader::kBlock>> planes(a->audio.channels());
            for (FramePos f = 0; f < length; ++f) {
                const auto local = unsigned(f%ClipReader::kBlock);
                if (!local) {
                    const auto count = std::uint32_t(std::min<FramePos>(ClipReader::kBlock,length-f));
                    aReader.read(aClip,f,count,cancel); bReader.read(bClip,f,count,cancel);
                }
                if ((f & 4095) == 0) checkCancel(cancel);
                const double t = static_cast<double>(f) / static_cast<double>(length - 1);
                const float wa = static_cast<float>(equalPower ? std::cos(t * std::numbers::pi / 2) : 1 - t);
                const float wb = static_cast<float>(equalPower ? std::sin(t * std::numbers::pi / 2) : t);
                for (std::uint32_t c = 0; c < a->audio.channels(); ++c) {
                    const float ga = left.channelEnvelopeAt(std::clamp(start + f - left.timelineStart, FramePos{0}, left.length - 1),c,a->audio.channels());
                    const float gb = right.channelEnvelopeAt(std::clamp(start + f - right.timelineStart, FramePos{0}, right.length - 1),c,b->audio.channels());
                    const double mixed = static_cast<double>(aReader.plane(c)[local]) * ga * wa
                        + static_cast<double>(bReader.plane(c)[local]) * gb * wb;
                    if (!std::isfinite(mixed) || std::abs(mixed) > std::numeric_limits<float>::max())
                        throw std::runtime_error("crossfade produced an invalid or overflowing sample");
                    planes[c][local] = static_cast<float>(mixed);
                }
                if (local + 1 == ClipReader::kBlock || f + 1 == length)
                    for (std::uint32_t c = 0; c < a->audio.channels(); ++c)
                        audio.write(c, std::size_t(f - local), std::span<const float>(planes[c]).first(local + 1));
            }
            checkCancel(cancel);
            auto rendered = audio.finish();
            auto recipe = CrossfadeRecipe{left, right, start, end, equalPower};
            recipe.left.group = recipe.right.group = kInvalidClipGroup;
            const auto source = project.addSource(equalPower ? "Crossfade (equal power)" : "Crossfade (linear)", "",
                std::move(rendered), std::nullopt, Uuid{}, recipe);
            auto& clips = project.track(id)->clips;
            edits::overwriteRendered(clips, Clip{.source = source, .timelineStart = start, .length = length}, clips);
            return true;
        }
        throw std::runtime_error("select a range across exactly one adjacent clip join to crossfade");
    });
}

SampleEditResult reverseSelection(Project snapshot, TrackId id, FramePos start,
                                  FramePos end, const std::atomic<bool>* cancel)
{
    return process(std::move(snapshot), cancel, [=](Project& project) {
        const auto& track = checkedTrack(project, id);
        const auto duration = track.length();
        if (start < 0 || end < start || end > duration)
            throw std::runtime_error("reverse selection is outside the track");
        if (start == end)
            return false;
        const auto selected = edits::copyRange(track.clips, start, end);
        if (selected.empty())
            return false;
        auto clips = track.clips;
        edits::clearRange(clips, start, end);
        for (const auto& part : selected) {
            checkCancel(cancel);
            const auto* source = project.source(part.source);
            const auto audio = renderClipRange(project,{part},part.timelineStart,part.timelineEnd(),source->audio.channels(),cancel);
            checkCancel(cancel);
            render::AudioStore reversed(audio.channels(), audio.frames());
            std::array<float, render::AudioStore::blockFrames> block;
            for (unsigned c = 0; c < audio.channels(); ++c) {
                const auto input = audio.plane(c);
                for (std::size_t at = 0; at < audio.frames();) {
                    checkCancel(cancel);
                    const auto count = std::min(block.size(), audio.frames() - at);
                    std::reverse_copy(input.end() - at - count, input.end() - at, block.begin());
                    reversed.write(c, at, std::span<const float>(block).first(count));
                    at += count;
                    if (audio.diskStorage()) audio.diskStorage()->releaseMappedPages();
                }
            }
            const auto sourceId = addAudio(project, source, reversed.finish(), " (reversed)", cancel);
            clips.push_back(Clip{.source = sourceId,
                .timelineStart = start + (end - start - part.timelineEnd()),
                .length = part.length, .group = part.group});
        }
        std::sort(clips.begin(), clips.end(), [](const Clip& a, const Clip& b) {
            return a.timelineStart < b.timelineStart;
        });
        const auto newEnd = edits::totalLength(clips);
        if (newEnd < duration) {
            checkCancel(cancel);
            const auto silence = addAudio(project, nullptr,
                AudioBuffer::silentSource(1, static_cast<std::size_t>(duration - newEnd)), "", cancel);
            clips.push_back(Clip{.source = silence, .timelineStart = newEnd,
                                .length = duration - newEnd});
        }
        project.track(id)->clips = std::move(clips);
        return true;
    });
}

SampleEditResult convertTrackChannels(Project snapshot, TrackId id,
                                      std::uint32_t channels, const std::atomic<bool>* cancel)
{
    return process(std::move(snapshot), cancel, [=](Project& project) {
        const auto& track = checkedTrack(project, id);
        if (channels != 1 && channels != 2)
            throw std::runtime_error("choose mono or stereo");
        for (const auto& clip : track.clips) {
            if (channels == 2 && project.source(clip.source)->audio.channels() > 2)
                throw std::runtime_error("convert multichannel audio to mono before making stereo");
        }
        std::unordered_map<SourceId, SourceId> converted;
        auto clips = track.clips;
        for (auto& clip : clips) {
            checkCancel(cancel);
            const auto* source = project.source(clip.source);
            if (source->audio.channels() == channels)
                continue;
            if (const auto found = converted.find(clip.source); found != converted.end()) {
                clip.source = found->second;
                continue;
            }
            render::AudioStore audio(channels, source->audio.frames());
            std::array<float, render::AudioStore::blockFrames> block;
            for (std::size_t at = 0; at < source->audio.frames();) {
                checkCancel(cancel);
                const auto count = std::min(block.size(), source->audio.frames() - at);
                for (std::size_t i = 0; i < count; ++i) {
                    if (channels == 1) {
                        double sum = 0;
                        for (std::uint32_t c = 0; c < source->audio.channels(); ++c)
                            sum += source->audio.plane(c)[at + i];
                        block[i] = static_cast<float>(sum / source->audio.channels());
                    } else block[i] = source->audio.plane(0)[at + i];
                }
                for (std::uint32_t c = 0; c < channels; ++c)
                    audio.write(c, at, std::span<const float>(block).first(count));
                at += count;
                if (source->audio.diskStorage()) source->audio.diskStorage()->releaseMappedPages();
            }
            const auto replacement = addAudio(project, source, audio.finish(),
                                               channels == 1 ? " (mono)" : " (stereo)", cancel);
            converted.emplace(clip.source, replacement);
            clip.source = replacement;
        }
        if (converted.empty())
            return false;
        project.track(id)->clips = std::move(clips);
        return true;
    });
}
} // namespace omawave::session
