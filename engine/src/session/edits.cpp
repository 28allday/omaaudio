#include "edits.h"

#include "project.h"
#include "effect_racks.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace omawave::session::edits {
namespace {

void sortAndPrune(std::vector<Clip>& clips)
{
    // Zero-length clips are meaningless and would divide by zero in a fade, so they
    // never survive an edit.
    clips.erase(std::remove_if(clips.begin(), clips.end(),
                               [](const Clip& c) { return c.length <= 0; }),
                clips.end());
    std::sort(clips.begin(), clips.end(), [](const Clip& a, const Clip& b) {
        return a.timelineStart < b.timelineStart;
    });
}

bool validEffects(const Project& project)
{
    bool present = false;
    for (const auto& track : project.tracks()) for (const auto& clip : track.clips) present |= bool(clip.rack);
    return !present || (validateClipRacks(project).empty() && validateAutomation(project).empty());
}

void partitionLane(std::vector<Clip>& clips, std::unordered_set<EffectOwner>& seen)
{
    const Clip* previous = nullptr;
    std::shared_ptr<const ClipRack> previousOriginal, assigned;
    for (auto& clip : clips) {
        if (!clip.hasValidEffects()) throw std::invalid_argument("invalid clip effect region mapping");
        const auto original = clip.rack;
        if (!original) { previous = nullptr; previousOriginal.reset(); continue; }
        const bool continuous = previous && previousOriginal
            && previousOriginal->owner == original->owner
            && *previousOriginal == *original
            && sameInsertDescriptors(previousOriginal->inserts, original->inserts)
            && previous->timelineEnd() == clip.timelineStart
            && previous->effectOffset + previous->length == clip.effectOffset;
        if (!continuous)
            assigned = seen.insert(original->owner).second ? original : forkClipRack(original);
        clip.rack = assigned;
        previous = &clip; previousOriginal = original;
    }
}

} // namespace

void forkClipCopies(std::vector<Clip>& clips, ClipRackForkMap& forks)
{
    auto candidate = clips;
    auto nextForks = forks;
    for (auto& clip : candidate) {
        if (!clip.hasValidEffects()) throw std::invalid_argument("invalid copied clip effect region mapping");
        clip.forkIdentity();
        if (!clip.rack) continue;
        const auto owner = clip.rack->owner;
        auto found = nextForks.find(owner);
        if (found == nextForks.end()) found = nextForks.emplace(owner, forkClipRack(clip.rack)).first;
        else if (*found->second != *clip.rack)
            throw std::invalid_argument("one copied clip effect region has incompatible settings");
        clip.rack = found->second;
    }
    clips.swap(candidate); forks.swap(nextForks);
}

void partitionClipRacks(std::vector<Clip>& clips)
{
    if (std::none_of(clips.begin(), clips.end(), [](const auto& clip) { return clip.rack != nullptr; })) return;
    auto candidate = clips;
    std::unordered_set<EffectOwner> seen;
    partitionLane(candidate, seen);
    clips.swap(candidate);
}

void partitionClipRacks(std::vector<Track>& tracks)
{
    if (std::none_of(tracks.begin(), tracks.end(), [](const auto& track) {
        return std::any_of(track.clips.begin(), track.clips.end(), [](const auto& clip) { return clip.rack != nullptr; });
    })) return;
    auto candidate = tracks;
    std::unordered_set<EffectOwner> seen;
    for (auto& track : candidate) partitionLane(track.clips, seen);
    tracks.swap(candidate);
}

bool canSlipClip(const Project& project, TrackId id, std::size_t index, FramePos offset) noexcept
{
    const auto* track = project.track(id);
    if (!track || index >= track->clips.size() || offset < 0) return false;
    const auto& clip = track->clips[index];
    const auto* source = project.source(clip.source);
    if (!source) return false;
    const auto frames = clip.sourceFrames(FramePos(source->audio.frames()));
    return clip.validSource(FramePos(source->audio.frames())) && offset <= frames && clip.length <= frames-offset;
}

