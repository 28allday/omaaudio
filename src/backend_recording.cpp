#include "backend.h"
#include "audiocommand.h"
#include "storagepaths.h"

#include "session/edits.h"
#include "session/uuid.h"
#include "engine/stream_timing.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <limits>

qint64 Backend::recordingFrames() const
{
    const auto frames = m_captureClock.frames.load(std::memory_order_relaxed);
    const auto audible = std::max<qint64>(0, frames - std::max<qint64>(0,
        m_recordPlacement.lead + m_recordPlacement.alignment + m_recordPlacement.residual));
    return m_recordPlacement.limit > 0 ? std::min<qint64>(audible, m_recordPlacement.limit) : audible;
}

void Backend::setRecordingLeadIn(double seconds, int beats, double tempo)
{
    if (recording() || !std::isfinite(seconds) || !std::isfinite(tempo) || seconds < 0 || seconds > 10
        || beats < 0 || beats > 16 || tempo < 20 || tempo > 300) return;
    m_recordPreRoll = seconds; m_recordCountIn = beats; m_recordTempo = tempo;
    emit recordingChanged();
}

void Backend::setRecordingAlignment(bool automatic, double residual)
{
    if (recording() || !std::isfinite(residual) || residual < -2000 || residual > 2000) return;
    m_recordAlign = automatic; m_recordOffsetMs = residual; emit recordingChanged();
}

void Backend::setRecordLoopTakes(int count)
{
    if (recording() || count < 0 || count > 16) return;
    m_recordLoopTakes = count; emit recordingChanged();
}

void Backend::addRecordInput(const QString& input, int channels, int track)
{
    if (recording() || input.contains(QChar::Null) || (channels != 1 && channels != 2)
        || m_additionalRecordInputs.size() >= 7 || input == m_recordInput
        || (track >= 0 && !m_project.track(static_cast<session::TrackId>(track)))) return;
    for (const auto& value : m_additionalRecordInputs) {
        const auto row = value.toMap();
        if (row.value("input").toString() == input || (track >= 0 && row.value("track").toInt() == track)) return;
    }
    if (track >= 0 && track == m_recordArmedTrack) return;
    m_additionalRecordInputs.append(QVariantMap{{"input", input}, {"channels", channels}, {"track", track}});
    emit recordingChanged();
}

void Backend::removeRecordInput(int index)
{
    if (recording() || index < 0 || index >= m_additionalRecordInputs.size()) return;
    m_additionalRecordInputs.removeAt(index); emit recordingChanged();
}

QString Backend::recordingsDirectory()
{
    return omaaudio::storage::locations().recordingsDir;
}

void Backend::setRecordArmedTrack(int track)
{
    if (recording()) return;
    if (track >= 0 && !m_project.track(static_cast<session::TrackId>(track))) return;
    m_recordArmedTrack = track < 0 ? -1 : track;
    emit recordingChanged();
}
void Backend::setRecordInput(const QString& input)
{
    if (recording() || input.contains(QChar::Null)) return;
    m_recordInput = input;
    emit recordingChanged();
}
void Backend::setRecordChannels(int channels)
{
    if (recording() || (channels != 1 && channels != 2)) return;
    m_recordChannels = channels;
    emit recordingChanged();
}
void Backend::setRecordMonitor(bool enabled)
{
    if (recording()) return;
    m_recordMonitor = enabled;
    emit recordingChanged();
}
void Backend::setRecordPunch(bool enabled)
{
    if (recording()) return;
    m_recordPunch = enabled;
    emit recordingChanged();
}

void Backend::refreshRecordingInputs()
{
    scanRecordingRecovery();
    if (m_inputScan || m_shuttingDown) return;
    m_inputScan = new AudioCommand(this, QStringLiteral("pw-dump"), {}, 3000, 16 * 1024 * 1024,
        [this](AudioCommand::Result result) {
        m_inputScan = nullptr;
        QStringList names{QStringLiteral("System default input")}, ids{QString()};
        QSet<QString> jackIds;
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(result.output, &error);
        if (!result.ok || error.error != QJsonParseError::NoError || !document.isArray()) {
            setError(result.error.isEmpty() ? tr("Could not list PipeWire inputs. Refresh to retry.") : result.error);
        } else {
            for (const auto& value : document.array()) {
                const auto node = value.toObject();
                if (node.value("type").toString() != QStringLiteral("PipeWire:Interface:Node")) continue;
                const auto props = node.value("info").toObject().value("props").toObject();
                const auto mediaClass = props.value("media.class").toString();
                // Sources, and JACK clients that put audio out: pipewire-jack makes
                // them ordinary nodes a capture stream can target. Such a node has no
                // media.class, only client.api and its port counts.
                const bool jackOutput = props.value("client.api").toString() == QLatin1String("jack")
                                        && node.value("info").toObject().value("n-output-ports").toInt() > 0;
                if (mediaClass != QStringLiteral("Audio/Source") && !jackOutput) continue;
                const auto name = props.value("node.name").toString();
                const QString id = name;
                if (id.isEmpty()) continue;
                auto label = props.value("node.description").toString(props.value("application.name").toString(name));
                names.append(label + (jackOutput ? QStringLiteral(" (JACK client %1)") : QStringLiteral(" (%1)")).arg(id));
                ids.append(id);
                if (jackOutput) jackIds.insert(id);
            }
            m_recordingInputNames = names;
            m_recordingInputIds = ids;
            m_jackInputIds = jackIds;
            // Keep a disconnected selection explicit; never silently choose another input.
            if (!ids.contains(m_recordInput)) {
                m_recordingInputNames.append(QStringLiteral("Unavailable: %1").arg(m_recordInput));
                m_recordingInputIds.append(m_recordInput);
            }
        }
        emit recordingInputsChanged();
    });
    emit recordingInputsChanged();
}

