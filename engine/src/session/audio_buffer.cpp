#include "audio_buffer.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace omawave::session {

AudioBuffer::AudioBuffer(std::uint32_t channels, std::size_t frames)
    : channels_(channels), frames_(frames)
{
    if (channels && frames > std::numeric_limits<std::size_t>::max() / sizeof(float) / channels)
        throw std::length_error("audio buffer geometry exceeds addressable storage");
    storage_.resize(std::size_t(channels) * frames, 0.0f);
}

AudioBuffer AudioBuffer::fromDisk(std::shared_ptr<const DiskAudio> backing)
{
    if (!backing || !backing->sealed()) throw std::runtime_error("disk audio must be sealed before use");
    AudioBuffer out;
    out.channels_ = backing->channels(); out.frames_ = backing->frames(); out.disk_ = std::move(backing);
    return out;
}

void AudioBuffer::storeOnDisk(std::size_t threshold, const std::atomic<bool>* cancel)
{
    if (disk_ || empty() || storage_.size() * sizeof(float) < threshold) return;
    auto backing = DiskAudio::create(channels_, frames_);
    for (std::uint32_t c = 0; c < channels_; ++c) {
        const auto samples = static_cast<const AudioBuffer&>(*this).plane(c);
        for (std::size_t at = 0; at < frames_; at += 8192) {
            if (cancel && cancel->load(std::memory_order_relaxed)) return;
            backing->writePlane(c, at, samples.subspan(at, std::min<std::size_t>(8192, frames_ - at)));
        }
    }
    backing->seal(); disk_ = std::move(backing);
    std::vector<float>().swap(storage_);
}

AudioBuffer AudioBuffer::silentSource(std::uint32_t channels, std::size_t frames)
{
    if (!channels || frames < kDiskThreshold / sizeof(float) / channels) return AudioBuffer(channels, frames);
    auto silence = DiskAudio::create(channels, frames); silence->seal(); return fromDisk(std::move(silence));
}

void AudioBuffer::ensureMutable()
{
    if (!disk_) return;
    std::vector<float> samples(std::size_t(channels_) * frames_);
    for (std::uint32_t c = 0; c < channels_; ++c) {
        const auto source = disk_->plane(c);
        std::copy(source.begin(), source.end(), samples.begin() + std::size_t(c) * frames_);
    }
    storage_ = std::move(samples); disk_.reset();
}

void AudioBuffer::clear()
{
    if (disk_) {
        auto silence = DiskAudio::create(channels_, frames_);
        silence->seal(); disk_ = std::move(silence);
    } else std::fill(storage_.begin(), storage_.end(), 0.0f);
}

} // namespace omawave::session
