#include "project_rate.h"
#include "edits.h"
#include "clip_envelope.h"
#include "effect_racks.h"
#include "io/resampler.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace omawave::session {
FramePos ratePosition(FramePos frame, std::uint32_t from, std::uint32_t to)
{
    if (!from || !to || frame < 0) throw std::runtime_error("invalid sample-rate position");
    const long double value = std::round(static_cast<long double>(frame) * to / from);
    if (value > std::numeric_limits<FramePos>::max()) throw std::runtime_error("rate conversion would exceed the timeline");
    return FramePos(value);
}
namespace {
void scaleCurve(AutomationCurve& curve, std::uint32_t from, std::uint32_t to)
{
    std::vector<AutomationPoint> points;
    for (auto point : curve.points) {
        point.frame = ratePosition(point.frame, from, to);
        if (!points.empty() && points.back().frame == point.frame) points.back() = point;
        else points.push_back(point);
    }
    curve.points = std::move(points);
}
void scaleInserts(std::vector<Insert>& inserts, std::uint32_t from, std::uint32_t to)
{
    for (auto& insert : inserts) for (auto& control : insert.controls) {
        scaleCurve(control.automation, from, to);
        for (auto& instance : control.instanceAutomation) if (instance) scaleCurve(instance->curve, from, to);
    }
}
// Retained source recipes may hold an older immutable version of a live owner.
// Only aliases of that exact version share a converted specification.
using RateRacks = std::unordered_map<const ClipRack*, std::shared_ptr<const ClipRack>>;
void scaleEffects(Clip& out, const Clip& original, std::uint32_t from, std::uint32_t to, RateRacks& racks)
{
    if (!original.rack) return;
    if (!original.hasValidEffects()) throw std::runtime_error("invalid rate-conversion clip effect region");
    const auto origin = original.timelineStart - original.effectOffset;
    const auto scaledOrigin = origin < 0 ? -ratePosition(-origin, from, to) : ratePosition(origin, from, to);
    if (scaledOrigin < 0 && out.timelineStart > std::numeric_limits<FramePos>::max() + scaledOrigin)
        throw std::runtime_error("rate conversion would exceed clip effect coordinates");
    const auto offset = out.timelineStart - scaledOrigin;
    auto found = racks.find(original.rack.get());
    if (found == racks.end()) {
        if (origin > 0 && original.rack->length > std::numeric_limits<FramePos>::max() - origin)
            throw std::runtime_error("clip effect region exceeds the timeline");
        const auto end = ratePosition(origin + original.rack->length, from, to);
        if (scaledOrigin < 0 && end > std::numeric_limits<FramePos>::max() + scaledOrigin)
            throw std::runtime_error("rate conversion would exceed clip effect coordinates");
        auto rack = std::make_shared<ClipRack>(*original.rack);
        rack->source = out.source; rack->sourceOffset = out.sourceOffset - offset;
        rack->length = end - scaledOrigin; rack->elastic = out.elastic;
        const auto scale = [&](AutomationCurve& curve) {
            FramePos previous = -1;
            for (auto& point : curve.points) {
                point.frame = ratePosition(point.frame, from, to);
                if (point.frame <= previous)
                    throw std::runtime_error("rate conversion would merge clip effect automation points; separate them first");
                previous = point.frame;
            }
        };
        for (auto& insert : rack->inserts) for (auto& control : insert.controls) {
            scale(control.automation);
            for (auto& instance : control.instanceAutomation) if (instance) scale(instance->curve);
        }
        if (const auto error = validateAutomation(rack->inserts); !error.empty()) throw std::runtime_error(error);
        found = racks.emplace(original.rack.get(), std::move(rack)).first;
    }
    out.rack = found->second; out.effectOffset = offset;
    if (!out.hasValidEffects()) throw std::runtime_error("rate conversion cannot preserve this clip effect region");
}
Clip scaledClip(const Clip& clip, const Project& project, const std::vector<SourceId>& mapped,
                std::uint32_t from, std::uint32_t to, RateRacks& racks)
{
    if (clip.timelineStart < 0 || clip.length <= 0 || clip.timelineStart > std::numeric_limits<FramePos>::max() - clip.length
        || std::size_t(clip.source) >= mapped.size()) throw std::runtime_error("invalid rate-conversion clip");
    auto out = clip;
    out.timelineStart = ratePosition(clip.timelineStart, from, to);
    out.length = ratePosition(clip.timelineEnd(), from, to) - out.timelineStart;
    if (!out.length) throw std::runtime_error("rate conversion would collapse a short clip; merge it first");
    out.source = mapped[std::size_t(clip.source)];
    const auto* source = project.source(out.source);
    if (!source) throw std::runtime_error("rate conversion lost a clip source");
    if (out.elastic) {
        auto& spec = *out.elastic;
        spec.inputStart = ratePosition(spec.inputStart,from,to);
        spec.inputFrames = ratePosition(clip.elastic->inputStart+clip.elastic->inputFrames,from,to)-spec.inputStart;
        spec.outputFrames = ratePosition(spec.outputFrames,from,to);
        if (spec.inputStart > FramePos(source->audio.frames()) || spec.inputFrames > FramePos(source->audio.frames())-spec.inputStart)
            throw std::runtime_error("rate conversion cannot preserve the elastic input interval");
    }
    const auto available = out.sourceFrames(FramePos(source->audio.frames()));
    // Where the clip reads follows where it sits: the distance between the two
    // is what converts. Fragments of one take share that distance, so each still
    // begins on the sample after the one before it ended.
    const auto lead = clip.sourceOffset - clip.timelineStart;
    const auto offset = out.timelineStart + (lead < 0 ? -ratePosition(-lead, from, to) : ratePosition(lead, from, to));
    out.sourceOffset = std::clamp<FramePos>(offset, 0, available);
    // The timeline ends and the resampled source length are rounded separately,
    // so a clip reaching the end of its source can come out a frame or two longer
    // than the audio now is. It gives those frames up at its end.
    if (out.length > available - out.sourceOffset) {
        if (out.length - (available - out.sourceOffset) > 2) throw std::runtime_error("rate conversion would shorten a clip's source");
        out.length = available - out.sourceOffset;
        if (out.length <= 0) throw std::runtime_error("rate conversion would collapse a short clip; merge it first");
    }
    if (!out.validSource(FramePos(source->audio.frames()))) throw std::runtime_error("rate conversion cannot preserve this elastic ratio");
    scaleClipEnvelopes(out,to,from);
    out.fadeIn = ratePosition(clip.fadeIn, from, to);
    out.fadeOut = ratePosition(clip.fadeOut, from, to);
    out.fadeInOffset = out.fadeIn ? std::min(out.fadeIn - 1, ratePosition(clip.fadeInOffset, from, to)) : 0;
    out.fadeOutOffset = out.fadeOut ? std::min(out.fadeOut - 1, ratePosition(clip.fadeOutOffset, from, to)) : 0;
    scaleEffects(out, clip, from, to, racks);
    return out;
}
} // namespace
SampleEditResult convertProjectRate(Project project, std::uint32_t rate, std::uint32_t exportBits,
                                     const std::atomic<bool>* cancel)
{
    auto cancelled = [&] { return cancel && cancel->load(std::memory_order_relaxed); };
    try {
        if (rate < 8000 || rate > 192000 || (exportBits != 16 && exportBits != 24 && exportBits != 32))
            throw std::runtime_error("choose a sample rate from 8 to 192 kHz and a 16/24/float32 delivery type");
        if (cancelled()) return {std::nullopt, true, {}};
        if (const auto error = validateAutomation(project); !error.empty()) throw std::runtime_error(error);
        if (const auto error = validateClipEnvelopes(project); !error.empty()) throw std::runtime_error(error);
        bool withEffects = false;
        for (const auto& track : project.tracks()) for (const auto& clip : track.clips) withEffects |= bool(clip.rack);
        if (withEffects)
            if (const auto error = validateClipRacks(project); !error.empty()) throw std::runtime_error(error);
        const auto oldRate = project.sampleRate();
        if (oldRate == rate && project.defaultExportBits() == exportBits) return {};
        for (const auto& track : project.tracks()) {
            if (!edits::isWellFormed(track.clips)) throw std::runtime_error("invalid track geometry");
            for (const auto& clip : track.clips) {
                const auto* source = project.source(clip.source);
                if (!source || source->sampleRate != oldRate) throw std::runtime_error("a live clip does not match the project sample rate");
            }
        }
        project.setDefaultExportBits(exportBits);
        if (oldRate == rate) return {std::move(project), false, {}};
        const auto originalCount = project.sources().size();
        std::vector<SourceId> mapped(originalCount, kInvalidSource);
        project.setSampleRate(rate);
        for (std::size_t i = 0; i < originalCount; ++i) {
            if (cancelled()) return {std::nullopt, true, {}};
            const auto* source = project.source(SourceId(i));
            if (!source) continue;
            if (source->sampleRate == rate) { mapped[i] = source->id; continue; }
            if (!source->sampleRate) throw std::runtime_error("a source has no sample rate");
            io::Resampler resampler(source->sampleRate, rate);
            auto audio = resampler.process(source->audio, cancel);
            if (cancelled()) return {std::nullopt, true, {}};
            std::optional<CrossfadeRecipe> recipe;
            if (source->crossfade) {
                const auto& old = *source->crossfade;
                RateRacks recipeRacks;
                recipe = CrossfadeRecipe{
                    scaledClip(old.left, project, mapped, source->sampleRate, rate, recipeRacks),
                    scaledClip(old.right, project, mapped, source->sampleRate, rate, recipeRacks),
                    ratePosition(old.start, source->sampleRate, rate),
                    ratePosition(old.end, source->sampleRate, rate), old.equalPower};
                // A transition's independently rounded interval can be a frame
                // longer/shorter than resampling its local buffer.
                const auto frames = std::size_t(recipe->end - recipe->start);
                if (audio.frames() != frames) audio.resize(audio.channels(), frames);
                if (!validCrossfadeRecipe(project, *recipe, audio.frames(), audio.channels()))
                    throw std::runtime_error("rate conversion cannot preserve this editable transition's geometry");
            }
            audio.storeOnDisk(AudioBuffer::kDiskThreshold, cancel);
            if (cancelled()) return {std::nullopt, true, {}};
            // What the file said about itself follows the audio to the new rate.
            AudioMetadata provenance = source->provenance;
            for (auto& marker : provenance.markers) {
                const auto end = rescaleFrames(marker.position + marker.length, source->sampleRate, rate);
                marker.position = rescaleFrames(marker.position, source->sampleRate, rate);
                marker.length = std::max<std::int64_t>(0, end - marker.position);
            }
            if (provenance.bext.present)
                provenance.bext.timeReference = rescaleFrames(provenance.bext.timeReference, source->sampleRate, rate);
            mapped[i] = project.addSource(source->name + " (" + std::to_string(rate) + " Hz)", source->originalPath,
                std::move(audio), source->originalSampleRate.value_or(source->sampleRate), Uuid{}, std::move(recipe),
                0, std::move(provenance));
        }
        RateRacks racks;
        for (auto& track : project.tracks()) {
            for (auto& clip : track.clips) clip = scaledClip(clip, project, mapped, oldRate, rate, racks);
            scaleCurve(track.volumeAutomation, oldRate, rate); scaleCurve(track.panAutomation, oldRate, rate);
            scaleInserts(track.inserts, oldRate, rate);
        }
        for (auto& marker : project.markers()) {
            if (marker.position < 0 || marker.length < 0 || marker.position > std::numeric_limits<FramePos>::max() - marker.length)
                throw std::runtime_error("invalid marker geometry");
            const auto end = ratePosition(marker.end(), oldRate, rate);
            marker.position = ratePosition(marker.position, oldRate, rate); marker.length = end - marker.position;
        }
        scaleCurve(project.masterVolumeAutomation(), oldRate, rate); scaleInserts(project.masterInserts(), oldRate, rate);
        if (project.metadata().timeReferenceSet)
            project.metadata().timeReference = rescaleFrames(project.metadata().timeReference, oldRate, rate);
        // The picture keeps its place in seconds, like a marker.
        if (!project.video().empty()) {
            const bool negative = project.video().offset < 0;
            const auto magnitude = rescaleFrames(negative ? -project.video().offset : project.video().offset, oldRate, rate);
            project.video().offset = negative ? -magnitude : magnitude;
        }
        if (cancelled()) return {std::nullopt, true, {}};
        if (const auto error = validateAutomation(project); !error.empty()) throw std::runtime_error(error);
        if (const auto error = validateClipEnvelopes(project); !error.empty()) throw std::runtime_error(error);
        if (withEffects)
            if (const auto error = validateClipRacks(project); !error.empty()) throw std::runtime_error(error);
        return {std::move(project), false, {}};
    } catch (const std::exception& e) { return {std::nullopt, false, e.what()}; }
}
} // namespace omawave::session
