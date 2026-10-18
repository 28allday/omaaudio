#include "render_plan.h"
#include "session/clip_envelope.h"
#include <stdexcept>
#include "routing_render.h"
#include "stream_cache.h"
#include "elastic_runtime.h"

#include <algorithm>
#include <limits>
#include <new>

namespace omawave::engine {

PlanTrack buildPlanTrack(const session::Project& project,
                         const std::vector<session::Clip>& clips)
{
    // The model and block kernel are integrated before public rack controls.
    // Reject an unbound rack rather than publish a plan which plays it dry.
    for (const auto& clip : clips)
        if (clip.rack) throw std::runtime_error("clip effects require prepared render bindings");
    if (const auto error = session::validateClipEnvelopes(clips); !error.empty()) throw std::runtime_error(error);
    PlanTrack out;
    out.clips.reserve(clips.size());

    for (const session::Clip& c : clips) {
        const session::AudioSource* source = project.source(c.source);
        if (source == nullptr || source->audio.empty() || c.length <= 0)
            continue;   // a clip the audio thread could not read anyway

        PlanClip pc;
        pc.source = &source->audio;
        pc.owner = project.sources()[std::size_t(c.source)];
        pc.elastic = c.elastic;
        pc.timelineStart = c.timelineStart;
        pc.sourceOffset = c.sourceOffset;
        pc.length = c.length;
        pc.gain = c.gain;
        pc.fadeIn = c.fadeIn;
        pc.fadeOut = c.fadeOut;
        pc.fadeInOffset = c.fadeInOffset;
        pc.fadeOutOffset = c.fadeOutOffset;
        pc.fadeInShape = c.fadeInShape;
        pc.fadeOutShape = c.fadeOutShape;
        pc.fadeInCurve = c.fadeInCurve;
        pc.fadeOutCurve = c.fadeOutCurve;
        pc.volumeEnvelope = c.volumeEnvelope; pc.panEnvelope = c.panEnvelope; pc.envelopeOffset = c.envelopeOffset;
        pc.mono = source->audio.channels() == 1;
        if (!pc.mono)
            out.mono = false;
        out.length = std::max(out.length, pc.timelineEnd());
        out.clips.push_back(std::move(pc));
    }

    // indexAt() is a binary search, so the ordering is load-bearing rather than tidy.
    std::sort(out.clips.begin(), out.clips.end(),
              [](const PlanClip& a, const PlanClip& b) {
                  return a.timelineStart < b.timelineStart;
              });

    return out;
}

RenderPlan buildRenderPlan(const session::Project& project, std::uint32_t channels, const RenderPlan* previous)
{
    if (const auto error = session::validateAutomation(project); !error.empty()) throw std::runtime_error(error);
    if (const auto error = session::validateClipEnvelopes(project); !error.empty()) throw std::runtime_error(error);
    RenderPlan plan;
    plan.channels = channels;
    plan.sampleRate = project.sampleRate();
    plan.musicalTime = project.musicalTime();
    plan.routing = session::compileRouting(project);
    plan.masterVolumeAutomation = project.masterVolumeAutomation();
    plan.masterControls = buildControlAutomation(project.masterInserts());
    plan.tracks.reserve(project.tracks().size());

    const bool anySolo = project.anySoloed();

    for (const session::Track& track : project.tracks()) {
        PlanTrack pt = buildPlanTrack(project, track.clips);
        pt.id = track.id;
        pt.gain = plan.routing.active ? track.gain : track.effectiveGain(anySolo);
        pt.muted = track.muted;
        pt.pan = session::panGainsFor(track.pan);
        pt.volumeAutomation = track.volumeAutomation;
        pt.panAutomation = track.panAutomation;
        pt.controls = buildControlAutomation(track.inserts);
        plan.length = std::max(plan.length, pt.length);
        plan.tracks.push_back(std::move(pt));
    }

    const auto count = plan.tracks.size();
    std::vector<bool> own(count, !anySolo), audible(count, !anySolo), key(count, false);
    for (std::size_t i = 0; i < count; ++i) if (project.tracks()[i].soloed) { own[i] = true; audible[i] = true; }
    // Soloing a bus keeps its feeds; soloing a source keeps its downstream buses
    // while excluding unrelated clips on those buses.
    for (auto i = plan.routing.order.rbegin(); i != plan.routing.order.rend(); ++i)
        for (const auto& edge : plan.routing.edges)
            if (!edge.sidechain && edge.to == *i && own[*i]) own[edge.from] = true;
    for (const auto i : plan.routing.order)
        for (const auto& edge : plan.routing.edges)
            if (!edge.sidechain && edge.from == i && audible[i] && edge.to < count) audible[edge.to] = true;
    // External keys continue to drive a soloed path without enabling their main
    // master output. Walk dependencies backwards, including nested key sources.
    for (auto i = plan.routing.order.rbegin(); i != plan.routing.order.rend(); ++i)
        for (const auto& edge : plan.routing.edges)
            if (edge.to == *i && (key[*i] || (edge.sidechain && (audible[*i] || own[*i])))) key[edge.from] = true;
    for (std::size_t i = 0; i < count; ++i) plan.tracks[i].ownClips = own[i] || key[i];
    for (const auto& edge : plan.routing.edges) {
        const bool enabled = edge.to == count ? audible[edge.from]
            : edge.sidechain ? audible[edge.to] || own[edge.to] || key[edge.to]
            : own[edge.to] || audible[edge.from] || (key[edge.from] && key[edge.to]);
        plan.routeEnabled.push_back(enabled);
        if (!edge.sidechain && edge.to < count) {
            plan.tracks[edge.to].receivesAudio = true; plan.tracks[edge.to].mono = false;
        }
    }

    prepareRoutingRender(plan, previous);
    prepareElastic(plan, previous);
    return plan;
}

void prepareRoutingRender(RenderPlan& plan, const RenderPlan* previous)
{
    if (!plan.routing.active || !plan.routing.ok || plan.tracks.size() > RoutingRender::maxTracks
        || !plan.channels || plan.channels > RoutingRender::maxChannels) return;
    if (plan.routingRender && plan.routingRender->fits(plan.tracks.size(), plan.channels)) return;
    if (previous && previous->routingRender && previous->routingRender->fits(plan.tracks.size(), plan.channels))
        plan.routingRender = previous->routingRender;
    else plan.routingRender = std::make_shared<RoutingRender>(plan.tracks.size(), plan.channels);
}

void prepareStreaming(RenderPlan& plan, const RenderPlan* previous, std::size_t cacheBytes)
{
    prepareElastic(plan, previous);
    const auto config = StreamCache::Config::forBytes(cacheBytes);
    for (auto& track : plan.tracks) for (auto& clip : track.clips) {
        clip.realtime = true; clip.stream = nullptr;
        if (clip.elastic || !clip.source || !clip.source->diskStorage()) continue;
        if (!plan.streamCache) {
            plan.streamCache = previous ? previous->streamCache : nullptr;
            if (plan.streamCache && plan.streamCache->requested() != config) plan.streamCache = nullptr;
            if (!plan.streamCache) {
                // A reservation the system refuses falls back to the small default,
                // which then answers for that size until a different one is asked for.
                try { plan.streamCache = std::make_shared<StreamCache>(config); }
                catch (const std::bad_alloc&) {
                    plan.streamCache = std::make_shared<StreamCache>();
                    plan.streamCache->standInFor(config);
                }
            }
        }
        plan.streamCache->retain(clip.source->diskStorage());
        clip.stream = plan.streamCache.get();
    }
}

void prefetchStreaming(const RenderPlan& plan, session::FramePos position, bool realtime, bool readAhead) noexcept
{
    if (!plan.streamCache || position < 0) return;
    const auto distance = readAhead ? session::FramePos(StreamCache::kPageFrames * plan.streamCache->readAhead()) : session::FramePos(0);
    const auto end = position > std::numeric_limits<session::FramePos>::max() - distance
        ? std::numeric_limits<session::FramePos>::max() : position + distance;
    for (const auto& track : plan.tracks)
        for (auto i = track.indexAt(position); i < track.clips.size(); ++i) {
            const auto& clip = track.clips[i]; if (clip.timelineStart > end) break;
            if (!clip.stream || !clip.source || !clip.source->diskStorage()) continue;
            const auto offset = clip.sourceOffset + std::max(position, clip.timelineStart) - clip.timelineStart;
            if (offset < 0) continue;
            if (realtime) clip.stream->prefetchRealtime(clip.source->diskStorage().get(), std::size_t(offset), readAhead);
            else clip.stream->prefetch(clip.source->diskStorage().get(), std::size_t(offset), readAhead);
        }
}

} // namespace omawave::engine