bool slipClip(Project& project, TrackId track, std::size_t index, FramePos offset)
{
    if (!canSlipClip(project, track, index, offset)) return false;
    auto& clip = project.track(track)->clips[index];
    if (clip.sourceOffset == offset) return false;
    auto changed = clip;
    changed.sourceOffset = offset;
    if (changed.rack) {
        auto rack = std::make_shared<ClipRack>(*forkClipRack(changed.rack));
        rack->sourceOffset = offset - changed.effectOffset;
        changed.rack = std::move(rack);
        if (!changed.hasValidEffects()) return false;
    }
    auto tracks = project.tracks();
    tracks[*project.trackIndex(track)].clips[index] = std::move(changed);
    partitionClipRacks(tracks);
    auto candidate = project;
    candidate.tracks() = std::move(tracks);
    if (!validEffects(candidate)) return false;
    // Slip changes neither track nor clip membership. Keep existing references
    // valid while publishing the prepared offset and immutable rack partitions.
    clip.sourceOffset = offset;
    for (std::size_t t = 0; t < project.tracks().size(); ++t)
        for (std::size_t c = 0; c < project.tracks()[t].clips.size(); ++c)
            project.tracks()[t].clips[c].rack = std::move(candidate.tracks()[t].clips[c].rack);
    return true;
}

ClipTrimLimits clipTrimLimits(const Project& project, TrackId id, std::size_t index) noexcept
{
    const auto* track = project.track(id);
    if (!track || index >= track->clips.size()) return {};
    const auto& clip = track->clips[index];
    const auto* source = project.source(clip.source);
    // A crossfade is shaped by its own edges, not trimmed.
    if (!source || source->crossfade || !clip.validSource(FramePos(source->audio.frames()))) return {};
    const auto frames = clip.sourceFrames(FramePos(source->audio.frames()));
    ClipTrimLimits limits{true, clip.timelineStart - std::min(clip.sourceOffset, clip.timelineStart),
                          clip.timelineEnd() + (frames - clip.sourceOffset - clip.length)};
    if (clip.rack) {
        if (!clip.hasValidEffects()) return {};
        limits.earliestStart = std::max(limits.earliestStart, clip.timelineStart - clip.effectOffset);
        limits.latestEnd = std::min(limits.latestEnd, clip.timelineStart + (clip.rack->length - clip.effectOffset));
    }
    if (clip.volumeEnvelope.hasData() || clip.panEnvelope.hasData())
        limits.earliestStart = std::max(limits.earliestStart, clip.timelineStart - clip.envelopeOffset);
    if (index > 0) limits.earliestStart = std::max(limits.earliestStart, track->clips[index - 1].timelineEnd());
    if (index + 1 < track->clips.size()) limits.latestEnd = std::min(limits.latestEnd, track->clips[index + 1].timelineStart);
    return limits;
}

bool trimClip(Project& project, TrackId id, std::size_t index, FramePos start, FramePos end)
{
    const auto limits = clipTrimLimits(project, id, index);
    if (!limits.valid || start < limits.earliestStart || end > limits.latestEnd || start >= end) return false;
    const auto& original = project.track(id)->clips[index];
    if (start == original.timelineStart && end == original.timelineEnd()) return false;
    auto changed = original;
    const FramePos shift = start - original.timelineStart;   // how far the left edge moved
    changed.timelineStart = start;
    changed.sourceOffset += shift;
    changed.length = end - start;
    if (changed.volumeEnvelope.hasData() || changed.panEnvelope.hasData()) changed.envelopeOffset += shift;
    else changed.envelopeOffset = std::max<FramePos>(0, changed.envelopeOffset + shift);
    if (changed.rack) changed.effectOffset += shift;
    // A whole fade stays on its edge and fits the clip; part of a split fade is kept.
    if (changed.fadeInOffset == 0) changed.fadeIn = std::min(changed.fadeIn, changed.length);
    if (changed.fadeOutOffset == 0) changed.fadeOut = std::min(changed.fadeOut, changed.length);
    if (!changed.hasValidFades() || !changed.hasValidEnvelopes() || !changed.hasValidEffects()) return false;
    auto tracks = project.tracks();
    tracks[*project.trackIndex(id)].clips[index] = std::move(changed);
    partitionClipRacks(tracks);
    auto candidate = project;
    candidate.tracks() = std::move(tracks);
    if (!validEffects(candidate) || !isWellFormed(candidate.track(id)->clips)) return false;
    // Like slip: the same tracks and clips, so references to them stay good.
    project.track(id)->clips[index] = candidate.track(id)->clips[index];
    for (std::size_t t = 0; t < project.tracks().size(); ++t)
        for (std::size_t c = 0; c < project.tracks()[t].clips.size(); ++c)
            project.tracks()[t].clips[c].rack = std::move(candidate.tracks()[t].clips[c].rack);
    return true;
}