void Backend::cancelRecordingInputScan()
{
    if (!m_inputScan) return;
    m_inputScan->cancel();
    m_inputScan = nullptr;
    emit recordingInputsChanged();
}

void Backend::startRecording()
{
    if (recording()) return;
    if (deliveryBoundaryActive()) {
        setError(tr("Wait for the current job to finish preparing its effect settings before recording."));
        return;
    }
    if (m_loading || m_projectLoadActive) {
        setError(QStringLiteral("wait for the project to finish opening before recording"));
        return;
    }
    auto target = m_recordArmedTrack < 0 ? session::kInvalidTrack : static_cast<session::TrackId>(m_recordArmedTrack);
    if (target != session::kInvalidTrack && !m_project.track(target)) {
        setError(QStringLiteral("the armed track was removed; arm a track or choose a new take"));
        return;
    }
    if (m_recordPunch && (m_selection.empty() || target == session::kInvalidTrack)) {
        setError(QStringLiteral("arm a track and select a time range for punch recording"));
        return;
    }
    if (m_recordLoopTakes > 0 && (m_selection.empty()
        || m_recordLoopTakes - (target != session::kInvalidTrack ? 1 : 0) > maxTracks() - trackCount()
        || m_selection.length() > std::numeric_limits<qint64>::max() / m_recordLoopTakes - sampleRate() * 60)) {
        setError(QStringLiteral("Select a loop range and leave enough free tracks for its passes (maximum 16).")); return;
    }
    if (target == session::kInvalidTrack && trackCount() >= maxTracks()) {
        setError(QStringLiteral("arm an existing track: this session has reached its track limit"));
        return;
    }
    const int passes = std::max(1, m_recordLoopTakes);
    int tracksNeeded = passes - (target != session::kInvalidTrack ? 1 : 0);
    QStringList inputs{m_recordInput};
    std::vector<int> targets{m_recordArmedTrack};
    for (const auto& value : m_additionalRecordInputs) {
        const auto row = value.toMap(); const auto input = row.value("input").toString();
        const int track = row.value("track").toInt();
        if (inputs.contains(input) || (track >= 0 && (std::find(targets.begin(), targets.end(), track) != targets.end()
            || !m_project.track(static_cast<session::TrackId>(track))))) {
            setError(QStringLiteral("Each input needs a distinct device and destination track; update the additional inputs.")); return;
        }
        inputs.append(input); targets.push_back(track); tracksNeeded += passes - (track >= 0 ? 1 : 0);
    }
    if (tracksNeeded > maxTracks() - trackCount()) {
        setError(QStringLiteral("Not enough free tracks for the selected inputs and loop passes.")); return;
    }
    const QString directory = recordingsDirectory();
    if (!QDir().mkpath(directory) || !QFile::setPermissions(directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
        setError(QStringLiteral("could not create the recording recovery directory"));
        return;
    }
    stop();
    setLoopEnabled(false);
    cancelClipMove();
    cancelSampleEdit();
    m_recordPlacement = {};
    m_recordPlacement.target = target;
    m_recordPlacement.anchor = m_focused;
    m_recordPlacement.start = m_recordPunch || m_recordLoopTakes > 0 ? m_selection.start : std::max<qint64>(0, m_playhead);
    m_recordPlacement.passLength = m_recordLoopTakes > 0 ? m_selection.length() : 0;
    m_recordPlacement.passes = m_recordLoopTakes;
    m_recordPlacement.limit = m_recordLoopTakes > 0 ? m_selection.length() * m_recordLoopTakes : m_recordPunch ? m_selection.length() : 0;
    const auto beatFrames = static_cast<session::FramePos>(std::llround(sampleRate() * 60.0 / m_recordTempo));
    const auto countIn = beatFrames * m_recordCountIn;
    m_recordPlacement.lead = static_cast<session::FramePos>(std::llround(m_recordPreRoll * sampleRate())) + countIn;
    m_recordPlacement.residual = static_cast<session::FramePos>(std::llround(m_recordOffsetMs * sampleRate() / 1000));
    m_recordPlacement.align = m_recordAlign && m_ready && (target != session::kInvalidTrack || !m_additionalRecordInputs.empty());
    m_recordPlacement.alignmentReady = !m_recordPlacement.align;
    m_recordPlacement.generation = m_sessionGeneration;
    m_recordPlacement.path = directory + QStringLiteral("/take-%1-%2.wav")
        .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss")),
             QString::fromStdString(session::Uuid::generate().toString()));
    if (const auto* track = m_project.track(target)) m_recordPlacement.expected = track->clips;
    m_captureClock.start.store(m_recordPlacement.start - m_recordPlacement.lead, std::memory_order_relaxed);
    m_captureClock.firstInputNs.store(0, std::memory_order_relaxed);
    m_captureClock.firstOutputNs.store(0, std::memory_order_relaxed);
    m_captureClock.countInFrames.store(countIn, std::memory_order_relaxed);
    m_captureClock.beatFrames.store(beatFrames, std::memory_order_relaxed);
    m_captureClock.frames.store(0, std::memory_order_relaxed);
    m_captureClock.channels.store(static_cast<unsigned>(m_recordChannels), std::memory_order_relaxed);
    m_captureClock.monitor.store(m_recordMonitor, std::memory_order_relaxed);
    m_captureClock.groupActive.store(!m_additionalRecordInputs.empty(), std::memory_order_relaxed);
    m_captureClock.epoch.fetch_add(1, std::memory_order_release);
    m_lastCaptureFrames = 0;
    m_recordingError.clear();
    m_recordStopping = false;
    m_captureProgress.start();
    try {
        m_recording = std::make_unique<engine::Recording>(m_captureClock, m_recordPlacement.path.toStdString(),
            static_cast<unsigned>(sampleRate()), static_cast<unsigned>(m_recordChannels),
            m_recordPlacement.limit > 0 ? std::max<qint64>(1, m_recordPlacement.limit + m_recordPlacement.lead
                + m_recordPlacement.residual + (m_recordPlacement.align ? sampleRate() * 2 : 0)) : 0);
        for (const auto& value : m_additionalRecordInputs) {
            const auto row = value.toMap(); auto input = std::make_unique<AdditionalRecording>();
            input->input = row.value("input").toString(); input->channels = row.value("channels").toUInt();
            input->placement = m_recordPlacement;
            input->placement.target = row.value("track").toInt() < 0 ? session::kInvalidTrack : static_cast<session::TrackId>(row.value("track").toInt());
            input->placement.expected.clear();
            if (const auto* track = m_project.track(input->placement.target)) input->placement.expected = track->clips;
            input->placement.path = directory + QStringLiteral("/take-%1.wav").arg(QString::fromStdString(session::Uuid::generate().toString()));
            input->placement.align = true; input->placement.alignmentReady = false;
            input->writer = std::make_unique<engine::Recording>(input->clock, input->placement.path.toStdString(),
                static_cast<unsigned>(sampleRate()), input->channels, input->placement.limit > 0
                ? std::max<qint64>(1, input->placement.limit + input->placement.lead + input->placement.residual + sampleRate() * 2) : 0);
            input->progress.start(); m_additionalRecordings.push_back(std::move(input));
        }
    } catch (const std::exception& error) {
        stopRecording();
        setError(QString::fromUtf8(error.what()));
        return;
    }
    m_recordingStatus = QStringLiteral("Preparing recording…");
    setError({});
    if (m_recordPlacement.passes > 0) {
        m_loopEnabled = true; m_loopStart = m_recordPlacement.start;
        m_loopEnd = m_loopStart + m_recordPlacement.passLength;
        enqueueAudioCommand(engine::Command::setLoop(true, m_loopStart, m_loopEnd)); emit loopChanged();
    }
    publishPlan();
    publishTrackParams();
    emit recordingChanged();
}

