#include "multitrack_clipboard.h"
#include "edits.h"
#include "clip_envelope.h"
#include "elastic.h"
#include "effect_racks.h"
#include "frame_scale.h"
#include "render.h"
#include "io/time_stretch.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace omawave::session {
namespace {
constexpr auto maximum = std::numeric_limits<FramePos>::max();
struct Cancelled {};
void check(const std::atomic<bool>* cancel)
{
    if (cancel && cancel->load(std::memory_order_relaxed)) throw Cancelled{};
}
void require(bool valid, const char* reason)
{
    if (!valid) throw std::runtime_error(reason);
}
std::uint32_t validateClips(const Project& project, const std::vector<Clip>& clips)
{
    FramePos end = 0;
    std::uint32_t channels = 1;
    for (const auto& clip : clips) {
        require(clip.timelineStart >= end && clip.length > 0 && clip.timelineStart <= maximum - clip.length,
                "invalid or overlapping clip geometry");
        const auto* source = project.source(clip.source);
        require(source && source->sampleRate == project.sampleRate() && source->audio.channels() >= 1
                && source->audio.channels() <= 8 && source->audio.frames() <= std::uint64_t(maximum),
                "clipboard audio source is missing or incompatible");
        require(clip.validSource(FramePos(source->audio.frames())) && clip.hasValidFades() && clip.hasValidEnvelopes()
                && clip.hasValidEffects() && std::isfinite(clip.gain),
                "invalid clipboard source interval or envelope");
        require(!clip.elastic || (source->audio.channels() <= 2 && clip.gain >= 0),
                "invalid elastic clip mapping or channel layout");
        channels = std::max(channels, source->audio.channels());
        end = clip.timelineEnd();
    }
    return channels;
}
std::vector<std::size_t> trackIndices(const Project& project, const std::vector<TrackId>& ids)
{
    require(!ids.empty(), "select tracks first");
    std::vector<std::size_t> indices;
    for (const auto id : ids) {
        const auto index = project.trackIndex(id);
        require(index.has_value(), "a selected track no longer exists");
        indices.push_back(*index);
    }
    std::sort(indices.begin(), indices.end());
    require(std::adjacent_find(indices.begin(), indices.end()) == indices.end(), "a track was selected twice");
    return indices;
}
using Address = std::pair<std::size_t, std::size_t>;
std::vector<Address> clipIndices(const Project& project, const std::vector<ClipAddress>& selected)
{
    require(!selected.empty(), "select clips first");
    std::vector<Address> indices;
    std::set<std::size_t> validated;
    for (const auto& address : selected) {
        const auto track = project.trackIndex(address.track);
        require(track.has_value(), "a selected track no longer exists");
        const auto& clips = project.tracks()[*track].clips;
        require(address.index < clips.size() && clips[address.index].sameDescriptor(address.expected),
                "a selected clip changed; select it again");
        if (validated.insert(*track).second) (void)validateClips(project, clips);
        indices.emplace_back(*track, address.index);
    }
    std::sort(indices.begin(), indices.end());
    require(std::adjacent_find(indices.begin(), indices.end()) == indices.end(), "a clip was selected twice");
    return indices;
}
ClipboardTrack lane(const Project& project, std::size_t index, std::size_t first)
{
    const auto& track = project.tracks()[index];
    return {index - first, track.name, track.colour, track.role, validateClips(project, track.clips), {}};
}
void identify(const Project& project, MultitrackClipboard& clipboard)
{
    std::set<ClipGroupId> groups;
    for (const auto& track : clipboard.tracks) for (const auto& clip : track.clips) {
        clipboard.sources.emplace(clip.source, project.source(clip.source)->uuid);
        if (clip.group != kInvalidClipGroup && groups.insert(clip.group).second) {
            const auto* group = project.clipGroup(clip.group);
            require(group && group->valid(), "a copied clip group is missing or invalid");
            clipboard.groups.push_back(*group);
        }
    }
}
void validateClipboard(const Project& project, const MultitrackClipboard& clipboard, std::size_t maxTracks)
{
    require(clipboard.sampleRate == project.sampleRate() && clipboard.sampleRate >= 8000 && clipboard.sampleRate <= 192000,
            "clipboard sample rate differs from this project");
    require(clipboard.frames > 0 && !clipboard.tracks.empty(), "nothing to paste");
    require(clipboard.tracks.front().offset == 0, "invalid clipboard track origin");
    std::set<ClipGroupId> groups;
    for (const auto& group : clipboard.groups)
        require(group.valid() && groups.insert(group.id).second, "invalid clipboard group metadata");
    std::size_t previous = 0;
    for (std::size_t index = 0; index < clipboard.tracks.size(); ++index) {
        const auto& track = clipboard.tracks[index];
        require(track.offset < maxTracks && (!index || track.offset > previous), "invalid clipboard track offsets");
        previous = track.offset;
        const auto channels = validateClips(project, track.clips);
        require(track.channels >= channels && track.channels <= 8, "invalid clipboard channel layout");
        require(edits::totalLength(track.clips) <= clipboard.frames, "clipboard clips exceed their time span");
        require(!clipboard.wholeClips || !track.clips.empty(), "empty whole-clip selection");
        for (const auto& clip : track.clips) {
            require(clip.group == kInvalidClipGroup || groups.contains(clip.group), "missing clipboard group metadata");
            const auto expected = clipboard.sources.find(clip.source);
            require(expected != clipboard.sources.end() && expected->second == project.source(clip.source)->uuid,
                    "clipboard belongs to another project or a replaced source");
        }
    }
}
void removeClips(std::vector<Track>& tracks, const std::vector<Address>& indices)
{
    for (auto it = indices.rbegin(); it != indices.rend(); ++it) {
        auto& clips = tracks[it->first].clips;
        clips.erase(clips.begin() + static_cast<std::ptrdiff_t>(it->second));
    }
}
FramePos translated(FramePos position, FramePos delta)
{
    require(position >= 0 && (delta >= 0 ? position <= maximum - delta : delta >= -position),
            "group movement is outside the timeline");
    return position + delta;
}
void installTracks(Project& project, std::vector<Track> tracks)
{
    edits::partitionClipRacks(tracks);
    auto candidate = project;
    candidate.tracks() = std::move(tracks);
    if (const auto error = validateClipEnvelopes(candidate); !error.empty()) throw std::runtime_error(error);
    if (const auto error = validateElastic(candidate); !error.empty()) throw std::runtime_error(error);
    bool withEffects = false;
    for (const auto& track : candidate.tracks()) for (const auto& clip : track.clips) withEffects |= bool(clip.rack);
    if (withEffects)
        if (const auto error = validateClipRacks(candidate); !error.empty()) throw std::runtime_error(error);
    project.tracks() = std::move(candidate.tracks());
}
} // namespace

