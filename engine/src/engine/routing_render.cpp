#include "routing_render.h"
#include "elastic_runtime.h"
#include "source_reader.h"
#include "render_plan.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace omawave::engine {
bool RoutingRender::prepareElastic(const RenderPlan& plan, session::FramePos raw,
    std::uint32_t frames, const RoutingTime& time) const noexcept {
    bool ready = true;
    for (std::size_t node = 0; node < tracks_; ++node) {
        const auto input = raw-upstream_[node];
        if (input >= time.origin && (plan.tracks[node].ownClips || ownGain_[node] > 0))
            ready = prepareElasticTrack(plan.tracks[node],time.at(input),frames) && ready;
    }
    return ready;
}
RoutingRender::RoutingRender(std::size_t tracks, std::uint32_t channels)
    : tracks_(tracks), channels_(channels), capacity_(static_cast<std::uint32_t>(std::min<std::size_t>(
        maxLatency + lv2::kMaxBlock, historyBytes / (std::max<std::size_t>(1, tracks * channels) * 2 * sizeof(float)))))
{
    history_.resize(tracks_ * channels_ * 2 * capacity_, 0);
}

bool RoutingRender::prepare(const RenderPlan& plan, bool masterBypass, bool& changed) noexcept
{
    changed = false;
    if (!plan.routing.ok || !fits(plan.tracks.size(), plan.channels) || tracks_ > maxTracks
        || !channels_ || channels_ > maxChannels || plan.routing.edges.size() > maxEdges
        || plan.routing.order.size() != tracks_ || plan.routeEnabled.size() != plan.routing.edges.size()) return false;
    for (const auto& edge : plan.routing.edges) if (edge.from >= tracks_ || edge.to > tracks_ || edge.from == edge.to) return false;
    std::array<std::uint32_t, maxTracks> upstream{}, path{}, delays{};
    std::uint32_t longest = 0;
    for (const auto node : plan.routing.order) {
        if (node >= tracks_) return false;
        for (const auto& edge : plan.routing.edges)
            if (edge.to == node) upstream[node] = std::max(upstream[node], path[edge.from]);
        if (const auto* chain = plan.tracks[node].chain) {
            if (!chain->channels() || chain->channels() > maxChannels || !chain->maxBlock()) return false;
            delays[node] = chain->latencyFrames();
        }
        const auto sum = static_cast<std::uint64_t>(upstream[node]) + delays[node];
        if (sum > supportedLatency()) return false;
        path[node] = static_cast<std::uint32_t>(sum); longest = std::max(longest, path[node]);
    }
    const auto master = plan.master && !masterBypass ? plan.master->latencyFrames() : 0;
    if (plan.master && (!plan.master->channels() || plan.master->channels() > maxChannels || !plan.master->maxBlock())) return false;
    if (static_cast<std::uint64_t>(longest) + master > supportedLatency()) return false;
    changed = upstream_ != upstream || path_ != path || delays_ != delays || master_ != master;
    upstream_ = upstream; path_ = path; delays_ = delays; longest_ = longest; master_ = master; total_ = longest + master;
    return true;
}

std::uint32_t RoutingRender::block(const RenderPlan& plan, session::FramePos raw,
    std::uint32_t maximum, const RoutingTime& time, bool masterBypass) const noexcept
{
    auto count = std::min(maximum, lv2::kMaxBlock);
    const auto bound = [&](session::FramePos frame) {
        if (frame < time.origin) count = static_cast<std::uint32_t>(std::min<session::FramePos>(count, time.origin - frame));
        const auto at = time.at(frame);
        if (time.looping && at < time.loopEnd)
            count = static_cast<std::uint32_t>(std::min<session::FramePos>(count, time.loopEnd - at));
    };
    for (std::size_t i = 0; i < tracks_; ++i) {
        const auto input = raw - upstream_[i]; bound(input); bound(raw - path_[i]);
        if (const auto* chain = plan.tracks[i].chain)
            count = automationBlock(*chain, plan.tracks[i].controls, time.at(std::max(input,time.origin)), std::min(count,chain->maxBlock()));
    }
    bound(raw - longest_); bound(raw - total_);
    if (plan.master && !masterBypass)
        count = automationBlock(*plan.master,plan.masterControls,time.at(std::max(raw - longest_,time.origin)),std::min(count,plan.master->maxBlock()));
    return count;
}

float RoutingRender::history(std::size_t track, bool pre, std::uint32_t channel, std::int64_t frame) const noexcept
{
    if (frame < 0 || frame < written_ - capacity_) return 0;
    const auto base = ((track * 2 + (pre ? 0 : 1)) * channels_ + channel) * capacity_;
    return history_[base + static_cast<std::size_t>(frame) % capacity_];
}

void RoutingRender::remember(std::size_t track, bool pre, std::uint32_t channel, std::int64_t frame, float value) noexcept
{
    const auto base = ((track * 2 + (pre ? 0 : 1)) * channels_ + channel) * capacity_;
    history_[base + static_cast<std::size_t>(frame) % capacity_] = value;
}