void Backend::stopRecording()
{
    ++m_jackLinkGeneration;
    if (m_jackLinkScan) { m_jackLinkScan->cancel(); m_jackLinkScan = nullptr; }
    if (m_jackInputLinks) { m_jackInputLinks->cancel(); m_jackInputLinks = nullptr; }
    if (!m_recording || m_recordStopping) return;
    // Quiesce the sole producer before signaling the writer or freeing its buffers.
    m_captureInput.reset();
    for (auto& input : m_additionalRecordings) { input->capture.reset(); input->writer->requestStop(); }
    m_captureClock.groupActive.store(false, std::memory_order_release);
    m_recording->requestStop();
    m_recordStopping = true;
    m_recordingStatus = QStringLiteral("Finishing recording…");
    enqueueAudioCommand(engine::Command::stop());
    emit recordingChanged();
}

void Backend::updateRecordingTiming()
{
    if (!m_recording) return;
    const auto output = m_captureClock.firstOutputNs.load(std::memory_order_acquire);
    const auto primary = m_captureClock.firstInputNs.load(std::memory_order_acquire);
    const auto update = [&](RecordingPlacement& placement, engine::CaptureClock& clock,
                            engine::Recording& writer, qint64 anchor, QString& error) {
        if (placement.alignmentReady) return;
        const auto alignment = engine::recordingAlignment(clock.firstInputNs.load(std::memory_order_acquire),
            anchor, static_cast<unsigned>(sampleRate()));
        if (alignment != std::numeric_limits<std::int64_t>::min()) {
            placement.alignment = alignment; placement.alignmentReady = true;
        } else if (clock.frames.load(std::memory_order_relaxed) > sampleRate() / 2 || m_recordStopping) {
            placement.alignmentReady = true;
            if (clock.frames.load(std::memory_order_relaxed) > 0) {
                if (!error.isEmpty()) error += QStringLiteral("; ");
                error += QStringLiteral("Device timing was unavailable; automatic alignment was skipped. The raw take is preserved.");
            }
        }
        if (placement.alignmentReady && placement.limit > 0)
            writer.setFrameLimit(std::max<qint64>(1, placement.lead + placement.alignment + placement.residual + placement.limit));
    };
    update(m_recordPlacement, m_captureClock, *m_recording, output, m_recordingError);
    for (auto& input : m_additionalRecordings)
        update(input->placement, input->clock, *input->writer,
            m_recordPlacement.align ? output : primary, input->error);
}

