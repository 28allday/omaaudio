#include "backend.h"

#include <algorithm>
#include <cmath>

QVariantMap Backend::trackLoudnessReport() const
{
    QVariantMap report = reportToMap(m_trackLoudness.measurement.report);
    const auto *track = m_project.track(m_trackLoudness.track);
    report["valid"] = m_trackLoudness.measurement.ok && track != nullptr
        && m_trackLoudness.generation == m_sessionGeneration;
    report["stale"] = m_trackLoudnessStale
        || !analysisCurrent(m_trackLoudness.generation, m_trackLoudness.revision, m_trackLoudness.analysisEpoch);
    report["track"] = int(m_trackLoudness.track);
    report["name"] = track ? QString::fromStdString(track->name) : QString();
    report["stamp"] = QString::number(m_trackLoudness.generation) + ":" + QString::number(m_trackLoudness.revision);
    report["message"] = m_trackLoudnessMessage;
    return report;
}

bool Backend::measureTrackLoudness(int track)
{
    if (m_shuttingDown || m_recording || automationWriting() || m_loading || m_saving || m_scanning
        || m_measuringTrackLoudness || m_busy.load(std::memory_order_acquire)) {
        setError(tr("Finish recording or the current background job before measuring the track.")); return false;
    }
    const auto id = trackArg(track);
    std::string error;
    auto isolated = render::isolateTrackOutput(m_project, id, error);
    if (!isolated || isolated->length() <= 0) {
        setError(isolated ? tr("This track has no audio or incoming feeds to measure.") : QString::fromStdString(error)); return false;
    }
    isolated.reset();
    joinWorker();
    setError({});
    m_trackLoudnessMessage = tr("Measuring track output…");
    m_trackLoudnessProgress = 0;
    m_cancelTrackLoudness.store(false, std::memory_order_relaxed);
    m_jobProgress.store(0, std::memory_order_relaxed);
    m_busy.store(true, std::memory_order_release);
    m_measuringTrackLoudness = true;
    emit trackLoudnessChanged();
    const auto generation = m_sessionGeneration;
    // Routing may feed this track from several others. Capture the accepted
    // project first, then isolate its inputs; model-only state can miss a native
    // gesture that is still live and has not yet reached History.
    requestTimelineSnapshot([this, generation] {
        return m_shuttingDown || generation != m_sessionGeneration
            || m_cancelTrackLoudness.load(std::memory_order_relaxed);
    }, [this, id](DeliverySnapshot snapshot, QString error, bool cancelled) {
        startCapturedTrackLoudness(std::move(snapshot), id, std::move(error), cancelled);
    });
    return true;
}

void Backend::startCapturedTrackLoudness(DeliverySnapshot captured, session::TrackId id, QString error, bool cancelled)
{
    const auto failed = [&](const QString& message) {
        TrackLoudnessJob result;
        result.generation = captured.generation; result.revision = captured.revision;
        result.analysisEpoch = captured.analysisEpoch; result.track = id;
        result.measurement.cancelled = cancelled; result.measurement.error = message.toStdString();
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_pendingTrackLoudness.push_back(std::move(result));
            m_finishedJobs.push_back(Job::TrackLoudness);
        }
        m_busy.store(false, std::memory_order_release);
        emit workerFinished();
    };
    if (cancelled || !error.isEmpty()) { failed(error); return; }
    std::string reason;
    auto isolated = render::isolateTrackOutput(captured.project, id, reason);
    if (!isolated) { failed(QString::fromStdString(reason)); return; }
    m_offlineFeatures = std::make_shared<lv2::HostFeatures>(m_urids, isolated->sampleRate(), lv2::kMaxBlock);
    std::vector<std::shared_ptr<lv2::Chain>> chains;
    std::vector<render::TrackChain> chainList;
    for (const auto &t : isolated->tracks()) {
        if (t.inserts.empty()) continue;
        auto chain = std::make_shared<lv2::Chain>(kOutputChannels, lv2::kMaxBlock, host::ProcessingMode::Offline);
        for (const auto &insert : t.inserts) {
            if (insert.bypass) { chain->appendMissing(insert.identity); continue; }
            if (!m_lv2->scanned()) m_lv2->scan();
            if (!chain->append(*m_lv2, *m_offlineFeatures, insert, reason)) {
                failed(tr("Cannot measure an active effect: ") + QString::fromStdString(reason)); return;
            }
        }
        chainList.push_back({t.id, chain.get()}); chains.push_back(std::move(chain));
    }
    m_offlineChains = chains;
    const auto generation = captured.generation, revision = captured.revision, epoch = captured.analysisEpoch;
    m_worker = std::thread([this, snapshot = std::move(*isolated), id, generation, revision, epoch, chains, chainList] {
        TrackLoudnessJob result;
        result.generation = generation; result.revision = revision; result.track = id;
        result.analysisEpoch = epoch;
        bool warmed = true;
        for (const auto& chain : chains) {
            if (!chain->warmUp(&m_cancelTrackLoudness)) { warmed = false; break; }
        }
        if (warmed) {
            result.measurement = render::measureMixInput(snapshot, kOutputChannels, chainList, &m_cancelTrackLoudness,
                [this](float progress) { m_jobProgress.store(progress, std::memory_order_relaxed); });
        } else {
            result.measurement.cancelled = m_cancelTrackLoudness.load(std::memory_order_relaxed);
            result.measurement.error = result.measurement.cancelled ? "Effect warm-up cancelled." : "Effect warm-up failed.";
        }
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_pendingTrackLoudness.push_back(std::move(result));
            m_finishedJobs.push_back(Job::TrackLoudness);
        }
        m_busy.store(false, std::memory_order_release);
        emit workerFinished();
    });
}