ClipStretchPlan planSelectedClipStretch(const Project& project, const std::vector<ClipAddress>& selected,
                                        FramePos targetFrames, bool realtime, double pitchChange)
{
    ClipStretchPlan plan;
    try {
        (void)clipIndices(project, selected);
        require(project.validateClipGroups().empty(), "the project has invalid clip groups");
        require(std::isfinite(pitchChange) && (realtime || pitchChange == 0),
                "rendered group stretch preserves pitch; use realtime mode to change it");
        FramePos end = 0; plan.origin = maximum;
        for (const auto& address : selected) {
            plan.origin = std::min(plan.origin, address.expected.timelineStart);
            end = std::max(end, address.expected.timelineEnd());
        }
        plan.frames = end - plan.origin;
        require(targetFrames > 0 && targetFrames <= maximum - plan.origin,
                "the selected duration exceeds the timeline");
        require(targetFrames >= plan.frames / 4 + (plan.frames % 4 != 0)
                && (plan.frames > maximum / 4 || targetFrames <= plan.frames * 4),
                "choose 25–400% of the selected span");
        auto tracks = project.tracks();
        for (const auto& address : selected) {
            const auto& old = address.expected;
            const auto start = plan.origin + scaledFrames(old.timelineStart-plan.origin, targetFrames, plan.frames);
            const auto endFrame = plan.origin + scaledFrames(old.timelineEnd()-plan.origin, targetFrames, plan.frames);
            const auto length = endFrame - start;
            const auto* source = project.source(old.source);
            require(source->audio.channels() <= 2 && old.length >= project.sampleRate()/20
                    && length >= project.sampleRate()/20,
                    "stretch needs mono or stereo clips with input and output of at least 50 ms");
            auto clip = old;
            if (realtime) {
                const double pitch = (old.elastic ? old.elastic->semitones : 0.0) + pitchChange;
                require(std::isfinite(pitch) && pitch >= -12 && pitch <= 12,
                        "a selected clip would exceed pitch from -12 to +12 semitones");
                clip = makeElasticClip(project, old, length, float(pitch));
            } else {
                require(length >= old.length / 4 + (old.length % 4 != 0)
                        && (old.length > maximum / 4 || length <= old.length * 4),
                        "each rendered clip needs 25–400% of its current duration");
                clip.length = length;
                scaleClipEnvelopes(clip,length,old.length);
                clip.fadeIn = scaledFrames(old.fadeIn, length, old.length);
                clip.fadeOut = scaledFrames(old.fadeOut, length, old.length);
                clip.fadeInOffset = clip.fadeIn ? std::min(clip.fadeIn-1, scaledFrames(old.fadeInOffset,length,old.length)) : 0;
                clip.fadeOutOffset = clip.fadeOut ? std::min(clip.fadeOut-1, scaledFrames(old.fadeOutOffset,length,old.length)) : 0;
                rescaleClipEffects(clip, old, length, old.length);
            }
            clip.timelineStart = start;
            require(clip.hasValidFades(), "scaled fades are outside the supported range");
            plan.changed |= clip != old;
            plan.clips.push_back(clip);
            tracks[*project.trackIndex(address.track)].clips[address.index] = clip;
        }
        for (auto& track : tracks) {
            std::sort(track.clips.begin(),track.clips.end(),[](const auto& a, const auto& b) { return a.timelineStart < b.timelineStart; });
            require(edits::isWellFormed(track.clips), "stretched clips would overlap an unselected clip");
        }
        if (realtime) { auto candidate = project; installTracks(candidate, std::move(tracks)); }
        else if (const auto error = validateElastic(project); !error.empty()) throw std::runtime_error(error);
        return plan;
    } catch (const std::exception& error) { plan.clips.clear(); plan.changed = false; plan.error = error.what(); return plan; }
}