bool canMoveClip(const Project& project, TrackId from, std::size_t index,
                 TrackId to, FramePos position) noexcept
{
    const auto* origin = project.track(from);
    const auto* target = project.track(to);
    if (!origin || !target || index >= origin->clips.size() || position < 0)
        return false;
    const auto& clip = origin->clips[index];
    if (clip.length <= 0 || position > std::numeric_limits<FramePos>::max() - clip.length)
        return false;
    for (std::size_t i = 0; i < target->clips.size(); ++i) {
        if (from == to && i == index)
            continue;
        if (target->clips[i].intersects(position, position + clip.length))
            return false;
    }
    return true;
}

bool moveClip(Project& project, TrackId from, std::size_t index,
              TrackId to, FramePos position)
{
    if (!canMoveClip(project, from, index, to, position))
        return false;
    auto tracks = project.tracks();
    auto& origin = tracks[*project.trackIndex(from)];
    Clip moved = origin.clips[index];
    if (from == to && moved.timelineStart == position)
        return false;
    moved.timelineStart = position;
    // Prepare first, so an allocation failure cannot remove the original clip.
    auto destination = tracks[*project.trackIndex(to)].clips;
    if (from == to)
        destination.erase(destination.begin() + static_cast<std::ptrdiff_t>(index));
    destination.push_back(moved);
    sortAndPrune(destination);
    if (from != to)
        origin.clips.erase(origin.clips.begin() + static_cast<std::ptrdiff_t>(index));
    tracks[*project.trackIndex(to)].clips = std::move(destination);
    partitionClipRacks(tracks);
    auto candidate = project;
    candidate.tracks() = std::move(tracks);
    if (!validEffects(candidate)) return false;
    project.tracks() = std::move(candidate.tracks());
    return true;
}

void splitAt(std::vector<Clip>& clips, FramePos position)
{
    for (std::size_t i = 0; i < clips.size(); ++i) {
        const Clip& c = clips[i];
        // Strictly inside: a split exactly on a boundary already exists.
        if (position <= c.timelineStart || position >= c.timelineEnd())
            continue;

        const Clip head = c.headBefore(position);
        Clip tail = c.tailAfter(position);
        tail.forkIdentity();
        auto candidate = clips;
        candidate[i] = head;
        candidate.insert(candidate.begin() + static_cast<std::ptrdiff_t>(i) + 1, tail);
        clips.swap(candidate);
        return;
    }
}

void removeRange(std::vector<Clip>& destination, FramePos start, FramePos end)
{
    auto clips = destination;
    if (end <= start)
        return;

    splitAt(clips, start);
    splitAt(clips, end);

    const FramePos span = end - start;

    std::vector<Clip> kept;
    kept.reserve(clips.size());
    for (const Clip& c : clips) {
        if (c.timelineStart >= start && c.timelineEnd() <= end)
            continue;                                   // entirely inside the cut

        Clip moved = c;
        if (moved.timelineStart >= end)
            moved.timelineStart -= span;                // close the gap
        kept.push_back(moved);
    }

    clips = std::move(kept);
    sortAndPrune(clips);
    partitionClipRacks(clips);
    destination.swap(clips);
}

