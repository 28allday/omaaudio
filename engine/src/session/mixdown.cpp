#include "mixdown.h"

#include "track.h"
#include "clip_envelope.h"
#include "clip_reader.h"
#include <algorithm>

#include <span>
#include <stdexcept>

namespace omawave::session {

void mixTrack(const Project& project, const Track& track, AudioBuffer& out, bool anySolo, const std::atomic<bool>* cancel)
{
    if (const auto error = validateAutomation(track); !error.empty()) throw std::runtime_error(error);
    if (const auto error = validateClipEnvelopes(track.clips); !error.empty()) throw std::runtime_error(error);
    const auto channels = out.channels();
    const auto length = static_cast<FramePos>(out.frames());
    const float trackGain = track.effectiveGain(anySolo);
    if (trackGain == 0.0f)
        return;                         // muted, or not the soloed one

    const PanGains pan = panGainsFor(track.pan);

    ClipReader reader(project);
    for (const auto& clip : track.clips) {
        const auto* source = project.source(clip.source);
        if (!source || source->audio.empty()) continue;
        const bool mono = source->audio.channels() == 1;
        for (auto at = std::max<FramePos>(0,clip.timelineStart); at < std::min(length,clip.timelineEnd());) {
            const auto count = std::uint32_t(std::min<FramePos>(ClipReader::kBlock,std::min(length,clip.timelineEnd())-at));
            if (cancel && cancel->load(std::memory_order_relaxed)) throw std::runtime_error("render cancelled");
            reader.read(clip,at-clip.timelineStart,count,cancel);
            for (unsigned c = 0; c < channels; ++c) {
                const auto sc = mono ? 0 : c; if (sc >= reader.channels()) continue;
                const auto input = reader.plane(sc); auto output = out.plane(c);
                for (unsigned f = 0; f < count; ++f) {
                    const auto to = at+f;
                    const auto p = track.panAutomation.active() ? automationPan(track.panAutomation.valueAt(to,track.pan)) : pan;
                    const float panGain = channels > 1 && c < 2 ? (mono ? (c == 0 ? p.monoL : p.monoR) : (c == 0 ? p.stereoL : p.stereoR)) : 1;
                    output[std::size_t(to)] += input[f]*clip.channelEnvelopeAt(to-clip.timelineStart,c,reader.channels())
                        * (trackGain*panGain*track.volumeAutomation.valueAt(to,1));
                }
            }
            at += count;
        }
    }
}

AudioBuffer mixdown(const Project& project, std::uint32_t channels)
{
    if (const auto error = validateAutomation(project); !error.empty()) throw std::runtime_error(error);
    if (const auto error = validateClipEnvelopes(project); !error.empty()) throw std::runtime_error(error);
    const FramePos length = project.length();
    if (length <= 0 || channels == 0)
        return {};

    AudioBuffer out(channels, static_cast<std::size_t>(length));
    const bool anySolo = project.anySoloed();

    for (const Track& track : project.tracks())
        mixTrack(project, track, out, anySolo);

    return out;
}

} // namespace omawave::session