SelectedClipStretchResult stretchSelectedClips(Project project, const std::vector<ClipAddress>& selected,
                                               FramePos frames, bool realtime, double pitchChange,
                                               const std::atomic<bool>* cancel)
{
    try {
        check(cancel);
        auto plan = planSelectedClipStretch(project, selected, frames, realtime, pitchChange);
        if (!plan.error.empty()) throw std::runtime_error(plan.error);
        if (!plan.changed) return {};
        auto tracks = project.tracks();
        for (std::size_t i = 0; i < selected.size(); ++i) {
            check(cancel);
            const auto& old = selected[i].expected;
            auto& clip = plan.clips[i];
            if (!realtime && clip.length != old.length) {
                const auto* source = project.source(old.source);
                auto dryClip = old; dryClip.timelineStart = 0; dryClip.gain = 1;
                dryClip.fadeIn = dryClip.fadeOut = dryClip.fadeInOffset = dryClip.fadeOutOffset = 0;
                dryClip.volumeEnvelope = {}; dryClip.panEnvelope = {}; dryClip.envelopeOffset = 0;
                dryClip.rack.reset(); dryClip.effectOffset = 0;
                auto dry = renderTimeline(project, {dryClip}, source->audio.channels(), cancel);
                check(cancel);
                auto result = io::stretchAudio(dry, project.sampleRate(), std::size_t(clip.length), cancel);
                if (result.cancelled) throw Cancelled{};
                if (!result.error.empty()) throw std::runtime_error(result.error);
                result.audio.storeOnDisk(AudioBuffer::kDiskThreshold, cancel); check(cancel);
                require(project.sources().size() < std::size_t(kInvalidSource), "too many audio sources");
                clip.source = project.addSource(source->name + " (stretched)", source->originalPath,
                    std::move(result.audio), source->originalSampleRate);
                clip.sourceOffset = 0; clip.elastic.reset();
                rescaleClipEffects(clip, old, clip.length, old.length);
            }
            tracks[*project.trackIndex(selected[i].track)].clips[selected[i].index] = clip;
        }
        for (auto& track : tracks) {
            std::sort(track.clips.begin(), track.clips.end(), [](const auto& a, const auto& b) { return a.timelineStart < b.timelineStart; });
            (void)validateClips(project, track.clips);
        }
        installTracks(project, std::move(tracks));
        std::vector<ClipAddress> result;
        for (std::size_t i = 0; i < selected.size(); ++i) {
            const auto& track = *project.track(selected[i].track);
            const auto found = std::find_if(track.clips.begin(), track.clips.end(), [&](const auto& current) {
                return current.identity == plan.clips[i].identity;
            });
            require(found != track.clips.end(), "stretched clip could not be located");
            result.push_back({track.id, std::size_t(found-track.clips.begin()), *found});
        }
        check(cancel);
        return {{std::move(project), false, {}}, std::move(result)};
    } catch (const Cancelled&) { return {{std::nullopt, true, {}}, {}}; }
      catch (const std::exception& error) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return {{std::nullopt, true, {}}, {}};
        return {{std::nullopt, false, error.what()}, {}};
    }
}

