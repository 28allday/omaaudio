#include "clip_envelope.h"
#include "frame_scale.h"
#include "project.h"
#include <stdexcept>

namespace omawave::session {
void scaleClipEnvelopes(Clip& clip, FramePos numerator, FramePos denominator)
{
    if (numerator <= 0 || denominator <= 0) throw std::runtime_error("invalid clip envelope scale");
    if (!clip.hasEnvelopes() || numerator == denominator) return;
    auto volume = clip.volumeEnvelope, pan = clip.panEnvelope;
    const auto offset = scaledFrames(clip.envelopeOffset,numerator,denominator);
    const auto scale = [&](AutomationCurve& curve) {
        FramePos previous = -1;
        for (auto& point : curve.points) {
            point.frame = scaledFrames(point.frame,numerator,denominator);
            if (point.frame <= previous) throw std::runtime_error("stretch or rate conversion would merge clip envelope points; separate them first");
            previous = point.frame;
        }
    };
    scale(volume); scale(pan);
    if (clip.length < 0 || offset > std::numeric_limits<FramePos>::max()-clip.length
        || !volume.valid(0,4) || !pan.valid(-1,1)) throw std::runtime_error("scaled clip envelope exceeds its supported range");
    clip.volumeEnvelope = std::move(volume); clip.panEnvelope = std::move(pan); clip.envelopeOffset = offset;
}

std::string validateClipEnvelopes(const std::vector<Clip>& clips)
{
    std::size_t points = 0;
    for (const auto& clip : clips) {
        if (!clip.hasValidEnvelopes()) return "a clip has invalid volume or stereo balance envelope metadata";
        points += clip.volumeEnvelope.points.size()+clip.panEnvelope.points.size();
        if (points > kMaxProjectAutomationPoints) return "clip envelopes exceed the project limit of 65536 points";
    }
    return {};
}

std::string validateClipEnvelopes(const Project& project)
{
    for (const auto& track : project.tracks()) for (const auto& clip : track.clips)
        if (!clip.hasValidEnvelopes()) return "a clip has invalid volume or stereo balance envelope metadata";
    for (const auto& source : project.sources()) if (source && source->crossfade)
        if (!source->crossfade->left.hasValidEnvelopes() || !source->crossfade->right.hasValidEnvelopes())
            return "a crossfade recipe has invalid clip envelope metadata";
    if (project.automationPointCount() > kMaxProjectAutomationPoints)
        return "clip envelopes and automation exceed the project limit of 65536 points";
    return {};
}
} // namespace omawave::session
