// The reference picture (W11): the Backend's side of video/reference.h.
//
// The audio is the clock. A timer, running only while a frame item is visible,
// reads the published playhead, asks the decoder for the frame at that moment
// and publishes it; the decoder never touches the audio thread and nothing
// here waits for it. The file is probed on its own thread whenever the
// project's reference changes, which is how open, undo, relink and recovery
// all end up in the same place.

#include "backend.h"

#include "session/state.h"
#include "video/timecode.h"

#include <QFileInfo>
#include <QUrl>

#include <cmath>

namespace {

std::string localPath(const QString &path)
{
    const QUrl url(path);
    return (url.isLocalFile() ? url.toLocalFile() : path).toStdString();
}

QString secondsText(double seconds)
{
    const bool negative = seconds < 0;
    seconds = std::fabs(seconds);
    const auto whole = static_cast<qint64>(seconds);
    const int ms = static_cast<int>(std::lround((seconds - static_cast<double>(whole)) * 1000.0));
    return QStringLiteral("%1%2:%3:%4.%5").arg(negative ? QStringLiteral("-") : QString())
        .arg(whole / 3600, 2, 10, QLatin1Char('0')).arg(whole / 60 % 60, 2, 10, QLatin1Char('0'))
        .arg(whole % 60, 2, 10, QLatin1Char('0')).arg(ms, 3, 10, QLatin1Char('0'));
}

} // namespace

double Backend::videoSeconds(qint64 playhead) const
{
    const double rate = static_cast<double>(std::max<std::uint32_t>(1, m_project.sampleRate()));
    return static_cast<double>(playhead - m_project.video().offset) / rate;
}

bool Backend::commitVideo(const char *label, const session::VideoReference &next)
{
    if (const auto deferred = deferAutomationEdit([this, label = std::string(label), next] {
        return commitVideo(label.c_str(), next);
    })) return *deferred;
    if (m_loading || m_saving) { setError(QStringLiteral("wait until the file has finished opening before changing the video reference")); return false; }
    if (next == m_project.video()) { setError({}); return true; }
    const auto before = captureEditState();
    m_project.video() = next;
    commit(label, session::kInvalidTrack, before);
    setError({});
    syncVideoReference();
    emit videoChanged();
    return true;
}

bool Backend::importVideo(const QString &path)
{
    const std::string local = localPath(path);
    if (local.empty()) { setError(QStringLiteral("choose a video file")); return false; }
    if (const std::string reason = video::videoSupportReason(); !reason.empty()) { setError(QString::fromStdString(reason)); return false; }
    if (!QFileInfo::exists(QString::fromStdString(local))) { setError(QStringLiteral("the video file does not exist: ") + QString::fromStdString(local)); return false; }
    session::VideoReference next;
    next.path = local;
    next.offset = 0;
    next.dropFrame = m_project.video().dropFrame;
    return commitVideo("import video", next);
}

bool Backend::relinkVideo(const QString &path)
{
    if (m_project.video().empty()) { setError(QStringLiteral("there is no video reference to relink")); return false; }
    const std::string local = localPath(path);
    if (local.empty() || !QFileInfo::exists(QString::fromStdString(local))) { setError(QStringLiteral("the video file does not exist: ") + QString::fromStdString(local)); return false; }
    session::VideoReference next = m_project.video();
    next.path = local;
    next.bundlePath.clear();
    return commitVideo("relink video", next);
}

bool Backend::clearVideo()
{
    if (m_project.video().empty()) { setError({}); return true; }
    return commitVideo("clear video", session::VideoReference{});
}

bool Backend::setVideoOffset(qint64 frames)
{
    if (m_project.video().empty()) { setError(QStringLiteral("import a video reference first")); return false; }
    session::VideoReference next = m_project.video();
    next.offset = frames;
    return commitVideo("place video", next);
}

