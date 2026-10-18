#pragma once

#include "render_plan.h"
#include "session/track.h"
#include "session/types.h"
#include "session/effect_owner.h"

#include <cstdint>

namespace omawave::engine {

enum class CommandType : std::uint8_t {
    None = 0,
    Play,
    Stop,
    Seek,
    SetGain,
    SetPlan,
    SetTrackParams,
    SetPluginControl,
    SetPluginBypass,
    SetMasterTrim,
    SetMasterBypass,
    ResetLoudness,
    SetLoop,
    SetMetronome,
    SetSpeed,
};

// Trivially copyable by construction -- see the static_assert in CommandQueue.
//
// SetPlan carries a borrowed pointer. The audio thread only ever reads through it, and
// the engine keeps the plan (and the audio it references) alive until the audio thread
// acknowledges the generation, so it is never dangling.
struct Command {
    CommandType type = CommandType::None;

    session::FramePos frame = 0;        // Seek
    session::FramePos endFrame = 0;     // SetLoop exclusive end
    float gain = 1.0f;                  // SetGain / SetTrackParams, linear

    const RenderPlan* plan = nullptr;               // SetPlan, borrowed
    std::uint32_t planGeneration = 0;               // SetPlan

    // SetTrackParams. Addressed by id rather than by index: a track removed from the
    // middle of the list shifts every index after it, and a command in flight across
    // that change would land on the wrong track.
    session::TrackId track = session::kInvalidTrack;
    session::PanGains pan;                          // SetTrackParams, precomputed

    // SetPluginControl / SetPluginBypass use an explicit rack owner. Commands
    // retain FIFO ordering with plan publication and native parameter versions.
    // Invalid and unresolved clip owners are deliberately inert.
    session::EffectOwner effectOwner{};
    std::size_t slot = 0;
    std::uint32_t port = 0;
    double parameter = 0;                          // SetPluginControl, format-native precision
    std::uint64_t parameterVersion = UINT64_MAX;    // native value version when this control was accepted
    bool flag = false;

    // Assigned rather than brace-initialised: a designated initialiser that skips
    // defaulted members trips -Wmissing-field-initializers, and section 8 keeps
    // -Wextra -Werror on.
    static Command play() noexcept
    {
        Command c;
        c.type = CommandType::Play;
        return c;
    }

    static Command stop() noexcept
    {
        Command c;
        c.type = CommandType::Stop;
        return c;
    }

    static Command setLoop(bool enabled, session::FramePos start, session::FramePos end) noexcept
    {
        Command c;
        c.type = CommandType::SetLoop;
        c.flag = enabled;
        c.frame = start;
        c.endFrame = end;
        return c;
    }

    // Shuttle speed: whole multiples of normal speed, negative for backwards
    // (-8..8, never 0). Stop puts it back to 1.
    static Command setSpeed(int speed) noexcept
    {
        Command c;
        c.type = CommandType::SetSpeed;
        c.frame = speed;
        return c;
    }

    static Command seek(session::FramePos f) noexcept
    {
        Command c;
        c.type = CommandType::Seek;
        c.frame = f;
        return c;
    }

    static Command setGain(float linear) noexcept
    {
        Command c;
        c.type = CommandType::SetGain;
        c.gain = linear;
        return c;
    }

    // Gain here is already effective: the main thread resolves mute and solo, because
    // solo is a property of the whole track set and the audio thread can only see one
    // track at a time. Pan gains are precomputed for the same reason no trig runs here.
    static Command setTrackParams(session::TrackId track, float effectiveGain,
                                  session::PanGains pan) noexcept
    {
        Command c;
        c.type = CommandType::SetTrackParams;
        c.track = track;
        c.gain = effectiveGain;
        c.pan = pan;
        return c;
    }

    // The audio thread is the only writer of a port buffer, which is the whole reason
    // a knob move travels as a command rather than as a float written from the main
    // thread. The authoritative value lives in the model; this is a copy in flight.
    static Command setPluginControl(session::EffectOwner owner, std::size_t slot,
                                    std::uint32_t port, double value, std::uint64_t version = UINT64_MAX) noexcept
    {
        Command c;
        c.type = CommandType::SetPluginControl;
        c.effectOwner = owner;
        c.slot = slot;
        c.port = port;
        c.parameter = value;
        c.parameterVersion = version;
        return c;
    }

    static Command setPluginBypass(session::EffectOwner owner, std::size_t slot,
                                   bool bypass) noexcept
    {
        Command c;
        c.type = CommandType::SetPluginBypass;
        c.effectOwner = owner;
        c.slot = slot;
        c.flag = bypass;
        return c;
    }

    // The master bus trim, linear, applied to the summed mix before its inserts.
    // Instant rather than ramped, like the monitor level.
    static Command setMasterTrim(float linear) noexcept
    {
        Command c;
        c.type = CommandType::SetMasterTrim;
        c.gain = linear;
        return c;
    }

    // Hear the mix before the bus: the trim and the master chain are skipped and
    // `makeupLinear` applied instead, so a before/after comparison can be at the
    // same loudness. A monitor-side switch: exports never see it.
    static Command setMasterBypass(bool on, float makeupLinear) noexcept
    {
        Command c;
        c.type = CommandType::SetMasterBypass;
        c.flag = on;
        c.gain = makeupLinear;
        return c;
    }

    // Restarts the loudness measurement. A command rather than a call, because the
    // meter's state belongs to the audio thread.
    static Command resetLoudness() noexcept
    {
        Command c;
        c.type = CommandType::ResetLoudness;
        return c;
    }

    // Live monitoring only: an offline transport never receives it, so renders
    // and exports never contain the click.
    static Command setMetronome(bool on, float linear) noexcept
    {
        Command c;
        c.type = CommandType::SetMetronome;
        c.flag = on;
        c.gain = linear;
        return c;
    }

    static Command setPlan(const RenderPlan* plan, std::uint32_t generation, bool curvesOnly = false) noexcept
    {
        Command c;
        c.type = CommandType::SetPlan;
        c.plan = plan;
        c.planGeneration = generation;
        c.flag = curvesOnly;
        return c;
    }
};

} // namespace omawave::engine
