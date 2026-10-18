#pragma once

#include "session/audio_buffer.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

namespace omawave::render {

// Worker-owned intermediate audio. Small jobs stay in RAM; long jobs are built
// in an unlinked file and become immutable when finished. Writes/adds are blocks,
// including when clips overlap. Never used by the realtime callback.
class AudioStore {
public:
    static constexpr std::size_t blockFrames = 8192;

    AudioStore(std::uint32_t channels, std::size_t frames)
        : channels_(channels), frames_(frames)
    {
        if (!channels || !frames || frames > std::numeric_limits<std::size_t>::max() / sizeof(float) / channels)
            throw std::length_error("invalid render audio geometry");
        if (frames * channels * sizeof(float) >= session::AudioBuffer::kDiskThreshold)
            disk_ = session::DiskAudio::create(channels, frames);
        else
            memory_ = session::AudioBuffer(channels, frames);
    }

    void write(std::uint32_t channel, std::size_t first, std::span<const float> samples)
    {
        check(channel, first, samples.size());
        if (disk_) disk_->writePlane(channel, first, samples);
        else std::copy(samples.begin(), samples.end(), memory_.plane(channel).begin() + first);
    }

    void add(std::uint32_t channel, std::size_t first, std::span<const float> samples)
    {
        check(channel, first, samples.size());
        // Unwritten regions are already silent. Preserve sparse gaps instead of
        // making an hour of empty timeline occupy an hour of physical disk space.
        if (std::all_of(samples.begin(), samples.end(), [](float value) { return value == 0; })) return;
        if (disk_) {
            std::array<float, blockFrames> block;
            auto values = std::span<float>(block).first(samples.size());
            if (!disk_->readPlane(channel, first, values)) throw std::runtime_error("could not read render scratch audio");
            for (std::size_t i = 0; i < samples.size(); ++i) values[i] += samples[i];
            disk_->writePlane(channel, first, values);
        } else {
            auto values = memory_.plane(channel).subspan(first, samples.size());
            for (std::size_t i = 0; i < samples.size(); ++i) values[i] += samples[i];
        }
    }

    // Existing in-memory callers may hold a plane span across an in-place edit.
    // Preserve that allocation; immutable disk input is replaced atomically.
    void finishInto(session::AudioBuffer& destination, const std::atomic<bool>* cancel = nullptr)
    {
        const auto stopped = [&] { return cancel && cancel->load(std::memory_order_relaxed); };
        if (stopped()) return;
        if (destination.channels() != channels_ || destination.frames() != frames_)
            throw std::invalid_argument("replacement audio geometry changed");
        auto result = finish();
        if (destination.diskStorage()) { destination = std::move(result); return; }
        for (std::uint32_t ch = 0; ch < channels_; ++ch) {
            const auto src = std::as_const(result).plane(ch);
            auto dst = destination.plane(ch);
            for (std::size_t at = 0; at < frames_;) {
                if (stopped()) return;
                const auto count = std::min(blockFrames, frames_ - at);
                std::copy_n(src.data() + at, count, dst.data() + at);
                at += count;
                if (result.diskStorage()) result.diskStorage()->releaseMappedPages();
            }
        }
    }

    [[nodiscard]] session::AudioBuffer finish()
    {
        if (disk_) {
            disk_->seal();
            return session::AudioBuffer::fromDisk(std::move(disk_));
        }
        return std::move(memory_);
    }

private:
    void check(std::uint32_t channel, std::size_t first, std::size_t count) const
    {
        if (channel >= channels_ || first > frames_ || count > frames_ - first || count > blockFrames)
            throw std::out_of_range("invalid render audio block");
    }
    std::uint32_t channels_;
    std::size_t frames_;
    session::AudioBuffer memory_;
    std::shared_ptr<session::DiskAudio> disk_;
};

} // namespace omawave::render