bool Backend::setVideoOffsetText(const QString &text)
{
    QString t = text.trimmed();
    if (t.isEmpty()) { setError(QStringLiteral("type an offset: a time like 00:01:02.500, seconds, or a sample count")); return false; }
    bool negative = false;
    if (t.startsWith(QLatin1Char('-')) || t.startsWith(QStringLiteral("−"))) { negative = true; t.remove(0, 1); t = t.trimmed(); }
    const int rate = static_cast<int>(m_project.sampleRate());
    qint64 frames = -1;
    bool plainNumber = false;
    const double seconds = t.toDouble(&plainNumber);
    if (plainNumber && t.contains(QLatin1Char('.'))) frames = static_cast<qint64>(std::llround(seconds * rate));
    else frames = parseTimeReference(t, rate);
    if (frames < 0) { setError(QStringLiteral("the offset must be hh:mm:ss.mmm, seconds like 12.5, or a sample count")); return false; }
    return setVideoOffset(negative ? -frames : frames);
}

bool Backend::setVideoStartAtPlayhead()
{
    return setVideoOffset(m_playhead);
}

bool Backend::alignVideoToTimeReference()
{
    if (const auto deferred = deferAutomationEdit([this] { return alignVideoToTimeReference(); })) return *deferred;
    if (m_project.video().empty()) { setError(QStringLiteral("import a video reference first")); return false; }
    if (!m_videoInfo.ok || m_videoProbedPath != m_project.video().path) { setError(QStringLiteral("the video has not been read yet")); return false; }
    if (m_videoInfo.timecodeStart < 0) { setError(QStringLiteral("this video carries no timecode, so it cannot be placed by one; set its offset instead")); return false; }
    const double rate = static_cast<double>(m_project.sampleRate());
    const double videoStart = video::frameStart(m_videoInfo.timecodeStart, m_videoInfo.rate);   // seconds since midnight
    if (!m_project.metadata().timeReferenceSet) {
        // W05's rule: the first timestamped thing into a project without a
        // reference sets it, and sits at zero.
        if (m_loading || m_saving) { setError(QStringLiteral("wait until the file has finished opening")); return false; }
        const auto before = captureEditState();
        m_project.metadata().timeReferenceSet = true;
        m_project.metadata().timeReference = static_cast<std::int64_t>(std::llround(videoStart * rate));
        m_project.video().offset = 0;
        commit("place video by timecode", session::kInvalidTrack, before);
        setError({});
        emit metadataChanged();
        emit videoChanged();
        return true;
    }
    const double projectStart = static_cast<double>(m_project.metadata().timeReference) / rate;
    session::VideoReference next = m_project.video();
    next.offset = static_cast<qint64>(std::llround((videoStart - projectStart) * rate));
    return commitVideo("place video by timecode", next);
}

bool Backend::setVideoDropFrame(bool on)
{
    if (m_project.video().empty()) { setError(QStringLiteral("import a video reference first")); return false; }
    session::VideoReference next = m_project.video();
    next.dropFrame = on;
    return commitVideo("video timecode display", next);
}

bool Backend::videoStep(int frames)
{
    if (m_project.video().empty() || !m_videoInfo.ok || m_videoProbedPath != m_project.video().path || frames == 0) return false;
    const auto &times = m_videoInfo.timestamps;
    const double now = videoSeconds(m_playhead);
    std::int64_t index = m_videoInfo.frameAt(now);
    // Standing inside a frame and stepping back goes to that frame's start
    // first; a step forward goes to the next frame's. "Inside" allows for the
    // playhead being a whole sample, which a frame start seldom is.
    const double slack = 1.5 / static_cast<double>(std::max<std::uint32_t>(1, m_project.sampleRate()));
    if (frames < 0 && index >= 0 && now > times[static_cast<std::size_t>(index)] + slack) index += 1;
    index += frames;
    index = std::clamp<std::int64_t>(index, 0, static_cast<std::int64_t>(times.size()) - 1);
    const double target = times[static_cast<std::size_t>(index)];
    const qint64 frame = m_project.video().offset + static_cast<qint64>(std::llround(target * static_cast<double>(m_project.sampleRate())));
    seekFrames(std::max<qint64>(0, frame));
    return true;
}