bool removeRangeAllTracks(Project& project, FramePos start, FramePos end)
{
    start = std::max(FramePos{0}, start);
    end = std::min(project.length(), end);
    if (end <= start) return false;
    if (!validateAutomation(project).empty()) return false;
    // Prepare only editable metadata: workers may be reading the source deque.
    auto tracks=project.tracks();
    auto master=project.masterVolumeAutomation();
    auto inserts=project.masterInserts();
    std::size_t count=0;
    const auto clipPoints=[&](const Clip& clip) {
        if (!clip.hasValidEnvelopes()) return false;
        count+=clip.volumeEnvelope.points.size()+clip.panEnvelope.points.size();
        return count<=kMaxProjectAutomationPoints;
    };
    for (const auto& source:project.sources()) if (source && source->crossfade)
        if (!clipPoints(source->crossfade->left) || !clipPoints(source->crossfade->right)) return false;
    const auto trimCurve=[&](auto& curve) {
        if (!curve.removeTime(start,end)) return false;
        count+=curve.points.size();
        return count<=kMaxProjectAutomationPoints;
    };
    const auto trimInserts=[&](auto& list) {
        for (auto& insert:list) for (auto& control:insert.controls) {
            if (!trimCurve(control.automation)) return false;
            for (auto& instance : control.instanceAutomation) if (instance && !trimCurve(instance->curve)) return false;
        }
        return true;
    };
    for (Track& track:tracks) {
        if (!trimCurve(track.volumeAutomation) || !trimCurve(track.panAutomation)
            || !trimInserts(track.inserts)) return false;
        removeRange(track.clips,start,end);
        for (const auto& clip:track.clips) if (!clipPoints(clip)) return false;
    }
    if (!trimCurve(master) || !trimInserts(inserts)) return false;
    auto candidate=project;
    candidate.tracks()=std::move(tracks);
    candidate.masterVolumeAutomation()=std::move(master);
    candidate.masterInserts()=std::move(inserts);
    if (!validateAutomation(candidate).empty()) return false;
    if (!validEffects(candidate)) return false;
    project.tracks()=std::move(candidate.tracks());
    project.masterVolumeAutomation()=std::move(candidate.masterVolumeAutomation());
    project.masterInserts()=std::move(candidate.masterInserts());
    return true;
}

bool removeRangesOnTracks(Project& project, const std::vector<TrackId>& ids,
                          std::vector<std::pair<FramePos, FramePos>> ranges, bool moveMarkers)
{
    std::erase_if(ranges, [](const auto& r) { return r.second <= r.first; });
    if (ranges.empty() || ids.empty()) return false;
    std::sort(ranges.begin(), ranges.end());
    for (std::size_t i = 1; i < ranges.size(); ++i) if (ranges[i].first < ranges[i - 1].second) return false;
    if (!validateAutomation(project).empty()) return false;
    auto candidate = project;
    for (auto& track : candidate.tracks()) {
        if (std::find(ids.begin(), ids.end(), track.id) == ids.end()) continue;
        // The latest first, so the earlier positions still hold.
        for (auto r = ranges.rbegin(); r != ranges.rend(); ++r) {
            if (!track.volumeAutomation.removeTime(r->first, r->second) || !track.panAutomation.removeTime(r->first, r->second))
                return false;
            for (auto& insert : track.inserts) for (auto& control : insert.controls) {
                if (!control.automation.removeTime(r->first, r->second)) return false;
                for (auto& instance : control.instanceAutomation)
                    if (instance && !instance->curve.removeTime(r->first, r->second)) return false;
            }
            removeRange(track.clips, r->first, r->second);
        }
    }
    if (moveMarkers) {
        // Where a position lands once every range before it is gone.
        const auto moved = [&](FramePos at) {
            FramePos shift = 0;
            for (const auto& [start, end] : ranges) {
                if (at >= end) shift += end - start;
                else if (at > start) { shift += at - start; break; }
                else break;
            }
            return at - shift;
        };
        for (auto& marker : candidate.markers()) {
            const auto start = moved(marker.position);
            const auto end = moved(marker.end());
            marker.position = start;
            if (marker.length > 0) marker.length = std::max<FramePos>(1, end - start);
        }
    }
    if (!validateAutomation(candidate).empty() || !validEffects(candidate)) return false;
    project.tracks() = std::move(candidate.tracks());
    project.markers() = std::move(candidate.markers());
    return true;
}

