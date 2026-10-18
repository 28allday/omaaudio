#include "transport.h"
#include "source_reader.h"
#include "routing_render.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace omawave::engine {
namespace {
using session::FramePos;
constexpr std::uint32_t maxChannels=8;
FramePos advance(FramePos frame,std::uint32_t count) noexcept {
    return frame>std::numeric_limits<FramePos>::max()-count?std::numeric_limits<FramePos>::max():frame+count;
}
}
host::ParameterTime Transport::automationInputTime(std::optional<session::TrackId> track, bool effect) noexcept
{
    if (!plan_) return {};
    const auto index = track ? slotOf(*track) : trackCount_;
    if (track && index >= trackCount_) return {};
    const auto end = std::min(loopEnd_, plan_->length);
    const bool loop = looping_ && end > loopStart_;
    auto position = position_;
    if (loop && ((!followingCapture_ && position < loopStart_) || position >= end)) position = loopStart_;
    FramePos input = position;
    if (plan_->routing.active) {
        auto* graph = plan_->routingRender.get(); bool changed = false;
        if (!graph || !graph->fits(trackCount_, plan_->channels) || !graph->prepare(*plan_, masterBypass_, changed)) return {};
        // prepare() observes current native latency. Keep a detected transition
        // pending for the renderer; querying must not hide its required reset.
        if (changed) latencyReset_ = true;
        const auto raw = latencyReset_ ? (followingCapture_ ? advance(position, graph->latency()) : position) : routingFrame_;
        const auto origin = latencyReset_ ? position : routingOrigin_;
        input = std::max(origin, raw - graph->automationDelay(index, effect));
    } else {
        std::array<std::uint32_t, kMaxTracks> delays{};
        std::uint32_t longest = 0;
        for (std::size_t i = 0; i < trackCount_; ++i) if (auto* chain = plan_->tracks[i].chain) {
            delays[i] = chain->latencyFrames(); longest = std::max(longest, delays[i]);
        }
        const auto master = plan_->master && !masterBypass_ ? plan_->master->latencyFrames() : 0;
        const auto total = static_cast<std::uint64_t>(longest) + master;
        if (total > kMaxCompensatedLatency) return {};
        if (total) {
            const bool reset = latencyReset_ || delays != trackLatencies_ || master != masterLatency_;
            const auto remaining = reset ? static_cast<std::uint32_t>(total) : primeRemaining_;
            const auto origin = reset ? position : primeOrigin_;
            const auto raw = remaining && !followingCapture_ ? origin - remaining : position;
            input = advance(raw, master);
            if (track && effect) input = advance(input, delays[index]);
            if (remaining) input = std::max(origin, input);
        }
    }
    if (loop && input >= end) input = loopStart_ + (input - loopStart_) % (end - loopStart_);
    return pluginContext(input).recordTime();
}
FramePos Transport::latencyTime(FramePos raw) const noexcept
{
    if (primeRemaining_ && raw<primeOrigin_) return primeOrigin_;
    const auto end=std::min(loopEnd_,timelineLength());
    if (looping_ && end>loopStart_ && (!followingCapture_ || raw >= loopStart_)) {
        const auto span=end-loopStart_;
        auto offset=(raw-loopStart_)%span;
        if(offset<0)offset+=span;
        return loopStart_+offset;
    }
    return followingCapture_ ? raw : std::max(FramePos{0},raw);
}

void Transport::checkLatencyTransition(float* const* planes,std::uint32_t channels,std::uint32_t frames,FramePos before,bool wasPlaying) noexcept
{
    if(!plan_ || !wasPlaying)return;
    bool changed=false;
    for(std::size_t t=0;t<trackCount_;++t) if(plan_->tracks[t].chain && plan_->tracks[t].chain->latencyFrames()!=trackLatencies_[t])changed=true;
    if(plan_->master && !masterBypass_ && plan_->master->latencyFrames()!=masterLatency_)changed=true;
    if(!changed)return;
    for(std::uint32_t c=0;c<channels;++c)if(planes[c])std::fill_n(planes[c],frames,0.0f);
    if(!followingCapture_)position_=before;
    playing_=true;latencyReset_=true;
    for(std::size_t t=0;t<trackCount_;++t)meters_.publishTrack(t,0,0);
    meters_.publish(position_,true,0,0, playbackEpoch_);
}