GroupEditResult editSelectedClipProperties(Project& project, const std::vector<ClipAddress>& selected,
                                           const ClipPropertyEdit& edit)
{
    try {
        const auto indices = clipIndices(project, selected);
        require(std::isfinite(edit.gainDb) && edit.gainDb >= -80 && edit.gainDb <= 48,
                "choose a level from -80 to +48 dB");
        require(!edit.fadeShapes || edit.fadeShapes->valid(), "choose valid fade shapes and bends from -1 to +1");
        require(!edit.replaceFades || (edit.fadeIn >= 0 && edit.fadeOut >= 0 && edit.fadeReferenceLength >= 0
                && (!edit.fadeReferenceLength || (edit.fadeIn <= edit.fadeReferenceLength && edit.fadeOut <= edit.fadeReferenceLength))),
                "choose fades within the reference clip's length");
        auto tracks = project.tracks();
        const auto factor = !edit.relativeGain && edit.gainDb <= -80 ? 0.0 : std::pow(10.0, edit.gainDb / 20.0);
        const auto maximumGain = std::pow(10.0, 48.0 / 20.0);
        const auto limit = std::max(maximumGain, double(float(maximumGain)));
        bool changed = false;
        for (const auto& [row, index] : indices) {
            auto& clip = tracks[row].clips[index];
            const auto before = clip;
            require(clip.gain >= 0, "selected clip has an invalid level");
            const double gain = edit.relativeGain ? double(clip.gain) * factor : factor;
            require(std::isfinite(gain) && gain <= limit,
                    "a selected clip would exceed +48 dB; choose a smaller level change");
            const double currentDb = clip.gain > 0 ? std::max(-80.0, 20.0 * std::log10(double(clip.gain))) : -80.0;
            if (edit.relativeGain ? edit.gainDb != 0 : std::abs(edit.gainDb - currentDb) > 0.000001)
                clip.gain = float(gain);
            if (edit.replaceFades) {
                const auto fade = [&](FramePos value) {
                    if (!edit.fadeReferenceLength) {
                        require(value <= clip.length, "a fade exceeds the shortest selected clip; use proportional fades or shorten it");
                        return value;
                    }
                    return scaledFrames(value, clip.length, edit.fadeReferenceLength);
                };
                clip.fadeIn = fade(edit.fadeIn); clip.fadeOut = fade(edit.fadeOut);
                clip.fadeInOffset = clip.fadeOutOffset = 0;
                if (edit.fadeShapes) {
                    clip.fadeInShape = edit.fadeShapes->fadeInShape;
                    clip.fadeOutShape = edit.fadeShapes->fadeOutShape;
                    clip.fadeInCurve = edit.fadeShapes->fadeInCurve;
                    clip.fadeOutCurve = edit.fadeShapes->fadeOutCurve;
                }
            }
            changed |= clip != before;
        }
        if (changed) installTracks(project, std::move(tracks));
        return {changed, {}};
    } catch (const std::exception& error) { return {false, error.what()}; }
}

