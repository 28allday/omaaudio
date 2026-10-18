#pragma once

#include "types.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace omawave::session {

// What a broadcast-wave file says about itself (EBU Tech 3285 `bext`), as plain
// values. Strings are UTF-8 here and clipped to the chunk's fixed widths only
// when written. `timeReference` is in samples since midnight at the rate of the
// audio it belongs to: a file's own rate on import, the project's rate once it
// is the project's.
struct BroadcastInfo {
    bool present = false;
    std::string description;            // 256 bytes in the chunk
    std::string originator;             // 32
    std::string originatorReference;    // 32
    std::string originationDate;        // "yyyy-mm-dd"
    std::string originationTime;        // "hh:mm:ss"
    std::int64_t timeReference = 0;
    int version = 0;                    // 0, 1 (UMID) or 2 (loudness)
    std::string umidHex;                // 64 bytes as 128 hex digits, empty when none
    std::string codingHistory;
    // Version 2 loudness, LUFS / LU / dBTP; absent when the file had none.
    std::optional<double> loudnessValue, loudnessRange, maxTruePeak,
        maxMomentaryLoudness, maxShortTermLoudness;

    [[nodiscard]] bool operator==(const BroadcastInfo&) const = default;
};

// A cue point or region carried by a file, in that file's own frames.
struct FileMarker {
    std::int64_t position = 0;
    std::int64_t length = 0;            // 0 for a point
    std::string label;

    [[nodiscard]] bool operator==(const FileMarker&) const = default;
};

// Everything preserved from a file besides its samples. Kept on the source it
// came from exactly as read (provenance) and never merged into the project's
// own metadata; the export writes the project's.
struct AudioMetadata {
    BroadcastInfo bext;
    std::vector<FileMarker> markers;
    std::string ixml;                   // the iXML chunk's text, verbatim
    std::map<std::string, std::string> strings;   // INFO: title, artist, comment, date, software, ...

    [[nodiscard]] bool empty() const noexcept
    {
        return !bext.present && markers.empty() && ixml.empty() && strings.empty();
    }
    [[nodiscard]] bool operator==(const AudioMetadata&) const = default;
};

// The project's own broadcast description, written to every BWF it renders.
struct ProjectMetadata {
    std::string description;
    std::string originator;
    std::string originatorReference;
    std::string originationDate;        // empty: the render's date
    std::string originationTime;        // empty: the render's time
    bool timeReferenceSet = false;
    std::int64_t timeReference = 0;     // project-rate samples since midnight
    std::string codingHistory;          // any imported history the owner keeps; the render appends its own line

    [[nodiscard]] bool empty() const noexcept
    {
        return description.empty() && originator.empty() && originatorReference.empty()
            && originationDate.empty() && originationTime.empty() && !timeReferenceSet
            && codingHistory.empty();
    }
    [[nodiscard]] bool operator==(const ProjectMetadata&) const = default;
};

// Rescales a position in samples-since-midnight or file frames between rates,
// rounding to the nearest frame; the seconds are what is preserved.
[[nodiscard]] inline std::int64_t rescaleFrames(std::int64_t frames, std::uint32_t from, std::uint32_t to) noexcept
{
    if (from == 0 || to == 0 || from == to) return frames;
    const long double scaled = static_cast<long double>(frames) * to / from;
    return static_cast<std::int64_t>(scaled + (scaled >= 0 ? 0.5L : -0.5L));
}

} // namespace omawave::session
