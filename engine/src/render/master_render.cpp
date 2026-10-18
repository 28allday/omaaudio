#include "master_render.h"
#include "audio_store.h"
#include "session/clip_reader.h"
#include "session/edits.h"
#include "session/clip_envelope.h"

#include "session/render.h"
#include "session/mixdown.h"
#include "engine/automation.h"
#include "engine/render_plan.h"
#include "engine/routing_render.h"
#include "engine/elastic_runtime.h"
#include <thread>
#include <chrono>

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <stdexcept>

namespace omawave::render {
namespace {

MixRender mixRouting(const session::Project& project,std::uint32_t channels,
    const std::vector<TrackChain>& chains,const std::atomic<bool>* cancel,const ProgressFn& progress,
    const MixBlockVisitor& sink = {}, session::FramePos end = -1)
{
    MixRender result; auto plan = engine::buildRenderPlan(project,channels);
    if (!plan.routing.ok) { result.error = plan.routing.error; return result; }
    // The streaming meter also uses this renderer for a single direct track.
    if (!plan.routingRender && plan.tracks.size() <= engine::RoutingRender::maxTracks
        && channels > 0 && channels <= engine::RoutingRender::maxChannels)
        plan.routingRender = std::make_shared<engine::RoutingRender>(plan.tracks.size(), channels);
    for (auto& track : plan.tracks) for (const auto& chain : chains)
        if (chain.track == track.id) { track.chain = chain.chain; break; }
    auto* graph = plan.routingRender.get(); bool changed = false;
    if (!graph || !graph->prepare(plan,true,changed)) {
        result.error = "routing exceeds the supported channel, track or delay capacity"; return result;
    }
    graph->reset(); const auto latency = graph->latency();
    const auto timelineEnd = end < 0 ? project.length() : end;
    if (timelineEnd < 0 || std::uint64_t(timelineEnd) > std::min(
            std::uint64_t(std::numeric_limits<session::FramePos>::max()),
            std::uint64_t(std::numeric_limits<std::size_t>::max())) - latency) {
        result.error = "render endpoint and latency exceed the supported timeline"; return result;
    }
    const auto frames = static_cast<std::size_t>(timelineEnd);
    std::unique_ptr<AudioStore> stored;
    if (!sink) stored = std::make_unique<AudioStore>(channels, frames);
    session::AudioBuffer scratch(channels,lv2::kMaxBlock);
    std::array<float*,engine::RoutingRender::maxChannels> output{};
    std::array<engine::RoutingLevel,engine::RoutingRender::maxTracks> levels{};
    std::array<float,engine::RoutingRender::maxTracks> left{},right{};
    for (std::uint32_t c = 0; c < channels; ++c) output[c] = scratch.plane(c).data();
    for (std::size_t i = 0; i < plan.tracks.size(); ++i) {
        const auto& t = plan.tracks[i]; levels[i] = {t.gain,t.pan.monoL,t.pan.monoR,t.pan.stereoL,t.pan.stereoR};
    }
    for (std::size_t pos = 0; pos < frames + latency;) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            result.cancelled = true; result.error = "render cancelled"; result.audio = {}; return result;
        }
        const auto count = graph->block(plan,static_cast<session::FramePos>(pos),
            static_cast<std::uint32_t>(std::min<std::size_t>(lv2::kMaxBlock,frames + latency - pos)),{},true);
        if (!count) { result.error = "routing could not advance"; result.audio = {}; return result; }
        while (!graph->prepareElastic(plan,pos,count,{})) {
            if (cancel && cancel->load(std::memory_order_relaxed)) {
                result.cancelled = true; result.error = "render cancelled"; result.audio = {}; return result;
            }
            if (plan.elasticRuntime && plan.elasticRuntime->failed()) {
                result.error = "elastic audio preparation failed"; result.audio = {}; return result;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        graph->process(plan,pos,count,output.data(),levels.data(),levels.data(),{},1,true,1,left.data(),right.data());
        if (!graph->prepare(plan,true,changed) || changed) {
            result.error = "a routed plugin changed its delay during rendering; retry after its settings settle";
            result.audio = {}; return result;
        }
        const auto first = std::min<std::size_t>(count, pos < latency ? latency - pos : 0);
        const auto last = std::min<std::size_t>(count, frames + latency - pos);
        if (first < last) {
            if (sink) {
                std::array<const float*, engine::RoutingRender::maxChannels> planes{};
                for (std::uint32_t c = 0; c < channels; ++c) planes[c] = output[c] + first;
                sink(static_cast<session::FramePos>(pos + first - latency), planes.data(), channels,
                     static_cast<std::uint32_t>(last - first));
            } else {
                for (std::uint32_t c = 0; c < channels; ++c)
                    stored->write(c, pos + first - latency, {output[c] + first, last - first});
            }
        }
        pos += count;
        if ((pos & 65535U) < count || pos == frames + latency) {
            for (const auto& source : project.sources())
                if (source && source->audio.diskStorage()) source->audio.diskStorage()->releaseMappedPages();
            if (progress) progress(static_cast<float>(pos) / (frames + latency));
        }
    }
    if (stored) result.audio = stored->finish();
    result.ok = true; return result;
}

// The existing direct renderer's clip order, per-clip mono/stereo pan law and
// elastic cursor are retained; only the destination is now written in blocks.
void mixClipBlocks(const session::Project& project, const session::Track& track,
                   AudioStore& out, std::uint32_t channels, bool trackLevel,
                   const std::atomic<bool>* cancel)
{
    session::ClipReader reader(project);
    std::array<float, session::ClipReader::kBlock> block;
    const float gain = track.effectiveGain(project.anySoloed());
    const auto pan = session::panGainsFor(track.pan);
    session::AutomationCursor volume(track.volumeAutomation), panPosition(track.panAutomation);
    for (const auto& clip : track.clips) {
        const auto* source = project.source(clip.source);
        if (!source || source->audio.empty()) continue;
        const bool mono = source->audio.channels() == 1;
        for (auto at = std::max<session::FramePos>(0, clip.timelineStart); at < clip.timelineEnd();) {
            if (cancel && cancel->load(std::memory_order_relaxed)) throw std::runtime_error("render cancelled");
            const auto count = static_cast<std::uint32_t>(std::min<session::FramePos>(session::ClipReader::kBlock, clip.timelineEnd() - at));
            reader.read(clip, at - clip.timelineStart, count, cancel);
            for (std::uint32_t c = 0; c < channels; ++c) {
                const auto sc = mono ? 0 : c;
                if (sc >= reader.channels()) continue;
                const auto input = reader.plane(sc);
                for (std::uint32_t f = 0; f < count; ++f) {
                    const auto frame = at + f;
                    float sample = input[f] * clip.channelEnvelopeAt(frame - clip.timelineStart,c,reader.channels());
                    if (trackLevel) {
                        const auto p = track.panAutomation.active() ? session::automationPan(panPosition.valueAt(frame, track.pan)) : pan;
                        const float panGain = channels > 1 && c < 2 ? (mono ? (c == 0 ? p.monoL : p.monoR) : (c == 0 ? p.stereoL : p.stereoR)) : 1;
                        sample *= gain * panGain * volume.valueAt(frame, 1);
                    }
                    block[f] = sample;
                }
                out.add(c, static_cast<std::size_t>(at), std::span<const float>(block).first(count));
            }
            at += count;
        }
    }
}

// Keep each plugin alive for the complete programme, with exactly the same run
// boundaries, automation clock and latency compensation as the buffered render.
bool runThroughChain(const session::AudioBuffer& in, lv2::Chain& chain,
                     session::AudioBuffer& rendered, const std::atomic<bool>* cancel,
                     const std::vector<session::Insert>& inserts, float trim = 1,
                     const session::AutomationCurve& volume = {}, const ProgressFn& progress = {},
                     const session::MusicalTime& musicalTime = {})
{
    const auto channels = in.channels(), chainChannels = chain.channels();
    const auto maxBlock = chain.maxBlock(), latency = chain.latencyFrames();
    const auto frames = in.frames(), total = frames + latency;
    if (!channels || !chainChannels || !maxBlock || maxBlock > AudioStore::blockFrames) return false;
    const auto controls = engine::buildControlAutomation(inserts);
    AudioStore output(channels, frames);
    session::AutomationCursor masterVolume(volume);
    engine::RenderPlan contextPlan; contextPlan.musicalTime = musicalTime;
    for (std::size_t pos = 0; pos < total;) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return false;
        const auto sub = engine::automationBlock(chain, controls, static_cast<session::FramePos>(pos),
            static_cast<std::uint32_t>(std::min<std::size_t>(maxBlock, total - pos)));
        if (!sub) return false;
        for (std::uint32_t c = 0; c < chainChannels; ++c) {
            const auto src = in.plane(std::min(c, channels - 1));
            auto* input = chain.input(c);
            for (std::uint32_t f = 0; f < sub; ++f) {
                const auto at = pos + f;
                input[f] = at < frames ? src[at] : 0;
                if (at < frames && (trim != 1 || volume.active()))
                    input[f] *= trim * masterVolume.valueAt(static_cast<session::FramePos>(at), 1);
            }
        }
        engine::applyControlAutomation(chain, controls, static_cast<session::FramePos>(pos), sub);
        chain.process(sub, false, engine::processContext(contextPlan, static_cast<session::FramePos>(pos)));
        if (chain.latencyFrames() != latency) return false;
        const auto first = std::min<std::size_t>(sub, pos < latency ? latency - pos : 0);
        if (first < sub) for (std::uint32_t c = 0; c < channels; ++c)
            output.write(c, pos + first - latency, {chain.output(std::min(c, chainChannels - 1)) + first, sub - first});
        pos += sub;
        if ((pos & 65535U) < sub || pos == total) {
            if (in.diskStorage()) in.diskStorage()->releaseMappedPages();
            if (progress) progress(static_cast<float>(pos) / total);
        }
    }
    rendered = output.finish();
    return true;
}

} // namespace