bool Transport::processLatency(float* const* planes,std::uint32_t channels,std::uint32_t frames) noexcept
{
    std::array<std::uint32_t,kMaxTracks> delays{};
    std::uint32_t longest=0,master=0;
    bool unsupported=channels>maxChannels;
    if(plan_) {
        for(std::size_t t=0;t<trackCount_;++t) if(auto* chain=plan_->tracks[t].chain) {
            delays[t]=chain->latencyFrames();longest=std::max(longest,delays[t]);
            unsupported=unsupported || chain->channels()>maxChannels || chain->channels()==0 || chain->maxBlock()==0;
        }
        if(plan_->master && !masterBypass_) {
            master=plan_->master->latencyFrames();
            unsupported=unsupported || plan_->master->channels()>maxChannels || plan_->master->channels()==0 || plan_->master->maxBlock()==0;
        }
    }
    const auto sum=static_cast<std::uint64_t>(longest)+master;
    unsupported=unsupported || sum>kMaxCompensatedLatency;
    const auto total=static_cast<std::uint32_t>(std::min<std::uint64_t>(sum,std::numeric_limits<std::uint32_t>::max()));
    if(delays!=trackLatencies_ || master!=masterLatency_)latencyReset_=true;
    trackLatencies_=delays;masterLatency_=master;totalLatency_=total;
    if(total==0) {
        primeRemaining_=0;
        meters_.publishLatency(0,false,false);
        return false; // Preserve the existing zero-latency render path exactly.
    }
    for(std::uint32_t c=0;c<channels;++c)if(planes[c])std::fill_n(planes[c],frames,0.0f);
    if(unsupported) {
        playing_=false;latencyReset_=true;
        meters_.publishLatency(total,false,true);meters_.publish(position_,false,0,0, playbackEpoch_);
        for(std::size_t t=0;t<trackCount_;++t)meters_.publishTrack(t,0,0);
        return true;
    }
    if(!frames || !channels)return true;
    if(!playing_ || !plan_ || (!followingCapture_ && plan_->empty())) {
        for(std::size_t t=0;t<trackCount_;++t) {
            const float delta=static_cast<float>(frames)/kGainRampFrames;
            current_[t].gain=approach(current_[t].gain,target_[t].gain,delta);
            current_[t].monoL=approach(current_[t].monoL,target_[t].monoL,delta);
            current_[t].monoR=approach(current_[t].monoR,target_[t].monoR,delta);
            current_[t].stereoL=approach(current_[t].stereoL,target_[t].stereoL,delta);
            current_[t].stereoR=approach(current_[t].stereoR,target_[t].stereoR,delta);
            meters_.publishTrack(t,0,0);
        }
        if(capture_)(void)capture_->monitoring.read(nullptr,nullptr,frames);
        meters_.publishLatency(total,false,false);meters_.publish(position_,playing_,0,0, playbackEpoch_);
        return true;
    }
    const auto loopEnd=std::min(loopEnd_,plan_->length);
    const bool loop=looping_ && loopEnd>loopStart_;
    if(loop && ((!followingCapture_ && position_<loopStart_) || position_>=loopEnd))position_=loopStart_;
    if(latencyReset_) {
        primeRemaining_=total;primeOrigin_=position_;latencyReset_=false;
    }
    const FramePos originalPosition=position_;
    float scratch[maxChannels][lv2::kMaxBlock]{};
    std::array<float,kMaxTracks> peakL{},peakR{};
    std::array<float*,maxChannels> bus{};
    std::uint32_t offset=0;
    bool changed=false;
    while(offset<frames && playing_) {
        if(!primeRemaining_ && !followingCapture_ && !loop && position_>=plan_->length) {playing_=false;break;}
        const auto raw=primeRemaining_ && !followingCapture_ ? primeOrigin_-primeRemaining_ : position_;
        const auto mixRaw=advance(raw,master);
        const auto mixTime=latencyTime(mixRaw);
        auto count=std::min(frames-offset,lv2::kMaxBlock);
        if(primeRemaining_)count=std::min(count,primeRemaining_);
        else if(loop || !followingCapture_)count=static_cast<std::uint32_t>(std::min<FramePos>(count,(loop?loopEnd:plan_->length)-position_));
        // Input time can wrap separately for each lookahead. No plugin run crosses
        // an input automation point/grid or loop boundary.
        const auto bound=[&](FramePos inputRaw,FramePos time,std::uint32_t limit) {
            if(primeRemaining_ && inputRaw<primeOrigin_)
                limit=static_cast<std::uint32_t>(std::min<FramePos>(limit,primeOrigin_-inputRaw));
            else if(loop)limit=static_cast<std::uint32_t>(std::min<FramePos>(limit,loopEnd-time));
            return limit;
        };
        count=bound(mixRaw,mixTime,count);
        if(plan_->master && !masterBypass_)
            count=automationBlock(*plan_->master,plan_->masterControls,mixTime,std::min(count,plan_->master->maxBlock()));
        for(std::size_t t=0;t<trackCount_;++t) {
            const auto& track=plan_->tracks[t];
            const auto inputRaw=advance(mixRaw,delays[t]);const auto time=latencyTime(inputRaw);
            count=bound(inputRaw,time,count);
            if(track.chain)count=automationBlock(*track.chain,track.controls,time,std::min(count,track.chain->maxBlock()));
        }
        if(!count)break;
        bool elasticReady = true;
        for (std::size_t t = 0; t < trackCount_; ++t) {
            const auto input = advance(mixRaw,delays[t]);
            if (!primeRemaining_ || input >= primeOrigin_)
                elasticReady = prepareElasticTrack(plan_->tracks[t],latencyTime(input),count) && elasticReady;
        }
        if (!elasticReady && !followingCapture_) break;
        for(std::uint32_t c=0;c<channels;++c)bus[c]=planes[c]?planes[c]+offset:nullptr;
        for(std::size_t t=0;t<trackCount_;++t) {
            const auto& track=plan_->tracks[t];auto* chain=track.chain;
            const auto inputRaw=advance(mixRaw,delays[t]);const auto time=latencyTime(inputRaw);
            const bool inputSilent=primeRemaining_ && inputRaw<primeOrigin_;
            const auto n=chain?chain->channels():channels;
            const Lanes start=current_[t];Lanes end;
            const float delta=static_cast<float>(count)/kGainRampFrames;
            end.gain=approach(start.gain,target_[t].gain,delta);
            end.monoL=approach(start.monoL,target_[t].monoL,delta);end.monoR=approach(start.monoR,target_[t].monoR,delta);
            end.stereoL=approach(start.stereoL,target_[t].stereoL,delta);end.stereoR=approach(start.stereoR,target_[t].stereoR,delta);
            current_[t]=end;
            RecordingCursor volume(track.volumeAutomation, 1, track.volumeRecording,
                pluginContext(mixTime, primeRemaining_ && mixRaw < primeOrigin_), count);
            RecordingCursor panPosition(track.panAutomation, 0, track.panRecording,
                pluginContext(mixTime, primeRemaining_ && mixRaw < primeOrigin_), count);
            const auto gainAt=[&](std::uint32_t f,std::uint32_t c,bool mono) {
                const float alpha=static_cast<float>(f)/count;
                const float gain=start.gain+(end.gain-start.gain)*alpha;
                const auto at=latencyTime(advance(mixRaw,f));
                float value=gain;
                if(channels>1 && c<2) {
                    if(panPosition.active()) {
                        const auto pan=session::automationPan(panPosition.valueAt(at,0));
                        value*=mono?(c==0?pan.monoL:pan.monoR):(c==0?pan.stereoL:pan.stereoR);
                    } else {
                        const float a=start.gain*(mono?(c==0?start.monoL:start.monoR):(c==0?start.stereoL:start.stereoR));
                        const float b=end.gain*(mono?(c==0?end.monoL:end.monoR):(c==0?end.stereoL:end.stereoR));
                        value=a+(b-a)*alpha;
                    }
                }
                return value*volume.valueAt(at,1);
            };
            for(std::uint32_t c=0;c<n;++c)std::fill_n(chain?chain->input(c):scratch[c],count,0.0f);
            if(!inputSilent)for(auto i=track.indexAt(time);i<track.clips.size();++i) {
                const auto& clip=track.clips[i];if(clip.timelineStart>=advance(time,count))break;
                if(!clip.source)continue;
                SourceReader samples(clip);
                const auto from=std::max(time,clip.timelineStart),to=std::min(advance(time,count),clip.timelineEnd());
                for(std::uint32_t c=0;c<n;++c) {
                    const auto sourceChannel=clip.mono?0:c;if(sourceChannel>=clip.source->channels())continue;
                    auto* dst=chain?chain->input(c):scratch[c];
                    for(auto f=from;f<to;++f) {
                        const auto local=f-clip.timelineStart,sourceFrame=clip.sourceOffset+local;
                        if(sourceFrame<0 || sourceFrame>=clip.sourceFrames())continue;
                        const auto at=static_cast<std::uint32_t>(f-time);
                        dst[at]+=samples.at(sourceChannel, sourceFrame)*clip.channelEnvelopeAt(local,c)*(chain?1.0f:gainAt(at,c,clip.mono));
                    }
                }
            }
            if(chain) {
                processControlAutomation(*chain,track.controls,time,count,false,pluginContext(time,inputSilent));
            }
            for(std::uint32_t c=0;c<channels;++c)if(bus[c]) {
                const auto* source=chain?chain->output(std::min(c,n-1)):scratch[c];
                for(std::uint32_t f=0;f<count;++f) {
                    const auto value=source[f]*(chain?gainAt(f,c,track.mono):1.0f);
                    bus[c][f]+=value;
                    if(!primeRemaining_) {if(c==0)peakL[t]=std::max(peakL[t],std::abs(value));if(c==1)peakR[t]=std::max(peakR[t],std::abs(value));}
                }
            }
            if(chain && chain->latencyFrames()!=delays[t])changed=true;
        }
        RecordingCursor masterVolume(plan_->masterVolumeAutomation, 1, masterBypass_ ? RecordingLane{} : plan_->masterVolumeRecording,
            pluginContext(mixTime, primeRemaining_ && mixRaw < primeOrigin_), count);
        for(std::uint32_t c=0;c<channels;++c)if(bus[c])for(std::uint32_t f=0;f<count;++f) {
            const auto at=latencyTime(advance(mixRaw,f));
            bus[c][f]*=primeRemaining_ && mixRaw<primeOrigin_?0.0f:
                masterBypass_?masterMakeup_:masterTrim_*masterVolume.valueAt(at,1);
        }
        if(plan_->master && !masterBypass_) {
            auto& chain=*plan_->master;
            for(std::uint32_t c=0;c<chain.channels();++c) {
                const auto from=std::min(c,channels-1);auto* dst=chain.input(c);
                if(bus[from])std::copy_n(bus[from],count,dst);else std::fill_n(dst,count,0.0f);
            }
            processControlAutomation(chain,plan_->masterControls,mixTime,count,false,
                pluginContext(mixTime,primeRemaining_ && mixRaw<primeOrigin_));
            for(std::uint32_t c=0;c<channels;++c)if(bus[c])std::copy_n(chain.output(std::min(c,chain.channels()-1)),count,bus[c]);
            if(chain.latencyFrames()!=master)changed=true;
        }
        if(changed)break;
        if(primeRemaining_) {
            for(std::uint32_t c=0;c<channels;++c)if(bus[c])std::fill_n(bus[c],count,0.0f);
            primeRemaining_-=count;
            if(followingCapture_) {
                position_=advance(position_,count);
                if (loop && position_ >= loopEnd) position_ = loopStart_ + (position_ - loopEnd) % (loopEnd - loopStart_);
            }
        } else {
            if(speed_==1)loudness_.process(bus.data(),channels,count);
            scope_.write(bus.data(),channels,count);
            metronome(bus.data(),channels,count,position_); // heard, never measured
            position_=advance(position_,count);
            if(loop && position_>=loopEnd)position_=loopStart_;
            else if(!followingCapture_ && position_>=plan_->length) {position_=plan_->length;playing_=false;}
        }
        offset+=count;
    }
    if(changed) {
        for(std::uint32_t c=0;c<channels;++c)if(planes[c])std::fill_n(planes[c],frames,0.0f);
        peakL.fill(0);peakR.fill(0);latencyReset_=true;playing_=true;
        position_=followingCapture_?advance(originalPosition,frames):originalPosition;
    }
    if(capture_) {
        const float monitor=followingCapture_ && capture_->monitor.load(std::memory_order_relaxed)
            ? (capture_->channels.load(std::memory_order_relaxed)==1?session::PanGains::kCentre:1.0f):0.0f;
        capture_->monitoring.mix(planes,channels,frames,monitor);
    }
    float peaks[2]{};
    for(std::uint32_t c=0;c<channels;++c)if(planes[c])for(std::uint32_t f=0;f<frames;++f) {
        planes[c][f]*=gain_;if(c<2)peaks[c]=std::max(peaks[c],std::abs(planes[c][f]));
    }
    for(std::size_t t=0;t<trackCount_;++t)meters_.publishTrack(t,peakL[t],channels==1?peakL[t]:peakR[t]);
    meters_.publishLatency(total,primeRemaining_!=0 || latencyReset_,false);
    meters_.publish(position_,playing_,peaks[0],channels==1?peaks[0]:peaks[1], playbackEpoch_);
    return true;
}
} // namespace omawave::engine
