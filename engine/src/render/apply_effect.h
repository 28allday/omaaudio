#pragma once

#include "lv2/chain.h"
#include "session/insert.h"
#include "session/project.h"
#include "session/sample_edit.h"

#include <atomic>
#include <string>
#include <vector>

namespace omawave::render {

// Worker-only. One track's audio over [start, end), as its clips play (gain,
// fades and envelopes, before the track's own inserts), through an offline chain,
// written back over exactly that range as new audio named `name`. Outside the
// range nothing changes, and the effect's tail is not added: the selection keeps
// its length. The first and last few milliseconds blend from the original into
// the processed audio so the edit has no click at either edge.
//
// The caller builds `chain` (Offline mode) on the main thread and keeps it alive
// until this returns; it is warmed here. `inserts` are the chain's settings, for
// its control automation. Returns no project when nothing changed.
[[nodiscard]] session::SampleEditResult applyEffect(session::Project snapshot, session::TrackId track,
    session::FramePos start, session::FramePos end, lv2::Chain& chain,
    const std::vector<session::Insert>& inserts, const std::string& name,
    const std::atomic<bool>* cancel = nullptr);

} // namespace omawave::render