ClipSelectionResult expandClipGroups(const Project& project, const std::vector<ClipAddress>& selected)
{
    try {
        if (selected.empty()) return {};
        const auto indices = clipIndices(project, selected);
        std::set<Address> keep(indices.begin(), indices.end());
        std::set<ClipGroupId> groups;
        for (const auto& [track, index] : indices) {
            const auto id = project.tracks()[track].clips[index].group;
            if (id == kInvalidClipGroup) continue;
            const auto* group = project.clipGroup(id);
            require(group && group->valid(), "a selected clip group is missing or invalid");
            groups.insert(id);
        }
        ClipSelectionResult result;
        for (std::size_t t = 0; t < project.tracks().size(); ++t) {
            const auto& track = project.tracks()[t];
            for (std::size_t c = 0; c < track.clips.size(); ++c)
                if (keep.contains({t, c}) || groups.contains(track.clips[c].group))
                    result.clips.push_back({track.id, c, track.clips[c]});
        }
        // A group may reach another track. Validate those new addresses too.
        (void)clipIndices(project, result.clips);
        return result;
    } catch (const std::exception& error) { return {{}, error.what()}; }
}

ClipGroupResult createClipGroup(Project& project, const std::vector<ClipAddress>& selected,
                               std::string name, std::string colour)
{
    try {
        require(selected.size() >= 2, "select at least two clips to group");
        const auto indices = clipIndices(project, selected);
        require(project.validateClipGroups().empty(), "the project has invalid clip groups");
        const auto id = project.addClipGroup(std::move(name), std::move(colour));
        require(id != kInvalidClipGroup, "choose a valid group name and colour, with an available group identity");
        for (const auto& [track, clip] : indices) project.tracks()[track].clips[clip].group = id;
        return {id, {}};
    } catch (const std::exception& error) { return {kInvalidClipGroup, error.what()}; }
}

GroupEditResult detachClipGroupMembers(Project& project, const std::vector<ClipAddress>& selected)
{
    try {
        const auto indices = clipIndices(project, selected);
        bool changed = false;
        for (const auto& [track, clip] : indices) {
            auto& member = project.tracks()[track].clips[clip].group;
            changed |= member != kInvalidClipGroup; member = kInvalidClipGroup;
        }
        return {changed, {}};
    } catch (const std::exception& error) { return {false, error.what()}; }
}

ClipboardCapture copyTrackRange(const Project& project, const std::vector<TrackId>& ids, FramePos start, FramePos end)
{
    try {
        require(start >= 0 && end > start, "select a positive time range");
        const auto indices = trackIndices(project, ids);
        MultitrackClipboard clipboard{project.sampleRate(), end - start, false, {}, {}, {}};
        for (const auto index : indices) {
            auto track = lane(project, index, indices.front());
            track.clips = edits::copyRange(project.tracks()[index].clips, start, end);
            clipboard.tracks.push_back(std::move(track));
        }
        identify(project, clipboard);
        return {std::move(clipboard), {}};
    } catch (const std::exception& error) { return {std::nullopt, error.what()}; }
}

