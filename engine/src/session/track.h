#pragma once

#include "ducking_recipe.h"
#include <optional>

#include "clip.h"
#include "automation.h"
#include "insert.h"
#include "types.h"

#include <cmath>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace omawave::session {

// What a track is for. A role is a label and a colour; Routing below specifies the
// actual signal path independently. Stored in
// the project by name, so a reordering of this enum can never relabel a session.
enum class TrackRole : std::uint8_t {
    Dialogue,
    Host,
    Guest,
    Music,
    Ambience,
    Sfx,
    Foley,
    Aux,
    Bus,
    Master,
};

// How tall a lane draws. A view preference that lives in the project, because a
// session laid out for a podcast should reopen laid out for a podcast -- but it is
// not an undo step; see restoreState.
enum class TrackHeight : std::uint8_t {
    Minimized,
    Compact,
    Normal,
    Expanded,
};

// Lower-case names, which is what the project file and the UI both use. Parsing is
// forgiving of nothing: an unknown name is nullopt and the caller keeps the default,
// so a hand-edited typo cannot become a fourth kind of track.
[[nodiscard]] constexpr std::string_view roleName(TrackRole role) noexcept
{
    switch (role) {
    case TrackRole::Dialogue: return "dialogue";
    case TrackRole::Host: return "host";
    case TrackRole::Guest: return "guest";
    case TrackRole::Music: return "music";
    case TrackRole::Ambience: return "ambience";
    case TrackRole::Sfx: return "sfx";
    case TrackRole::Foley: return "foley";
    case TrackRole::Aux: return "aux";
    case TrackRole::Bus: return "bus";
    case TrackRole::Master: return "master";
    }
    return "dialogue";
}

[[nodiscard]] constexpr std::optional<TrackRole> roleFromName(std::string_view name) noexcept
{
    for (const TrackRole r : {TrackRole::Dialogue, TrackRole::Host, TrackRole::Guest,
                              TrackRole::Music, TrackRole::Ambience, TrackRole::Sfx,
                              TrackRole::Foley, TrackRole::Aux, TrackRole::Bus,
                              TrackRole::Master}) {
        if (roleName(r) == name)
            return r;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr std::string_view heightName(TrackHeight height) noexcept
{
    switch (height) {
    case TrackHeight::Minimized: return "minimized";
    case TrackHeight::Compact: return "compact";
    case TrackHeight::Normal: return "normal";
    case TrackHeight::Expanded: return "expanded";
    }
    return "normal";
}

[[nodiscard]] constexpr std::optional<TrackHeight>
heightFromName(std::string_view name) noexcept
{
    for (const TrackHeight h : {TrackHeight::Minimized, TrackHeight::Compact,
                                TrackHeight::Normal, TrackHeight::Expanded}) {
        if (heightName(h) == name)
            return h;
    }
    return std::nullopt;
}

// Gain factors for one track's contribution to a stereo bus.
//
// Two laws, because one law cannot be right for both kinds of source:
//
//   * A MONO source has no image of its own, so panning it is placing it. Constant
//     power (-3 dB at centre) is what keeps a sweep from bulging in the middle.
//   * A STEREO source already has an image, so panning it is balancing it. Centre
//     must be unity, or every stereo clip quietly loses 3 dB for no reason.
//
// Both agree at the extremes, so a hard-panned track sounds the same either way.
// Which law applies is decided per clip by its source's channel count, not per track,
// because one track can hold both kinds.
// Defaults are centre, not unity, so a default-constructed PanGains behaves exactly
// like panGainsFor(0.0f). Defaulting to unity would make an un-panned track quietly
// louder than a centre-panned one, which is the kind of difference nobody finds by
// reading the code.
struct PanGains {
    static constexpr float kCentre = 0.70710678118654752440f;   // -3 dB

    float monoL = kCentre;
    float monoR = kCentre;
    float stereoL = 1.0f;
    float stereoR = 1.0f;
};

// `pan` is -1 (hard left) to +1 (hard right). Evaluated on the main thread only: the
// audio thread must never run a trig function (BRIEF.md section 2).
[[nodiscard]] inline PanGains panGainsFor(float pan) noexcept
{
    const float p = pan < -1.0f ? -1.0f : (pan > 1.0f ? 1.0f : pan);

    // Constant power: quarter-turn from hard left to hard right.
    constexpr float kQuarterPi = 0.78539816339744830961f;
    const float angle = (p + 1.0f) * kQuarterPi;

    PanGains g;
    g.monoL = std::cos(angle);
    g.monoR = std::sin(angle);

    // Balance: attenuate the side being panned away from, leave the other at unity.
    g.stereoL = p > 0.0f ? 1.0f - p : 1.0f;
    g.stereoR = p < 0.0f ? 1.0f + p : 1.0f;
    return g;
}

inline constexpr TrackId kNoTrackOutput{0xFFFFFFFEU};
inline constexpr std::size_t kMaxTrackSends = 8;

struct TrackSend {
    TrackId target = kInvalidTrack;
    float gain = 1.0f;
    bool preFader = false;
    bool operator==(const TrackSend&) const = default;
};

struct TrackRouting {
    // kInvalidTrack is the master; kNoTrackOutput disconnects the main output.
    TrackId output = kInvalidTrack;
    std::vector<TrackSend> sends;
    TrackId sidechain = kInvalidTrack;
    [[nodiscard]] bool direct() const noexcept {
        return output == kInvalidTrack && sends.empty() && sidechain == kInvalidTrack;
    }
    bool operator==(const TrackRouting&) const = default;
};

struct Track {
    TrackId id = kInvalidTrack;
    std::string name;

    TrackRole role = TrackRole::Dialogue;

    // An explicit colour as "#RRGGBB", or empty to take the role's. Kept as text
    // because the model has no colour type and does not want one; the UI owns what a
    // colour looks like.
    std::string colour;

    TrackHeight height = TrackHeight::Normal;

    // Linear gain, not dB. Conversion happens at the UI boundary so the audio path
    // never runs a std::pow per block.
    float gain = 1.0f;

    // -1 hard left, 0 centre, +1 hard right.
    float pan = 0.0f;

    bool muted = false;
    bool soloed = false;

    AutomationCurve volumeAutomation{};
    AutomationCurve panAutomation{};
    std::vector<Clip> clips;

    // Inserts, in signal order. Pre-fader and pre-pan: a compressor should not hear
    // your fader moves, and a fader is a listening decision made after the processing.
    std::vector<Insert> inserts;

    TrackRouting routing{};

    // Generated ducking that follows its trigger tracks (see ducking_recipe.h).
    std::optional<DuckingRecipe> ducking{};

    [[nodiscard]] FramePos length() const noexcept
    {
        FramePos end = 0;
        for (const Clip& c : clips) {
            if (c.timelineEnd() > end)
                end = c.timelineEnd();
        }
        return end;
    }

    // Gain with mute and solo resolved, so the audio thread never evaluates solo
    // logic -- it cannot, since solo is a property of the whole track set.
    //
    // Mute beats solo: soloing a track you have explicitly muted leaves it silent.
    // The alternative -- solo un-muting - means a solo can make something audible
    // that you deliberately switched off, which is the more surprising of the two.
    [[nodiscard]] float effectiveGain(bool anySoloed) const noexcept
    {
        if (muted)
            return 0.0f;
        if (anySoloed && !soloed)
            return 0.0f;
        return gain;
    }
};

} // namespace omawave::session