MixStreamResult visitMixInput(const session::Project& project, std::uint32_t channels,
    session::FramePos end, const MixBlockVisitor& visitor, const std::vector<TrackChain>& chains,
    const std::atomic<bool>* cancel, const ProgressFn& progress)
{
    MixStreamResult result;
    try {
        if (!visitor || !channels || channels > engine::RoutingRender::maxChannels || end < 0) {
            result.error = "stream analysis needs a visitor, supported channels and a valid timeline endpoint"; return result;
        }
        for (const auto& track:project.tracks()) if (!session::edits::isWellFormed(track.clips)) {
            result.error = "stream analysis needs valid clip geometry"; return result;
        }
        if (cancel && cancel->load(std::memory_order_relaxed)) { result.cancelled = true; return result; }
        if (!end) { result.ok = true; return result; }
        auto rendered = mixRouting(project, channels, chains, cancel, progress,
            [&](session::FramePos at, const float* const* planes, std::uint32_t count, std::uint32_t frames) {
                for (std::uint32_t c = 0; c < count; ++c) for (std::uint32_t f = 0; f < frames; ++f)
                    if (!std::isfinite(planes[c][f])) throw std::runtime_error("track output contains a non-finite sample");
                visitor(at, planes, count, frames); result.frames = at + frames;
            }, end);
        result.cancelled = rendered.cancelled || (cancel && cancel->load(std::memory_order_relaxed));
        result.ok = rendered.ok && !result.cancelled;
        result.error = result.cancelled ? std::string{} : rendered.error;
    } catch (const std::exception& error) {
        result.cancelled = cancel && cancel->load(std::memory_order_relaxed);
        result.error = result.cancelled ? std::string{} : error.what();
    }
    return result;
}