ClipboardCapture copySelectedClips(const Project& project, const std::vector<ClipAddress>& selected)
{
    try {
        const auto indices = clipIndices(project, selected);
        FramePos first = maximum, last = 0;
        for (const auto& [track, clip] : indices) {
            const auto& c = project.tracks()[track].clips[clip];
            first = std::min(first, c.timelineStart); last = std::max(last, c.timelineEnd());
        }
        MultitrackClipboard clipboard{project.sampleRate(), last - first, true, {}, {}, {}};
        for (const auto& [track, clip] : indices) {
            if (clipboard.tracks.empty() || clipboard.tracks.back().offset != track - indices.front().first)
                clipboard.tracks.push_back(lane(project, track, indices.front().first));
            auto c = project.tracks()[track].clips[clip]; c.timelineStart -= first;
            clipboard.tracks.back().clips.push_back(std::move(c));
        }
        identify(project, clipboard);
        return {std::move(clipboard), {}};
    } catch (const std::exception& error) { return {std::nullopt, error.what()}; }
}

SampleEditResult pasteTracks(Project project, const MultitrackClipboard& clipboard, std::size_t firstTrack,
                             FramePos at, bool ripple, std::size_t maxTracks, const std::atomic<bool>* cancel)
{
    try {
        check(cancel);
        validateClipboard(project, clipboard, maxTracks);
        require(at >= 0 && at <= maximum - clipboard.frames, "paste position exceeds the timeline");
        require(firstTrack <= project.tracks().size() && firstTrack < maxTracks
                && clipboard.tracks.back().offset < maxTracks - firstTrack
                && project.tracks().size() <= maxTracks, "paste exceeds the available track slots");
        // Validate every affected destination before creating any tracks/silence.
        for (const auto& incoming : clipboard.tracks) {
            const auto index = firstTrack + incoming.offset;
            if (index >= project.tracks().size()) continue;
            const auto& clips = project.tracks()[index].clips;
            (void)validateClips(project, clips);
            require(!ripple || edits::totalLength(clips) <= maximum - clipboard.frames,
                    "ripple paste exceeds the timeline");
        }
        const auto existing = project.tracks().size();
        std::map<ClipGroupId, ClipGroupId> groups;
        for (const auto& group : clipboard.groups) {
            const auto fresh = project.addClipGroup(group.name, group.colour);
            require(fresh != kInvalidClipGroup, "cannot allocate pasted clip group");
            groups.emplace(group.id, fresh);
        }
        const auto lastTrack = firstTrack + clipboard.tracks.back().offset;
        while (project.tracks().size() <= lastTrack)
            (void)project.addTrack("track " + std::to_string(project.tracks().size() + 1));
        // Share silence only for equal lengths/layouts; it is never copied from
        // the source material or flattened together with the copied clips.
        std::map<std::pair<std::uint32_t, FramePos>, SourceId> silence;
        edits::ClipRackForkMap forks;
        for (const auto& incoming : clipboard.tracks) {
            check(cancel);
            auto& track = project.tracks()[firstTrack + incoming.offset];
            if (firstTrack + incoming.offset >= existing) {
                track.name = incoming.name; track.role = incoming.role; track.colour = incoming.colour;
            }
            if (ripple) {
                edits::splitAt(track.clips, at);
                for (auto& clip : track.clips) if (clip.timelineStart >= at) clip.timelineStart += clipboard.frames;
            } else if (!clipboard.wholeClips) {
                edits::clearRange(track.clips, at, at + clipboard.frames);
            } else {
                for (const auto& clip : incoming.clips)
                    edits::clearRange(track.clips, at + clip.timelineStart, at + clip.timelineEnd());
            }
            auto clips = incoming.clips;
            edits::forkClipCopies(clips, forks);
            for (auto& clip : clips) if (clip.group != kInvalidClipGroup) clip.group = groups.at(clip.group);
            const auto end = edits::totalLength(clips);
            if (!clipboard.wholeClips && end < clipboard.frames && track.length() < at + clipboard.frames) {
                const auto tail = clipboard.frames - end;
                require(std::uint64_t(tail) <= std::numeric_limits<std::size_t>::max() / (sizeof(float) * incoming.channels),
                        "clipboard silence duration is too large");
                const auto key = std::pair{incoming.channels, tail};
                if (!silence.contains(key)) silence[key] = project.addSource("Pasted silence", "",
                    AudioBuffer::silentSource(incoming.channels, std::size_t(tail)));
                clips.push_back(Clip{.source = silence[key], .timelineStart = end, .length = tail});
            }
            for (auto clip : clips) { clip.timelineStart += at; track.clips.push_back(std::move(clip)); }
            std::sort(track.clips.begin(), track.clips.end(), [](const auto& a, const auto& b) { return a.timelineStart < b.timelineStart; });
            (void)validateClips(project, track.clips);
        }
        edits::partitionClipRacks(project.tracks());
        check(cancel);
        if (const auto error = validateClipEnvelopes(project); !error.empty()) throw std::runtime_error(error);
        if (const auto error = validateElastic(project); !error.empty()) throw std::runtime_error(error);
        bool withEffects = false;
        for (const auto& track : project.tracks()) for (const auto& clip : track.clips) withEffects |= bool(clip.rack);
        if (withEffects)
            if (const auto error = validateClipRacks(project); !error.empty()) throw std::runtime_error(error);
        return {std::move(project), false, {}};
    } catch (const Cancelled&) { return {std::nullopt, true, {}}; }
    catch (const std::exception& error) { return {std::nullopt, false, error.what()}; }
}

