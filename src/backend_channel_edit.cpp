#include "backend.h"
#include <algorithm>
#include <cmath>

void Backend::setSelectionChannel(int channel)
{
    const auto* track = m_project.track(m_focused);
    if (channel < -1 || (channel >= 0 && (!track || channel >= trackChannels(*track)))) {
        setError(QStringLiteral("choose a channel present in the focused take")); return;
    }
    if (channel >= 0) clearGroupSelection();
    if (m_selection.channel == channel && m_selection.track == m_focused) return;
    m_selection.channel = channel; m_selection.track = m_focused;
    emit selectionChanged();
}

QString Backend::clipboardSummary() const
{
    if (m_multitrackClipboard) {
        const auto& copied = *m_multitrackClipboard;
        const auto duration = QString::number(double(copied.frames) / m_project.sampleRate(), 'f', 3);
        if (copied.wholeClips) {
            std::size_t clips = 0; for (const auto& track : copied.tracks) clips += track.clips.size();
            const auto clipCount = clips == 1 ? tr("1 clip") : tr("%1 clips").arg(clips);
            const auto trackCount = copied.tracks.size() == 1 ? tr("1 track") : tr("%1 tracks").arg(copied.tracks.size());
            return tr("%1 s · %2 on %3").arg(duration, clipCount, trackCount);
        }
        return tr("%1 s · %2 tracks").arg(duration).arg(copied.tracks.size());
    }
    if (m_clipboardFrames <= 0) return QStringLiteral("Clipboard empty");
    return QStringLiteral("%1 s · %2").arg(double(m_clipboardFrames) / m_project.sampleRate(), 0, 'f', 3)
        .arg(m_clipboardChannel < 0 ? QStringLiteral("all channels") : QStringLiteral("channel %1").arg(m_clipboardChannel + 1));
}

void Backend::startChannelEdit(session::ChannelEdit operation, double gain, qint64 at,
                              double incomingGain, bool newTrack)
{
    if (!std::isfinite(gain) || !std::isfinite(incomingGain)) { setError(QStringLiteral("invalid edit gain")); return; }
    session::ChannelEditSpec spec;
    spec.operation = operation; spec.start = m_selection.start; spec.end = m_selection.end;
    spec.channel = newTrack ? -1 : m_selection.channel; spec.gain = float(gain);
    const bool paste = operation == session::ChannelEdit::Paste || operation == session::ChannelEdit::MixPaste;
    if (paste) {
        if (m_multitrackClipboard && m_multitrackClipboard->tracks.size() != 1) {
            setError(tr("Paste and Mix needs a single-track clipboard. Use Paste for multiple tracks.")); return;
        }
        const auto frames = m_multitrackClipboard ? m_multitrackClipboard->frames : m_clipboardFrames;
        if (frames <= 0) { setError(QStringLiteral("nothing to paste")); return; }
        spec.start = at < 0 ? m_playhead : at;
        spec.clipboard = m_multitrackClipboard ? m_multitrackClipboard->tracks.front().clips : m_clipboard;
        spec.clipboardFrames = frames;
        spec.clipboardChannel = m_multitrackClipboard ? -1 : m_clipboardChannel; spec.incomingGain = float(incomingGain);
        spec.ripple = m_ripple && !newTrack; spec.newTrack = newTrack;
    }
    if (newTrack && m_project.tracks().size() >= engine::Transport::kMaxTracks) {
        setError(QStringLiteral("this session already has as many tracks as OmaAudio plays (%1); remove one to add another").arg(engine::Transport::kMaxTracks)); return;
    }
    startSampleEdit(newTrack ? SampleEditKind::PasteNew : SampleEditKind::Channels, 0, 0, 0, 0, std::move(spec));
}

void Backend::mixPaste(qint64 frame, double incomingDb, double destinationDb)
{
    if (!std::isfinite(incomingDb) || !std::isfinite(destinationDb)
        || incomingDb < -90 || incomingDb > 24 || destinationDb < -90 || destinationDb > 24) {
        setError(QStringLiteral("mix gains must be between -90 and +24 dB")); return;
    }
    startChannelEdit(session::ChannelEdit::MixPaste, linearFromDb(destinationDb), frame, linearFromDb(incomingDb));
}

