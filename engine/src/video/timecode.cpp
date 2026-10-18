#include "timecode.h"

#include <algorithm>
#include <cmath>
#include <charconv>
#include <cstdio>
#include <limits>
#include <numeric>
#include <string_view>

namespace omawave::video {

namespace {
std::int64_t dropPerMinute(FrameRate rate) noexcept
{
    return rate.num == 60000 && rate.den == 1001 ? 4 : 2;
}

std::string_view trimmed(std::string_view text)
{
    constexpr std::string_view space = " \t\r\n";
    const auto first = text.find_first_not_of(space);
    if (first == std::string_view::npos) return {};
    return text.substr(first, text.find_last_not_of(space) - first + 1);
}

bool whole(std::string_view text, std::uint64_t& out)
{
    if (text.empty()) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), out);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
} // namespace

bool FrameRate::dropFrameCapable() const noexcept
{
    return den == 1001 && (num == 30000 || num == 60000);
}

std::int64_t FrameRate::nominal() const noexcept
{
    if (!valid()) return 0;
    return num / den + (num % den != 0);   // ceil, without overflowing num + den
}

std::string FrameRate::text() const
{
    if (!valid()) return "unknown";
    if (num % den == 0) return std::to_string(num / den);
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.3f", fps());
    std::string s = buffer;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

std::string FrameRate::ratioText() const
{
    return valid() ? std::to_string(num) + "/" + std::to_string(den) : "0/0";
}

FrameRate parseFrameRate(const std::string& text)
{
    const auto s = trimmed(text);
    if (s.empty()) return {};
    const auto slash = s.find('/');
    if (slash != std::string_view::npos) {
        std::uint64_t num = 0, den = 0;
        constexpr auto maximum = std::uint64_t(std::numeric_limits<std::int64_t>::max());
        if (!whole(s.substr(0, slash), num) || !whole(s.substr(slash + 1), den)
            || num == 0 || den == 0 || num > maximum || den > maximum) return {};
        FrameRate rate{static_cast<std::int64_t>(num), static_cast<std::int64_t>(den)};
        const auto g = std::gcd(rate.num, rate.den);
        rate.num /= g; rate.den /= g;
        return rate;
    }
    long double fps = 0;
    const auto parsed = std::from_chars(s.data(), s.data() + s.size(), fps);
    if (parsed.ec != std::errc{} || parsed.ptr != s.data() + s.size() || !std::isfinite(fps) || fps <= 0) return {};
    // The familiar decimals are the 1001 rates, not what they print as.
    for (const std::int64_t n : {24000, 30000, 60000, 48000, 120000}) {
        if (std::fabs(fps - static_cast<long double>(n) / 1001.0L) < 0.002L) return {n, 1001};
    }
    const bool integer = std::fabs(fps - std::round(fps)) < 1e-6L;
    const auto numerator = std::round(fps * (integer ? 1 : 1000));
    if (numerator < 1 || numerator >= 9223372036854775808.0L) return {};
    return {static_cast<std::int64_t>(numerator), integer ? 1 : 1000};
}

std::int64_t frameIndexAt(double seconds, FrameRate rate) noexcept
{
    if (!rate.valid() || !std::isfinite(seconds)) return 0;
    // A hair of slack so a frame's own start time is that frame, not the one before.
    const auto index = std::floor(static_cast<long double>(seconds) * rate.num / rate.den + 1e-6L);
    if (index >= 9223372036854775808.0L) return std::numeric_limits<std::int64_t>::max();
    if (index < -9223372036854775808.0L) return std::numeric_limits<std::int64_t>::min();
    return static_cast<std::int64_t>(index);
}

double frameStart(std::int64_t index, FrameRate rate) noexcept
{
    if (!rate.valid()) return 0.0;
    return static_cast<double>(index) * static_cast<double>(rate.den) / static_cast<double>(rate.num);
}

std::string timecode(std::int64_t frameIndex, FrameRate rate, bool dropFrame)
{
    if (!rate.valid()) return "--:--:--:--";
    const bool negative = frameIndex < 0;
    std::uint64_t index = negative ? std::uint64_t(-(frameIndex + 1)) + 1 : std::uint64_t(frameIndex);
    const auto nominal = std::uint64_t(rate.nominal());
    const bool df = dropFrame && rate.dropFrameCapable();
    if (df) {
        // Skipped numbers are added back so the digits read like the clock.
        const auto drop = std::uint64_t(dropPerMinute(rate));
        const auto perTenMinutes = nominal * 600 - drop * 9;   // 17982
        const auto perMinute = nominal * 60 - drop;             // 1798
        const auto tens = index / perTenMinutes;
        const auto rest = index % perTenMinutes;
        index += drop * 9 * tens;
        if (rest > drop) index += drop * ((rest - drop) / perMinute);
    }
    const auto ff = index % nominal;
    const auto seconds = index / nominal;
    char buffer[48];
    std::snprintf(buffer, sizeof buffer, "%s%02lld:%02lld:%02lld%c%02lld", negative ? "-" : "",
                  static_cast<long long>(seconds / 3600), static_cast<long long>(seconds / 60 % 60),
                  static_cast<long long>(seconds % 60), df ? ';' : ':', static_cast<long long>(ff));
    return buffer;
}

std::int64_t parseTimecode(const std::string& text, FrameRate rate, bool dropFrame)
{
    if (!rate.valid()) return -1;
    auto s = trimmed(text);
    bool negative = false;
    if (!s.empty() && s.front() == '-') { negative = true; s.remove_prefix(1); }
    std::uint64_t fields[4]{};
    char sep = ':';
    const auto nominal = std::uint64_t(rate.nominal());
    for (unsigned i = 0; i < 4; ++i) {
        const auto end = i == 3 ? s.size() : s.find_first_of(":;,");
        if (end == std::string_view::npos) return -1;
        const auto digits = s.substr(0, end);
        if ((i == 1 || i == 2) && digits.size() > 2) return -1;
        if (i == 3 && digits.size() > std::max<std::size_t>(2, std::to_string(nominal - 1).size())) return -1;
        if (!whole(digits, fields[i])) return -1;
        if (i < 3) {
            if (i < 2 && s[end] != ':') return -1;
            if (i == 2) sep = s[end];
            s.remove_prefix(end + 1);
        }
    }
    const auto [hh, mm, ss, ff] = fields;
    if (mm > 59 || ss > 59) return -1;
    if (ff >= nominal) return -1;
    const bool df = (sep != ':' || dropFrame) && rate.dropFrameCapable();
    // Count in unsigned arithmetic until drop-frame numbers have been removed;
    // a valid signed frame count can temporarily exceed INT64_MAX here.
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (hh > (maximum - mm * 60 - ss) / 3600) return -1;
    const auto seconds = hh * 3600 + mm * 60 + ss;
    if (seconds > (maximum - ff) / nominal) return -1;
    auto index = seconds * nominal + ff;
    if (df) {
        const auto drop = std::uint64_t(dropPerMinute(rate));
        const auto minutes = hh * 60 + mm;
        if (ss == 0 && mm % 10 != 0 && ff < drop) return -1;   // a number that never occurs
        index -= drop * (minutes - minutes / 10);
    }
    const auto limit = std::uint64_t(std::numeric_limits<std::int64_t>::max()) + (negative ? 1 : 0);
    if (index > limit) return -1;
    if (negative && index == limit) return std::numeric_limits<std::int64_t>::min();
    return negative ? -static_cast<std::int64_t>(index) : static_cast<std::int64_t>(index);
}

} // namespace omawave::video
