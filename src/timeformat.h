#pragma once

#include <QString>
#include <QStringList>

#include "video/timecode.h"
#include "session/musical_time.h"

// How positions and lengths on the timeline are written and read, everywhere
// in the interface at once. The project counts in samples; a person counts in
// time. One setting chooses the form, and every field and read-out follows it:
//
//   time      00:00:01.500       hours:minutes:seconds.milliseconds (the default)
//   samples   72000              the count itself
//   seconds   1.500              decimal seconds
//   cdframes  00:01:37           minutes:seconds:CD frames, 75 frames/second
//   barsbeats 1:1:000            musical position (duration starts at 0:0:000)
//   tc..      00:00:01:12        timecode at a picture rate; ';' before the
//                                frames for drop-frame counting
//
// Pure functions over a sample rate, so the tests can hand them anything.
namespace omaaudio::timeformat {

enum class Mode { Time, Samples, Seconds, CdFrames, BarsBeats, Timecode };

struct Display {
    Mode mode = Mode::Time;
    omawave::video::FrameRate rate{25, 1};   // Timecode only
    bool dropFrame = false;                  // Timecode only
    omawave::session::MusicalTime music{};    // BarsBeats only
};

// The settings a person can choose, as stored, and as named in a menu.
[[nodiscard]] QStringList keys();
[[nodiscard]] QString label(const QString &key);
// An unknown key is the default: a settings file from elsewhere must not break time.
[[nodiscard]] QString validKey(const QString &key);
[[nodiscard]] Display displayFor(const QString &key);
// "Timecode at the video's rate": the rate and the drop-frame choice of the
// project's reference picture. With no picture it is plain time.
[[nodiscard]] QString videoKey();
[[nodiscard]] Display displayFor(const QString &key, omawave::video::FrameRate picture, bool dropFrame);

// A position, or a duration when requested, in samples at `sampleRate`.
// Musical positions are one-based; musical durations count from zero.
[[nodiscard]] QString format(qint64 samples, quint32 sampleRate, const Display &display, bool duration = false);

// What a person typed, as samples; -1 for text that is not a time.
//
// Whatever the display, these are understood: a plain number in the display's
// own unit (seconds, or samples), "1:02.5" and "0:01:02.500", and a number with
// a unit: "25ms", "1.5s", "1200smp" (also "samples"). In a timecode display
// "1:00:12" is read as timecode, right-aligned to its four fields, and a plain
// number is seconds.
[[nodiscard]] qint64 parse(const QString &text, quint32 sampleRate, const Display &display, bool duration = false);

// A short length for a read-out: "25.0 ms" in time and timecode, "1200 samples" in samples.
[[nodiscard]] QString shortLength(qint64 samples, quint32 sampleRate, const Display &display);
// What one screen pixel covers at a zoom, in the display's unit.
[[nodiscard]] QString perPixel(double samplesPerPixel, quint32 sampleRate, const Display &display);
// What the arrow keys move a field by: a millisecond, a sample, or a picture frame.
[[nodiscard]] qint64 step(quint32 sampleRate, const Display &display);
// Move by display steps, clamped to nonnegative representable samples. CD
// movement lands on frame boundaries, avoiding drift at non-divisible rates.
// Zero steps preserves the exact input, including a sub-frame position.
[[nodiscard]] qint64 advance(qint64 samples, int steps, quint32 sampleRate, const Display &display);
// An example of what may be typed, for a field's help.
[[nodiscard]] QString hint(const Display &display, bool duration = false);

} // namespace omaaudio::timeformat