std::optional<session::Project> isolateTrackOutput(const session::Project& project,
    session::TrackId track, std::string& error)
{
    error.clear();
    const auto graph = session::compileRouting(project);
    const auto selected = project.trackIndex(track);
    if (!graph.ok || !selected) {
        error = graph.ok ? "choose a track to measure" : graph.error;
        return std::nullopt;
    }
    const auto count = project.tracks().size();
    std::vector<bool> needed(count, false); needed[*selected] = true;
    for (auto node = graph.order.rbegin(); node != graph.order.rend(); ++node)
        if (needed[*node]) for (const auto& edge : graph.edges)
            if (edge.to == *node) needed[edge.from] = true;
    auto isolated = project;
    const auto retained = [&](session::TrackId id) {
        const auto index = project.trackIndex(id); return index && needed[*index];
    };
    std::erase_if(isolated.tracks(), [&](const auto& t) { return !retained(t.id); });
    for (auto& t : isolated.tracks()) {
        t.soloed = false;
        if (t.id == track) {
            t.routing.output = session::kInvalidTrack;
            t.routing.sends.clear();
        } else {
            if (!retained(t.routing.output)) t.routing.output = session::kNoTrackOutput;
            std::erase_if(t.routing.sends, [&](const auto& send) { return !retained(send.target); });
        }
    }
    return isolated;
}

