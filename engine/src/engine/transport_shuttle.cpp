// Shuttle playback: faster than normal and backwards (J/K/L). Audio thread only,
// under the same rules as the rest of the transport (BRIEF.md section 2): the
// buffers were allocated with the transport, nothing here allocates or locks.
//
// Faster: render() runs for speed times the frames asked for and each run of
// `speed` frames is averaged into one, which is a plain anti-alias filter good
// enough for finding a place. Effects, latency compensation, automation and loops
// all see the timeline exactly as they do at normal speed; only the result is
// squeezed, so it sounds like tape run fast.
//
// Backwards: render() runs forwards over a short stretch ending at the playhead
// (seeking there, so latency is primed from the audio before it), and that
// stretch is played reversed, then the stretch before it, and so on. A loop is
// ignored backwards; playback stops at the start.
#include "transport.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace omawave::engine {

using session::FramePos;

void Transport::leaveBackwards() noexcept
{
    if (!backwards_) return;
    backwards_ = false;
    position_ = audible_;
    latencyReset_ = true;
    grainStart_ = grainEnd_ = audible_;
}

// Renders [audible_ - length, audible_) forwards into grain_: longer when faster,
// so a pass covers about the same time heard. Frames the latency
// priming spends are silent and do not move the position, so only the frames that
// did (always the last ones of a pass) are kept.
bool Transport::fillBackwards(std::uint32_t channels, std::uint32_t speed) noexcept
{
    const FramePos end = audible_;
    const FramePos start = std::max<FramePos>(0, end - FramePos(std::min(kShuttleGrain, speed * 2048)));
    if (end <= start) return false;
    const bool loop = looping_;
    looping_ = false;
    position_ = start;
    latencyReset_ = true;
    std::array<float*, kShuttleChannels> chunk{};
    for (std::uint32_t c = 0; c < channels; ++c) chunk[c] = shuttleInput_.get() + std::size_t(c) * kShuttleGrain;
    FramePos have = start;
    // Enough passes to prime the longest latency the transport compensates.
    for (std::uint32_t pass = 0; have < end && playing_ && pass < kMaxCompensatedLatency / kShuttleBlock + 64; ++pass) {
        const auto before = position_;
        const auto n = std::uint32_t(std::min<FramePos>(kShuttleBlock, end - have));
        render(chunk.data(), channels, n);
        const auto advanced = std::clamp<FramePos>(position_ - before, 0, n);
        for (std::uint32_t c = 0; c < channels; ++c)
            std::copy_n(chunk[c] + (n - advanced), advanced, grain_.get() + std::size_t(c) * kShuttleGrain + (have - start));
        have += advanced;
    }
    // Whatever could not be rendered (a stretched clip not prepared yet) is silent.
    for (std::uint32_t c = 0; c < channels; ++c)
        std::fill(grain_.get() + std::size_t(c) * kShuttleGrain + (have - start),
                  grain_.get() + std::size_t(c) * kShuttleGrain + (end - start), 0.0f);
    looping_ = loop;
    // Rendering up to the end of the timeline stops the transport; backwards it is
    // where playback came from, not where it ends.
    playing_ = true;
    grainStart_ = start;
    grainEnd_ = end;
    return true;
}

void Transport::shuttle(float* const* planes, std::uint32_t channelCount, std::uint32_t frames) noexcept
{
    const bool usable = plan_ != nullptr && !followingCapture_ && channelCount <= kShuttleChannels
        && (capture_ == nullptr || !capture_->active.load(std::memory_order_acquire));
    if (!usable || !playing_ || speed_ > 0) leaveBackwards();
    if (!usable || !playing_ || speed_ == 1) { render(planes, channelCount, frames); return; }

    const auto speed = std::uint32_t(std::abs(speed_));
    const float scale = 1.0f / float(speed);
    std::array<float*, kShuttleChannels> input{};
    for (std::uint32_t c = 0; c < channelCount; ++c)
        input[c] = planes[c] ? shuttleInput_.get() + std::size_t(c) * kShuttleGrain : nullptr;

    if (speed_ > 0) {
        for (std::uint32_t done = 0; done < frames;) {
            const auto n = std::min(kShuttleBlock, frames - done);
            render(input.data(), channelCount, n * speed);
            for (std::uint32_t c = 0; c < channelCount; ++c) {
                if (!planes[c]) continue;
                const float* in = input[c];
                for (std::uint32_t f = 0; f < n; ++f, in += speed) {
                    float sum = 0.0f;
                    for (std::uint32_t k = 0; k < speed; ++k) sum += in[k];
                    planes[c][done + f] = sum * scale;
                }
            }
            done += n;
        }
        return;
    }

    if (!backwards_) {
        backwards_ = true;
        audible_ = position_;
        grainStart_ = grainEnd_ = position_;
    }
    for (std::uint32_t c = 0; c < channelCount; ++c)
        if (planes[c]) std::fill_n(planes[c], frames, 0.0f);
    std::array<float, kShuttleChannels> sum{};
    float peaks[2]{};
    for (std::uint32_t f = 0; f < frames && playing_; ++f) {
        sum.fill(0.0f);
        for (std::uint32_t k = 0; k < speed; ++k) {
            if (audible_ <= 0) { playing_ = false; break; }
            if (audible_ <= grainStart_ || audible_ > grainEnd_)
                if (!fillBackwards(channelCount, speed)) { playing_ = false; break; }
            const auto at = std::size_t(audible_ - 1 - grainStart_);
            for (std::uint32_t c = 0; c < channelCount; ++c)
                sum[c] += grain_[std::size_t(c) * kShuttleGrain + at];
            --audible_;
        }
        for (std::uint32_t c = 0; c < channelCount; ++c) {
            if (!planes[c]) continue;
            planes[c][f] = sum[c] * scale;
            if (c < 2) peaks[c] = std::max(peaks[c], std::abs(planes[c][f]));
        }
    }
    if (!playing_) { position_ = audible_; backwards_ = false; latencyReset_ = true; }
    // render() published where it rendered; what is heard is the playhead.
    meters_.publish(audible_, playing_, peaks[0], channelCount == 1 ? peaks[0] : peaks[1], playbackEpoch_);
}

} // namespace omawave::engine
