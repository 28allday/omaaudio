#pragma once

#include "lv2/chain.h"
#include "automation.h"
#include "session/audio_buffer.h"
#include "session/clip.h"
#include "session/project.h"
#include "session/track.h"
#include "session/types.h"
#include "session/routing.h"

#include <cstdint>
#include <vector>

namespace omawave::engine {
class RoutingRender;
class StreamCache;
class ElasticRuntime;

// What the audio thread plays.
//
// Built on the main thread from a Project, then handed to the audio thread by borrowed
// pointer with the same generation handshake that keeps a replaced plan alive until the
// audio thread has moved on. The audio thread only ever reads it, so it must be treated
// as immutable once published.
//
// Source pointers are resolved here rather than in the callback, because resolving a
// SourceId means a lookup the audio thread has no business doing. Those pointers
// stay valid because Project owns immutable sources at stable addresses and keeps
// them alive through the retirement handshake -- see Project::sources().
struct PlanClip {
    const session::AudioBuffer* source = nullptr;   // borrowed, immutable
    StreamCache* stream = nullptr; // owned by the live RenderPlan; null for memory/offline
    bool realtime = false;
    std::shared_ptr<const session::AudioSource> owner;
    std::optional<session::ElasticSpec> elastic;
    ElasticRuntime* elasticRuntime = nullptr;
    std::size_t elasticVoice = 0;
    [[nodiscard]] session::FramePos sourceFrames() const noexcept {
        return elastic ? elastic->outputFrames : source ? session::FramePos(source->frames()) : 0;
    }
    session::FramePos timelineStart = 0;
    session::FramePos sourceOffset = 0;
    session::FramePos length = 0;
    float gain = 1.0f;
    session::FramePos fadeIn = 0;
    session::FramePos fadeOut = 0;
    session::FramePos fadeInOffset = 0;
    session::FramePos fadeOutOffset = 0;

    // Cached from the source, because which of the two pan laws applies is decided per
    // clip and the audio thread should not be chasing a pointer to find out.
    bool mono = false;
    session::FadeShape fadeInShape = session::FadeShape::Linear, fadeOutShape = session::FadeShape::Linear;
    float fadeInCurve = 0, fadeOutCurve = 0;
    session::AutomationCurve volumeEnvelope{}, panEnvelope{};
    session::FramePos envelopeOffset = 0;

    [[nodiscard]] constexpr session::FramePos timelineEnd() const noexcept
    {
        return timelineStart + length;
    }

    // Same envelope as session::Clip. Duplicated rather than shared so the audio
    // thread's inner loop does not reach into the Session layer.
    [[nodiscard]] float envelopeAt(session::FramePos offsetInClip) const noexcept
    {
        if (offsetInClip < 0 || offsetInClip >= length)
            return 0.0f;

        float envelope = gain;
        if (fadeIn > 0 && offsetInClip < fadeIn - fadeInOffset)
            envelope *= session::fadeCurveAt(static_cast<float>(offsetInClip + fadeInOffset)
                      / static_cast<float>(fadeIn), fadeInShape, fadeInCurve);
        if (fadeOut > 0) {
            const session::FramePos fromEnd = length - offsetInClip;
            if (fromEnd <= fadeOut - fadeOutOffset)
                envelope *= session::fadeCurveAt(static_cast<float>(fromEnd + fadeOutOffset)
                          / static_cast<float>(fadeOut), fadeOutShape, fadeOutCurve);
        }
        if (volumeEnvelope.active()) envelope *= volumeEnvelope.valueAt(envelopeOffset + offsetInClip,1);
        return envelope;
    }
    [[nodiscard]] float channelEnvelopeAt(session::FramePos local, std::uint32_t channel) const noexcept {
        auto value = envelopeAt(local);
        if (local >= 0 && local < length && !mono && channel < 2 && panEnvelope.active()) {
            const auto pan = panEnvelope.valueAt(envelopeOffset + local,0);
            value *= channel == 0 ? (pan > 0 ? 1-pan : 1) : (pan < 0 ? 1+pan : 1);
        }
        return value;
    }
};

// One track's worth of the plan.
//
// The clip list is per track and not one flat list carrying a track index, and that is
// the load-bearing decision of Phase 4: clips on different tracks overlap in time by
// definition, so a flat list would break the sorted, non-overlapping invariant that
// indexAt()'s binary search depends on. Per track, the invariant holds and the search
// still works.
struct PlanTrack {
    // Carried so the audio thread can match a track across a plan swap by identity
    // rather than by position, which is what lets a gain ramp survive a track being
    // removed from above it.
    session::TrackId id = session::kInvalidTrack;

    std::vector<PlanClip> clips;        // sorted by timelineStart, non-overlapping
    session::FramePos length = 0;

    // Effective gain: the track's own gain with mute and solo already resolved, so the
    // audio thread never evaluates solo logic.
    float gain = 1.0f;
    session::PanGains pan;
    session::AutomationCurve volumeAutomation{};
    session::AutomationCurve panAutomation{};
    RecordingLane volumeRecording{}, panRecording{};
    ControlAutomations controls{};

