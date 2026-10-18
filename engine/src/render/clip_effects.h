#pragma once

#include "lv2/chain.h"
#include "lv2/host_features.h"
#include "lv2/world.h"
#include "session/audio_buffer.h"
#include "session/project.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Clip effects as heard. A clip's effect rack is processed once per shared
// region, offline, into sealed private audio; playback, export, analysis and
// every other reader then read that audio exactly as they read any source.
// Live and delivered sound therefore share one rendering, and seeking, looping
// and plan swaps need no plugin preparation in the audio callback.
//
// The region is rendered from its own origin through its full retained length,
// so trimmed-off context still feeds stateful effects and unchanged split views
// share one continuous result. Effect latency is compensated here. Output stops
// at each visible view; an effect tail never extends a clip.
namespace omawave::render {

// True when at least one insert would change the sound. A rack that is empty or
// entirely bypassed plays the dry source, retaining its settings.
[[nodiscard]] bool clipEffectsActive(const session::ClipRack& rack) noexcept;

// One shared processing region as heard in a particular project context. The
// immutable rack pointer is the identity: equal settings never imply sharing.
struct ClipEffectRegion {
    std::shared_ptr<const session::ClipRack> rack;
    session::FramePos origin = 0;          // view.timelineStart - view.effectOffset
    std::uint32_t channels = 0;            // the dry source's channel count (the rendering
                                           // is stereo when a mono clip needs a stereo effect)
    std::uint32_t sampleRate = 0;
    session::MusicalTime musicalTime{};    // plug-ins see the project's transport
    [[nodiscard]] bool operator==(const ClipEffectRegion& other) const noexcept {
        return rack == other.rack && origin == other.origin && channels == other.channels
            && sampleRate == other.sampleRate && musicalTime == other.musicalTime;
    }
};

// Every distinct region whose rack is active, in track then timeline order.
// Throws std::runtime_error for a model that violates the region invariants.
[[nodiscard]] std::vector<ClipEffectRegion> clipEffectRegions(const session::Project& project);

// Builds the rack's chain at the region's channel count on the calling (main)
// thread. Bypassed slots keep their identity without loading the plug-in. An
// unavailable active plug-in fails rather than being skipped.
[[nodiscard]] std::unique_ptr<lv2::Chain> buildClipEffectChain(const lv2::World& world,
    lv2::HostFeatures& features, const ClipEffectRegion& region, std::string& error);

struct ClipEffectRender {
    bool ok = false, cancelled = false;
    std::string error;
    session::AudioBuffer audio;            // rack->length frames at the chain's width, sealed
};

// Worker: warms the caller's private chain, then processes the region. The
// chain must have been built for this region and is used by nothing else.
[[nodiscard]] ClipEffectRender renderClipEffectRegion(const session::Project& project,
    const ClipEffectRegion& region, lv2::Chain& chain,
    const std::atomic<bool>* cancel = nullptr, const std::function<void(float)>& progress = {});

// Rendered regions, keyed by region. Copies share the immutable audio.
class ClipEffectRenders {
public:
    void insert(const ClipEffectRegion& region, session::AudioBuffer audio);
    [[nodiscard]] const session::AudioBuffer* find(const ClipEffectRegion& region) const noexcept;
    // The newest rendering of an earlier version of the same region (same owner,
    // origin, retained source span and context): what playback keeps hearing
    // while a changed setting is prepared, so adjustments never drop out.
    [[nodiscard]] const session::AudioBuffer* findStandIn(const ClipEffectRegion& region) const noexcept;
    // Keeps only the listed regions; returns how many were released.
    std::size_t retain(const std::vector<ClipEffectRegion>& wanted);
    // Keeps the listed regions and up to `recent` others, newest first.
    std::size_t trim(const std::vector<ClipEffectRegion>& wanted, std::size_t recent);
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] std::size_t bytes() const noexcept;
    void clear() noexcept { entries_.clear(); }
private:
    struct Entry { ClipEffectRegion region; std::shared_ptr<const session::AudioBuffer> audio; };
    std::vector<Entry> entries_;
};

struct AppliedClipEffects {
    session::Project project{48000};
    std::vector<ClipEffectRegion> missing;  // active regions without a rendering
};

// A copy of the project in which every clip plays its effects: an active view
// reads the region's rendering; an inactive rack plays dry. A missing rendering
// is an error (std::runtime_error) unless the caller is live playback: then the
// view plays an earlier version's rendering when allowStandIn finds one, or is
// left out (silent), and the region is reported missing either way. Nothing is
// ever played or delivered dry by accident. Sources are shared, never copied.
enum class MissingClipEffects { Refuse, Silence, StandIn };
[[nodiscard]] AppliedClipEffects applyClipEffects(const session::Project& project,
    const ClipEffectRenders& renders, MissingClipEffects missing = MissingClipEffects::Refuse);

// Convenience for workers that own private chains: renders every missing region
// with chains the caller built beforehand (same order as `regions`), adds them to
// `renders`, then applies. Fails on the first region that cannot be rendered.
struct PreparedClipEffects {
    bool ok = false, cancelled = false;
    std::string error;
    session::Project project{48000};
};
[[nodiscard]] PreparedClipEffects prepareClipEffects(const session::Project& project,
    ClipEffectRenders& renders, const std::vector<ClipEffectRegion>& regions,
    const std::vector<lv2::Chain*>& chains, const std::atomic<bool>* cancel = nullptr,
    const std::function<void(float)>& progress = {});

} // namespace omawave::render