void Backend::pollRecording()
{
    if (!m_recording) return;
    updateRecordingTiming();
    const bool writersReady = std::all_of(m_additionalRecordings.begin(), m_additionalRecordings.end(),
        [](const auto& input) { return input->writer->ready() || input->writer->finished(); });
    if (!m_recordStopping && writersReady && !m_captureInput && m_recording->ready() && !m_recording->inputDone()) {
        m_captureInput = std::make_unique<engine::PwCapture>(*m_recording, static_cast<unsigned>(sampleRate()),
                                                          static_cast<unsigned>(m_recordChannels));
        std::string error;
        // A JACK client is not a target the session manager resolves: the stream
        // starts unlinked and its ports are wired to the client's by hand.
        const bool jackInput = m_jackInputIds.contains(m_recordInput);
        if (!m_captureInput->start(m_recordInput.toStdString(), error, !jackInput)) {
            m_recordingError = QString::fromStdString(error); stopRecording();
        } else {
            m_captureProgress.restart();
            m_recordingStatus = jackInput ? QStringLiteral("Connecting to the JACK client…") : QStringLiteral("Waiting for input…");
            if (jackInput) { m_jackLinkAttempts = 0; linkJackInput(); }
            for (auto& input : m_additionalRecordings) {
                if (input->writer->inputDone() || input->writer->finished()) continue;
                if (m_jackInputIds.contains(input->input)) {
                    input->error = QStringLiteral("a JACK client can be the primary input only"); input->writer->requestStop();
                    continue;
                }
                input->capture = std::make_unique<engine::PwCapture>(*input->writer, static_cast<unsigned>(sampleRate()), input->channels);
                if (!input->capture->start(input->input.toStdString(), error)) {
                    input->error = QString::fromStdString(error); input->capture.reset(); input->writer->requestStop();
                    continue;
                }
                input->progress.restart();
            }
        }
    }
    if (m_captureInput && !m_captureInput->error().empty()) {
        m_recordingError = QString::fromStdString(m_captureInput->error()); stopRecording();
    }
    const auto frames = m_captureClock.frames.load(std::memory_order_relaxed);
    if (!m_recordStopping) {
        if (frames != m_lastCaptureFrames) {
            m_captureProgress.restart(); m_lastCaptureFrames = frames;
            if (frames < m_recordPlacement.lead)
                m_recordingStatus = QStringLiteral("Lead-in · %1 s to recording").arg(double(m_recordPlacement.lead - frames) / sampleRate(), 0, 'f', 1);
            else if (m_recordPlacement.passes > 0)
                m_recordingStatus = QStringLiteral("Loop take %1 of %2").arg(std::min<qint64>(m_recordPlacement.passes,
                    recordingFrames() / m_recordPlacement.passLength + 1)).arg(m_recordPlacement.passes);
            else m_recordingStatus = m_recordPunch ? QStringLiteral("Punch recording") : QStringLiteral("Recording");
            if (!m_additionalRecordings.empty()) m_recordingStatus += QStringLiteral(" · %1 inputs").arg(m_additionalRecordings.size() + 1);
        } else if (!m_recording->inputDone() && m_captureProgress.elapsed() > 4000) {
            m_recordingError = QStringLiteral("recording stopped: the primary input did not supply audio"); stopRecording();
        }
    }
    for (auto& input : m_additionalRecordings) {
        if (m_recordStopping) break;
        const auto captured = input->clock.frames.load(std::memory_order_relaxed);
        if (captured != input->lastFrames) { input->lastFrames = captured; input->progress.restart(); }
        if (input->capture && !input->capture->error().empty()) {
            input->error = QString::fromStdString(input->capture->error()); stopRecording(); break;
        }
        if (!input->writer->inputDone() && input->progress.elapsed() > 4000) {
            input->error = QStringLiteral("selected input did not supply audio"); stopRecording(); break;
        }
        if (input->writer->inputDone() || input->writer->finished()) input->capture.reset();
    }
    if (m_recording->inputDone() || m_recording->finished()) m_captureInput.reset();
    const bool allDone = (m_recording->inputDone() || m_recording->finished())
        && std::all_of(m_additionalRecordings.begin(), m_additionalRecordings.end(),
            [](const auto& input) { return input->writer->inputDone() || input->writer->finished(); });
    if (allDone) stopRecording();
    const bool allFinished = m_recording->finished() && std::all_of(m_additionalRecordings.begin(), m_additionalRecordings.end(),
        [](const auto& input) { return input->writer->finished(); });
    if (allFinished && !m_busy.load(std::memory_order_acquire)) finishRecording();
    emit recordingChanged();
}