    // This track's inserts, borrowed. Null when the track has none, which is the case
    // the audio thread takes the direct path for -- an unprocessed track must sound
    // exactly as it did before Phase 5, down to the summation order.
    //
    // Borrowed, and mutable through the pointer on purpose: the audio thread writes
    // into the chain's scratch. It is kept alive across a plan swap by the same
    // generation handshake as the plan itself, and retired rather than destroyed.
    lv2::Chain* chain = nullptr;

    // Which pan law a processed track gets. Per clip is impossible once a chain has
    // summed the clips together, so it is decided per track: mono law only when every
    // clip on the track is mono. A track of mono takes keeps the law it had before a
    // plugin was added to it, which is the case that would otherwise surprise someone.
    bool mono = true;
    bool muted = false, ownClips = true, receivesAudio = false;

    // Index of the first clip ending after `frame`. Binary search, no allocation, so
    // it is safe to call from the audio thread after a seek.
    [[nodiscard]] std::size_t indexAt(session::FramePos frame) const noexcept
    {
        std::size_t lo = 0;
        std::size_t hi = clips.size();
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (clips[mid].timelineEnd() <= frame)
                lo = mid + 1;
            else
                hi = mid;
        }
        return lo;
    }
};

struct RenderPlan {
    std::shared_ptr<ElasticRuntime> elasticRuntime;
    std::uint32_t sampleRate = 48000;
    std::uint64_t document = 0; // live owner generation; zero for private renders
    session::MusicalTime musicalTime{};
    std::shared_ptr<StreamCache> streamCache;
    session::RoutingGraph routing;
    // Edge activation resolves solo off the callback. Runtime scratch is shared
    // only between successive plans used by the same transport, never live/export.
    std::vector<bool> routeEnabled;
    std::shared_ptr<RoutingRender> routingRender;
    std::vector<PlanTrack> tracks;
    session::FramePos length = 0;       // the longest track
    std::uint32_t channels = 0;

    // The master bus chain, borrowed exactly as a track's is: attached by whoever
    // builds the chains, null when there are no master inserts, retired rather than
    // destroyed while a plan referring to it may still be running. The summed mix
    // goes through it after the trim and before the monitor level.
    lv2::Chain* master = nullptr;
    session::AutomationCurve masterVolumeAutomation{};
    RecordingLane masterVolumeRecording{};
    ControlAutomations masterControls{};

    [[nodiscard]] bool empty() const noexcept
    {
        if (length <= 0)
            return true;
        for (const PlanTrack& t : tracks) {
            if (!t.clips.empty())
                return false;
        }
        return true;
    }
};

// Same segment position as control automation. Pre-roll can expose transport
// information without attributing its synthetic input to live automation edits.
[[nodiscard]] inline host::ProcessContext processContext(const RenderPlan& plan,
    session::FramePos frame, bool playing = true, std::uint32_t epoch = 0,
    bool recording = false, bool looping = false, session::FramePos loopStart = 0,
    session::FramePos loopEnd = 0, bool preroll = false) noexcept
{
    host::ProcessContext context;
    context.time = {frame, plan.document, epoch, playing, true};
    context.recording = recording; context.looping = looping;
    context.loopStart = loopStart; context.loopEnd = loopEnd; context.preroll = preroll;
    if (plan.musicalTime.enabled && plan.musicalTime.valid()) {
        context.tempo = plan.musicalTime.tempoMilli / 1000.;
        context.numerator = static_cast<std::uint16_t>(plan.musicalTime.beats);
        context.denominator = static_cast<std::uint16_t>(plan.musicalTime.unit);
    }
    return context;
}

// Main thread only: resolves sources and copies clip descriptors.
[[nodiscard]] RenderPlan buildRenderPlan(const session::Project& project,
                                         std::uint32_t channels, const RenderPlan* previous = nullptr);
void prepareRoutingRender(RenderPlan& plan, const RenderPlan* previous = nullptr);
// GUI only; call after any clip filtering, before publication to a live transport.
// cacheBytes is the playback cache budget; 0 keeps the 32 MiB default. A previous
// cache of another size is not reused, so a changed budget takes effect here.
void prepareStreaming(RenderPlan& plan, const RenderPlan* previous = nullptr, std::size_t cacheBytes = 0);
// Separate GUI/audio producer queues; no allocation or I/O in either hint path.
void prefetchStreaming(const RenderPlan& plan, session::FramePos position,
                        bool realtime, bool readAhead = true) noexcept;

// One track's clips, with unity gain and centre pan. The Phase 3 shape is the
// single-track case of the Phase 4 one; kept because tests and peak generation both
// want to reason about one clip list at a time.
[[nodiscard]] PlanTrack buildPlanTrack(const session::Project& project,
                                       const std::vector<session::Clip>& clips);

} // namespace omawave::engine
