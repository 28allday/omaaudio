#pragma once
#include "project.h"
#include "engine/elastic_processor.h"
#include <atomic>

namespace omawave::session {
// Offline cursor. Adjacent split fragments retain the same processor state.
class ClipReader {
public:
    static constexpr std::uint32_t kBlock = 2048;
    // Readers hear what plays, so a clip with effects is refused unless the
    // caller explicitly wants the dry view (a waveform, a dry-media transform).
    // Effected audio is read from render/clip_effects.h renderings instead.
    enum class Effects { Refuse, Dry };
    explicit ClipReader(const Project& project, Effects effects = Effects::Refuse)
        : project_(project), effects_(effects) {}
    void read(const Clip& clip, FramePos local, std::uint32_t frames,
              const std::atomic<bool>* cancel = nullptr);
    std::span<const float> plane(std::uint32_t channel) const { return output_.plane(channel); }
    std::uint32_t channels() const noexcept { return output_.channels(); }
private:
    const Project& project_;
    Effects effects_;
    AudioBuffer output_;
    const AudioSource* source_ = nullptr;
    std::optional<ElasticSpec> mapping_;
    std::unique_ptr<engine::ElasticProcessor> processor_;
    FramePos nextTimeline_ = -1, nextSource_ = -1;
};
} // namespace omawave::session
