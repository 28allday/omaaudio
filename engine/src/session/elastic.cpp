#include "elastic.h"
#include "clip_envelope.h"
#include "frame_scale.h"
#include "edits.h"
#include "effect_racks.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace omawave::session {
void rescaleClipEffects(Clip& clip, const Clip& original, FramePos numerator, FramePos denominator) {
    if (!original.rack) return;
    if (!original.hasValidEffects() || numerator <= 0 || denominator <= 0)
        throw std::runtime_error("invalid clip effect retiming");
    if (numerator == denominator && clip.source == original.source && clip.sourceOffset == original.sourceOffset
        && clip.elastic == original.elastic && clip.length == original.length) return;
    auto rack = std::make_shared<ClipRack>(*forkClipRack(original.rack));
    const auto offset = scaledFrames(original.effectOffset, numerator, denominator);
    rack->length = scaledFrames(original.rack->length, numerator, denominator);
    // Rounded view boundaries can consume one more frame than a separately
    // rounded hidden origin. Keep all of the visible view inside its region.
    if (clip.length <= 0 || offset > std::numeric_limits<FramePos>::max() - clip.length)
        throw std::runtime_error("retimed clip effects exceed the timeline");
    rack->length = std::max(rack->length, offset + clip.length);
    rack->source = clip.source; rack->sourceOffset = clip.sourceOffset - offset; rack->elastic = clip.elastic;
    const auto scale = [&](AutomationCurve& curve) {
        FramePos previous = -1;
        for (auto& point : curve.points) {
            point.frame = scaledFrames(point.frame, numerator, denominator);
            if (point.frame <= previous)
                throw std::runtime_error("stretch would merge clip effect automation points; separate them first");
            previous = point.frame;
        }
    };
    for (auto& insert : rack->inserts) for (auto& control : insert.controls) {
        scale(control.automation);
        for (auto& instance : control.instanceAutomation) if (instance) scale(instance->curve);
    }
    if (!rack->validOrigin()) throw std::runtime_error("retimed clip effect origin exceeds its supported range");
    if (const auto error = validateAutomation(rack->inserts); !error.empty()) throw std::runtime_error(error);
    clip.rack = std::move(rack); clip.effectOffset = offset;
}

std::string validateElastic(const Project& project) {
    std::size_t runs = 0;
    for (const auto& track : project.tracks()) {
        const Clip* previous = nullptr;
        for (const auto& clip : track.clips) {
            if (!clip.elastic) { previous = nullptr; continue; }
            const auto* source = project.source(clip.source);
            if (!source || !source->audio.channels() || source->audio.channels() > 2
                || !clip.validSource(FramePos(source->audio.frames())) || clip.timelineStart < 0
                || !std::isfinite(clip.gain) || clip.gain < 0 || !clip.hasValidFades() || !clip.hasValidEnvelopes()
                || clip.timelineStart > std::numeric_limits<FramePos>::max()-clip.length)
                return "invalid elastic clip mapping or channel layout";
            if (!previous || previous->source != clip.source || previous->elastic != clip.elastic
                || previous->timelineEnd() != clip.timelineStart || previous->sourceOffset+previous->length != clip.sourceOffset) ++runs;
            previous = &clip;
        }
    }
    return runs > 64 ? "at most 64 separate elastic clip runs are supported; reset or render some clips first" : std::string{};
}
ElasticSpec elasticInput(const Clip& clip) {
    if (!clip.elastic) return {clip.sourceOffset, clip.length, clip.length, 0};
    const auto& e = *clip.elastic;
    if (!e.valid() || clip.sourceOffset < 0 || clip.length <= 0
        || clip.sourceOffset > e.outputFrames || clip.length > e.outputFrames-clip.sourceOffset)
        throw std::runtime_error("invalid elastic source mapping");
    const auto start = scaledFrames(clip.sourceOffset,e.inputFrames,e.outputFrames);
    const auto end = scaledFrames(clip.sourceOffset+clip.length,e.inputFrames,e.outputFrames);
    return {e.inputStart+start,end-start,clip.length,e.semitones};
}
Clip makeElasticClip(const Project& project, const Clip& current, FramePos frames, float semitones) {
    const auto* source = project.source(current.source);
    if (!source || source->audio.channels() > 2 || !source->audio.channels()
        || !current.validSource(FramePos(source->audio.frames()))) throw std::runtime_error("elastic processing needs a valid mono or stereo clip");
    auto spec = elasticInput(current); spec.outputFrames = frames; spec.semitones = semitones;
    if (!spec.valid() || spec.inputFrames < project.sampleRate()/20 || frames < project.sampleRate()/20)
        throw std::runtime_error("choose 50–200% of the original duration, at least 50 ms, and pitch from -12 to +12 semitones");
    // Avoid rebasing an unchanged split mapping: that would restart its phase.
    if (frames == current.length && semitones == (current.elastic ? current.elastic->semitones : 0)) return current;
    auto clip = current;
    clip.length = frames; clip.sourceOffset = 0; clip.elastic = spec;
    clip.fadeIn = scaledFrames(current.fadeIn,frames,current.length);
    clip.fadeOut = scaledFrames(current.fadeOut,frames,current.length);
    clip.fadeInOffset = clip.fadeIn ? std::min(clip.fadeIn-1,scaledFrames(current.fadeInOffset,frames,current.length)) : 0;
    clip.fadeOutOffset = clip.fadeOut ? std::min(clip.fadeOut-1,scaledFrames(current.fadeOutOffset,frames,current.length)) : 0;
    scaleClipEnvelopes(clip,frames,current.length);
    if (frames == spec.inputFrames && semitones == 0) {
        clip.elastic.reset(); clip.sourceOffset = spec.inputStart;
    }
    rescaleClipEffects(clip, current, frames, current.length);
    return clip;
}
std::string setElasticClip(Project& project, TrackId id, std::size_t index, FramePos frames, float semitones) {
    auto* track = project.track(id);
    if (!track || index >= track->clips.size()) return "choose a clip to stretch";
    const auto current = track->clips[index];
    try {
        const auto clip = makeElasticClip(project, current, frames, semitones);
        if (current.timelineStart < 0 || current.timelineStart > std::numeric_limits<FramePos>::max()-frames
            || (index+1 < track->clips.size() && current.timelineStart+frames > track->clips[index+1].timelineStart))
            return "the elastic clip would overlap the next clip or exceed the timeline";
        auto candidate = project;
        candidate.track(id)->clips[index] = clip;
        edits::partitionClipRacks(candidate.tracks());
        if (const auto error = validateElastic(candidate); !error.empty()) return error;
        if (current.rack) {
            if (const auto error = validateClipRacks(candidate); !error.empty()) return error;
            if (const auto error = validateAutomation(candidate); !error.empty()) return error;
        }
        // No membership changes: keep existing Track/Clip references valid.
        for (std::size_t t = 0; t < project.tracks().size(); ++t)
            for (std::size_t c = 0; c < project.tracks()[t].clips.size(); ++c)
                project.tracks()[t].clips[c] = std::move(candidate.tracks()[t].clips[c]);
        return {};
    } catch (const std::exception& e) { return e.what(); }
}
} // namespace omawave::session
