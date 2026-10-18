#include "timeformat.h"

#include <QCoreApplication>
#include <QRegularExpression>

#include <cmath>
#include <limits>

using omawave::video::FrameRate;

namespace omaaudio::timeformat {
namespace {

struct Choice { const char *key; const char *label; Display display; };
const Choice kChoices[] = {
    {"time", QT_TRANSLATE_NOOP("timeformat", "Time (hh:mm:ss.ms)"), {Mode::Time, {25, 1}, false}},
    {"samples", QT_TRANSLATE_NOOP("timeformat", "Samples"), {Mode::Samples, {25, 1}, false}},
    {"seconds", QT_TRANSLATE_NOOP("timeformat", "Decimal seconds"), {Mode::Seconds, {25, 1}, false}},
    {"cdframes", QT_TRANSLATE_NOOP("timeformat", "CD frames (75 fps)"), {Mode::CdFrames, {75, 1}, false}},
    {"barsbeats", QT_TRANSLATE_NOOP("timeformat", "Bars and Beats"), {Mode::Time, {25, 1}, false}},
    // Resolved by the caller, who knows the picture: see videoKey().
    {"tcvideo", QT_TRANSLATE_NOOP("timeformat", "Timecode at the video's rate"), {Mode::Time, {25, 1}, false}},
    {"tc24", QT_TRANSLATE_NOOP("timeformat", "Timecode 24 fps"), {Mode::Timecode, {24, 1}, false}},
    {"tc25", QT_TRANSLATE_NOOP("timeformat", "Timecode 25 fps"), {Mode::Timecode, {25, 1}, false}},
    {"tc2997df", QT_TRANSLATE_NOOP("timeformat", "Timecode 29.97 fps drop-frame"), {Mode::Timecode, {30000, 1001}, true}},
    {"tc2997", QT_TRANSLATE_NOOP("timeformat", "Timecode 29.97 fps"), {Mode::Timecode, {30000, 1001}, false}},
    {"tc30", QT_TRANSLATE_NOOP("timeformat", "Timecode 30 fps"), {Mode::Timecode, {30, 1}, false}},
};

const Choice *find(const QString &key)
{
    for (const Choice &choice : kChoices) if (key == QLatin1String(choice.key)) return &choice;
    return nullptr;
}

QString two(qint64 value) { return QStringLiteral("%1").arg(value, 2, 10, QLatin1Char('0')); }

QString clock(qint64 samples, quint32 rate, bool decimal = false)
{
    const bool negative = samples < 0;
    const quint64 magnitude = negative ? quint64(-(samples + 1)) + 1 : quint64(samples);
    // To the nearest millisecond: 1200 samples at 48 kHz is 25 ms, not 24.
    // Split before scaling: multiplying a valid sample count by 1000 can wrap.
    quint64 seconds = magnitude / rate;
    quint64 ms = (magnitude % rate * 1000 + rate / 2) / rate;
    seconds += ms / 1000;
    if (decimal)
        return QStringLiteral("%1%2.%3").arg(negative ? QStringLiteral("-") : QString())
            .arg(seconds).arg(ms % 1000, 3, 10, QLatin1Char('0'));
    return QStringLiteral("%1%2:%3:%4.%5").arg(negative ? QStringLiteral("-") : QString())
        .arg(two(qint64(seconds / 3600)), two(qint64(seconds / 60 % 60)), two(qint64(seconds % 60)))
        .arg(ms % 1000, 3, 10, QLatin1Char('0'));
}

QString cdClock(qint64 samples, quint32 rate)
{
    const bool negative = samples < 0;
    const quint64 magnitude = negative ? quint64(-(samples + 1)) + 1 : quint64(samples);
    const quint64 seconds = magnitude / rate;
    const quint64 frame = (magnitude % rate) * 75 / rate;
    return QStringLiteral("%1%2:%3:%4").arg(negative ? QStringLiteral("-") : QString())
        .arg(two(qint64(seconds / 60)), two(qint64(seconds % 60)), two(qint64(frame)));
}

// A CD frame starts at the first sample on or after its rational time. Keep
// the whole seconds separate so both parsing and stepping remain exact at the
// signed sample limit and at project rates which are not divisible by 75.
qint64 cdSamples(quint64 seconds, quint64 frame, quint32 rate)
{
    constexpr auto maximum = quint64(std::numeric_limits<qint64>::max());
    const quint64 tail = (frame * rate + 74) / 75;
    if (seconds > (maximum - tail) / rate) return -1;
    return qint64(seconds * rate + tail);
}

qint64 parseCd(const QString &text, quint32 rate)
{
    static const QRegularExpression fields(QStringLiteral("^(\\d+):([0-5]?\\d):(\\d{1,2})$"));
    const auto match = fields.match(text);
    if (!match.hasMatch()) return -1;
    bool ok = false;
    const quint64 minutes = match.captured(1).toULongLong(&ok);
    const quint64 seconds = match.captured(2).toUInt();
    const quint64 frame = match.captured(3).toUInt();
    if (!ok || frame >= 75 || minutes > (std::numeric_limits<quint64>::max() - seconds) / 60) return -1;
    return cdSamples(minutes * 60 + seconds, frame, rate);
}

QString musicalClock(qint64 samples, quint32 rate, const Display &display, bool duration)
{
    const auto tick = display.music.tickAt(samples, rate);
    const qint64 perBeat = display.music.ticksPerBeat();
    const qint64 perBar = perBeat * display.music.beats;
    if (tick < 0 || perBeat <= 0 || perBar <= 0) return QStringLiteral("--:--:---");
    const int origin = duration ? 0 : 1;
    return QStringLiteral("%1:%2:%3").arg(tick / perBar + origin)
        .arg(tick / perBeat % display.music.beats + origin).arg(tick % perBeat, 3, 10, QLatin1Char('0'));
}

qint64 parseMusic(const QString &text, quint32 rate, const Display &display, bool duration)
{
    static const QRegularExpression fields(QStringLiteral("^(\\d+):(\\d+):(\\d+)$"));
    const auto match = fields.match(text);
    if (!match.hasMatch() || !display.music.valid()) return -1;
    bool barOk = false, beatOk = false, tickOk = false;
    const auto bar = match.captured(1).toLongLong(&barOk);
    const auto beat = match.captured(2).toLongLong(&beatOk);
    const auto tick = match.captured(3).toLongLong(&tickOk);
    const int origin = duration ? 0 : 1;
    const qint64 perBeat = display.music.ticksPerBeat();
    const qint64 perBar = perBeat * display.music.beats;
    if (!barOk || !beatOk || !tickOk || bar < origin || beat < origin
        || beat - origin >= display.music.beats || tick >= perBeat) return -1;
    const auto tail = (beat - origin) * perBeat + tick;
    if (bar - origin > (std::numeric_limits<qint64>::max() - tail) / perBar) return -1;
    return display.music.sampleAt((bar - origin) * perBar + tail, rate);
}

// Samples for a count of seconds, or -1 when it does not fit.
qint64 fromSeconds(double seconds, quint32 rate)
{
    if (!std::isfinite(seconds) || seconds < 0) return -1;
    const double samples = std::round(seconds * double(rate));
    return samples > 9.0e18 ? -1 : qint64(samples);
}

bool number(const QString &text, double &out)
{
    static const QRegularExpression plain(QStringLiteral("^\\d+(\\.\\d*)?$|^\\.\\d+$"));
    if (!plain.match(text).hasMatch()) return false;
    bool ok = false;
    out = text.toDouble(&ok);
    return ok && std::isfinite(out);
}

// The grammar has already been checked. Do not send integer sample counts
// through double: integers above 2^53 would silently move to a nearby sample.
qint64 sampleCount(const QString &text)
{
    const auto dot = text.indexOf(QLatin1Char('.'));
    if (dot >= 0) {
        for (auto i = dot + 1; i < text.size(); ++i)
            if (text[i] != QLatin1Char('0')) return -1;
    }
    const QString whole = dot >= 0 ? text.first(dot) : text;
    if (whole.isEmpty()) return 0; // .0 samples
    bool ok = false;
    const qint64 result = whole.toLongLong(&ok);
    return ok ? result : -1;
}

// "h:mm:ss.fff", "m:ss.fff": every part but the last a whole number.
bool colonSeconds(const QString &text, double &out)
{
    const auto parts = text.split(QLatin1Char(':'));
    if (parts.size() < 2 || parts.size() > 3) return false;
    double total = 0;
    for (qsizetype i = 0; i < parts.size(); ++i) {
        double value = 0;
        if (!number(parts[i], value)) return false;
        if (i + 1 < parts.size() && parts[i].contains(QLatin1Char('.'))) return false;
        if (i > 0 && value >= 60) return false;   // minutes and seconds stay under sixty
        total = total * 60 + value;
    }
    out = total;
    return true;
}

} // namespace

QStringList keys()
{
    QStringList out;
    for (const Choice &choice : kChoices) out.append(QLatin1String(choice.key));
    return out;
}

QString label(const QString &key)
{
    const Choice *choice = find(validKey(key));
    return QCoreApplication::translate("timeformat", choice->label);
}

QString validKey(const QString &key) { return find(key) ? key : QStringLiteral("time"); }
QString videoKey() { return QStringLiteral("tcvideo"); }
Display displayFor(const QString &key, omawave::video::FrameRate picture, bool dropFrame)
{
    if (validKey(key) != videoKey()) return displayFor(key);
    // No picture, no rate to count at: plain time until there is one.
    if (!picture.valid()) return Display{};
    return Display{Mode::Timecode, picture, dropFrame && picture.dropFrameCapable()};
}
Display displayFor(const QString &key) { return find(validKey(key))->display; }

QString format(qint64 samples, quint32 sampleRate, const Display &display, bool duration)
{
    if (!sampleRate) sampleRate = 48000;
    switch (display.mode) {
    case Mode::BarsBeats: return musicalClock(samples, sampleRate, display, duration);
    case Mode::Samples: return QString::number(samples);
    case Mode::Seconds: return clock(samples, sampleRate, true);
    case Mode::CdFrames: return cdClock(samples, sampleRate);
    case Mode::Timecode: {
        // The picture frame this sample falls in.
        if (!display.rate.valid()) return QStringLiteral("--:--:--:--");
        const long double frames = std::floor((static_cast<long double>(samples) * display.rate.num) / (static_cast<long double>(sampleRate) * display.rate.den) + 1e-9L);
        if (frames >= 9223372036854775808.0L || frames < -9223372036854775808.0L)
            return QStringLiteral("--:--:--:--");
        return QString::fromStdString(omawave::video::timecode(static_cast<std::int64_t>(frames), display.rate, display.dropFrame));
    }
    case Mode::Time: break;
    }
    return clock(samples, sampleRate);
}

qint64 parse(const QString &typed, quint32 sampleRate, const Display &display, bool duration)
{
    if (!sampleRate) sampleRate = 48000;
    const QString text = typed.trimmed().toLower();
    if (text.isEmpty()) return -1;

    // A number with its unit says what it is, in any display.
    static const QRegularExpression unit(QStringLiteral("^(\\d+(?:\\.\\d*)?|\\.\\d+)\\s*(ms|s|sec|smp|sa|samples?)$"));
    if (const auto match = unit.match(text); match.hasMatch()) {
        const QString suffix = match.captured(2);
        if (suffix == QLatin1String("smp") || suffix == QLatin1String("sa") || suffix.startsWith(QLatin1String("sample")))
            return sampleCount(match.captured(1));
        double value = 0;
        if (!number(match.captured(1), value)) return -1;
        if (suffix == QLatin1String("ms")) return fromSeconds(value / 1000.0, sampleRate);
        return fromSeconds(value, sampleRate);
    }

    // A decimal clock is unambiguous in every display. Integer-only colon
    // forms in Timecode keep their established right-aligned frame meaning.
    double seconds = 0;
    if (text.contains(QLatin1Char(':')) && text.contains(QLatin1Char('.')))
        return colonSeconds(text, seconds) ? fromSeconds(seconds, sampleRate) : -1;

    if (display.mode == Mode::BarsBeats && text.contains(QLatin1Char(':')))
        return parseMusic(text, sampleRate, display, duration);

    if (display.mode == Mode::CdFrames && text.contains(QLatin1Char(':')))
        return parseCd(text, sampleRate);

    if (display.mode == Mode::Timecode && (text.contains(QLatin1Char(':')) || text.contains(QLatin1Char(';')))) {
        // Right-aligned to hh:mm:ss:ff, keeping the separators that were typed.
        // The engine checks each field against the picture rate. Hours can
        // exceed two digits, and frame numbers can exceed 99 at high rates.
        static const QRegularExpression fields(QStringLiteral("^\\d+(?:[:;]\\d+){1,3}$"));
        if (!fields.match(text).hasMatch()) return -1;
        QString whole = text;
        for (auto count = text.count(QLatin1Char(':')) + text.count(QLatin1Char(';')); count < 3; ++count)
            whole.prepend(QStringLiteral("00:"));
        const auto index = omawave::video::parseTimecode(whole.toStdString(), display.rate, display.dropFrame);
        if (index < 0) return -1;
        // The first sample of that picture frame.
        const long double samples = std::ceil((static_cast<long double>(index) * display.rate.den * sampleRate) / display.rate.num - 1e-9L);
        return samples > 9.0e18L ? -1 : qint64(samples);
    }

    if (text.contains(QLatin1Char(':'))) return colonSeconds(text, seconds) ? fromSeconds(seconds, sampleRate) : -1;
    double value = 0;
    if (!number(text, value)) return -1;
    if (display.mode == Mode::Samples) {
        // The display's own unit; a decimal point can only have meant seconds.
        if (text.contains(QLatin1Char('.'))) return fromSeconds(value, sampleRate);
        return sampleCount(text);
    }
    return fromSeconds(value, sampleRate);
}

QString shortLength(qint64 samples, quint32 sampleRate, const Display &display)
{
    if (!sampleRate) sampleRate = 48000;
    if (display.mode == Mode::Samples)
        return samples == 1 ? QCoreApplication::translate("timeformat", "1 sample")
                            : QCoreApplication::translate("timeformat", "%1 samples").arg(samples);
    const double ms = double(samples) * 1000.0 / sampleRate;
    return ms < 1000.0 ? QCoreApplication::translate("timeformat", "%1 ms").arg(ms, 0, 'f', ms < 100 ? 1 : 0)
                       : QCoreApplication::translate("timeformat", "%1 s").arg(ms / 1000.0, 0, 'f', 2);
}

QString perPixel(double samplesPerPixel, quint32 sampleRate, const Display &display)
{
    if (!sampleRate) sampleRate = 48000;
    if (display.mode == Mode::Samples)
        return QCoreApplication::translate("timeformat", "%1 samples/px").arg(samplesPerPixel, 0, 'f', 1);
    const double ms = samplesPerPixel * 1000.0 / sampleRate;
    return ms < 1000.0 ? QCoreApplication::translate("timeformat", "%1 ms/px").arg(ms, 0, 'f', ms < 10 ? 2 : 1)
                       : QCoreApplication::translate("timeformat", "%1 s/px").arg(ms / 1000.0, 0, 'f', 2);
}

qint64 step(quint32 sampleRate, const Display &display)
{
    if (!sampleRate) sampleRate = 48000;
    switch (display.mode) {
    case Mode::BarsBeats: return std::max<qint64>(1, display.music.sampleAt(1, sampleRate));
    case Mode::Samples: return 1;
    case Mode::CdFrames: return std::max<qint64>(1, (quint64(sampleRate) + 37) / 75);
    case Mode::Timecode: {
        if (!display.rate.valid()) return 1;
        const auto samples = std::round(static_cast<long double>(sampleRate) * display.rate.den / display.rate.num);
        if (samples >= 9223372036854775808.0L) return std::numeric_limits<qint64>::max();
        return std::max<qint64>(1, qint64(samples));
    }
    case Mode::Seconds:
    case Mode::Time: break;
    }
    return std::max<qint64>(1, sampleRate / 1000);
}

qint64 advance(qint64 samples, int steps, quint32 sampleRate, const Display &display)
{
    if (!sampleRate) sampleRate = 48000;
    samples = std::max<qint64>(0, samples);
    if (!steps) return samples;
    constexpr auto maximum = std::numeric_limits<qint64>::max();
    if (display.mode == Mode::BarsBeats) {
        const auto tick = display.music.tickAt(samples, sampleRate);
        if (tick < 0) return samples;
        if (steps < 0 && tick < -qint64(steps)) return 0;
        if (steps > 0 && tick > maximum - steps) return maximum;
        const auto next = display.music.sampleAt(tick + steps, sampleRate);
        return next < 0 ? maximum : next;
    }
    if (display.mode != Mode::CdFrames) {
        const long double next = static_cast<long double>(samples)
            + static_cast<long double>(steps) * step(sampleRate, display);
        if (next <= 0) return 0;
        return next >= static_cast<long double>(maximum) ? maximum : qint64(next);
    }
    qint64 seconds = samples / sampleRate;
    const qint64 frames = (samples % sampleRate) * 75 / sampleRate + qint64(steps);
    const qint64 carry = frames >= 0 ? frames / 75 : (frames - 74) / 75;
    if (carry < 0 && seconds < -carry) return 0;
    if (carry > 0 && seconds > maximum - carry) return maximum;
    seconds += carry;
    const auto next = cdSamples(quint64(seconds), quint64(frames - carry * 75), sampleRate);
    return next < 0 ? maximum : next;
}

QString hint(const Display &display, bool duration)
{
    switch (display.mode) {
    case Mode::BarsBeats: return duration
        ? QCoreApplication::translate("timeformat", "Duration as bars:beats:ticks, starting at 0:0:000. Ticks count 960 per quarter note. Arrow keys and dragging move one tick. 1.5s, 0:01.5 and 1200smp are understood too.")
        : QCoreApplication::translate("timeformat", "Position as bar:beat:tick, starting at 1:1:000. Ticks count 960 per quarter note. Arrow keys and dragging move one tick. 1.5s, 0:01.5 and 1200smp are understood too.");
    case Mode::Samples: return QCoreApplication::translate("timeformat", "A sample count such as 72000. A time is understood too: 1.5s, 25ms or 0:01.5.");
    case Mode::CdFrames: return QCoreApplication::translate("timeformat", "Minutes:seconds:frames, such as 00:01:37. CD time counts 75 frames per second (00–74). Arrow keys and dragging move to frame boundaries. 1.5s, 0:01.5 and 1200smp are understood too.");
    case Mode::Timecode: return QCoreApplication::translate("timeformat", "Timecode such as 00:00:01:12, or a time such as 0:01.5 or 1.5 (seconds). 25ms and 1200smp are understood too.");
    case Mode::Seconds:
    case Mode::Time: break;
    }
    return QCoreApplication::translate("timeformat", "A time such as 0:01.5 or 1.5 (seconds). 25ms and 1200smp (samples) are understood too.");
}

} // namespace omaaudio::timeformat
