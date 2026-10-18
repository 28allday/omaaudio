#pragma once

#include "types.h"

#include <string>

namespace omawave::session {

// The picture a session is being cut against (W11). One file, placed on the
// timeline by `offset`: the project frame at which the video's own time zero
// sits (negative when the picture starts before the project). The file is read
// from `path` when the project opens; a missing file keeps its place and its
// offset so relinking is the only repair. `dropFrame` is how its timecode is
// shown, nothing more. Everything here is an undo step like any other edit.
struct VideoReference {
    std::string path;
    FramePos offset = 0;
    bool dropFrame = false;
    // Format 15: media/<filename> within the bundle. path remains the resolved
    // runtime path; Save As/recovery copy this immutable reference if needed.
    std::string bundlePath;

    [[nodiscard]] bool empty() const noexcept { return path.empty(); }
    [[nodiscard]] bool operator==(const VideoReference&) const noexcept = default;
};

} // namespace omawave::session
