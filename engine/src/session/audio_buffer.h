#pragma once

#include "disk_audio.h"

#include <cassert>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace omawave::session {

// 32-bit float, non-interleaved (BRIEF.md section 2, "no exceptions").
//
// Mutable working buffers own a contiguous allocation. Large immutable sources
// can use private planar disk storage; const plane() can fault pages and is for non-realtime readers.
// Playback of disk sources must use the I/O worker's bounded page cache.
class AudioBuffer {
public:
    AudioBuffer() = default;

    AudioBuffer(std::uint32_t channels, std::size_t frames);
    static constexpr std::size_t kDiskThreshold = 8 * 1024 * 1024;
    [[nodiscard]] static AudioBuffer fromDisk(std::shared_ptr<const DiskAudio> backing);
    // Worker only. Copies in bounded blocks, then releases the memory allocation.
    void storeOnDisk(std::size_t threshold = kDiskThreshold, const std::atomic<bool>* cancel = nullptr);
    [[nodiscard]] static AudioBuffer silentSource(std::uint32_t channels, std::size_t frames);
    [[nodiscard]] const std::shared_ptr<const DiskAudio>& diskStorage() const noexcept { return disk_; }
    [[nodiscard]] std::size_t memoryBytes() const noexcept { return storage_.capacity() * sizeof(float); }

    [[nodiscard]] std::uint32_t channels() const noexcept { return channels_; }
    [[nodiscard]] std::size_t frames() const noexcept { return frames_; }
    [[nodiscard]] bool empty() const noexcept { return frames_ == 0 || channels_ == 0; }

    [[nodiscard]] std::span<float> plane(std::uint32_t channel)
    {
        assert(channel < channels_);
        ensureMutable();
        return {storage_.data() + static_cast<std::size_t>(channel) * frames_, frames_};
    }

    [[nodiscard]] std::span<const float> plane(std::uint32_t channel) const noexcept
    {
        assert(channel < channels_);
        if (disk_) return disk_->plane(channel);
        return {storage_.data() + static_cast<std::size_t>(channel) * frames_, frames_};
    }

    // Frames are dropped or zero-filled at the end; existing content is preserved
    // only where the two sizes overlap. Main/worker thread only -- this allocates.
    void resize(std::uint32_t channels, std::size_t frames)
    {
        // Region readers often already produced this exact shape. Preserve the
        // allocation (and immutable disk backing) when there is nothing to resize.
        if (channels == channels_ && frames == frames_)
            return;
        AudioBuffer next(channels, frames);
        const std::uint32_t copyChannels = channels < channels_ ? channels : channels_;
        const std::size_t copyFrames = frames < frames_ ? frames : frames_;
        for (std::uint32_t c = 0; c < copyChannels; ++c) {
            const std::span<const float> src = static_cast<const AudioBuffer&>(*this).plane(c);
            const std::span<float> dst = next.plane(c);
            for (std::size_t i = 0; i < copyFrames; ++i)
                dst[i] = src[i];
        }
        *this = std::move(next);
    }

    void clear();

private:
    void ensureMutable();
    std::uint32_t channels_ = 0;
    std::size_t frames_ = 0;
    std::vector<float> storage_;
    std::shared_ptr<const DiskAudio> disk_;
};

} // namespace omawave::session
