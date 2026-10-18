#include "timeline_peaks.h"

#include "session/edits.h"
#include "session/clip_envelope.h"
#include <stdexcept>
#include "session/clip_reader.h"

#include <algorithm>
#include <limits>

namespace omawave::io {

PeakLevel buildTimelinePeaks(const session::Project& project,
                             const std::vector<session::Clip>& clips,
                             std::uint32_t samplesPerPeak, std::uint32_t channels, const std::atomic<bool>* cancel)
{
    PeakLevel level;
    const auto stopped = [&] { return cancel && cancel->load(std::memory_order_relaxed); };
    if (stopped()) return level;
    if (clips.empty() || samplesPerPeak == 0 || channels == 0)
        return level;

    if (const auto error = session::validateClipEnvelopes(clips); !error.empty()) throw std::runtime_error(error);
    const session::FramePos length = session::edits::totalLength(clips);
    if (length <= 0)
        return level;

    const auto spp = static_cast<session::FramePos>(samplesPerPeak);

    level.samplesPerPeak = samplesPerPeak;
    level.channels = channels;
    level.count = static_cast<std::size_t>((length + spp - 1) / spp);   // ceil
    // Silence, so gaps between clips read as silence rather than as nothing at all.
    level.data.assign(level.count * channels * 2, 0.0f);

    // The waveform shows each clip's own audio; clip effects are heard, not drawn.
    session::ClipReader reader(project, session::ClipReader::Effects::Dry);
    for (const auto& clip : clips) {
        const auto* source = project.source(clip.source);
        if (!source || source->audio.empty()) continue;
        for (session::FramePos local = 0; local < clip.length;) {
            if (stopped()) return {};
            const auto count = std::uint32_t(std::min<session::FramePos>(session::ClipReader::kBlock,clip.length-local));
            reader.read(clip,local,count,cancel);
            for (unsigned c = 0; c < channels; ++c) {
                const auto sc = reader.channels() == 1 ? 0 : c; if (sc >= reader.channels()) continue;
                const auto input = reader.plane(sc);
                for (unsigned f = 0; f < count; ++f) {
                    const auto to = clip.timelineStart+local+f; if (to < 0 || to >= length) continue;
                    const auto at = (std::size_t(c)*level.count+std::size_t(to/spp))*2;
                    const auto value = input[f]*clip.channelEnvelopeAt(local+f,c,reader.channels());
                    level.data[at] = std::min(level.data[at],value);
                    level.data[at+1] = std::max(level.data[at+1],value);
                }
            }
            local += count;
        }
    }

    return level;
}

} // namespace omawave::io
