#include "transport.h"
#include "routing_render.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace omawave::engine {
static_assert(RoutingRender::maxTracks == Transport::kMaxTracks, "routed playback renders every track the transport plays");
bool Transport::processRouting(float* const* planes, std::uint32_t channels, std::uint32_t frames) noexcept
{
    if (!plan_ || !plan_->routing.active) return false;
    for (std::uint32_t c = 0; c < channels; ++c) if (planes[c]) std::fill_n(planes[c], frames, 0.0f);
    auto* graph = plan_->routingRender.get(); bool changed = false;
    if (!graph || !graph->fits(trackCount_, channels) || !graph->prepare(*plan_, masterBypass_, changed)) {
        playing_ = false; latencyReset_ = true;
        meters_.publishLatency(graph ? graph->supportedLatency() : 0, false, true);
        meters_.publish(position_, false, 0, 0, playbackEpoch_);
        for (std::size_t i = 0; i < trackCount_; ++i) meters_.publishTrack(i,0,0);
        return true;
    }
    if (changed) latencyReset_ = true;
    totalLatency_ = graph->latency();
    const auto ramp = [&](std::size_t i, std::uint32_t count) {
        const float delta = static_cast<float>(count) / kGainRampFrames;
        auto& a = current_[i]; const auto& b = target_[i];
        a.gain = approach(a.gain,b.gain,delta); a.monoL = approach(a.monoL,b.monoL,delta); a.monoR = approach(a.monoR,b.monoR,delta);
        a.stereoL = approach(a.stereoL,b.stereoL,delta); a.stereoR = approach(a.stereoR,b.stereoR,delta);
    };
    if (!playing_ || (!followingCapture_ && plan_->empty()) || !channels || !frames) {
        for (std::size_t i = 0; i < trackCount_; ++i) { ramp(i,frames); meters_.publishTrack(i,0,0); }
        if (capture_) (void)capture_->monitoring.read(nullptr,nullptr,frames);
        meters_.publishLatency(totalLatency_,false,false); meters_.publish(position_,playing_,0,0, playbackEpoch_); return true;
    }
    const auto loopEnd = std::min(loopEnd_,plan_->length);
    const bool loop = looping_ && loopEnd > loopStart_;
    if (loop && ((!followingCapture_ && position_ < loopStart_) || position_ >= loopEnd)) position_ = loopStart_;
    if (latencyReset_) {
        graph->reset(); routingOrigin_ = routingFrame_ = position_; primeRemaining_ = totalLatency_; latencyReset_ = false;
        // Capture cannot pause its clock while priming. Read ahead so the first
        // audible block after priming still belongs to the advancing record time.
        if (followingCapture_) routingFrame_ = position_ > std::numeric_limits<session::FramePos>::max() - totalLatency_
            ? std::numeric_limits<session::FramePos>::max() : position_ + totalLatency_;
    }
    const RoutingTime time{routingOrigin_,loopStart_,loopEnd,loop,playbackEpoch_,followingCapture_};
    const auto before = position_;
    std::array<float,kMaxTracks> left{},right{};
    std::array<RoutingLevel,kMaxTracks> start{},end{};
    std::array<float*,RoutingRender::maxChannels> output{};
    bool transition = false, unsupported = false;
    const auto advance = [](session::FramePos at,std::uint32_t n) {
        return at > std::numeric_limits<session::FramePos>::max() - n ? std::numeric_limits<session::FramePos>::max() : at + n;
    };
    for (std::uint32_t offset = 0; offset < frames && playing_;) {
        if (!primeRemaining_ && !followingCapture_ && !loop && position_ >= plan_->length) { playing_ = false; break; }
        auto limit = frames - offset;
        if (primeRemaining_) limit = std::min(limit,primeRemaining_);
        else if (!followingCapture_ && !loop) limit = static_cast<std::uint32_t>(std::min<session::FramePos>(limit,plan_->length - position_));
        const auto count = graph->block(*plan_,routingFrame_,limit,time,masterBypass_);
        if (!count) { playing_ = false; break; }
        if (!graph->prepareElastic(*plan_,routingFrame_,count,time) && !followingCapture_) break;
        for (std::size_t i = 0; i < trackCount_; ++i) {
            const auto a = current_[i]; start[i] = {a.gain,a.monoL,a.monoR,a.stereoL,a.stereoR}; ramp(i,count);
            const auto b = current_[i]; end[i] = {b.gain,b.monoL,b.monoR,b.stereoL,b.stereoR};
        }
        for (std::uint32_t c = 0; c < channels; ++c) output[c] = planes[c] ? planes[c] + offset : nullptr;
        graph->process(*plan_,routingFrame_,count,output.data(),start.data(),end.data(),time,
            masterTrim_,masterBypass_,masterMakeup_,left.data(),right.data());
        if (!graph->prepare(*plan_,masterBypass_,changed)) { unsupported = true; transition = true; break; }
        if (changed) { transition = true; break; }
        if (primeRemaining_) {
            for (std::uint32_t c = 0; c < channels; ++c) if (output[c]) std::fill_n(output[c],count,0.0f);
            primeRemaining_ -= count; left.fill(0); right.fill(0);
            if (followingCapture_) position_ = advance(position_,count);
        } else {
            if (speed_ == 1) loudness_.process(output.data(),channels,count);
            scope_.write(output.data(),channels,count);
            metronome(output.data(),channels,count,position_); // heard, never measured
            position_ = advance(position_,count);
        }
        if (loop && position_ >= loopEnd) position_ = loopStart_ + (position_ - loopEnd) % (loopEnd - loopStart_);
        routingFrame_ = advance(routingFrame_,count); offset += count;
        if (!followingCapture_ && !loop && !primeRemaining_ && position_ >= plan_->length) playing_ = false;
    }
    if (transition) {
        for (std::uint32_t c = 0; c < channels; ++c) if (planes[c]) std::fill_n(planes[c],frames,0.0f);
        left.fill(0); right.fill(0); latencyReset_ = true; playing_ = !unsupported;
        position_ = followingCapture_ ? advance(before,frames) : before;
        if (loop && position_ >= loopEnd) position_ = loopStart_ + (position_ - loopEnd) % (loopEnd - loopStart_);
    }
    if (capture_) {
        const float monitor = followingCapture_ && capture_->monitor.load(std::memory_order_relaxed)
            ? (capture_->channels.load(std::memory_order_relaxed) == 1 ? session::PanGains::kCentre : 1.0f) : 0.0f;
        capture_->monitoring.mix(planes,channels,frames,monitor);
    }
    float peaks[2]{};
    for (std::uint32_t c = 0; c < channels; ++c) if (planes[c]) for (std::uint32_t f = 0; f < frames; ++f) {
        planes[c][f] *= gain_; if (c < 2) peaks[c] = std::max(peaks[c],std::abs(planes[c][f]));
    }
    for (std::size_t i = 0; i < trackCount_; ++i) meters_.publishTrack(i,left[i],right[i]);
    meters_.publishLatency(totalLatency_,primeRemaining_ > 0 || latencyReset_,unsupported);
    meters_.publish(position_,playing_,peaks[0],channels == 1 ? peaks[0] : peaks[1], playbackEpoch_);
    return true;
}
} // namespace omawave::engine
