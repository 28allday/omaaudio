#pragma once

#include "io/project_file.h"

namespace omawave::io {

struct CollectResult {
    bool ok = false, cancelled = false, published = false;
    std::string error, path;
    std::size_t sources = 0, plugins = 0;
    bool video = false;
};

// Worker only. Snapshot all retained sources, view and reference media into a
// new directory. Existing/racing destinations are never replaced. Copies no
// plugin binaries; requirements.toml inventories every insert, including bypass.
// Progress phases: audio, video, metadata, inventory, publish; fractions 0..1.
// published distinguishes a late directory-sync failure from a failed copy.
[[nodiscard]] CollectResult collectProject(const session::Project& project,
    const ProjectView& view, const std::string& path, const SaveOptions& options = {});

} // namespace omawave::io