void Backend::insertSilence(qint64 frames, bool allTracks, qint64 at)
{
    if (frames <= 0) { setError(QStringLiteral("silence needs a positive duration")); return; }
    startSampleEdit(SampleEditKind::InsertSilence, frames, 0, at < 0 ? m_playhead : at, 0,
                    std::nullopt, {}, allTracks);
}

void Backend::mapTrackChannels(const QVariantList& rows, const QString& stamp)
{
    if (!stamp.isEmpty() && stamp != automationStamp()) { setError(QStringLiteral("the channel mapping target changed")); return; }
    std::vector<std::vector<float>> matrix;
    if (rows.empty() || rows.size() > 8) { setError(QStringLiteral("choose 1 to 8 output channels")); return; }
    for (const auto& row : rows) {
        const auto values = row.toList();
        if (values.empty() || values.size() > 8) { setError(QStringLiteral("invalid channel mapping row")); return; }
        std::vector<float> cells;
        for (const auto& v : values) {
            bool ok = false; const double gain = v.toDouble(&ok);
            if (!ok || !std::isfinite(gain) || std::abs(gain) > 4) { setError(QStringLiteral("channel gains must be between -4 and 4")); return; }
            cells.push_back(float(gain));
        }
        matrix.push_back(std::move(cells));
    }
    startSampleEdit(SampleEditKind::MapChannels, 0, 0, 0, 0, std::nullopt, std::move(matrix));
}


void Backend::changeProjectSamples(int rate, int exportBits, const QString& stamp)
{
    if ((!stamp.isEmpty() && stamp != automationStamp()) || rate < 8000 || rate > 192000
        || (exportBits != 16 && exportBits != 24 && exportBits != 32)) {
        setError(QStringLiteral("choose valid sample settings for the current project")); return;
    }
    // ProjectRate uses targetFrames for the target rate and rangeStart for bits.
    startSampleEdit(SampleEditKind::ProjectRate, rate, 0, exportBits);
}

bool Backend::prepareProjectRate(std::uint32_t rate)
{
    if (rate == m_output.sampleRate()) return false;
    const bool restart = m_ready;
    m_output.stop();
    flushStoppedCommands();
    m_transport.configureStopped(rate);
    (void)m_output.setSampleRateStopped(rate);
    m_ready = false; m_playing = false;
    emit readyChanged(); emit statusChanged();
    // Plugins may retain pointers to their options features. Keep old feature
    // storage until all their retired chains are destroyed at Backend shutdown.
    if (m_features) m_oldRateFeatures.push_back(std::move(m_features));
    m_chainSpecs.clear();
    return restart;
}

void Backend::rescaleProjectView(std::uint32_t oldRate, bool preservePosition)
{
    if (oldRate == m_project.sampleRate()) return;
    if (preservePosition) {
        const auto scale = [&](qint64 frame) { return session::ratePosition(std::max<qint64>(0, frame), oldRate, m_project.sampleRate()); };
        m_playhead = scale(m_playhead);
        m_loopStart = scale(m_loopStart); m_loopEnd = scale(m_loopEnd);
        m_selection.start = scale(m_selection.start); m_selection.end = scale(m_selection.end);
    } else m_playhead = 0;
}

void Backend::finishProjectRate(bool restart, std::uint32_t oldRate, std::uint32_t oldBits)
{
    if (oldRate != m_project.sampleRate()) {
        m_clipboard.clear(); m_clipboardFrames = 0; m_clipboardChannel = -1;
        m_multitrackClipboard.reset(); m_selectedTracks.clear(); m_selectedClips.clear(); m_trackSelectionAnchor = session::kInvalidTrack;
        ++m_trackSelectionRevision;
        clearNoisePrint(); clearClicks();
        const double nyquist = m_project.sampleRate() / 2.0;
        m_bandLowHz = std::min(m_bandLowHz, nyquist);
        m_bandHighHz = std::min(m_bandHighHz, nyquist);
        if (m_bandHighHz <= m_bandLowHz) m_bandLowHz = m_bandHighHz = 0;
        m_playhead = std::min(m_playhead, qint64(m_project.length()));
        enqueueAudioCommand(engine::Command::seek(m_playhead));
        refreshLoop(); clampSelection();
        emit statusChanged(); emit takeChanged(); emit loopChanged();
    }
    if (oldRate != m_project.sampleRate() || oldBits != m_project.defaultExportBits())
        emit projectSampleSettingsChanged();
    if (restart) (void)start();
}