MixMeasurement measureMixInput(const session::Project& project, std::uint32_t channels,
    const std::vector<TrackChain>& chains, const std::atomic<bool>* cancel, const ProgressFn& progress)
{
    MixMeasurement result;
    try {
        if (channels == 0 || channels > engine::kLoudnessChannels || project.length() <= 0) {
            result.error = "measurement needs nonempty mono or stereo audio"; return result;
        }
        engine::LoudnessMeter meter(project.sampleRate());
        auto rendered = mixRouting(project, channels, chains, cancel, progress,
            [&](session::FramePos, const float* const* planes, std::uint32_t count, std::uint32_t frames) {
                for (std::uint32_t c = 0; c < count; ++c) for (std::uint32_t f = 0; f < frames; ++f)
                    if (!std::isfinite(planes[c][f]))
                        throw std::runtime_error("track output contains a non-finite sample; check source audio and active effects");
                meter.process(planes, count, frames);
            });
        result.cancelled = rendered.cancelled || (cancel && cancel->load(std::memory_order_relaxed));
        result.ok = rendered.ok && !result.cancelled;
        result.error = result.cancelled ? "measurement cancelled" : rendered.error;
        if (result.ok) result.report = meter.report();
    } catch (const std::exception& e) {
        result.cancelled = cancel && cancel->load(std::memory_order_relaxed);
        result.error = result.cancelled ? "measurement cancelled" : e.what();
    }
    return result;
}