void Backend::cancelTrackLoudness()
{
    if (!m_measuringTrackLoudness) return;
    m_cancelTrackLoudness.store(true, std::memory_order_relaxed);
    m_trackLoudnessMessage = tr("Cancelling measurement…");
    emit trackLoudnessChanged();
}

void Backend::finishTrackLoudness(TrackLoudnessJob result)
{
    m_measuringTrackLoudness = false;
    if (m_shuttingDown || result.generation != m_sessionGeneration) {
        emit trackLoudnessChanged(); return;
    }
    if (result.measurement.cancelled || m_cancelTrackLoudness.load(std::memory_order_relaxed)) {
        m_trackLoudnessMessage = tr("Measurement cancelled.");
    } else if (!result.measurement.ok) {
        m_trackLoudnessMessage = QString::fromStdString(result.measurement.error);
        setError(m_trackLoudnessMessage);
    } else {
        m_trackLoudness = std::move(result);
        consumePluginNotifications();
        m_trackLoudnessStale = !analysisCurrent(m_trackLoudness.generation, m_trackLoudness.revision, m_trackLoudness.analysisEpoch);
        m_trackLoudnessProgress = 1;
        m_trackLoudnessMessage = m_trackLoudnessStale ? tr("The mix changed during measurement. Measure again.") : tr("Track output measured.");
    }
    emit trackLoudnessChanged();
}

QVariantMap Backend::trackLoudnessProposal(int track, double targetLufs, double ceilingDbTp) const
{
    const auto reject = [](const QString &reason) { return QVariantMap{{"valid", false}, {"reason", reason}}; };
    if (!std::isfinite(targetLufs) || targetLufs < -40 || targetLufs > -5
        || !std::isfinite(ceilingDbTp) || ceilingDbTp < -12 || ceilingDbTp > 0)
        return reject(tr("Choose a target from -40 to -5 LUFS and a ceiling from -12 to 0 dBTP."));
    const auto *t = m_project.track(trackArg(track));
    if (!t || t->id != m_trackLoudness.track || !m_trackLoudness.measurement.ok
        || m_trackLoudness.generation != m_sessionGeneration)
        return reject(tr("Measure this track before matching its loudness."));
    if (m_measuringTrackLoudness || m_trackLoudnessStale
        || !analysisCurrent(m_trackLoudness.generation, m_trackLoudness.revision, m_trackLoudness.analysisEpoch))
        return reject(tr("Measure the current track output before applying a match."));
    const auto &report = m_trackLoudness.measurement.report;
    if (!std::isfinite(report.integrated) || !std::isfinite(report.truePeak)
        || report.integrated <= -100 || report.truePeak <= -100 || !std::isfinite(t->gain) || t->gain <= 0)
        return reject(tr("No measurable loudness. Check for silence, a muted feed or audio shorter than 400 ms."));
    const double current = dbFromLinear(t->gain);
    const double requested = targetLufs - report.integrated;
    const double peakLimit = ceilingDbTp - report.truePeak;
    const double delta = std::min({requested, peakLimit, 12 - current});
    if (current + delta < -60)
        return reject(tr("This match needs a fader below -60 dB. Lower the source or effect output and measure again."));
    return {{"valid", true}, {"gainDb", current + delta}, {"changeDb", delta},
            {"estimatedLufs", report.integrated + delta}, {"estimatedPeak", report.truePeak + delta},
            {"peakLimited", peakLimit < requested - 0.01 && peakLimit <= 12 - current},
            {"gainLimited", 12 - current < std::min(requested, peakLimit) - 0.01},
            {"stamp", automationStamp()}};
}

bool Backend::applyTrackLoudness(int track, const QString &stamp, double targetLufs, double ceilingDbTp)
{
    if (const auto deferred = deferAutomationEdit([this, track, stamp, targetLufs, ceilingDbTp] { return applyTrackLoudness(track, stamp, targetLufs, ceilingDbTp); })) return *deferred;
    consumePluginNotifications();
    if (m_shuttingDown || recording() || loading() || sampleEditing() || m_repairing
        || storageBusy() || m_clipMove || automationWriting()) {
        setError(tr("Finish recording or the current edit before matching loudness.")); return false;
    }
    if (!automationStampCurrent(stamp)) {
        setError(tr("The mix changed. Measure the track again before applying.")); return false;
    }
    const auto proposal = trackLoudnessProposal(track, targetLufs, ceilingDbTp);
    if (!proposal.value("valid").toBool()) { setError(proposal.value("reason").toString()); return false; }
    const auto id = trackArg(track);
    const auto before = captureEditState();
    setError({});
    const double gain = proposal.value("gainDb").toDouble();
    if (std::abs(gain - dbFromLinear(m_project.track(id)->gain)) > 0.000001)
        m_project.track(id)->gain = linearFromDb(gain);
    commit("match track loudness", id, before);
    return m_lastError.isEmpty();
}
