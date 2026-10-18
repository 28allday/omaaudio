#include "apply_effect.h"

#include "render/audio_store.h"
#include "render/master_render.h"
#include "session/clip_envelope.h"
#include "session/edits.h"
#include "session/render.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace omawave::render {
namespace {
struct Cancelled {};
void check(const std::atomic<bool>* cancel)
{
    if (cancel && cancel->load(std::memory_order_relaxed)) throw Cancelled{};
}
} // namespace

session::SampleEditResult applyEffect(session::Project snapshot, session::TrackId id,
    session::FramePos start, session::FramePos end, lv2::Chain& chain,
    const std::vector<session::Insert>& inserts, const std::string& name,
    const std::atomic<bool>* cancel)
{
    using session::FramePos;
    try {
        check(cancel);
        auto* track = snapshot.track(id);
        if (!track || track->clips.empty() || !session::edits::isWellFormed(track->clips)
            || start < 0 || end <= start || end > snapshot.length() || !snapshot.sampleRate())
            throw std::runtime_error("applying an effect needs a valid selection on a track with audio");
        unsigned channels = 0;
        for (const auto& clip : track->clips) {
            const auto* source = snapshot.source(clip.source);
            if (!source || !clip.validSource(FramePos(source->audio.frames()))
                || source->audio.channels() < 1 || source->audio.channels() > 2)
                throw std::runtime_error("applying an effect requires valid mono or stereo clips");
            if (channels && channels != source->audio.channels())
                throw std::runtime_error("convert this mixed-layout track to mono or stereo before applying an effect");
            channels = source->audio.channels();
        }
        const auto dry = session::renderClipRange(snapshot, track->clips, start, end, channels, cancel);
        check(cancel);
        if (dry.frames() != std::size_t(end - start))
            throw std::runtime_error("the selection could not be read");
        if (!chain.warmUp(cancel)) {
            check(cancel);
            throw std::runtime_error("the effect could not start");
        }
        auto wet = renderBus(dry, 1, &chain, cancel, {}, {}, inserts, snapshot.musicalTime());
        check(cancel);
        if (!wet.ok) throw std::runtime_error(wet.error.empty() ? "the effect could not process the selection" : wet.error);
        if (wet.audio.channels() != channels || wet.audio.frames() != dry.frames())
            throw std::runtime_error("the effect returned audio of a different shape");

        const auto frames = dry.frames();
        const auto edge = std::min<std::size_t>(snapshot.sampleRate() / 200, frames / 2);
        AudioStore result(channels, frames);
        std::array<float, AudioStore::blockFrames> block{};
        bool changed = false;
        for (unsigned c = 0; c < channels; ++c) {
            const std::span<const float> original = dry.plane(c), processed = wet.audio.plane(c);
            for (std::size_t at = 0; at < frames; at += block.size()) {
                check(cancel);
                const auto count = std::min(block.size(), frames - at);
                for (std::size_t i = 0; i < count; ++i) {
                    const auto f = at + i;
                    const double ramp = edge ? std::min({1.0, double(f) / edge, double(frames - 1 - f) / edge}) : 1.0;
                    const double value = original[f] + (double(processed[f]) - original[f]) * ramp;
                    if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
                        throw std::runtime_error("the effect produced an invalid sample");
                    block[i] = float(value);
                    changed = changed || block[i] != original[f];
                }
                result.write(c, at, std::span<const float>(block).first(count));
                if (dry.diskStorage()) dry.diskStorage()->releaseMappedPages();
                if (wet.audio.diskStorage()) wet.audio.diskStorage()->releaseMappedPages();
            }
        }
        check(cancel);
        if (!changed) return {std::nullopt, false, {}, true};
        if (snapshot.sources().size() >= std::size_t(session::kInvalidSource))
            throw std::runtime_error("too many audio sources");
        const auto source = snapshot.addSource(name, "", result.finish());
        session::edits::overwriteRendered(track->clips,
            session::Clip{.source = source, .timelineStart = start, .length = FramePos(frames)}, track->clips);
        if (const auto error = session::validateClipEnvelopes(snapshot); !error.empty()) throw std::runtime_error(error);
        return {std::move(snapshot), false, {}};
    } catch (const Cancelled&) {
        return {std::nullopt, true, {}};
    } catch (const std::exception& error) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return {std::nullopt, true, {}};
        return {std::nullopt, false, error.what()};
    }
}

} // namespace omawave::render