static MixRender mixTracksUnchecked(const session::Project& project, std::uint32_t channels,
                    const std::vector<TrackChain>& trackChains, const std::atomic<bool>* cancel,
                    const ProgressFn& progress)
{
    MixRender out;
    if (const auto error = session::validateAutomation(project); !error.empty()) { out.error = error; return out; }
    if (const auto error = session::validateClipEnvelopes(project); !error.empty()) { out.error = error; return out; }
    if (cancel && cancel->load(std::memory_order_relaxed)) { out.cancelled = true; out.error = "render cancelled"; return out; }
    if (channels == 0) {
        out.error = "a mix needs at least one channel";
        return out;
    }
    const session::FramePos length = project.length();
    if (length <= 0) {
        out.error = "nothing to render: the session is empty";
        return out;
    }
    if (std::any_of(project.tracks().begin(),project.tracks().end(),[](const auto& track) { return !track.routing.direct(); }))
        return mixRouting(project,channels,trackChains,cancel,progress);

    AudioStore mix(channels, static_cast<std::size_t>(length));
    const bool anySolo = project.anySoloed();
    const auto trackCount = static_cast<float>(std::max<std::size_t>(1, project.tracks().size()));
    float done = 0.0f;

    for (const session::Track& track : project.tracks()) {
        done += 1.0f;
        if (progress)
            progress(done / trackCount);
        const float trackGain = track.effectiveGain(anySolo);
        if (trackGain == 0.0f || track.clips.empty())
            continue;                       // muted, or not the soloed one

        const auto liveChain = std::find_if(trackChains.begin(), trackChains.end(), [&](const auto& tc) {
            return tc.track == track.id && tc.chain && !tc.chain->empty();
        });
        if (liveChain == trackChains.end()) {
            mixClipBlocks(project, track, mix, channels, true, cancel);
            continue;
        }

        // The track alone, with its clips' envelopes; a mono source fills every
        // channel, as the transport reads it.
        // The whole project, not only as far as this track's last clip: the
        // transport keeps running the chain to the end, so a reverb or delay
        // rings out over the other tracks, and the export has to match.
        if (session::edits::totalLength(track.clips) <= 0) continue;
        const auto trackFrames = length;
        AudioStore input(channels, static_cast<std::size_t>(trackFrames));
        mixClipBlocks(project, track, input, channels, false, cancel);
        session::AudioBuffer rendered = input.finish();
        if (cancel && cancel->load(std::memory_order_relaxed)) { out.cancelled = true; out.error = "render cancelled"; return out; }
        if (rendered.empty())
            continue;

        for (const TrackChain& tc : trackChains) {
            if (tc.track != track.id || tc.chain == nullptr || tc.chain->empty())
                continue;
            session::AudioBuffer through;
            if (!runThroughChain(rendered, *tc.chain, through, cancel, track.inserts, 1, {}, {}, project.musicalTime())) {
                out.cancelled = cancel != nullptr && cancel->load(std::memory_order_relaxed);
                out.error = out.cancelled ? "render cancelled" : "a track's inserts could not run";
                return out;
            }
            rendered = std::move(through);
            break;
        }

        // One pan law per track, as the transport applies it: mono when every clip
        // is mono, stereo otherwise.
        bool mono = true;
        for (const session::Clip& clip : track.clips) {
            const session::AudioSource* source = project.source(clip.source);
            if (source != nullptr && source->audio.channels() != 1)
                mono = false;
        }
        const session::PanGains pan = session::panGainsFor(track.pan);
        session::AutomationCursor volume(track.volumeAutomation), panPosition(track.panAutomation);
        const auto frames = static_cast<std::size_t>(rendered.frames());
        const auto& frozen = rendered;
        std::array<float, AudioStore::blockFrames> block;
        for (std::size_t first = 0; first < frames; first += AudioStore::blockFrames) {
            if (cancel && cancel->load(std::memory_order_relaxed)) throw std::runtime_error("render cancelled");
            const auto count = std::min(AudioStore::blockFrames, frames - first);
            for (std::uint32_t c = 0; c < channels; ++c) {
                const float panGain = channels > 1 && c < 2
                    ? (mono ? (c == 0 ? pan.monoL : pan.monoR) : (c == 0 ? pan.stereoL : pan.stereoR)) : 1;
                const float gain = trackGain * panGain;
                const auto src = frozen.plane(c);
                for (std::size_t f = 0; f < count; ++f) {
                    const auto at = static_cast<session::FramePos>(first + f);
                    float automatedGain = gain;
                    if (track.panAutomation.active() && channels > 1 && c < 2) {
                        const auto p = session::automationPan(panPosition.valueAt(at, track.pan));
                        automatedGain = trackGain * (mono ? (c == 0 ? p.monoL : p.monoR) : (c == 0 ? p.stereoL : p.stereoR));
                    }
                    block[f] = src[first + f] * automatedGain * volume.valueAt(at, 1);
                }
                mix.add(c, first, std::span<const float>(block).first(count));
            }
            if (frozen.diskStorage()) frozen.diskStorage()->releaseMappedPages();
        }
    }

    out.audio = mix.finish();
    out.ok = true;
    return out;
}

