#include "transport.h"
#include "source_reader.h"

#include <algorithm>
#include <cmath>

namespace omawave::engine {

void Transport::flushStopped() noexcept
{
    drainCommands();
    if (plan_ && plan_->elasticRuntime) {
        plan_->elasticRuntime->clearPreparing();
        if (plan_->elasticRuntime->failed() && !followingCapture_) playing_ = false;
    }
    meters_.publish(position_, false, 0, 0, playbackEpoch_);
}

void Transport::configureStopped(std::uint32_t rate) noexcept
{
    drainCommands();
    playing_ = false; position_ = 0;
    loudness_.configure(rate);
    latencyReset_ = true; primeRemaining_ = 0;
    meters_.publish(0, false, 0, 0, playbackEpoch_);
    meters_.publishLatency(0, false, false);
}

float Transport::approach(float current, float target, float maxDelta) noexcept
{
    const float delta = target - current;
    if (delta > maxDelta)
        return current + maxDelta;
    if (delta < -maxDelta)
        return current - maxDelta;
    return target;
}

std::size_t Transport::slotOf(session::TrackId id) const noexcept
{
    for (std::size_t i = 0; i < trackCount_; ++i) {
        if (trackIds_[i] == id)
            return i;
    }
    return kMaxTracks;
}

float Transport::trackGain(session::TrackId id) const noexcept
{
    const std::size_t slot = slotOf(id);
    return slot == kMaxTracks ? 0.0f : current_[slot].gain;
}

void Transport::adoptPlan(const RenderPlan* plan) noexcept
{
    const std::size_t count =
        plan == nullptr ? 0 : std::min(plan->tracks.size(), kMaxTracks);

    for (std::size_t i = 0; i < count; ++i) {
        const PlanTrack& pt = plan->tracks[i];

        Lanes wanted;
        wanted.gain = pt.gain;
        wanted.monoL = pt.pan.monoL;
        wanted.monoR = pt.pan.monoR;
        wanted.stereoL = pt.pan.stereoL;
        wanted.stereoR = pt.pan.stereoR;

        // A track that was already playing keeps where its ramp had got to, so an edit
        // to some other track cannot make this one jump. A track that is new to the
        // plan starts at its target rather than ramping up from silence -- fading in
        // every freshly imported track would be a bug, not a courtesy.
        const std::size_t previous = slotOf(pt.id);
        scratchIds_[i] = pt.id;
        scratchLanes_[i] = (previous == kMaxTracks) ? wanted : current_[previous];
        target_[i] = wanted;
    }

    for (std::size_t i = 0; i < count; ++i) {
        trackIds_[i] = scratchIds_[i];
        current_[i] = scratchLanes_[i];
    }
    trackCount_ = count;
}

void Transport::drainCommands() noexcept
{
    Command c;
    while (commands_.pop(c)) {
        switch (c.type) {
        case CommandType::Play:
            ++playbackEpoch_;
            // Starting from the very end rewinds, so pressing play on a finished take
            // does something rather than nothing.
            if (plan_ != nullptr && position_ >= plan_->length && speed_ > 0)
                position_ = 0;
            if (!playing_) latencyReset_ = true;
            playing_ = true;
            break;

        case CommandType::Stop:
            playing_ = false;
            speed_ = 1;
            break;

        case CommandType::SetSpeed:
            speed_ = int(std::clamp<session::FramePos>(c.frame, -kMaxSpeed, kMaxSpeed));
            if (speed_ == 0) speed_ = 1;
            break;

        case CommandType::Seek:
            latencyReset_ = true;
            seeked_ = true;
            position_ = std::clamp(c.frame, session::FramePos{0},
                                   plan_ != nullptr ? plan_->length : session::FramePos{0});
            break;

        case CommandType::SetGain:
            gain_ = c.gain;
            break;

        case CommandType::SetMetronome:
            metronomeOn_ = c.flag;
            metronomeGain_ = c.gain;
            break;

        case CommandType::SetTrackParams: {
            const std::size_t slot = slotOf(c.track);
            if (slot == kMaxTracks)
                break;              // a track that is no longer in the plan
            target_[slot].gain = c.gain;
            target_[slot].monoL = c.pan.monoL;
            target_[slot].monoR = c.pan.monoR;
            target_[slot].stereoL = c.pan.stereoL;
            target_[slot].stereoR = c.pan.stereoR;
            break;
        }

        case CommandType::SetPluginControl:
        case CommandType::SetPluginBypass: {
            // Track and master ownership are disjoint even if their payloads
            // coincide. Clip processors are not attached yet; never reinterpret
            // an unresolved target as another rack.
            if (plan_ == nullptr) break;
            lv2::Chain* chain = nullptr;
            if (c.effectOwner.isMaster()) chain = plan_->master;
            else if (c.effectOwner.isTrack()) {
                for (const PlanTrack& track : plan_->tracks)
                    if (track.id == c.effectOwner.trackId()) { chain = track.chain; break; }
            }
            if (chain) {
                if (c.type == CommandType::SetPluginControl)
                    chain->setControl(c.slot, c.port, c.parameter, c.parameterVersion);
                else chain->setBypass(c.slot, c.flag);
            }
            break;
        }

        case CommandType::SetPlan: {
            if (!c.flag) latencyReset_ = true;
            // An edit replaces the plan mid-session. Holding the playhead where it was
            // (clamped) rather than rewinding is what makes editing feel continuous --
            // jumping to zero after every cut would be unusable.
            const session::FramePos previous = position_;
            if (plan_) {
                for (const auto& track : plan_->tracks) if (track.chain) {
                    const ControlAutomations* replacement = nullptr;
                    if (c.plan) for (const auto& next : c.plan->tracks)
                        if (next.id == track.id && next.chain == track.chain) { replacement = &next.controls; break; }
                    resetControlAutomation(*track.chain, track.controls, replacement);
                }
                if (plan_->master) resetControlAutomation(*plan_->master, plan_->masterControls,
                    c.plan && c.plan->master == plan_->master ? &c.plan->masterControls : nullptr);
            }
            plan_ = c.plan;
            planGeneration_ = c.planGeneration;
            adoptPlan(plan_);
            position_ = followingCapture_ ? previous : std::clamp(previous, session::FramePos{0},
                                   plan_ != nullptr ? plan_->length : session::FramePos{0});
            if (plan_ == nullptr || (!followingCapture_ && (plan_->empty() || position_ >= plan_->length)))
                playing_ = false;
            // Publishing the acknowledgement is what lets the main thread free the
            // plan this one replaced.
            meters_.acknowledgePlan(planGeneration_);
            break;
        }

        case CommandType::SetMasterBypass:
            latencyReset_ = true;
            masterBypass_ = c.flag;
            masterMakeup_ = c.gain;
            break;

        case CommandType::SetMasterTrim:
            masterTrim_ = c.gain;
            break;

        case CommandType::ResetLoudness:
            loudness_.reset();
            break;

        case CommandType::SetLoop: {
            const auto start = std::max(session::FramePos{0}, c.frame);
            const auto end = c.endFrame;
            const bool enabled = c.flag && end > start;
            // Curve-only publications refresh loop settings too. Re-priming an
            // unchanged loop repeatedly stalls compensated playback/recording.
            if (enabled != looping_ || (enabled && (start != loopStart_ || end != loopEnd_)))
                latencyReset_ = true;
            loopStart_ = start; loopEnd_ = end; looping_ = enabled;
            break;
        }

        case CommandType::None:
            break;
        }
    }
}

void Transport::processChained(const PlanTrack& track, float* const* planes,
                               std::uint32_t channelCount, std::uint32_t frames,
                               session::FramePos blockStart, const Lanes& start,
                               const Lanes& end, float* trackPeak) noexcept
{
    lv2::Chain& chain = *track.chain;
    const std::uint32_t chainChannels = chain.channels();
    const std::uint32_t maxBlock = chain.maxBlock();
    if (frames == 0 || chainChannels == 0 || maxBlock == 0)
        return;

    // One pan law for the whole track: the clips have been summed by the time the
    // chain is done with them, so there is no longer a per-clip source to ask.
    const std::size_t law = track.mono ? 0u : 1u;
    float mixStart[2];
    float mixEnd[2];
    mixStart[0] = start.gain * (law == 0 ? start.monoL : start.stereoL);
    mixStart[1] = start.gain * (law == 0 ? start.monoR : start.stereoR);
    mixEnd[0] = end.gain * (law == 0 ? end.monoL : end.stereoL);
    mixEnd[1] = end.gain * (law == 0 ? end.monoR : end.stereoR);

    const float invFrames = 1.0f / static_cast<float>(frames);

    RecordingCursor volume(track.volumeAutomation, 1, track.volumeRecording, pluginContext(blockStart), frames);
    RecordingCursor panPosition(track.panAutomation, 0, track.panRecording, pluginContext(blockStart), frames);
    const bool volumeAutomated = volume.active();
    const bool panAutomated = panPosition.active();

    // Sub-blocked by the chain's maximum. The features were built promising
    // bufsize:boundedBlockLength with a maximum, so handing a plugin more than that
    // would make the promise a lie -- and a plugin that sized its buffers on it would
    // overrun. PipeWire's quantum is well under this today; that is not a reason to
    // trust the number the callback happens to arrive with.
    for (std::uint32_t offset = 0; offset < frames;) {
        const std::uint32_t sub = automationBlock(chain, track.controls, blockStart + offset, std::min(maxBlock, frames - offset));
        const session::FramePos subStart =
            blockStart + static_cast<session::FramePos>(offset);
        const session::FramePos subEnd =
            subStart + static_cast<session::FramePos>(sub);

        for (std::uint32_t c = 0; c < chainChannels; ++c) {
            float* in = chain.input(c);
            for (std::uint32_t f = 0; f < sub; ++f)
                in[f] = 0.0f;
        }

        // The clips, at unity into the chain's input. Envelope only: gain, pan and the
        // master level are all downstream of the inserts.
        for (std::size_t i = track.indexAt(subStart); i < track.clips.size(); ++i) {
            const PlanClip& clip = track.clips[i];
            if (clip.timelineStart >= subEnd)
                break;                              // sorted, nothing later overlaps

            const session::FramePos from = std::max(clip.timelineStart, subStart);
            const session::FramePos to = std::min(clip.timelineEnd(), subEnd);
            if (to <= from)
                continue;

            const session::AudioBuffer* source = clip.source;
            if (source == nullptr)
                continue;
            const auto sourceFrames = clip.sourceFrames();
            SourceReader samples(clip);

            for (std::uint32_t c = 0; c < chainChannels; ++c) {
                const std::uint32_t srcChannel = clip.mono ? 0 : c;
                if (srcChannel >= source->channels())
                    continue;

                float* dst = chain.input(c);


                for (session::FramePos f = from; f < to; ++f) {
                    const session::FramePos offsetInClip = f - clip.timelineStart;
                    const session::FramePos sourceFrame = clip.sourceOffset + offsetInClip;
                    if (sourceFrame < 0 || sourceFrame >= sourceFrames)
                        continue;

                    const auto index = static_cast<std::size_t>(f - subStart);
                    dst[index] += samples.at(srcChannel, sourceFrame)
                                * clip.channelEnvelopeAt(offsetInClip,c);
                }
            }
        }

        processControlAutomation(chain, track.controls, subStart, sub, false, pluginContext(subStart));

        // Out through the fader and the pan, on the ramp computed for the whole
        // callback -- so a sub-block boundary is not audible as a step.
        for (std::uint32_t c = 0; c < channelCount; ++c) {
            float* out = planes[c];
            if (out == nullptr)
                continue;
            const std::uint32_t chainChannel = c < chainChannels ? c : chainChannels - 1;
            const float* processed = chain.output(chainChannel);

            const bool panned = channelCount > 1 && c < 2;
            const float a = panned ? mixStart[c] : start.gain;
            const float b = panned ? mixEnd[c] : end.gain;
            const float step = (b - a) * invFrames;

            float peak = c < 2 ? trackPeak[c] : 0.0f;
            for (std::uint32_t f = 0; f < sub; ++f) {
                const std::uint32_t index = offset + f;
                float mix = a + step * static_cast<float>(index);
                if (panAutomated && panned) {
                    const auto pan = session::automationPan(panPosition.valueAt(blockStart + index, 0));
                    const float p = law == 0 ? (c == 0 ? pan.monoL : pan.monoR) : (c == 0 ? pan.stereoL : pan.stereoR);
                    mix = (start.gain + (end.gain - start.gain) * (static_cast<float>(index) * invFrames)) * p;
                }
                if (volumeAutomated) mix *= volume.valueAt(blockStart + index, 1);
                const float value = processed[f] * mix;
                out[index] += value;
                peak = std::max(peak, std::fabs(value));
            }
            if (c < 2)
                trackPeak[c] = peak;
        }
        offset += sub;
    }
}

void Transport::renderTracks(float* const* planes, std::uint32_t channelCount,
                             std::uint32_t frames, session::FramePos blockStart,
                             bool active, float maxDelta) noexcept
{
    const session::FramePos blockEnd = blockStart + static_cast<session::FramePos>(frames);

    for (std::size_t t = 0; t < trackCount_; ++t) {
        const Lanes start = current_[t];
        Lanes end;
        end.gain = approach(start.gain, target_[t].gain, maxDelta);
        end.monoL = approach(start.monoL, target_[t].monoL, maxDelta);
        end.monoR = approach(start.monoR, target_[t].monoR, maxDelta);
        end.stereoL = approach(start.stereoL, target_[t].stereoL, maxDelta);
        end.stereoR = approach(start.stereoR, target_[t].stereoR, maxDelta);
        current_[t] = end;

        // Every slot publishes every block, so a track that has gone quiet -- muted,
        // ended, stopped -- reads as silence rather than holding its last level.
        float trackPeak[2] = {0.0f, 0.0f};
        meters_.publishTrack(t, 0.0f, 0.0f);

        if (!active || t >= plan_->tracks.size())
            continue;

        const PlanTrack& track = plan_->tracks[t];
        if (track.clips.empty())
            continue;

        // A track with inserts goes the long way round: clips into the chain's scratch
        // at unity, the chain, then fader and pan on the way out. A muted one still
        // runs its chain -- stopping it would freeze a reverb tail mid-decay and make
        // unmuting click -- which is why this branch is taken before the mute skip.
        if (track.chain != nullptr) {
            processChained(track, planes, channelCount, frames, blockStart, start, end,
                           trackPeak);
            meters_.publishTrack(t, trackPeak[0],
                                 channelCount > 1 ? trackPeak[1] : trackPeak[0]);
            continue;
        }

        RecordingCursor volume(track.volumeAutomation, 1, track.volumeRecording, pluginContext(blockStart), frames);
        RecordingCursor panPosition(track.panAutomation, 0, track.panRecording, pluginContext(blockStart), frames);
        const bool volumeAutomated = volume.active();
        const bool panAutomated = panPosition.active();

        if (start.gain == 0.0f && end.gain == 0.0f)
            continue;               // muted for the whole block, and not moving

        // Gain and pan are two linear ramps, so their product is quadratic. It is
        // interpolated linearly over five milliseconds instead, which is inaudible and
        // keeps the inner loop to one multiply-add per sample.
        //
        // [mono?][channel]: which pan law applies is a property of the clip's source.
        float mixStart[2][2];
        float mixEnd[2][2];
        mixStart[0][0] = start.gain * start.monoL;
        mixStart[0][1] = start.gain * start.monoR;
        mixStart[1][0] = start.gain * start.stereoL;
        mixStart[1][1] = start.gain * start.stereoR;
        mixEnd[0][0] = end.gain * end.monoL;
        mixEnd[0][1] = end.gain * end.monoR;
        mixEnd[1][0] = end.gain * end.stereoL;
        mixEnd[1][1] = end.gain * end.stereoR;

        const float invFrames = frames == 0 ? 0.0f : 1.0f / static_cast<float>(frames);

        // Binary search rather than a scan: a heavily edited take has thousands of
        // clips, and this runs in the audio callback. The invariant it needs -- sorted
        // and non-overlapping -- holds per track, which is why the plan is per track.
        for (std::size_t i = track.indexAt(blockStart); i < track.clips.size(); ++i) {
            const PlanClip& clip = track.clips[i];
            if (clip.timelineStart >= blockEnd)
                break;                              // sorted, nothing later overlaps

            const session::FramePos from = std::max(clip.timelineStart, blockStart);
            const session::FramePos to = std::min(clip.timelineEnd(), blockEnd);
            if (to <= from)
                continue;

            const session::AudioBuffer* source = clip.source;
            if (source == nullptr)
                continue;
            const auto sourceFrames = clip.sourceFrames();
            SourceReader samples(clip);
            const std::size_t law = clip.mono ? 0u : 1u;

            for (std::uint32_t c = 0; c < channelCount; ++c) {
                float* dst = planes[c];
                if (dst == nullptr)
                    continue;

                // A mono source feeds every output channel; otherwise channels map one
                // to one.
                const std::uint32_t srcChannel = clip.mono ? 0 : c;
                if (srcChannel >= source->channels())
                    continue;

                // With one output channel there is nowhere to place an image, so pan is
                // bypassed rather than folded in. Channels past the stereo pair take the
                // track gain unpanned.
                const bool panned = channelCount > 1 && c < 2;
                const float a = panned ? mixStart[law][c] : start.gain;
                const float b = panned ? mixEnd[law][c] : end.gain;
                const float step = (b - a) * invFrames;



                // The meter reads the track's contribution before the master level,
                // so turning the monitor down does not make every strip go dark.
                float peak = c < 2 ? trackPeak[c] : 0.0f;
                for (session::FramePos f = from; f < to; ++f) {
                    const session::FramePos offsetInClip = f - clip.timelineStart;
                    const session::FramePos sourceFrame = clip.sourceOffset + offsetInClip;
                    if (sourceFrame < 0 || sourceFrame >= sourceFrames)
                        continue;

                    const auto index = static_cast<std::size_t>(f - blockStart);
                    float mix = a + step * static_cast<float>(index);
                    if (panAutomated && panned) {
                        const auto pan = session::automationPan(panPosition.valueAt(blockStart + index, 0));
                        const float p = law == 0 ? (c == 0 ? pan.monoL : pan.monoR) : (c == 0 ? pan.stereoL : pan.stereoR);
                        mix = (start.gain + (end.gain - start.gain) * (static_cast<float>(index) * invFrames)) * p;
                    }
                    if (volumeAutomated) mix *= volume.valueAt(blockStart + index, 1);

                    const float value = samples.at(srcChannel, sourceFrame)
                                      * clip.channelEnvelopeAt(offsetInClip,c) * mix;
                    dst[index] += value;
                    peak = std::max(peak, std::fabs(value));
                }
                if (c < 2)
                    trackPeak[c] = peak;
            }
        }

        meters_.publishTrack(t, trackPeak[0],
                             channelCount > 1 ? trackPeak[1] : trackPeak[0]);
    }
}

void Transport::applyMasterChain(float* const* planes, std::uint32_t channelCount,
                                 std::uint32_t frames) noexcept
{
    lv2::Chain& chain = *plan_->master;
    const std::uint32_t chainChannels = chain.channels();
    const std::uint32_t maxBlock = chain.maxBlock();
    if (frames == 0 || chainChannels == 0 || maxBlock == 0 || channelCount == 0)
        return;

    // Sub-blocked by the chain's maximum, as a track's is. The mix is copied in and
    // the result copied back: the chain owns its scratch, and the callback's planes
    // are PipeWire's.
    for (std::uint32_t offset = 0; offset < frames;) {
        const std::uint32_t sub = automationBlock(chain, plan_->masterControls, position_ + offset, std::min(maxBlock, frames - offset));

        for (std::uint32_t c = 0; c < chainChannels; ++c) {
            // A chain wider than the output takes the last plane again; narrower,
            // it hears the first planes only.
            const std::uint32_t from = c < channelCount ? c : channelCount - 1;
            const float* src = planes[from];
            float* in = chain.input(c);
            for (std::uint32_t f = 0; f < sub; ++f)
                in[f] = src != nullptr ? src[offset + f] : 0.0f;
        }

        processControlAutomation(chain, plan_->masterControls, position_ + offset, sub, false, pluginContext(position_ + offset));

        for (std::uint32_t c = 0; c < channelCount; ++c) {
            float* out = planes[c];
            if (out == nullptr)
                continue;
            const std::uint32_t chainChannel = c < chainChannels ? c : chainChannels - 1;
            const float* processed = chain.output(chainChannel);
            for (std::uint32_t f = 0; f < sub; ++f)
                out[offset + f] = processed[f];
        }
        offset += sub;
    }
}

void Transport::process(float* const* planes, std::uint32_t channelCount,
                        std::uint32_t frames) noexcept
{
    const auto processing = handoff_.enter();
    if (!processing) {
        if (planes) for (std::uint32_t channel = 0; channel < channelCount; ++channel)
            if (planes[channel]) std::fill_n(planes[channel], frames, 0.0f);
        return;
    }
    drainCommands();
    if (seeked_) {
        seeked_ = false;
        if (backwards_) audible_ = grainStart_ = grainEnd_ = position_;
    }
    if (speed_ != 1 || backwards_) { shuttle(planes, channelCount, frames); return; }
    render(planes, channelCount, frames);
}

void Transport::render(float* const* planes, std::uint32_t channelCount,
                       std::uint32_t frames) noexcept
{
    if (capture_) {
        const bool active = capture_->active.load(std::memory_order_acquire)
            || (capture_->groupActive.load(std::memory_order_acquire)
                && capture_->frames.load(std::memory_order_relaxed) > 0);
        const auto epoch = capture_->epoch.load(std::memory_order_relaxed);
        if (active && epoch != captureEpoch_) {
            captureEpoch_ = epoch;
            latencyReset_ = true;
            position_ = capture_->start.load(std::memory_order_relaxed);
            playing_ = true;
            followingCapture_ = true;
            captureOutputFrames_ = 0;
        } else if (followingCapture_ && !active) {
            playing_ = false;
            followingCapture_ = false;
        }
    }
    if (plan_ && plan_->elasticRuntime) {
        plan_->elasticRuntime->clearPreparing();
        if (plan_->elasticRuntime->failed() && !followingCapture_) playing_ = false;
    }
    if (plan_ && playing_) {
        prefetchStreaming(*plan_, position_, true);
        // With readahead, so the page after the loop start is there at the wrap too.
        if (looping_) prefetchStreaming(*plan_, loopStart_, true, true);
    }
    if (processRouting(planes, channelCount, frames)) { countIn(planes, channelCount, frames); return; }
    if (processLatency(planes, channelCount, frames)) { countIn(planes, channelCount, frames); return; }
    const auto latencyStart = position_;
    const auto latencyWasPlaying = playing_;
    const auto end = std::min(loopEnd_, timelineLength());
    // PipeWire's maximum channel layout is eight; retain the unrestricted legacy
    // path for other callers, but bound the loop's callback pointer scratch.
    constexpr std::size_t maxLoopChannels = 64;
    const bool loop = looping_ && end > loopStart_;
    const bool elastic = plan_ && plan_->elasticRuntime;
    if ((!loop && !elastic) || !playing_ || channelCount > maxLoopChannels || frames == 0) {
        processBlock(planes, channelCount, frames);
        checkLatencyTransition(planes, channelCount, frames, latencyStart, latencyWasPlaying);
        countIn(planes, channelCount, frames);
        return;
    }
    std::array<float*, maxLoopChannels> shifted{};
    std::array<float, kMaxTracks> left{}, right{};
    std::uint32_t offset = 0;
    for (std::uint32_t c = 0; c < channelCount; ++c) if (planes[c]) std::fill_n(planes[c],frames,0.0f);
    while (offset < frames && playing_) {
        if (loop && ((!followingCapture_ && position_ < loopStart_) || position_ >= end))
            position_ = loopStart_;
        auto count = frames-offset;
        if (loop) count = std::uint32_t(std::min<session::FramePos>(count,end-position_));
        if (elastic) count = std::min(count,std::uint32_t(ElasticRuntime::pageFrames));
        const auto before = position_;
        for (std::uint32_t c = 0; c < channelCount; ++c)
            shifted[c] = planes[c] ? planes[c] + offset : nullptr;
        processBlock(shifted.data(), channelCount, count);
        for (std::size_t t = 0; t < std::min(trackCount_, kMaxTracks); ++t) {
            left[t] = std::max(left[t], meters_.trackPeakLeft(t));
            right[t] = std::max(right[t], meters_.trackPeakRight(t));
        }
        if (position_ == before && elastic && playing_) break;
        if (loop) { if (position_ >= end) position_ = loopStart_; playing_ = true; }
        offset += count;
    }
    for (std::size_t t = 0; t < std::min(trackCount_, kMaxTracks); ++t)
        meters_.publishTrack(t, left[t], right[t]);
    float peaks[2]{};
    for (std::uint32_t c = 0; c < std::min(channelCount, 2U); ++c)
        if (planes[c])
            for (std::uint32_t i = 0; i < frames; ++i)
                peaks[c] = std::max(peaks[c], std::abs(planes[c][i]));
    meters_.publish(position_, playing_, peaks[0], channelCount == 1 ? peaks[0] : peaks[1], playbackEpoch_);
    checkLatencyTransition(planes, channelCount, frames, latencyStart, latencyWasPlaying);
    countIn(planes, channelCount, frames);
}

void Transport::countIn(float* const* planes, std::uint32_t channels, std::uint32_t frames) noexcept
{
    if (!capture_ || !followingCapture_) return;
    const auto duration = capture_->countInFrames.load(std::memory_order_relaxed);
    const auto beat = capture_->beatFrames.load(std::memory_order_relaxed);
    if (beat > 0 && captureOutputFrames_ < duration) {
        // Short decaying square pulse: fixed arithmetic, no allocation or trig.
        for (std::uint32_t f = 0; f < frames; ++f) {
            const auto at = captureOutputFrames_ + f;
            if (at >= duration) break;
            const auto phase = at % beat;
            const auto pulse = phase < 256 ? (phase / 24 % 2 ? -1.0f : 1.0f) * 0.12f * (1.0f - static_cast<float>(phase) / 256) * gain_ : 0.0f;
            for (std::uint32_t c = 0; c < channels; ++c) if (planes[c]) planes[c][f] = pulse;
        }
    }
    captureOutputFrames_ += frames;
}

void Transport::metronome(float* const* planes, std::uint32_t channels, std::uint32_t frames,
                          session::FramePos blockStart) noexcept
{
    if (!metronomeOn_ || !plan_ || frames == 0) return;
    // The session tempo and meter, whether or not the ruler shows bars and beats.
    const auto& time = plan_->musicalTime;
    const auto rate = plan_->sampleRate;
    const auto perBeat = time.ticksPerBeat();
    if (!time.valid() || !rate || perBeat <= 0) return;
    const session::FramePos length = rate / 40;   // 25 ms decaying tone
    const auto blockEnd = blockStart + frames;
    const auto firstTick = time.tickAt(std::max<session::FramePos>(0, blockStart - length), rate);
    if (firstTick < 0) return;
    for (auto beat = firstTick / perBeat;; ++beat) {
        const auto at = time.sampleAt(beat * perBeat, rate);
        if (at < 0 || at >= blockEnd) break;
        const bool downbeat = beat % time.beats == 0;
        const double step = 2.0 * 3.14159265358979323846 * (downbeat ? 1760.0 : 1320.0) / rate;
        const float level = metronomeGain_ * (downbeat ? 1.0f : 0.6f);
        const auto from = std::max(at, blockStart), to = std::min(at + length, blockEnd);
        for (auto f = from; f < to; ++f) {
            const auto n = f - at;
            const float envelope = 1.0f - float(n) / float(length);
            const float sample = level * envelope * envelope * float(std::sin(step * double(n)));
            for (std::uint32_t c = 0; c < channels; ++c)
                if (planes[c]) planes[c][f - blockStart] += sample;
        }
    }
}

void Transport::processBlock(float* const* planes, std::uint32_t channelCount,
                             std::uint32_t frames) noexcept
{

    // Always start from silence: gaps between clips, and the tail past the end of the
    // timeline, must not leave stale buffer contents behind.
    for (std::uint32_t c = 0; c < channelCount; ++c) {
        if (planes[c] == nullptr)
            continue;
        for (std::uint32_t f = 0; f < frames; ++f)
            planes[c][f] = 0.0f;
    }

    // Ramps advance whether or not anything is playing, so a mute pressed during a
    // stopped transport has finished travelling by the time playback resumes.
    // A zero-length block advances nothing. Treating it as "no limit" instead would
    // let an empty callback complete a whole ramp in no time at all.
    const float maxDelta = frames == 0
                             ? 0.0f
                             : static_cast<float>(frames) / kGainRampFrames;

    const bool active = playing_ && plan_ != nullptr
        && (followingCapture_ || (!plan_->empty() && position_ < plan_->length));

    if (active && !prepareElasticBlock(*plan_,position_,frames) && !followingCapture_) {
        for (std::size_t t = 0; t < trackCount_; ++t) meters_.publishTrack(t,0,0);
        meters_.publish(position_,playing_,0,0, playbackEpoch_); return;
    }
    const session::FramePos blockStart = position_;
    const session::FramePos blockEnd = blockStart + static_cast<session::FramePos>(frames);

    // The tracks sum at unity. Everything that is a property of the whole mix -- the
    // bus trim, the master inserts, the loudness meter, the monitor level -- happens
    // once on the sum, in that order, rather than per track.
    renderTracks(planes, channelCount, frames, blockStart, active, maxDelta);

    if (!active) {
        // A seek to the end while playing has nothing left to play: stop, as
        // running off the end does, rather than report "playing" for ever.
        if (playing_ && plan_ != nullptr && !followingCapture_ && !plan_->empty() && position_ >= plan_->length)
            playing_ = false;
        if (capture_) (void)capture_->monitoring.read(nullptr, nullptr, frames);
        // A stopped transport still publishes, so the UI's playhead settles at the
        // right place after a seek rather than keeping its last playing value.
        meters_.publish(position_, playing_, 0.0f, 0.0f, playbackEpoch_);
        return;
    }

    // The bus: trim, then the chain -- or, previewing "before", neither, with the
    // makeup gain that puts the plain mix at the mastered loudness.
    const float busGain = masterBypass_ ? masterMakeup_ : masterTrim_;
    if (busGain != 1.0f || (!masterBypass_ && (plan_->masterVolumeAutomation.active() || plan_->masterVolumeRecording.runtime))) {
        RecordingCursor masterVolume(plan_->masterVolumeAutomation, 1, masterBypass_ ? RecordingLane{} : plan_->masterVolumeRecording,
                                     pluginContext(blockStart), frames);
        for (std::uint32_t c = 0; c < channelCount; ++c) {
            float* out = planes[c];
            if (out == nullptr)
                continue;
            for (std::uint32_t f = 0; f < frames; ++f)
                out[f] *= busGain * (masterBypass_ ? 1.0f : masterVolume.valueAt(blockStart + f, 1));
        }
    }

    if (plan_->master != nullptr && !masterBypass_)
        applyMasterChain(planes, channelCount, frames);

    // Measured before the monitor level: what the programme is, not how loud it is
    // being listened to. Not while shuttling: that is not the programme.
    if (speed_ == 1) loudness_.process(planes, channelCount, frames);
    scope_.write(planes, channelCount, frames);

    if (capture_) {
        const float monitor = followingCapture_ && capture_->monitor.load(std::memory_order_relaxed)
            ? (capture_->channels.load(std::memory_order_relaxed) == 1 ? session::PanGains::kCentre : 1.0f) : 0.0f;
        capture_->monitoring.mix(planes, channelCount, frames, monitor);
    }
    // After loudness measurement: the click is heard, never measured.
    metronome(planes, channelCount, frames, blockStart);

    float peakLeft = 0.0f;
    float peakRight = 0.0f;
    for (std::uint32_t c = 0; c < channelCount; ++c) {
        float* out = planes[c];
        if (out == nullptr)
            continue;
        float peak = 0.0f;
        for (std::uint32_t f = 0; f < frames; ++f) {
            out[f] *= gain_;
            peak = std::max(peak, std::fabs(out[f]));
        }
        if (c == 0)
            peakLeft = peak;
        else if (c == 1)
            peakRight = peak;
    }
    if (channelCount == 1)
        peakRight = peakLeft;

    position_ = followingCapture_ ? blockEnd : std::min(blockEnd, plan_->length);
    if (!followingCapture_ && position_ >= plan_->length)
        playing_ = false;

    meters_.publish(position_, playing_, peakLeft, peakRight, playbackEpoch_);
}

} // namespace omawave::engine