void RoutingRender::process(const RenderPlan& plan, session::FramePos raw, std::uint32_t frames,
    float* const* output, const RoutingLevel* start, const RoutingLevel* end,
    const RoutingTime& time, float masterTrim, bool masterBypass, float masterMakeup,
    float* peakLeft, float* peakRight) noexcept
{
    if (!frames || frames > lv2::kMaxBlock) return;
    float input[maxChannels][lv2::kMaxBlock]{};
    float direct[maxChannels][lv2::kMaxBlock]{};
    if (!initialized_) {
        for (std::size_t i = 0; i < tracks_; ++i) { muteGain_[i] = plan.tracks[i].muted ? 0 : 1; ownGain_[i] = plan.tracks[i].ownClips ? 1 : 0; }
        for (std::size_t i = 0; i < plan.routing.edges.size(); ++i) edgeGain_[i] = plan.routeEnabled[i] ? 1 : 0;
        initialized_ = true;
    }
    const auto finishGate = [frames](float from,bool on) { const float step = static_cast<float>(frames) / 256;
        return from + std::clamp((on ? 1.0f : 0.0f) - from,-step,step); };
    const auto gate = [frames,&finishGate](float from,bool on,std::uint32_t f) { return from + (finishGate(from,on) - from) * (static_cast<float>(f) / frames); };
    for (const auto node : plan.routing.order) {
        const auto& track = plan.tracks[node]; auto* chain = track.chain;
        const auto laneRaw = raw - path_[node];
        const auto laneContext = processContext(plan, time.at(laneRaw), true, time.playbackEpoch,
            time.recording, time.looping, time.loopStart, time.loopEnd, laneRaw < time.origin);
        RecordingCursor volume(track.volumeAutomation, 1, track.volumeRecording, laneContext, frames);
        RecordingCursor panPosition(track.panAutomation, 0, track.panRecording, laneContext, frames);
        const auto level = [&](std::uint32_t channel, std::uint32_t f, bool mono, session::FramePos at) {
            const auto& a = start[node]; const auto& b = end[node];
            const float ratio = static_cast<float>(f) / frames;
            float gain = a.gain + (b.gain - a.gain) * ratio;
            if (channels_ > 1 && channel < 2) {
                if (panPosition.active()) {
                    const auto pan = session::automationPan(panPosition.valueAt(at,0));
                    gain *= mono ? (channel == 0 ? pan.monoL : pan.monoR) : (channel == 0 ? pan.stereoL : pan.stereoR);
                } else {
                    const auto p = mono ? (channel == 0 ? a.monoL : a.monoR) : (channel == 0 ? a.stereoL : a.stereoR);
                    const auto q = mono ? (channel == 0 ? b.monoL : b.monoR) : (channel == 0 ? b.stereoL : b.stereoR);
                    gain = a.gain * p + (b.gain * q - a.gain * p) * ratio;
                }
            }
            return gain * volume.valueAt(at,1);
        };
        const bool perClipPan = !chain && !track.receivesAudio;
        for (std::uint32_t c = 0; c < channels_; ++c) { std::fill_n(input[c],frames,0); std::fill_n(direct[c],frames,0); }
        const auto inputRaw = raw - upstream_[node], inputTime = time.at(inputRaw);
        if ((track.ownClips || ownGain_[node] > 0) && inputRaw >= time.origin) {
            for (auto i = track.indexAt(inputTime); i < track.clips.size(); ++i) {
                const auto& clip = track.clips[i]; if (clip.timelineStart >= inputTime + frames) break;
                if (!clip.source) continue;
                SourceReader samples(clip);
                const auto from = std::max(inputTime,clip.timelineStart), to = std::min(inputTime + frames,clip.timelineEnd());
                for (auto at = from; at < to; ++at) {
                    const auto source = clip.sourceOffset + at - clip.timelineStart;
                    if (source < 0 || source >= clip.sourceFrames()) continue;
                    const auto f = static_cast<std::uint32_t>(at - inputTime);
                    for (std::uint32_t c = 0; c < channels_; ++c) {
                        const auto sc = clip.mono ? 0 : c; if (sc >= clip.source->channels()) continue;
                        const float sample = samples.at(sc, source) * clip.channelEnvelopeAt(at - clip.timelineStart,c)
                            * gate(ownGain_[node],track.ownClips,f);
                        input[c][f] += sample;
                        if (perClipPan) direct[c][f] += sample * level(c,f,clip.mono,at);
                    }
                }
            }
        }
        for (std::size_t e = 0; e < plan.routing.edges.size(); ++e) {
            const auto& edge = plan.routing.edges[e];
            if ((!plan.routeEnabled[e] && edgeGain_[e] == 0) || edge.sidechain || edge.to != node) continue;
            const auto delay = upstream_[node] - path_[edge.from];
            for (std::uint32_t c = 0; c < channels_; ++c) for (std::uint32_t f = 0; f < frames; ++f)
                input[c][f] += edge.gain * gate(edgeGain_[e],plan.routeEnabled[e],f) * history(edge.from,edge.preFader,c,written_ + f - delay);
        }
        if (chain) {
            std::uint32_t prefix = 0;
            for (std::size_t slot = 0; slot < chain->size(); ++slot) {
                if (chain->hasSidechain(slot)) {
                    for (std::uint32_t c = 0; c < chain->channels(); ++c) {
                        auto* key = chain->sidechainInput(slot,c); std::fill_n(key,frames,0);
                        for (std::size_t e = 0; e < plan.routing.edges.size(); ++e) {
                            const auto& edge = plan.routing.edges[e];
                            if ((!plan.routeEnabled[e] && edgeGain_[e] == 0) || !edge.sidechain || edge.to != node) continue;
                            const auto delay = upstream_[node] - path_[edge.from] + prefix;
                            for (std::uint32_t f = 0; f < frames; ++f)
                                key[f] += gate(edgeGain_[e],plan.routeEnabled[e],f) * history(edge.from,false,std::min(c,channels_-1),written_ + f - delay);
                        }
                    }
                }
                prefix += chain->slotLatency(slot);
            }
            for (std::uint32_t c = 0; c < chain->channels(); ++c) std::copy_n(input[std::min(c,channels_-1)],frames,chain->input(c));
            processControlAutomation(*chain,track.controls,time.at(std::max(inputRaw,time.origin)),frames,true,
                processContext(plan,time.at(std::max(inputRaw,time.origin)),
                true,time.playbackEpoch,time.recording,time.looping,time.loopStart,time.loopEnd,inputRaw<time.origin));
        }
        for (std::uint32_t c = 0; c < channels_; ++c) for (std::uint32_t f = 0; f < frames; ++f) {
            const float mute = gate(muteGain_[node],!track.muted,f);
            const float pre = mute * (chain ? chain->output(std::min(c,chain->channels()-1))[f] : input[c][f]);
            const float post = perClipPan ? mute * direct[c][f] : pre * level(c,f,track.mono,time.at(raw - path_[node] + f));
            remember(node,true,c,written_ + f,pre); remember(node,false,c,written_ + f,post);
            if (c == 0) peakLeft[node] = std::max(peakLeft[node],std::abs(post));
            if (c == 1 || channels_ == 1) peakRight[node] = std::max(peakRight[node],std::abs(post));
        }
    }
    RecordingCursor masterVolume(plan.masterVolumeAutomation, 1, masterBypass ? RecordingLane{} : plan.masterVolumeRecording,
        processContext(plan, time.at(raw - longest_), true, time.playbackEpoch, time.recording,
            time.looping, time.loopStart, time.loopEnd, raw - longest_ < time.origin), frames);
    for (std::uint32_t c = 0; c < channels_; ++c) {
        std::fill_n(input[c],frames,0);
        for (std::size_t e = 0; e < plan.routing.edges.size(); ++e) {
            const auto& edge = plan.routing.edges[e]; if ((!plan.routeEnabled[e] && edgeGain_[e] == 0) || edge.to != tracks_) continue;
            const auto delay = longest_ - path_[edge.from];
            for (std::uint32_t f = 0; f < frames; ++f) input[c][f] += gate(edgeGain_[e],plan.routeEnabled[e],f) * history(edge.from,false,c,written_ + f - delay);
        }
        for (std::uint32_t f = 0; f < frames; ++f)
            input[c][f] *= masterBypass ? masterMakeup : masterTrim * masterVolume.valueAt(time.at(raw - longest_ + f),1);
    }
    if (plan.master && !masterBypass) {
        for (std::uint32_t c = 0; c < plan.master->channels(); ++c) std::copy_n(input[std::min(c,channels_-1)],frames,plan.master->input(c));
        processControlAutomation(*plan.master,plan.masterControls,time.at(std::max(raw - longest_,time.origin)),frames,false,
            processContext(plan,time.at(std::max(raw-longest_,time.origin)),
            true,time.playbackEpoch,time.recording,time.looping,time.loopStart,time.loopEnd,raw-longest_<time.origin));
        for (std::uint32_t c = 0; c < channels_; ++c) if (output[c]) std::copy_n(plan.master->output(std::min(c,plan.master->channels()-1)),frames,output[c]);
    } else for (std::uint32_t c = 0; c < channels_; ++c) if (output[c]) std::copy_n(input[c],frames,output[c]);
    written_ += frames;
    for (std::size_t i = 0; i < tracks_; ++i) { muteGain_[i] = finishGate(muteGain_[i],!plan.tracks[i].muted); ownGain_[i] = finishGate(ownGain_[i],plan.tracks[i].ownClips); }
    for (std::size_t i = 0; i < plan.routing.edges.size(); ++i) edgeGain_[i] = finishGate(edgeGain_[i],plan.routeEnabled[i]);
}
} // namespace omawave::engine