void Backend::finishRecording()
{
    if (!m_recording || !m_recording->finished() || std::any_of(m_additionalRecordings.begin(), m_additionalRecordings.end(),
        [](const auto& input) { return !input->writer->finished(); })) return;
    updateRecordingTiming();
    m_captureInput.reset(); m_captureClock.groupActive.store(false, std::memory_order_release);
    std::vector<CompletedRecording> takes;
    takes.push_back({m_recording->takeResult(), m_recordPlacement, m_recordInput, m_recordingError});
    m_recording.reset();
    for (auto& input : m_additionalRecordings) {
        input->capture.reset();
        takes.push_back({input->writer->takeResult(), input->placement, input->input, input->error});
    }
    m_additionalRecordings.clear();
    if (m_recordPlacement.passes > 0) setLoopEnabled(false);
    m_recordingStatus.clear(); m_recordStopping = false;
    installRecordings(std::move(takes));
}

void Backend::installRecordings(std::vector<CompletedRecording> takes)
{
    if (!m_applyingAutomationEdit && !m_applyingPluginEdit) {
        auto pending = std::make_shared<std::vector<CompletedRecording>>(std::move(takes));
        if (deferAutomationEdit([this, pending] { installRecordings(std::move(*pending)); return true; },
            [this] { scanRecordingRecovery(); emit recordingChanged(); })) return;
        takes = std::move(*pending);
    }
    if (takes.empty()) return;
    // One coherent edit for every usable input/pass. Source installation must not
    // race a display worker reading the source deque.
    joinWorker(); const auto before = captureEditState();
    QStringList errors, installed; bool changed = false;
    // New take tracks go just under the lane that was in focus when recording
    // began, in the order they are made; with no such lane, at the end.
    const auto anchorIndex = m_project.trackIndex(takes.front().placement.anchor);
    std::size_t insertAt = anchorIndex ? *anchorIndex + 1 : m_project.tracks().size();
    for (auto& take : takes) {
        auto& result = take.result; const auto& placement = take.placement;
        QStringList problems;
        if (!take.error.isEmpty()) problems.append(take.error);
        if (!result.error.empty()) problems.append(QString::fromStdString(result.error));
        const auto offset = placement.lead + placement.alignment + placement.residual;
        const auto rawFrames = static_cast<session::FramePos>(result.audio.frames());
        const auto end = placement.limit > 0 ? std::min(rawFrames - offset, placement.limit) : rawFrames - offset;
        const auto first = std::max<session::FramePos>(0, -offset);
        auto target = placement.target;
        const auto* track = m_project.track(target);
        if (target != session::kInvalidTrack && (!track || track->clips != placement.expected)) {
            target = session::kInvalidTrack;
            problems.append(QStringLiteral("the armed track changed; the recording was placed on a new track"));
        }
        if (end > first && placement.generation == m_sessionGeneration) {
            const auto period = placement.passLength > 0 ? placement.passLength : end;
            const auto firstPass = first / period;
            const auto lastPass = (end - 1) / period;
            const auto passes = lastPass - firstPass + 1;
            if (passes - (target != session::kInvalidTrack ? 1 : 0) > maxTracks() - trackCount()) {
                problems.append(QStringLiteral("recording kept in recovery: the session has reached its track limit"));
            } else {
                const auto name = QStringLiteral("Take %1").arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")))
                    + (takes.size() > 1 ? QStringLiteral(" · %1").arg(take.input.isEmpty() ? QStringLiteral("default input") : take.input) : QString{});
                const auto channels = result.audio.channels();
                const auto source = m_project.addSource(name.toStdString(), result.path, std::move(result.audio));
                for (auto pass = firstPass; pass <= lastPass; ++pass) {
                    const bool last = pass == lastPass;
                    auto destination = target;
                    if (!last || target == session::kInvalidTrack) {
                        destination = m_project.addTrack((name + (placement.passes > 0 ? QStringLiteral(" · pass %1").arg(pass + 1) : QString{})).toStdString());
                        insertAt = std::min(insertAt, m_project.tracks().size() - 1);
                        m_project.moveTrack(destination, insertAt++);
                    }
                    const auto from = std::max(first, pass * period), to = std::min(end, (pass + 1) * period);
                    session::Clip clip; clip.source = source; clip.sourceOffset = offset + from; clip.length = to - from;
                    const auto start = placement.start + from - pass * period;
                    session::edits::overwriteAt(m_project.track(destination)->clips, {clip}, start);
                    if (!last) m_project.track(destination)->muted = true;
                    if (last) { target = destination; m_selection = {start, start + clip.length, target}; }
                }
                m_focused = target; m_loaded = true; m_takePath = QString::fromStdString(result.path);
                m_takeName = name; m_takeChannels = static_cast<int>(channels); m_originalRate = sampleRate(); m_resampled = false;
                installed.append(m_takePath); changed = true;
            }
        } else if (!result.audio.empty()) {
            problems.append(placement.generation != m_sessionGeneration
                ? QStringLiteral("recording kept in recovery because the project changed")
                : QStringLiteral("Recording stopped during lead-in; the raw take is available in recovery."));
        } else if (result.error.empty()) QFile::remove(placement.path);
        if (!problems.empty()) errors.append((take.input.isEmpty() ? QStringLiteral("Default input") : take.input) + QStringLiteral(": ") + problems.join(QStringLiteral("; ")));
    }
    if (changed) {
        commit(takes.front().placement.passes > 0 ? "record loop takes" : takes.size() > 1 ? "record multiple inputs"
            : takes.front().placement.limit > 0 ? "punch record" : "record", m_focused, before);
        for (const auto& path : installed) rememberRecording(path);
        seekFrames(m_selection.end); emit takeChanged(); emit selectionChanged();
    }
    publishPlan();
    if (!errors.empty()) setError(errors.join(QStringLiteral("; ")));
    scanRecordingRecovery(); emit recordingChanged();
    pumpRenders();
}

