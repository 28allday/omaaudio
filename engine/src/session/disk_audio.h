#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace omawave::session {

// Private planar float backing. Build and seal on a worker before publication.
// The file is unlinked immediately; its descriptor owns it until the last source
// releases it. Mapping is for non-realtime readers only, never the audio callback.
class DiskAudio {
public:
    static std::shared_ptr<DiskAudio> create(std::uint32_t channels, std::size_t frames,
                                            const std::string& directory = {});
    ~DiskAudio();
    DiskAudio(const DiskAudio&) = delete;
    DiskAudio& operator=(const DiskAudio&) = delete;

    void writePlane(std::uint32_t channel, std::size_t frame, std::span<const float> samples);
    void seal(std::size_t validFrames = 0);
    [[nodiscard]] bool sealed() const noexcept { return mapping_ != nullptr; }
    [[nodiscard]] std::uint32_t channels() const noexcept { return channels_; }
    [[nodiscard]] std::size_t frames() const noexcept { return frames_; }
    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::span<const float> plane(std::uint32_t channel) const noexcept;
    // Worker only. An exact read, including retries/short-read checks. The owner
    // may also read while building, before seal(), to sum overlapping audio.
    [[nodiscard]] bool readPlane(std::uint32_t channel, std::size_t frame,
                                  std::span<float> samples) const noexcept;
    void releaseMappedPages() const noexcept;

private:
    DiskAudio(std::uint32_t channels, std::size_t frames, std::size_t bytes)
        : channels_(channels), frames_(frames), strideFrames_(frames), bytes_(bytes) {}
    std::uint32_t channels_;
    std::size_t frames_, strideFrames_, bytes_;
    int fd_ = -1;
    std::string directory_;   // where the storage lives, for out-of-space messages
    const float* mapping_ = nullptr;
};

} // namespace omawave::session
