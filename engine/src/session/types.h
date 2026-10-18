#pragma once

#include <cstdint>

namespace omawave::session {

// Sample position on the project timeline. Signed so that arithmetic around zero
// (seeking backwards, negative clip offsets during a trim) cannot silently wrap.
using FramePos = std::int64_t;

// Identifies imported audio inside a Project. Deliberately not a UUID yet: UUIDs
// exist to name files inside a saved .omaaudio (BRIEF.md section 4), and Phase 1 does
// not save projects. Introducing them now would be inventing format surface early.
enum class SourceId : std::uint32_t {};

enum class TrackId : std::uint32_t {};

// Markers are handed ids from a counter the same way tracks are, so a marker removed
// and restored by undo is still the marker it was.
enum class MarkerId : std::uint32_t {};
enum class ClipGroupId : std::uint32_t {};

constexpr SourceId kInvalidSource{0xFFFFFFFFU};
constexpr TrackId kInvalidTrack{0xFFFFFFFFU};
constexpr MarkerId kInvalidMarker{0xFFFFFFFFU};
constexpr ClipGroupId kInvalidClipGroup{0xFFFFFFFFU};

} // namespace omawave::session