void Backend::rememberRecording(const QString& path)
{
    if (QFileInfo(path).absolutePath() != recordingsDirectory() || !QFile::exists(path)) return;
    if (std::none_of(m_recordedSpools.begin(), m_recordedSpools.end(), [&path](const auto& spool) { return spool.path == path; }))
        m_recordedSpools.push_back({path, m_sessionGeneration, m_editRevision});
    scanRecordingRecovery();
}

void Backend::retireRecordings(std::uint64_t generation, std::uint64_t revision)
{
    // A manual project save serializes every immutable source, including undo sources.
    // Autosaves can be discarded when replacing a session, so retain spools until Save.
    std::erase_if(m_recordedSpools, [generation, revision](const auto& spool) {
        return spool.generation == generation && spool.revision <= revision && QFile::remove(spool.path);
    });
    scanRecordingRecovery();
}

void Backend::scanRecordingRecovery()
{
    QVariantList files;
    const QDir directory(recordingsDirectory());
    for (const auto& info : directory.entryInfoList({QStringLiteral("take-*.wav")}, QDir::Files | QDir::NoSymLinks, QDir::Time)) {
        const QString path = info.absoluteFilePath();
        if (m_recording && path == m_recordPlacement.path) continue;
        if (std::any_of(m_additionalRecordings.begin(), m_additionalRecordings.end(),
            [&](const auto& input) { return input->placement.path == path; })) continue;
        if (std::any_of(m_recordedSpools.begin(), m_recordedSpools.end(), [&](const auto& spool) {
                return spool.path == path && spool.generation == m_sessionGeneration;
            })) continue;
        files.append(QVariantMap{{"path", path}, {"name", info.lastModified().toString(QStringLiteral("dd MMM yyyy HH:mm:ss"))},
                                 {"bytes", info.size()}});
    }
    m_recoverableRecordings = files;
    emit recordingInputsChanged();
}

