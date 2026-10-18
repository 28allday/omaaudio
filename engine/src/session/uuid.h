#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace omawave::session {

// Names one imported source inside a saved project (BRIEF.md section 4).
//
// types.h has said since Phase 1 that UUIDs exist to name files inside a saved
// `.omaaudio` and that introducing them earlier would be inventing format surface.
// Phase 4B is the phase that needs them.
//
// SourceId stays what it is -- an index into the project's sources, valid only within
// one run. The uuid is what survives to disk, which is why clips reference their source
// by uuid in session.toml and by SourceId in memory.
struct Uuid {
    std::array<std::uint8_t, 16> bytes{};

    [[nodiscard]] bool operator==(const Uuid&) const = default;

    [[nodiscard]] bool isNull() const noexcept;

    // Canonical 8-4-4-4-12 hex form. Section 4 asks for human-readable where possible,
    // and a project directory whose filenames can be matched by eye to session.toml is
    // worth the four dashes.
    [[nodiscard]] std::string toString() const;

    // Version 4, from the system CSPRNG. Returns a null uuid only if that is
    // unreadable, which the caller must treat as a failure rather than ignore.
    [[nodiscard]] static Uuid generate();

    [[nodiscard]] static std::optional<Uuid> parse(std::string_view text);
};

} // namespace omawave::session