QString Backend::videoTimecodeAt(qint64 playhead) const
{
    if (m_project.video().empty()) return {};
    if (!m_videoInfo.ok || m_videoProbedPath != m_project.video().path) return QStringLiteral("--:--:--:--");
    const double seconds = videoSeconds(playhead);
    std::int64_t index = seconds < 0 ? -static_cast<std::int64_t>(std::ceil(-seconds * m_videoInfo.rate.fps() - 1e-6))
                                     : m_videoInfo.frameAt(seconds);
    if (seconds >= 0 && index < 0) index = 0;
    if (m_videoInfo.timecodeStart >= 0) index += m_videoInfo.timecodeStart;
    return QString::fromStdString(video::timecode(index, m_videoInfo.rate, m_project.video().dropFrame));
}

QVariantMap Backend::videoInfo() const
{
    QVariantMap out;
    const session::VideoReference &ref = m_project.video();
    out["present"] = !ref.empty();
    out["supportReason"] = QString::fromStdString(video::videoSupportReason());
    if (ref.empty()) { out["status"] = QStringLiteral("none"); return out; }
    out["path"] = QString::fromStdString(ref.path);
    out["name"] = QFileInfo(QString::fromStdString(ref.path)).fileName();
    out["offset"] = static_cast<qint64>(ref.offset);
    const double rate = static_cast<double>(std::max<std::uint32_t>(1, m_project.sampleRate()));
    out["offsetText"] = secondsText(static_cast<double>(ref.offset) / rate);
    out["dropFrame"] = ref.dropFrame;
    out["timeReferenceSet"] = m_project.metadata().timeReferenceSet;
    const bool current = m_videoProbedPath == ref.path;
    if (m_videoProbing || !current) { out["status"] = QStringLiteral("probing"); out["message"] = tr("Reading the video…"); return out; }
    if (!m_videoInfo.ok) {
        const bool missing = !QFileInfo::exists(QString::fromStdString(ref.path));
        out["status"] = missing ? QStringLiteral("missing") : QStringLiteral("unsupported");
        out["message"] = missing ? tr("The video file is missing. Its place and offset are kept; relink it to see the picture.")
                                 : tr("This video cannot be shown: %1").arg(QString::fromStdString(m_videoInfo.error));
        return out;
    }
    out["status"] = QStringLiteral("ready");
    out["width"] = static_cast<int>(m_videoInfo.width);
    out["height"] = static_cast<int>(m_videoInfo.height);
    out["codec"] = QString::fromStdString(m_videoInfo.codec);
    out["fps"] = QString::fromStdString(m_videoInfo.rate.text());
    out["rateRatio"] = QString::fromStdString(m_videoInfo.rate.ratioText());
    out["dropFrameCapable"] = m_videoInfo.rate.dropFrameCapable();
    out["variableRate"] = m_videoInfo.variableRate;
    out["frameCount"] = static_cast<qint64>(m_videoInfo.frameCount());
    out["duration"] = m_videoInfo.duration;
    out["durationText"] = secondsText(m_videoInfo.duration);
    out["timecode"] = QString::fromStdString(m_videoInfo.timecode);
    out["timecodeStart"] = static_cast<qint64>(m_videoInfo.timecodeStart);
    out["endFrame"] = static_cast<qint64>(ref.offset + std::llround(m_videoInfo.duration * rate));
    QString message;
    if (m_videoInfo.variableRate)
        message = tr("Variable frame rate: the frames' own timestamps are used, so stepping and timecode follow them rather than a fixed rate.");
    if (m_videoInfo.timecodeStart >= 0 && !m_project.metadata().timeReferenceSet)
        message += (message.isEmpty() ? QString() : QStringLiteral(" ")) + tr("The video carries timecode %1; the project has no time reference yet, so Place by timecode would adopt it.").arg(QString::fromStdString(m_videoInfo.timecode));
    out["message"] = message;
    return out;
}

QVariantMap Backend::videoStats() const
{
    QVariantMap out;
    if (!m_videoDecoder) return out;
    const video::DecoderStats s = m_videoDecoder->stats();
    out["decoded"] = static_cast<qint64>(s.decoded);
    out["delivered"] = static_cast<qint64>(s.delivered);
    out["dropped"] = static_cast<qint64>(s.dropped);
    out["restarts"] = static_cast<qint64>(s.restarts);
    out["late"] = static_cast<qint64>(s.late);
    out["running"] = s.running;
    out["error"] = QString::fromStdString(s.error);
    out["frameSeconds"] = m_videoFrameSeconds;
    return out;
}

