#pragma once

#include "types.h"
#include <compare>
#include <cstdint>
#include <functional>
#include <type_traits>

namespace omawave::session {

// A rack's owner, independent of timeline indices and numeric ID collisions.
// Invalid and future clip targets must never fall back to the master rack.
class EffectOwner {
public:
    enum class Kind : std::uint8_t { Invalid, Track, Master, Clip };

    constexpr EffectOwner() noexcept = default;
    [[nodiscard]] static constexpr EffectOwner track(TrackId id) noexcept
    {
        return static_cast<std::uint32_t>(id) < 0xFFFFFFFEU
            ? EffectOwner(Kind::Track, static_cast<std::uint32_t>(id)) : EffectOwner{};
    }
    [[nodiscard]] static constexpr EffectOwner master() noexcept { return {Kind::Master, 0}; }
    [[nodiscard]] static constexpr EffectOwner clip(std::uint64_t identity) noexcept
    {
        return identity ? EffectOwner(Kind::Clip, identity) : EffectOwner{};
    }
    [[nodiscard]] constexpr Kind kind() const noexcept { return kind_; }
    [[nodiscard]] constexpr bool isTrack() const noexcept { return kind_ == Kind::Track; }
    [[nodiscard]] constexpr bool isMaster() const noexcept { return kind_ == Kind::Master; }
    [[nodiscard]] constexpr bool isClip() const noexcept { return kind_ == Kind::Clip; }
    [[nodiscard]] constexpr bool valid() const noexcept { return kind_ != Kind::Invalid; }
    [[nodiscard]] constexpr TrackId trackId() const noexcept
    {
        return isTrack() ? TrackId(static_cast<std::uint32_t>(identity_)) : kInvalidTrack;
    }
    [[nodiscard]] constexpr std::uint64_t identity() const noexcept { return identity_; }
    [[nodiscard]] constexpr bool operator==(const EffectOwner&) const noexcept = default;
    [[nodiscard]] constexpr auto operator<=>(const EffectOwner&) const noexcept = default;

private:
    constexpr EffectOwner(Kind kind, std::uint64_t identity) noexcept : kind_(kind), identity_(identity) {}
    Kind kind_ = Kind::Invalid;
    std::uint64_t identity_ = 0;
};
static_assert(std::is_trivially_copyable_v<EffectOwner>);

} // namespace omawave::session

template <> struct std::hash<omawave::session::EffectOwner> {
    std::size_t operator()(omawave::session::EffectOwner owner) const noexcept
    {
        const auto value = std::hash<std::uint64_t>{}(owner.identity());
        const auto kind = static_cast<std::size_t>(owner.kind());
        return value ^ (kind + std::size_t{0x9e3779b9U} + (value << 6) + (value >> 2));
    }
};