void Backend::recoverRecording(const QString& path)
{
    scanRecordingRecovery();
    const auto found = std::find_if(m_recoverableRecordings.begin(), m_recoverableRecordings.end(), [&](const auto& item) {
        return item.toMap().value("path").toString() == path;
    });
    if (found == m_recoverableRecordings.end() || recording()) return;
    const QByteArray encoded = QFile::encodeName(path);
    const int fd = ::open(encoded.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) { setError(QStringLiteral("could not open the recoverable recording")); return; }
    const bool locked = ::flock(fd, LOCK_SH | LOCK_NB) == 0;
    ::close(fd);
    if (!locked) { setError(QStringLiteral("that recording is still being written by another window")); return; }
    importFile(path);
}

// --- Audio Setup (W06) ---------------------------------------------------------------
// A read-only picture of the graph from pw-dump and the kernel's ALSA lists, plus
// whether the JACK compatibility path is installed. Refreshed only when asked.

#include "audiosetup.h"
#include <QFile>
#include <QStandardPaths>
#include <QJsonDocument>

namespace {
QString readTextFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

// pipewire-jack ships a libjack that links libpipewire; jack2's does not. Reading
// the library for that name is enough to tell them apart without loading it.
QString libjackProvider()
{
    for (const char *candidate : {"/usr/lib/libjack.so.0", "/usr/lib64/libjack.so.0", "/usr/lib/x86_64-linux-gnu/libjack.so.0"}) {
        QFile f(QString::fromLatin1(candidate));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QByteArray bytes = f.readAll();
        return bytes.contains("libpipewire-0.3") ? QStringLiteral("pipewire") : QStringLiteral("other");
    }
    return QStringLiteral("missing");
}
}

void Backend::refreshAudioSetup()
{
    if (m_audioSetupScan || m_shuttingDown) return;
    m_audioSetupScan = new AudioCommand(this, QStringLiteral("pw-dump"), {}, 3000, 16 * 1024 * 1024,
        [this](AudioCommand::Result result) {
        m_audioSetupScan = nullptr;
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(result.output, &parseError);
        QString problem = result.error;
        if (result.ok && (parseError.error != QJsonParseError::NoError || !document.isArray()))
            problem = tr("The audio system returned an unreadable device list. Refresh to retry.");
        const auto dump = problem.isEmpty() ? document.array() : QJsonArray{};
        QVariantMap picture = omaaudio::audiosetup::parse(dump, readTextFile(QStringLiteral("/proc/asound/cards")),
                                                          readTextFile(QStringLiteral("/proc/asound/pcm")));
        QVariantMap jack;
        jack["pwJack"] = !QStandardPaths::findExecutable(QStringLiteral("pw-jack")).isEmpty();
        jack["libjack"] = libjackProvider();
        jack["available"] = jack["libjack"].toString() == QLatin1String("pipewire");
        picture["jack"] = jack;
        picture["projectRate"] = static_cast<int>(m_project.sampleRate());
        picture["graphRate"] = graphSampleRate();
        picture["problem"] = problem;
        picture["scannedAt"] = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
        m_audioSetup = picture;
        emit audioSetupChanged();
    });
    emit audioSetupChanged();
}

void Backend::cancelAudioSetupScan()
{
    if (!m_audioSetupScan) return;
    m_audioSetupScan->cancel();
    m_audioSetupScan = nullptr;
    m_audioSetup["problem"] = tr("Reading the audio system was cancelled. Refresh to try again.");
    emit audioSetupChanged();
}

namespace {
// Pairs OmaAudio's output ports with a JACK client's inputs by order: FL to the
// first input, FR to the second, and so on for as many as both have.
QList<QPair<int, int>> portPairs(const QVariantMap &picture, int nodeId)
{
    QList<QPair<int, int>> pairs;
    QVariantList ours = picture["ourOutputPorts"].toList(), theirs;
    for (const QVariant &c : picture["jackClients"].toList()) {
        const QVariantMap m = c.toMap();
        if (m["id"].toInt() == nodeId) { theirs = m["inputPorts"].toList(); break; }
    }
    for (int i = 0; i < ours.size() && i < theirs.size(); ++i)
        pairs.append({ours.at(i).toMap()["id"].toInt(), theirs.at(i).toMap()["id"].toInt()});
    return pairs;
}
}

bool Backend::linkOutputTo(int nodeId) { return startOutputLinks(nodeId, false); }
bool Backend::unlinkOutputFrom(int nodeId) { return startOutputLinks(nodeId, true); }

