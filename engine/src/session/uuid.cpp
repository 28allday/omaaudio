#include "uuid.h"

#include <cstdio>
#include <string_view>

namespace omawave::session {
namespace {

constexpr char kHex[] = "0123456789abcdef";

[[nodiscard]] std::optional<std::uint8_t> hexValue(char c) noexcept
{
    if (c >= '0' && c <= '9')
        return static_cast<std::uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f')
        return static_cast<std::uint8_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F')
        return static_cast<std::uint8_t>(c - 'A' + 10);
    return std::nullopt;
}

} // namespace

bool Uuid::isNull() const noexcept
{
    for (const std::uint8_t b : bytes) {
        if (b != 0)
            return false;
    }
    return true;
}

std::string Uuid::toString() const
{
    std::string out;
    out.reserve(36);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out.push_back('-');
        out.push_back(kHex[bytes[i] >> 4]);
        out.push_back(kHex[bytes[i] & 0x0F]);
    }
    return out;
}

Uuid Uuid::generate()
{
    Uuid id;

    // The system CSPRNG rather than <random>: a collision here silently overwrites
    // somebody's audio inside their project, so a seeded PRNG is not good enough.
    std::FILE* urandom = std::fopen("/dev/urandom", "rb");
    if (urandom == nullptr)
        return id;                          // null; the caller must check
    const std::size_t got = std::fread(id.bytes.data(), 1, id.bytes.size(), urandom);
    (void)std::fclose(urandom);
    if (got != id.bytes.size())
        return Uuid{};

    id.bytes[6] = static_cast<std::uint8_t>((id.bytes[6] & 0x0F) | 0x40);   // version 4
    id.bytes[8] = static_cast<std::uint8_t>((id.bytes[8] & 0x3F) | 0x80);   // variant 1
    return id;
}

std::optional<Uuid> Uuid::parse(std::string_view text)
{
    Uuid id;
    std::size_t byte = 0;
    std::size_t nibble = 0;
    std::uint8_t high = 0;

    for (const char c : text) {
        if (c == '-')
            continue;                       // position-insensitive: dashes are cosmetic
        const std::optional<std::uint8_t> value = hexValue(c);
        if (!value.has_value())
            return std::nullopt;
        if (byte >= id.bytes.size())
            return std::nullopt;            // too long

        if (nibble == 0) {
            high = static_cast<std::uint8_t>(*value << 4);
            nibble = 1;
        } else {
            id.bytes[byte++] = static_cast<std::uint8_t>(high | *value);
            nibble = 0;
        }
    }

    if (byte != id.bytes.size() || nibble != 0)
        return std::nullopt;                // too short, or a stray nibble
    return id;
}

} // namespace omawave::session