void clearRange(std::vector<Clip>& destination, FramePos start, FramePos end)
{
    auto clips = destination;
    if (end <= start)
        return;

    splitAt(clips, start);
    splitAt(clips, end);

    // After the splits, every clip is entirely inside the range or entirely outside
    // it. What is inside goes; what is outside does not move -- that is the whole
    // difference from removeRange.
    clips.erase(std::remove_if(clips.begin(), clips.end(),
                               [start, end](const Clip& c) {
                                   return c.timelineStart >= start
                                       && c.timelineEnd() <= end;
                               }),
                clips.end());
    sortAndPrune(clips);
    partitionClipRacks(clips);
    destination.swap(clips);
}

void trimToRange(std::vector<Clip>& clips, FramePos start, FramePos end)
{
    if (end <= start) {
        clips.clear();
        return;
    }

    auto candidate = copyRange(clips, start, end);
    sortAndPrune(candidate);
    partitionClipRacks(candidate);
    clips.swap(candidate);
}

std::vector<Clip> copyRange(const std::vector<Clip>& clips, FramePos start, FramePos end)
{
    std::vector<Clip> working;
    if (end <= start) return working;
    for (const auto& original : clips) {
        if (!original.intersects(start, end)) continue;
        auto view = original;
        if (view.timelineStart < start) view = view.tailAfter(start);
        if (view.timelineEnd() > end) view = view.headBefore(end);
        view.timelineStart -= start;
        working.push_back(std::move(view));
    }
    return working;
}

void insertAt(std::vector<Clip>& destination, const std::vector<Clip>& incoming,
              FramePos position)
{
    auto clips = destination;
    if (incoming.empty())
        return;

    auto copied = incoming;
    ClipRackForkMap forks;
    forkClipCopies(copied, forks);
    const FramePos span = totalLength(incoming);
    splitAt(clips, position);

    for (Clip& c : clips) {
        if (c.timelineStart >= position)
            c.timelineStart += span;
    }

    for (const Clip& c : copied) {
        Clip placed = c;
        placed.timelineStart += position;
        clips.push_back(placed);
    }

    sortAndPrune(clips);
    partitionClipRacks(clips);
    destination.swap(clips);
}

void overwriteAt(std::vector<Clip>& destination, const std::vector<Clip>& incoming,
                 FramePos position)
{
    auto clips = destination;
    if (incoming.empty() || position < 0)
        return;

    const FramePos span = totalLength(incoming);
    if (span <= 0)
        return;

    auto copied = incoming;
    ClipRackForkMap forks;
    forkClipCopies(copied, forks);
    clearRange(clips, position, position + span);
    for (const Clip& c : copied) {
        if (c.length <= 0 || c.timelineStart < 0
            || c.length > std::numeric_limits<FramePos>::max() - c.timelineStart)
            continue;
        Clip placed = c;
        placed.timelineStart += position;
        clips.push_back(placed);
    }
    sortAndPrune(clips);
    partitionClipRacks(clips);
    destination.swap(clips);
}