MixRender mixTracks(const session::Project& project, std::uint32_t channels,
    const std::vector<TrackChain>& chains, const std::atomic<bool>* cancel, const ProgressFn& progress) {
    try { return mixTracksUnchecked(project,channels,chains,cancel,progress); }
    catch (const std::exception& e) {
        MixRender out; out.cancelled = cancel && cancel->load(std::memory_order_relaxed);
        out.error = out.cancelled ? "render cancelled" : e.what(); return out;
    }
}

MasterRender renderMaster(const session::Project& project, std::uint32_t channels,
                          lv2::Chain* chain, const std::atomic<bool>* cancel,
                          const ProgressFn& progress, const std::vector<TrackChain>& trackChains)
{
    MasterRender out;

    if (channels == 0) {
        out.error = "a render needs at least one channel";
        return out;
    }

    MixRender mixed = mixTracks(project, channels, trackChains, cancel, [&](float f) {
        if (progress)
            progress(0.2f * f);
    });
    if (!mixed.ok) {
        out.cancelled = mixed.cancelled;
        out.error = mixed.error;
        return out;
    }
    return renderBus(std::move(mixed.audio), project.masterTrim(), chain, cancel, progress,
        project.masterVolumeAutomation(), project.masterInserts(), project.musicalTime());
}

MasterRender renderBus(session::AudioBuffer mix, float trim, lv2::Chain* chain,
                       const std::atomic<bool>* cancel, const ProgressFn& progress,
                       const session::AutomationCurve& volume, const std::vector<session::Insert>& inserts,
                       const session::MusicalTime& musicalTime)
{
    MasterRender out;
    try {
        if(!volume.valid(0,4)) throw std::runtime_error("the rendered bus has invalid volume automation");
        if(const auto error=session::validateAutomation(inserts);!error.empty()) throw std::runtime_error(error);
        if (mix.empty()) { out.error = "nothing to render: the session is empty"; return out; }
        if (cancel && cancel->load(std::memory_order_relaxed)) throw std::runtime_error("render cancelled");
        if (progress) progress(0.2f);
        if (chain && !chain->empty()) {
            out.latencyDropped = chain->latencyFrames();
            if (!runThroughChain(mix, *chain, out.audio, cancel, inserts, trim, volume,
                    [&](float f) { if (progress) progress(0.2f + 0.8f * f); }, musicalTime))
                throw std::runtime_error("the master inserts could not run or changed their delay; retry after their settings settle");
        } else if (trim != 1 || volume.active()) {
            session::AutomationCursor masterVolume(volume);
            AudioStore output(mix.channels(), mix.frames());
            std::array<float, AudioStore::blockFrames> block;
            const auto& frozen = mix;
            for (std::size_t first = 0; first < mix.frames(); first += AudioStore::blockFrames) {
                if (cancel && cancel->load(std::memory_order_relaxed)) throw std::runtime_error("render cancelled");
                const auto count = std::min(AudioStore::blockFrames, mix.frames() - first);
                for (std::uint32_t c = 0; c < mix.channels(); ++c) {
                    const auto src = frozen.plane(c);
                    for (std::size_t f = 0; f < count; ++f)
                        block[f] = src[first + f] * (trim * masterVolume.valueAt(static_cast<session::FramePos>(first + f), 1));
                    output.write(c, first, std::span<const float>(block).first(count));
                }
                if (mix.diskStorage()) mix.diskStorage()->releaseMappedPages();
                if (progress) progress(0.2f + 0.8f * static_cast<float>(first + count) / mix.frames());
            }
            out.audio = output.finish();
        } else out.audio = std::move(mix);
        if (cancel && cancel->load(std::memory_order_relaxed)) throw std::runtime_error("render cancelled");
        out.ok = true;
        if (progress) progress(1);
    } catch (const std::exception& e) {
        out.audio = {};
        out.cancelled = cancel && cancel->load(std::memory_order_relaxed);
        out.error = out.cancelled ? "render cancelled" : e.what();
    }
    return out;
}

} // namespace omawave::render
