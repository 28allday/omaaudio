#pragma once

#include "peak_builder.h"

#include <string>

namespace omawave::io {

// The `peaks/<uuid>.peak` file from BRIEF.md section 4.
//
// This is a CACHE, and everything here treats it as one: a missing, truncated,
// mismatched or corrupt file is not an error, it is a rebuild. Nothing in a project
// depends on it being present or being right, which is what makes it safe to keep a
// format this blunt.
//
// Little-endian, native float, no compression. Peaks are already a lossy summary of
// the audio; spending anything on encoding them would be spending it twice.

inline constexpr char kPeakMagic[8] = {'O', 'M', 'A', 'W', 'P', 'E', 'A', 'K'};
inline constexpr std::uint32_t kPeakVersion = 1;

// Returns an empty string on success, or the reason it failed.
[[nodiscard]] std::string writePeakCache(const std::string& path, const PeakLevel& level);

// Returns an empty level when the file is absent or unusable for any reason. The
// caller rebuilds; it never has to distinguish the cases.
[[nodiscard]] PeakLevel readPeakCache(const std::string& path);

} // namespace omawave::io