std::vector<Clip> withClipGroups(const Clip& rendered, const std::vector<Clip>& ownership)
{
    if (!isWellFormed({rendered}) || !isWellFormed(ownership))
        throw std::invalid_argument("rendered group membership needs valid clip geometry");
    struct Interval { FramePos start, end; ClipGroupId group; };
    std::vector<Interval> intervals;
    auto cursor = rendered.timelineStart;
    const auto end = rendered.timelineEnd();
    const auto append = [&](FramePos to, ClipGroupId group) {
        if (to <= cursor) return;
        if (!intervals.empty() && intervals.back().group == group) intervals.back().end = to;
        else intervals.push_back({cursor, to, group});
        cursor = to;
    };
    for (const auto& clip : ownership) {
        if (clip.timelineEnd() <= cursor) continue;
        if (clip.timelineStart >= end) break;
        append(std::min(clip.timelineStart, end), kInvalidClipGroup);
        append(std::min(clip.timelineEnd(), end), clip.group);
        if (cursor == end) break;
    }
    append(end, kInvalidClipGroup);
    std::vector<Clip> result;
    result.reserve(intervals.size());
    for (const auto& interval : intervals) {
        auto clip = rendered;
        if (!result.empty()) clip.forkIdentity();
        if (interval.start > clip.timelineStart) clip = clip.tailAfter(interval.start);
        if (interval.end < clip.timelineEnd()) clip = clip.headBefore(interval.end);
        clip.group = interval.group;
        result.push_back(clip);
    }
    return result;
}

void overwriteRendered(std::vector<Clip>& clips, const Clip& rendered,
                       const std::vector<Clip>& ownership)
{
    auto incoming = withClipGroups(rendered, ownership);
    for (auto& clip : incoming) clip.timelineStart -= rendered.timelineStart;
    overwriteAt(clips, incoming, rendered.timelineStart);
}

void applyGain(std::vector<Clip>& clips, FramePos start, FramePos end, float gain)
{
    if (end <= start)
        return;

    // Splitting at both edges is what confines the change to the selection.
    splitAt(clips, start);
    splitAt(clips, end);

    for (Clip& c : clips) {
        if (c.timelineStart >= start && c.timelineEnd() <= end)
            c.gain *= gain;
    }
}

void applyFadeIn(std::vector<Clip>& clips, FramePos start, FramePos end)
{
    if (end <= start)
        return;

    splitAt(clips, start);
    splitAt(clips, end);

    const FramePos span = end - start;
    for (Clip& c : clips) {
        if (c.timelineStart >= start && c.timelineEnd() <= end) {
            // The ramp is measured across the whole selection, so a selection covering
            // several clips fades continuously rather than restarting on each one.
            c.fadeIn = span;
            c.fadeInOffset = c.timelineStart - start;
            c.fadeInShape = FadeShape::Linear;
            c.fadeInCurve = 0;
        }
    }
}

void applyFadeOut(std::vector<Clip>& clips, FramePos start, FramePos end)
{
    if (end <= start)
        return;

    splitAt(clips, start);
    splitAt(clips, end);

    const FramePos span = end - start;
    for (Clip& c : clips) {
        if (c.timelineStart >= start && c.timelineEnd() <= end) {
            c.fadeOut = span;
            c.fadeOutOffset = end - c.timelineEnd();
            c.fadeOutShape = FadeShape::Linear;
            c.fadeOutCurve = 0;
        }
    }
}

FramePos totalLength(const std::vector<Clip>& clips) noexcept
{
    FramePos end = 0;
    for (const Clip& c : clips)
        end = std::max(end, c.timelineEnd());
    return end;
}

bool isWellFormed(const std::vector<Clip>& clips) noexcept
{
    FramePos previousEnd = 0;
    for (const Clip& c : clips) {
        if (c.length <= 0 || c.timelineStart < 0
            || c.timelineStart > std::numeric_limits<FramePos>::max() - c.length
            || c.sourceOffset < 0 || c.sourceOffset > std::numeric_limits<FramePos>::max() - c.length)
            return false;
        if (c.timelineStart < previousEnd)
            return false;                               // out of order or overlapping
        if (c.sourceOffset < 0)
            return false;
        if (!c.hasValidFades() || !c.hasValidEnvelopes() || !c.hasValidEffects())
            return false;
        previousEnd = c.timelineEnd();
    }
    return true;
}

} // namespace omawave::session::edits
