#include "render.h"
#include "edits.h"
#include "clip_envelope.h"
#include <stdexcept>
#include "clip_reader.h"
#include "render/audio_store.h"
#include <array>
#include <algorithm>

namespace omawave::session {
AudioBuffer renderClipRange(const Project& project, const std::vector<Clip>& clips,
    FramePos start, FramePos end, std::uint32_t channels, const std::atomic<bool>* cancel) {
    if (start < 0 || end <= start || !channels || (cancel && cancel->load(std::memory_order_relaxed))) return {};
    if (const auto error = validateClipEnvelopes(clips); !error.empty()) throw std::runtime_error(error);
    render::AudioStore out(channels,std::size_t(end-start));
    std::array<float, ClipReader::kBlock> block;
    ClipReader reader(project);
    for (const auto& clip : clips) {
        const auto* source = project.source(clip.source);
        if (!source || source->audio.empty()) continue;
        const auto first = std::max(start,clip.timelineStart), last = std::min(end,clip.timelineEnd());
        for (auto at = first; at < last;) {
            if (cancel && cancel->load(std::memory_order_relaxed)) return {};
            const auto count = std::uint32_t(std::min<FramePos>(ClipReader::kBlock,last-at));
            try { reader.read(clip,at-clip.timelineStart,count,cancel); }
            catch (const std::exception&) {
                if (cancel && cancel->load(std::memory_order_relaxed)) return {};
                throw;
            }
            for (unsigned c = 0; c < channels; ++c) {
                const auto sc = reader.channels() == 1 ? 0 : c;
                if (sc >= reader.channels()) continue;
                const auto input = reader.plane(sc);
                for (unsigned f = 0; f < count; ++f)
                    block[f] = input[f]*clip.channelEnvelopeAt(at-clip.timelineStart+f,c,reader.channels());
                out.add(c, std::size_t(at-start), std::span<const float>(block).first(count));
            }
            at += count;
        }
    }
    return out.finish();
}
AudioBuffer renderTimeline(const Project& project, const std::vector<Clip>& clips,
                           std::uint32_t channels, const std::atomic<bool>* cancel) {
    return renderClipRange(project,clips,0,edits::totalLength(clips),channels,cancel);
}
} // namespace omawave::session
