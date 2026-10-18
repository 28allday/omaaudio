#include "clip_reader.h"
#include <algorithm>
#include <stdexcept>
#include <limits>

namespace omawave::session {
void ClipReader::read(const Clip& clip, FramePos local, std::uint32_t frames,
                      const std::atomic<bool>* cancel) {
    // A rack must never be silently omitted by analysis, rendering or a sample edit.
    if (clip.rack && effects_ == Effects::Refuse)
        throw std::runtime_error("this clip has effects: bake its effects first, or remove them");
    if (frames > kBlock || local < 0 || local > clip.length
        || frames > std::uint64_t(clip.length-local)) throw std::runtime_error("invalid clip read range");
    const auto* source = project_.source(clip.source);
    const auto channels = source ? source->audio.channels() : 1;
    if (!channels) throw std::runtime_error("invalid clip channel count");
    if (output_.channels() != channels) output_ = AudioBuffer(channels,kBlock);
    for (unsigned c = 0; c < channels; ++c) std::fill_n(output_.plane(c).data(),frames,0);
    if (!source || !frames) return;
    if (clip.sourceOffset > std::numeric_limits<FramePos>::max()-local-frames
        || clip.timelineStart < 0 || clip.timelineStart > std::numeric_limits<FramePos>::max()-local-frames)
        throw std::runtime_error("clip read position overflow");
    const auto first = clip.sourceOffset+local;
    if (clip.elastic) {
        if (!clip.validSource(FramePos(source->audio.frames()))) throw std::runtime_error("invalid elastic clip source");
        if (!processor_ || source_ != source || mapping_ != clip.elastic
            || nextTimeline_ != clip.timelineStart+local || nextSource_ != first)
            processor_ = std::make_unique<engine::ElasticProcessor>(source->audio,*clip.elastic,project_.sampleRate());
        float* planes[2] = {output_.plane(0).data(),channels > 1 ? output_.plane(1).data() : nullptr};
        processor_->render(first,frames,planes,cancel);
    } else {
        processor_.reset();
        const auto begin = std::max<FramePos>(first,0);
        const auto end = std::min<FramePos>(first+frames,FramePos(source->audio.frames()));
        if (begin < end) for (unsigned c = 0; c < channels; ++c)
            std::copy_n(source->audio.plane(c).data()+begin,std::size_t(end-begin),output_.plane(c).data()+begin-first);
        if (source->audio.diskStorage() && (first/65536 != (first+frames)/65536 || local+frames == clip.length))
            source->audio.diskStorage()->releaseMappedPages();
    }
    source_ = source; mapping_ = clip.elastic;
    nextTimeline_ = clip.timelineStart+local+frames; nextSource_ = first+frames;
}
} // namespace omawave::session
