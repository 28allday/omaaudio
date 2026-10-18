#pragma once

#include "lv2/chain.h"
#include "engine/loudness.h"
#include "session/audio_buffer.h"
#include "session/project.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// The render layer sits above session/, engine/, io/ and lv2/: it is the one place
// that composes them into a finished programme, offline, on a worker thread. Nothing
// below it includes it.
namespace omawave::render {

// 0..1, called from the rendering thread between blocks. Never from the audio thread.
using ProgressFn = std::function<void(float)>;

// Worker-only analysis of the same latency-compensated track/bus output as an
// export. The visitor borrows at most one engine block and must not retain it.
// Frames are absolute timeline positions; audio is streamed from zero so effect
// history before the requested endpoint is represented correctly. The endpoint
// may follow the final clip so analysis can include an effect's remaining tail.
using MixBlockVisitor = std::function<void(session::FramePos, const float* const*, std::uint32_t, std::uint32_t)>;
struct MixStreamResult {
    bool ok = false, cancelled = false;
    std::string error;
    session::FramePos frames = 0;
};

// A track's inserts, instantiated offline, for the mix to run each track through
// before it is summed -- what the transport does live. A track with no entry is
// summed plain.
struct TrackChain {
    session::TrackId track = session::kInvalidTrack;
    lv2::Chain* chain = nullptr;
};

[[nodiscard]] MixStreamResult visitMixInput(const session::Project&, std::uint32_t channels,
    session::FramePos end, const MixBlockVisitor&, const std::vector<TrackChain>& chains = {},
    const std::atomic<bool>* cancel = nullptr, const ProgressFn& progress = {});

struct MixRender {
    bool ok = false;
    bool cancelled = false;
    std::string error;
    session::AudioBuffer audio;         // the bus input: every track through its
                                        // inserts, gain and pan, summed at unity
};

// The mix as the transport hears it, before the master bus: each track rendered
// from its clips, through its chain (with the chain's latency taken back out),
// then its gain and pan -- one law per track, mono when every clip is mono, as
// the transport pans -- and summed. Mute and solo apply. Long intermediate
// buffers are written in blocks to private disk storage, sealed before return.
[[nodiscard]] MixRender mixTracks(const session::Project& project, std::uint32_t channels,
                                  const std::vector<TrackChain>& trackChains = {},
                                  const std::atomic<bool>* cancel = nullptr,
                                  const ProgressFn& progress = {});

// A worker/main-thread snapshot of one track and all incoming audio/key feeds.
// Only its output reaches the master input. Solo is ignored; Mute is retained.
// Sources remain shared and immutable. The original project is never changed.
[[nodiscard]] std::optional<session::Project> isolateTrackOutput(
    const session::Project& project, session::TrackId track, std::string& error);

struct MixMeasurement {
    bool ok = false;
    bool cancelled = false;
    std::string error;
    engine::LoudnessReport report;
};

// The same latency-compensated track/bus signal as mixTracks, measured in blocks
// without retaining the complete rendered audio. No master chain/trim is applied.
[[nodiscard]] MixMeasurement measureMixInput(const session::Project& project,
    std::uint32_t channels, const std::vector<TrackChain>& trackChains = {},
    const std::atomic<bool>* cancel = nullptr, const ProgressFn& progress = {});

struct MasterRender {
    bool ok = false;
    bool cancelled = false;
    std::string error;

    session::AudioBuffer audio;         // at the project rate
    std::uint32_t latencyDropped = 0;   // frames of chain latency compensated away
};

// The whole session as the transport would play it, minus the monitor level: the
// tracks summed (session::mixdown), the bus trim, then the master inserts.
//
// `chain` is the master bus chain the CALLER built from project.masterInserts() and
// warmed up -- on the main thread, because instantiating a plugin means lilv, and
// lilv is not for a second thread while the live chains are being rebuilt. This only
// ever calls process() on it, which is the audio-thread class and safe anywhere that
// nothing else is running the same instance. A null chain is a bus with no inserts.
//
// Latency-compensated: the chain's reported latency is fed through as trailing
// silence and dropped from the front, so the render lines up with the timeline.
[[nodiscard]] MasterRender renderMaster(const session::Project& project,
                                        std::uint32_t channels, lv2::Chain* chain,
                                        const std::atomic<bool>* cancel = nullptr,
                                        const ProgressFn& progress = {},
                                        const std::vector<TrackChain>& trackChains = {});

// The bus alone: `mix` (the bus input, as mixTracks makes it) through the trim and
// the master chain. What renderMaster does once it has the mix.
[[nodiscard]] MasterRender renderBus(session::AudioBuffer mix, float trim, lv2::Chain* chain,
                                     const std::atomic<bool>* cancel = nullptr,
                                     const ProgressFn& progress = {},
                                     const session::AutomationCurve& volume = {},
                                     const std::vector<session::Insert>& inserts = {},
                                     const session::MusicalTime& musicalTime = {});

} // namespace omawave::render