void Backend::watchVideo(int delta)
{
    m_videoWatchers = std::max(0, m_videoWatchers + delta);
    const bool run = m_videoWatchers > 0 && m_videoDecoder != nullptr;
    if (run && !m_videoTimer->isActive()) { m_videoTimer->start(); videoTick(); }
    else if (!run && m_videoTimer->isActive()) m_videoTimer->stop();
}

void Backend::syncVideoReference()
{
    const session::VideoReference &ref = m_project.video();
    if (ref.path == m_videoProbedPath && (!ref.empty() || !m_videoDecoder)) {
        // Same file: only its placement or display may have changed.
        const QString tc = videoTimecodeAt(m_playhead);
        if (tc != m_videoTimecode) { m_videoTimecode = tc; emit videoTimecodeChanged(); }
        return;
    }
    // A different file (or none): drop the decoder, cancel any probe, start over.
    ++m_videoGeneration;
    m_videoProbeCancel.store(true, std::memory_order_release);
    if (m_videoProbeThread.joinable()) m_videoProbeThread.join();
    m_videoProbeCancel.store(false, std::memory_order_release);
    m_videoDecoder.reset();
    m_videoInfo = {};
    m_videoFrame = QImage();
    m_videoFrameSeconds = -1.0;
    m_videoProbedPath = ref.path;
    if (m_videoTimer->isActive()) m_videoTimer->stop();
    emit videoFrameChanged();
    if (ref.empty()) {
        m_videoProbing = false;
        m_videoTimecode.clear();
        emit videoChanged();
        emit videoTimecodeChanged();
        return;
    }
    m_videoProbing = true;
    emit videoChanged();
    const quint64 generation = m_videoGeneration;
    const std::string path = ref.path;
    m_videoProbeThread = std::thread([this, generation, path] {
        video::VideoInfo info = video::probeVideo(path, &m_videoProbeCancel);
        QMetaObject::invokeMethod(this, [this, generation, info = std::move(info)] { finishVideoProbe(generation, info); }, Qt::QueuedConnection);
    });
}

void Backend::finishVideoProbe(quint64 generation, const video::VideoInfo &info)
{
    if (generation != m_videoGeneration || m_shuttingDown) return;
    m_videoProbing = false;
    m_videoInfo = info;
    if (info.ok) {
        // A preview a dock can show; the decoder scales, never upsamples.
        m_videoDecoder = std::make_unique<video::Decoder>(info, 640);
        m_videoDecoder->prefetch(videoSeconds(m_playhead));
    }
    m_videoTimecode = videoTimecodeAt(m_playhead);
    emit videoChanged();
    emit videoTimecodeChanged();
    if (m_videoWatchers > 0 && m_videoDecoder && !m_videoTimer->isActive()) { m_videoTimer->start(); videoTick(); }
}

void Backend::videoTick()
{
    if (!m_videoDecoder) return;
    const double seconds = videoSeconds(m_playhead);
    video::Frame frame;
    if (m_videoDecoder->frameAt(seconds, frame)) {
        m_videoFrame = QImage(frame.rgba.data(), static_cast<int>(frame.width), static_cast<int>(frame.height),
                              static_cast<int>(frame.width) * 4, QImage::Format_RGBA8888).copy();
        m_videoFrameSeconds = frame.seconds;
        emit videoFrameChanged();
    } else if (seconds < 0.0 && !m_videoFrame.isNull()) {
        // Before the picture starts there is no picture.
        m_videoFrame = QImage();
        m_videoFrameSeconds = -1.0;
        emit videoFrameChanged();
    }
    const QString tc = videoTimecodeAt(m_playhead);
    if (tc != m_videoTimecode || (m_videoTimer->isActive() && m_playing)) { m_videoTimecode = tc; emit videoTimecodeChanged(); }
}