bool Backend::startOutputLinks(int nodeId, bool disconnectPorts)
{
    if (m_outputLinks || m_shuttingDown) return false;
    const auto pairs = portPairs(m_audioSetup, nodeId);
    if (pairs.isEmpty()) {
        setError(tr("No audio ports are available for this connection. Refresh Audio Setup."));
        return false;
    }
    setError({});
    m_audioLinkMessage = disconnectPorts ? tr("Disconnecting playback…") : tr("Connecting playback…");
    m_outputLinks = new AudioLinks(this, pairs, disconnectPorts,
        [this, disconnectPorts, total = pairs.size()](QString error, int completed) {
        m_outputLinks = nullptr;
        if (error.isEmpty()) {
            m_audioLinkMessage = disconnectPorts ? tr("Playback disconnected.") : tr("Playback connected.");
        } else {
            m_audioLinkMessage = tr("%1 of %2 channels completed. %3 Check the refreshed connections before trying again.")
                .arg(completed).arg(total).arg(error);
            setError(m_audioLinkMessage);
        }
        // A scan begun before the change must not publish an obsolete graph.
        if (m_audioSetupScan) { m_audioSetupScan->cancel(); m_audioSetupScan = nullptr; }
        refreshAudioSetup();
        emit audioSetupChanged();
    });
    emit audioSetupChanged();
    return true;
}

void Backend::cancelAudioLinks()
{
    if (!m_outputLinks) return;
    const int completed = m_outputLinks->completed();
    m_outputLinks->cancel();
    m_outputLinks = nullptr;
    m_audioLinkMessage = tr("Connection changes cancelled after %1 channels. A command already sent may have changed another channel; check the refreshed connections.").arg(completed);
    if (m_audioSetupScan) { m_audioSetupScan->cancel(); m_audioSetupScan = nullptr; }
    refreshAudioSetup();
    emit audioSetupChanged();
}

// Wire a JACK client's outputs to the recording stream's inputs, in order: the
// first output to the first input, and the last output to any inputs left over,
// so a mono client feeds a stereo take on both sides. Reads the graph once the
// stream has appeared; a few retries cover the moment between connect and
// registration.
void Backend::linkJackInput()
{
    if (!m_recording || m_recordStopping || !m_captureInput || m_jackLinkScan || m_jackInputLinks || m_shuttingDown) return;
    const QString target = m_recordInput;
    const quint64 generation = m_jackLinkGeneration;
    m_jackLinkScan = new AudioCommand(this, QStringLiteral("pw-dump"), {}, 3000, 16 * 1024 * 1024,
        [this, target, generation](AudioCommand::Result result) {
        m_jackLinkScan = nullptr;
        if (generation != m_jackLinkGeneration || !m_recording || m_recordStopping || !m_captureInput) return;
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(result.output, &error);
        if (!result.ok || error.error != QJsonParseError::NoError || !document.isArray()) {
            m_recordingError = result.error.isEmpty() ? tr("Could not read JACK connections. Refresh Audio Setup and try recording again.") : result.error;
            stopRecording();
            return;
        }
        const QVariantMap picture = omaaudio::audiosetup::parse(document.array(), QString(), QString());
        const QVariantList ours = picture["ourCapturePorts"].toList();
        QVariantList theirs;
        for (const QVariant &c : picture["jackClients"].toList())
            if (c.toMap()["name"].toString() == target) theirs = c.toMap()["outputPorts"].toList();
        if (ours.isEmpty() || theirs.isEmpty()) {
            if (++m_jackLinkAttempts < 6) {
                QTimer::singleShot(200, this, [this, generation] {
                    if (generation == m_jackLinkGeneration) linkJackInput();
                });
                return;
            }
            m_recordingError = theirs.isEmpty() ? tr("%1: the JACK client is no longer available. Start it and select it again.").arg(target)
                                                : tr("The recording connection did not appear. Refresh Audio Setup and retry.");
            stopRecording();
            return;
        }
        AudioLinks::Ports pairs;
        for (int i = 0; i < ours.size(); ++i)
            pairs.append({theirs.at(std::min(i, int(theirs.size()) - 1)).toMap()["id"].toInt(), ours.at(i).toMap()["id"].toInt()});
        m_jackInputLinks = new AudioLinks(this, pairs, false, [this, generation](QString problem, int) {
            m_jackInputLinks = nullptr;
            if (generation != m_jackLinkGeneration || !m_recording || m_recordStopping || !m_captureInput) return;
            if (!problem.isEmpty()) { m_recordingError = problem; stopRecording(); return; }
            m_recordingStatus = tr("Waiting for input…");
            emit recordingChanged();
        });
    });
}