GroupEditResult deleteTrackRange(Project& project, const std::vector<TrackId>& ids, FramePos start, FramePos end, bool ripple)
{
    try {
        require(start >= 0 && end > start, "select a positive time range");
        const auto indices = trackIndices(project, ids);
        auto tracks = project.tracks();
        bool changed = false;
        for (const auto index : indices) {
            (void)validateClips(project, tracks[index].clips);
            if (ripple) edits::removeRange(tracks[index].clips, start, end);
            else edits::clearRange(tracks[index].clips, start, end);
            changed |= tracks[index].clips != project.tracks()[index].clips;
        }
        if (changed) installTracks(project, std::move(tracks));
        return {changed, {}};
    } catch (const std::exception& error) { return {false, error.what()}; }
}

GroupEditResult deleteSelectedClips(Project& project, const std::vector<ClipAddress>& selected)
{
    try {
        const auto indices = clipIndices(project, selected);
        auto tracks = project.tracks(); removeClips(tracks, indices);
        installTracks(project, std::move(tracks));
        return {true, {}};
    } catch (const std::exception& error) { return {false, error.what()}; }
}

GroupEditResult moveSelectedClips(Project& project, const std::vector<ClipAddress>& selected, FramePos timeDelta, int trackDelta)
{
    try {
        const auto indices = clipIndices(project, selected);
        if (!timeDelta && !trackDelta) return {};
        auto tracks = project.tracks(); removeClips(tracks, indices);
        std::set<std::size_t> destinations;
        for (const auto& [index, clipIndex] : indices) {
            const auto target = static_cast<std::int64_t>(index) + trackDelta;
            require(target >= 0 && std::uint64_t(target) < tracks.size(), "group movement needs existing destination tracks");
            auto clip = project.tracks()[index].clips[clipIndex];
            (void)translated(clip.timelineEnd(), timeDelta);
            clip.timelineStart = translated(clip.timelineStart, timeDelta);
            tracks[std::size_t(target)].clips.push_back(std::move(clip));
            destinations.insert(std::size_t(target));
        }
        for (const auto index : destinations) {
            auto& clips = tracks[index].clips;
            std::sort(clips.begin(), clips.end(), [](const auto& a, const auto& b) { return a.timelineStart < b.timelineStart; });
            (void)validateClips(project, clips);
        }
        installTracks(project, std::move(tracks));
        return {true, {}};
    } catch (const std::exception& error) { return {false, error.what()}; }
}

} // namespace omawave::session
