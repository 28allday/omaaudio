#pragma once

#include "types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace omawave::session {

// A named position (or range) on the session timeline.
//
// Sequence markers only: a marker belongs to the session, not to a track or a clip,
// so an edit that moves clips does not move the markers over them. That is the
// behaviour every editor has settled on and the one that makes a chapter list
// trustworthy -- "Intro" stays at 0:00 whatever happens to the take.
enum class MarkerType : std::uint8_t {
    Cue,        // one position
    Range,      // position and length
    Chapter,    // one position, exported as a chapter point some day
    Warning,    // one position; drawn amber with an icon, never colour alone
};

[[nodiscard]] constexpr std::string_view markerTypeName(MarkerType type) noexcept
{
    switch (type) {
    case MarkerType::Cue: return "cue";
    case MarkerType::Range: return "range";
    case MarkerType::Chapter: return "chapter";
    case MarkerType::Warning: return "warning";
    }
    return "cue";
}

[[nodiscard]] constexpr std::optional<MarkerType>
markerTypeFromName(std::string_view name) noexcept
{
    for (const MarkerType t : {MarkerType::Cue, MarkerType::Range, MarkerType::Chapter,
                               MarkerType::Warning}) {
        if (markerTypeName(t) == name)
            return t;
    }
    return std::nullopt;
}

struct Marker {
    MarkerId id = kInvalidMarker;
    std::string name;
    FramePos position = 0;
    FramePos length = 0;            // zero for anything but a Range
    MarkerType type = MarkerType::Cue;
    std::string colour;             // "#RRGGBB" or empty for the type's colour
    std::string notes;

    [[nodiscard]] bool operator==(const Marker&) const = default;

    [[nodiscard]] constexpr FramePos end() const noexcept { return position + length; }
};

} // namespace omawave::session
