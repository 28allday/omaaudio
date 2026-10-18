#pragma once

#include "render_plan.h"
#include "stream_cache.h"
#include "elastic_runtime.h"

#include <algorithm>
#include <array>
#include <limits>
#include <span>

namespace omawave::engine {

// One clip's read cursor for a render block. Leases pin the block's pages across every
// channel/sample for its lifetime. A miss is retried next
// block, not for every sample. Offline plans can read mapped immutable planes.
class SourceReader {
public:
    explicit SourceReader(const PlanClip& clip) noexcept
        : source_(clip.source), cache_(clip.stream), realtime_(clip.realtime),
          elastic_(clip.elasticRuntime), voice_(clip.elasticVoice), frames_(clip.sourceFrames())
    {
        if (!source_) return;
        if (clip.elastic) return;
        disk_ = source_->diskStorage().get();
        if (disk_ && realtime_) return;
        for (std::uint32_t c = 0; c < std::min<std::uint32_t>(planes_.size(), source_->channels()); ++c)
            planes_[c] = source_->plane(c);
    }
    [[nodiscard]] float at(std::uint32_t channel, session::FramePos frame) noexcept
    {
        if (!source_ || channel >= source_->channels()
            || frame < 0 || frame >= frames_) return 0;
        const auto index = std::size_t(frame);
        if (elastic_) {
            const auto page = index/ElasticRuntime::pageFrames;
            auto& entry = elasticPages_[page%elasticPages_.size()];
            if (entry.page != page) { entry.lease = {}; entry.lease = elastic_->read(voice_,page); entry.page = page; }
            return entry.lease.at(channel,index%ElasticRuntime::pageFrames);
        }
        if (planes_[0].empty() && !disk_) return 0;
        if (!disk_ || !realtime_) return channel < planes_.size() ? planes_[channel][index] : source_->plane(channel)[index];
        if (!cache_) return 0; // Never fall back to synchronous disk access.
        const auto page = index / StreamCache::kPageFrames;
        auto& entry = pages_[page % pages_.size()];
        if (page != entry.page) {
            entry.lease = {}; entry.lease = cache_->read(disk_, page); entry.page = page;
        }
        return entry.lease.at(channel, index % StreamCache::kPageFrames);
    }
private:
    const session::AudioBuffer* source_;
    StreamCache* cache_;
    bool realtime_;
    ElasticRuntime* elastic_;
    std::size_t voice_;
    session::FramePos frames_;
    const session::DiskAudio* disk_ = nullptr;
    std::array<std::span<const float>, 8> planes_{};
    // Keep page-boundary decisions consistent when channel-major renderers
    // revisit the same frames on the next channel. Typical callbacks touch 1–2.
    struct ReadPage {
        StreamCache::Lease lease;
        std::size_t page = std::numeric_limits<std::size_t>::max();
    };
    std::array<ReadPage, 8> pages_{};
    struct ElasticPage {
        ElasticRuntime::Lease lease;
        std::size_t page = std::numeric_limits<std::size_t>::max();
    };
    std::array<ElasticPage, 2> elasticPages_{};
};

} // namespace omawave::engine
