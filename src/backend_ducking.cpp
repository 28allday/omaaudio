#include "backend.h"
#include "engine/routing_render.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <exception>

namespace {
bool duckNumber(const QVariant& value, double& out)
{
    switch (value.metaType().id()) {
    case QMetaType::Int: case QMetaType::UInt: case QMetaType::LongLong:
    case QMetaType::ULongLong: case QMetaType::Float: case QMetaType::Double:
        out = value.toDouble(); return std::isfinite(out);
    default: return false;
    }
}
bool duckInteger(const QVariant& value, qint64& out, double maximum)
{
    double number = 0;
    if (!duckNumber(value, number) || number < 0 || number > maximum || std::trunc(number) != number)
        return false;
    out = static_cast<qint64>(number); return true;
}
QVariantList duckPoints(const omawave::session::AutomationCurve& curve)
{
    QVariantList out;
    for (const auto& p : curve.points) {
        const bool smooth = p.interpolation == omawave::session::AutomationInterpolation::Smooth;
        QVariantMap point{{"frame", qint64(p.frame)}, {"value", p.value},
            {"interpolation", smooth ? 3 : curve.holds(p) ? 2 : 1}};
        if (smooth) { point.insert("smoothStart", p.smoothStart); point.insert("smoothEnd", p.smoothEnd); }
        out.append(point);
    }
    return out;
}
}

QVariantMap Backend::duckingPreview() const
{
    const auto* track = m_project.track(m_ducking.track);
    const bool stale = m_ducking.serial != 0 && (m_ducking.serial != m_duckingSerial
        || !analysisCurrent(m_ducking.generation, m_ducking.revision, m_ducking.analysisEpoch)
        || !track || track->volumeAutomation != m_ducking.replaces);
    QVariantList triggers;
    for (const auto id : m_ducking.triggers) triggers.append(int(id));
    // Keep complete original segments. The graph clips them to the analysed
    // range; inserting sampled boundaries would change a Smooth segment's phase.
    return {{"valid", m_duckingReady && !stale && !m_generatingDucking}, {"stale", stale},
        {"token", m_duckingReady ? QString::number(m_ducking.serial) : QString()},
        {"track", int(m_ducking.track)}, {"name", track ? QString::fromStdString(track->name) : QString()},
        {"start", qint64(m_ducking.settings.start)}, {"end", qint64(m_ducking.settings.end)},
        {"triggers", triggers}, {"points", duckPoints(m_ducking.result.curve)},
        {"previousPoints", duckPoints(m_ducking.baseline)},
        {"previousEnabled", m_ducking.baseline.enabled},
        {"previousHold", m_ducking.baseline.hold},
        {"pointCount", int(m_ducking.result.curve.points.size())},
        {"activityCount", int(m_ducking.result.regions)},
        {"changed", m_ducking.result.changed}, {"replacementRequired", m_ducking.result.replacementRequired},
        {"message", m_duckingReady && stale ? tr("The mix changed. Analyse again before applying.") : m_duckingMessage}};
}

bool Backend::analyseDucking(const QVariantMap& spec)
{
    const auto reject = [this](const QString& reason) { setError(reason); return false; };
    if (m_shuttingDown || recording() || automationWriting() || loading() || m_saving || m_scanning
        || m_generatingDucking || m_busy.load(std::memory_order_acquire) || sampleEditing()
        || storageBusy() || m_repairing || m_clipMove)
        return reject(tr("Finish recording or the current background edit before analysing ducking."));
    const QStringList keys{"track", "triggers", "start", "end", "thresholdDb", "reductionDb",
        "attackMs", "holdMs", "releaseMs", "replaceExisting", "stamp"};
    // Optional: "rebuild" starts again from the curve a stored recipe that still
    // follows its triggers was made from, so new settings replace that ducking
    // instead of adding to it.
    const bool rebuildKey = spec.contains("rebuild");
    if (spec.size() != keys.size() + (rebuildKey ? 1 : 0) || std::any_of(keys.begin(), keys.end(), [&](const auto& key) { return !spec.contains(key); }))
        return reject(tr("Ducking needs a target, trigger tracks, range, threshold, reduction, fade times, replacement choice and current stamp."));
    if (rebuildKey && spec.value("rebuild").metaType().id() != QMetaType::Bool)
        return reject(tr("Rebuilding must be an explicit on/off choice."));
    if (spec.value("stamp").metaType().id() != QMetaType::QString || spec.value("stamp").toString() != automationStamp())
        return reject(tr("The project changed. Reload the ducking settings and analyse again."));
    qint64 target = 0, start = 0, end = 0;
    if (!duckInteger(spec.value("track"), target, std::numeric_limits<int>::max())
        || !duckInteger(spec.value("start"), start, 9007199254740991.0)
        || !duckInteger(spec.value("end"), end, 9007199254740991.0)
        || end <= start || end > m_project.length())
        return reject(tr("Choose a valid target and a nonempty sample range inside the project."));
    DuckingJob job;
    job.track = static_cast<session::TrackId>(target);
    const auto* track = m_project.track(job.track);
    if (!track) return reject(tr("Choose an existing track to lower."));
    if (!track->volumeAutomation.enabled && !track->volumeAutomation.points.empty())
        return reject(tr("This track has disabled volume automation. Enable or clear it in Automation before generating ducking."));
    if (spec.value("triggers").metaType().id() != QMetaType::QVariantList
        || spec.value("replaceExisting").metaType().id() != QMetaType::Bool)
        return reject(tr("Trigger tracks must be a list and replacement must be an explicit on/off choice."));
    if (spec.value("triggers").toList().size() > qsizetype(engine::RoutingRender::maxTracks))
        return reject(tr("Analyse at most %1 trigger tracks at once.").arg(engine::Transport::kMaxTracks));
    std::set<session::TrackId> unique;
    for (const auto& value : spec.value("triggers").toList()) {
        qint64 id = 0;
        if (!duckInteger(value, id, std::numeric_limits<int>::max()))
            return reject(tr("Choose valid, distinct trigger tracks."));
        const auto trigger = static_cast<session::TrackId>(id);
        if (trigger == job.track || !m_project.track(trigger) || !unique.insert(trigger).second)
            return reject(tr("Choose distinct trigger tracks other than the track being lowered."));
        std::string error;
        const auto isolated = render::isolateTrackOutput(m_project, trigger, error);
        if (!isolated) return reject(QString::fromStdString(error));
        if (isolated->tracks().size() > engine::RoutingRender::maxTracks)
            return reject(tr("A trigger and its incoming feeds exceed the %1-track analysis routing limit.").arg(engine::Transport::kMaxTracks));
        if (isolated->track(job.track))
            return reject(tr("The track being lowered feeds a trigger. Choose independent trigger tracks."));
        job.triggers.push_back(trigger);
    }
    if (job.triggers.empty()) return reject(tr("Choose at least one trigger track."));
    double threshold = 0, reduction = 0, attack = 0, hold = 0, release = 0;
    if (!duckNumber(spec.value("thresholdDb"), threshold) || threshold < -80 || threshold > 0
        || !duckNumber(spec.value("reductionDb"), reduction) || reduction < 0 || reduction > 60
        || !duckNumber(spec.value("attackMs"), attack) || attack < 0 || attack > 10000
        || !duckNumber(spec.value("holdMs"), hold) || hold < 0 || hold > 10000
        || !duckNumber(spec.value("releaseMs"), release) || release < 0 || release > 10000)
        return reject(tr("Use threshold −80 to 0 dBFS, reduction 0 to 60 dB and fade/hold times 0 to 10000 ms."));
    job.settings = {.start=start, .end=end, .thresholdDb=threshold, .reductionDb=reduction,
        .attackSeconds=attack/1000, .holdSeconds=hold/1000, .releaseSeconds=release/1000,
        .replaceInRange=spec.value("replaceExisting").toBool()};
    job.rebuild = rebuildKey && spec.value("rebuild").toBool();
    joinWorker();
    job.serial = ++m_duckingSerial;
    m_duckingRunningSerial = job.serial;
    m_duckingReady = false; m_duckingStale = true; m_ducking = {};
    m_duckingProgress = 0; m_duckingMessage = tr("Analysing trigger audio…");
    m_cancelDucking.store(false, std::memory_order_relaxed);
    m_jobProgress.store(0, std::memory_order_relaxed);
    m_busy.store(true, std::memory_order_release); m_generatingDucking = true;
    setError({}); emit duckingChanged();
    const auto generation = m_sessionGeneration, serial = job.serial;
    requestTimelineSnapshot([this, generation, serial] {
        return m_shuttingDown || generation != m_sessionGeneration || serial != m_duckingSerial
            || m_cancelDucking.load(std::memory_order_relaxed);
    }, [this, job = std::move(job)](DeliverySnapshot snapshot, QString error, bool cancelled) mutable {
        startCapturedDucking(std::move(snapshot), std::move(job), std::move(error), cancelled);
    });
    return true;
}

void Backend::startCapturedDucking(DeliverySnapshot captured, DuckingJob job, QString error, bool cancelled)
{
    job.generation = captured.generation; job.revision = captured.revision; job.analysisEpoch = captured.analysisEpoch;
    if (job.follow) {
        // Regenerate from the curve as it was before the first generation.
        if (auto* track = captured.project.track(job.track)) track->volumeAutomation = job.baseline;
    } else if (auto* track = captured.project.track(job.track)) {
        job.replaces = track->volumeAutomation;
        // Only a recipe whose points were not edited by hand is rebuilt.
        if (job.rebuild && track->ducking && track->volumeAutomation == track->ducking->generated)
            track->volumeAutomation = track->ducking->baseline;
        job.baseline = track->volumeAutomation;
    }
    const auto failed = [&](const QString& reason) {
        job.result.cancelled = cancelled;
        job.result.error = reason.toStdString();
        { const std::lock_guard<std::mutex> lock(m_mutex);
          m_pendingDucking.push_back(std::move(job)); m_finishedJobs.push_back(Job::Ducking); }
        m_busy.store(false, std::memory_order_release); emit workerFinished();
    };
    if (cancelled || !error.isEmpty()) { failed(error); return; }
    try {
        m_offlineFeatures = std::make_shared<lv2::HostFeatures>(m_urids, captured.project.sampleRate(), lv2::kMaxBlock);
        std::vector<std::shared_ptr<lv2::Chain>> owners;
        std::vector<std::vector<render::TrackChain>> inputs;
        for (const auto trigger : job.triggers) {
            std::string reason;
            auto isolated = render::isolateTrackOutput(captured.project, trigger, reason);
            if (!isolated) { failed(QString::fromStdString(reason)); return; }
            std::vector<render::TrackChain> chains;
            for (const auto& track : isolated->tracks()) {
                if (track.inserts.empty()) continue;
                auto chain = std::make_shared<lv2::Chain>(kOutputChannels, lv2::kMaxBlock, host::ProcessingMode::Offline);
                for (const auto& insert : track.inserts) {
                    if (insert.bypass) { chain->appendMissing(insert.identity); continue; }
                    if (!m_lv2->scanned()) m_lv2->scan();
                    if (!chain->append(*m_lv2, *m_offlineFeatures, insert, reason)) {
                        failed(tr("Cannot analyse an active trigger effect: ") + QString::fromStdString(reason)); return;
                    }
                }
                chains.push_back({track.id, chain.get()}); owners.push_back(std::move(chain));
            }
            inputs.push_back(std::move(chains));
        }
        m_offlineChains = owners;
        m_worker = std::thread([this, snapshot = std::move(captured.project), job = std::move(job), owners, inputs = std::move(inputs)]() mutable {
            try {
                bool warmed = true;
                for (const auto& chain : owners) {
                    if (!chain->warmUp(&m_cancelDucking)) { warmed = false; break; }
                }
                if (warmed) job.result = render::generateDucking(snapshot, job.track, job.triggers, job.settings,
                    inputs, &m_cancelDucking, [this](float p) { m_jobProgress.store(p, std::memory_order_relaxed); });
                else {
                    job.result.cancelled = m_cancelDucking.load(std::memory_order_relaxed);
                    job.result.error = job.result.cancelled ? "Ducking analysis cancelled." : "Trigger effect warm-up failed.";
                }
            } catch (const std::exception& exception) { job.result.error = exception.what(); }
            catch (...) { job.result.error = "Ducking analysis failed."; }
            { const std::lock_guard<std::mutex> lock(m_mutex);
              m_pendingDucking.push_back(std::move(job)); m_finishedJobs.push_back(Job::Ducking); }
            m_busy.store(false, std::memory_order_release); emit workerFinished();
        });
    } catch (const std::exception& exception) { failed(QString::fromUtf8(exception.what())); }
}

void Backend::cancelDucking()
{
    m_cancelDucking.store(true, std::memory_order_relaxed);
    ++m_duckingSerial; m_duckingReady = false; m_duckingStale = true;
    m_duckingMessage = m_generatingDucking ? tr("Cancelling ducking analysis…") : tr("Ducking preview discarded.");
    emit duckingChanged();
}

void Backend::finishDucking(DuckingJob result)
{
    if (result.follow) { finishFollowedDucking(std::move(result)); return; }
    if (result.serial == m_duckingRunningSerial) m_generatingDucking = false;
    if (m_shuttingDown || result.generation != m_sessionGeneration) { emit duckingChanged(); return; }
    if (result.serial != m_duckingSerial || result.result.cancelled || m_cancelDucking.load(std::memory_order_relaxed)) {
        m_duckingReady = false; m_duckingMessage = tr("Ducking analysis cancelled.");
    } else {
        m_ducking = std::move(result);
        m_duckingReady = m_ducking.result.error.empty();
        consumePluginNotifications();
        m_duckingStale = !analysisCurrent(m_ducking.generation, m_ducking.revision, m_ducking.analysisEpoch);
        if (!m_duckingReady) { m_duckingMessage = QString::fromStdString(m_ducking.result.error); setError(m_duckingMessage); }
        else {
            m_duckingProgress = 1;
            m_duckingMessage = m_duckingStale ? tr("The mix changed during analysis. Analyse again.")
                : !m_ducking.result.changed ? tr("No level changes are needed. The existing automation is kept.")
                : tr("Preview ready. Apply installs editable volume points in one Undo step.");
        }
    }
    emit duckingChanged();
}

bool Backend::applyGeneratedDucking(const QString& token, bool keepUpdated)
{
    consumePluginNotifications();
    const auto preview = duckingPreview();
    if (token.isEmpty() || token != preview.value("token").toString() || !preview.value("valid").toBool()) {
        setError(tr("This ducking preview is no longer current. Analyse again before applying.")); return false;
    }
    if (m_shuttingDown || recording() || loading() || sampleEditing() || storageBusy() || m_repairing
        || m_clipMove || m_busy.load(std::memory_order_acquire)) {
        setError(tr("Finish recording or the current background edit before applying ducking.")); return false;
    }
    if (const auto deferred = deferAutomationEdit([this, token, keepUpdated] { return applyGeneratedDucking(token, keepUpdated); }, {}, true))
        return *deferred;
    auto* track = m_project.track(m_ducking.track);
    if (!track || track->volumeAutomation != m_ducking.replaces
        || !analysisCurrent(m_ducking.generation, m_ducking.revision, m_ducking.analysisEpoch)) {
        setError(tr("The volume automation changed. Analyse again before applying.")); return false;
    }
    if (m_project.automationPointCount() - track->volumeAutomation.points.size() + m_ducking.result.curve.points.size()
        > session::kMaxProjectAutomationPoints) {
        setError(tr("The project has reached its automation point limit.")); return false;
    }
    setError({});
    std::optional<session::DuckingRecipe> recipe;
    if (keepUpdated) {
        const auto& s = m_ducking.settings;
        recipe = session::DuckingRecipe{.triggers = m_ducking.triggers, .start = s.start, .end = s.end,
            .thresholdDb = s.thresholdDb, .reductionDb = s.reductionDb, .attackSeconds = s.attackSeconds,
            .holdSeconds = s.holdSeconds, .releaseSeconds = s.releaseSeconds, .replaceInRange = s.replaceInRange,
            .baseline = m_ducking.baseline, .generated = m_ducking.result.curve};
    }
    if (track->volumeAutomation != m_ducking.result.curve || track->ducking != recipe) {
        const auto before = captureEditState();
        track->volumeAutomation = m_ducking.result.curve;
        track->ducking = recipe;
        if (recipe) if (auto inputs = duckingInputs(m_project, *recipe)) m_duckingInputs[m_ducking.track] = std::move(*inputs);
        commit(keepUpdated ? "generate ducking (kept up to date)" : "generate ducking", m_ducking.track, before);
        if (!m_lastError.isEmpty()) return false;
        m_duckingMessage = keepUpdated
            ? tr("Ducking applied and kept up to date: it is generated again when the trigger tracks change. Editing its points by hand stops that.")
            : tr("Ducking applied. Edit the volume points in Automation; Undo restores the previous curve.");
    } else m_duckingMessage = tr("No level changes were needed.");
    m_duckingReady = false; m_duckingStale = true;
    emit duckingChanged();
    return true;
}

// --- generated ducking that follows its triggers -----------------------------------

std::optional<std::vector<session::TrackState>> Backend::duckingInputs(
    const session::Project& project, const session::DuckingRecipe& recipe) const
{
    // Everything that can change a trigger's sound: its own clips, effects,
    // fader and automation, and every track feeding it.
    std::vector<session::TrackState> inputs;
    for (const auto trigger : recipe.triggers) {
        std::string error;
        const auto isolated = render::isolateTrackOutput(project, trigger, error);
        if (!isolated) return std::nullopt;
        auto state = session::captureState(*isolated);
        for (auto& track : state.tracks) { track.name.clear(); track.colour.clear(); track.ducking.reset(); }
        inputs.insert(inputs.end(), state.tracks.begin(), state.tracks.end());
    }
    return inputs;
}

void Backend::scheduleDuckingFollow()
{
    if (m_shuttingDown || std::none_of(m_project.tracks().begin(), m_project.tracks().end(),
                                       [](const session::Track& t) { return t.ducking.has_value(); })) return;
    if (!m_duckingFollowTimer) {
        m_duckingFollowTimer = new QTimer(this);
        m_duckingFollowTimer->setSingleShot(true);
        m_duckingFollowTimer->setInterval(300);
        connect(m_duckingFollowTimer, &QTimer::timeout, this, &Backend::followGeneratedDucking);
    }
    m_duckingFollowTimer->start();
}

void Backend::rememberDuckingInputs()
{
    m_duckingInputs.clear();
    for (const auto& track : m_project.tracks())
        if (track.ducking) if (auto inputs = duckingInputs(m_project, *track.ducking)) m_duckingInputs[track.id] = std::move(*inputs);
}

void Backend::followGeneratedDucking()
{
    if (m_shuttingDown) return;
    if (m_followingDucking || m_generatingDucking || recording() || automationWriting() || loading() || m_saving
        || m_scanning || m_busy.load(std::memory_order_acquire) || sampleEditing() || storageBusy()
        || m_repairing || m_clipMove) {
        scheduleDuckingFollow();
        return;
    }
    for (const auto& track : m_project.tracks()) {
        if (!track.ducking) continue;
        const auto& recipe = *track.ducking;
        // Points edited by hand are the person's; the recipe no longer follows.
        if (track.volumeAutomation != recipe.generated) continue;
        auto inputs = duckingInputs(m_project, recipe);
        if (!inputs) continue;
        auto known = m_duckingInputs.find(track.id);
        if (known == m_duckingInputs.end()) { m_duckingInputs[track.id] = std::move(*inputs); continue; }
        if (known->second == *inputs) continue;
        if (recipe.end > m_project.length()) continue; // the range must still exist
        DuckingJob job;
        job.follow = true;
        job.track = track.id;
        job.triggers = recipe.triggers;
        job.settings = {.start = recipe.start, .end = recipe.end, .thresholdDb = recipe.thresholdDb,
            .reductionDb = recipe.reductionDb, .attackSeconds = recipe.attackSeconds,
            .holdSeconds = recipe.holdSeconds, .releaseSeconds = recipe.releaseSeconds,
            .replaceInRange = recipe.replaceInRange};
        job.baseline = recipe.baseline;
        job.inputs = std::move(*inputs);
        job.undoVersion = m_undo.version();
        joinWorker();
        m_followingDucking = true;
        m_cancelDucking.store(false, std::memory_order_relaxed);
        m_busy.store(true, std::memory_order_release);
        const auto generation = m_sessionGeneration;
        requestTimelineSnapshot([this, generation] {
            return m_shuttingDown || generation != m_sessionGeneration || m_cancelDucking.load(std::memory_order_relaxed);
        }, [this, job = std::move(job)](DeliverySnapshot snapshot, QString error, bool cancelled) mutable {
            startCapturedDucking(std::move(snapshot), std::move(job), std::move(error), cancelled);
        });
        return; // one at a time; the next check follows its completion
    }
}

void Backend::finishFollowedDucking(DuckingJob job)
{
    m_followingDucking = false;
    if (m_shuttingDown || job.generation != m_sessionGeneration) return;
    const auto again = qScopeGuard([this] { scheduleDuckingFollow(); });
    if (job.result.cancelled) return;
    auto* track = m_project.track(job.track);
    if (!track || !track->ducking || track->ducking->triggers != job.triggers
        || track->volumeAutomation != track->ducking->generated || track->ducking->baseline != job.baseline) return;
    if (!job.result.error.empty()) {
        m_duckingMessage = tr("Generated ducking could not follow the trigger change: %1").arg(QString::fromStdString(job.result.error));
        emit duckingChanged();
        return;
    }
    // A later edit changed the mix again: analyse that instead.
    if (!analysisCurrent(job.generation, job.revision, job.analysisEpoch)) return;
    if (m_project.automationPointCount() - track->volumeAutomation.points.size() + job.result.curve.points.size()
        > session::kMaxProjectAutomationPoints) {
        m_duckingMessage = tr("Generated ducking could not follow: the project has reached its automation point limit.");
        emit duckingChanged();
        return;
    }
    m_duckingInputs[job.track] = job.inputs;
    if (job.result.curve == track->volumeAutomation) return;
    const auto before = captureEditState();
    track->volumeAutomation = job.result.curve;
    track->ducking->generated = job.result.curve;
    // Part of the edit that changed the triggers, when that is still the newest
    // step: one Undo restores both. Otherwise it is its own step.
    if (m_undo.version() == job.undoVersion && m_undo.amendLatest(session::captureState(m_project))) {
        m_dirty = true;
        armAutosave();
        m_hostEditRevision = m_confirmedEditRevision = ++m_editRevision;
        publishPlan();
        publishTrackParams();
        rebuildTracks();
        emit timelineChanged();
    } else {
        commit("update generated ducking", job.track, before);
    }
    m_duckingMessage = tr("Generated ducking followed the change to its trigger tracks.");
    emit duckingChanged();
}

QVariantMap Backend::generatedDuckingState(int track) const
{
    const auto* target = m_project.track(session::TrackId(track));
    if (!target || !target->ducking) return {{"stored", false}, {"following", false}};
    QVariantList triggers;
    for (const auto id : target->ducking->triggers) triggers.append(int(id));
    const bool following = target->volumeAutomation == target->ducking->generated;
    const auto& r = *target->ducking;
    return {{"stored", true}, {"following", following}, {"triggers", triggers},
        {"start", qint64(r.start)}, {"end", qint64(r.end)},
        {"thresholdDb", r.thresholdDb}, {"reductionDb", r.reductionDb}, {"attackMs", r.attackSeconds * 1000},
        {"holdMs", r.holdSeconds * 1000}, {"releaseMs", r.releaseSeconds * 1000}, {"replaceExisting", r.replaceInRange},
        {"reason", following ? tr("Generated again when the trigger tracks change.")
                             : tr("The points were edited by hand, so they are no longer generated again.")}};
}

bool Backend::stopFollowingDucking(int track)
{
    auto* target = m_project.track(session::TrackId(track));
    if (!target || !target->ducking) { setError(tr("This track has no generated ducking that follows its triggers.")); return false; }
    if (const auto deferred = deferAutomationEdit([this, track] { return stopFollowingDucking(track); })) return *deferred;
    const auto before = captureEditState();
    m_project.track(session::TrackId(track))->ducking.reset();
    m_duckingInputs.erase(session::TrackId(track));
    commit("stop following ducking", session::TrackId(track), before);
    emit duckingChanged();
    return m_lastError.isEmpty();
}

bool Backend::removeGeneratedDucking(int track)
{
    auto* target = m_project.track(session::TrackId(track));
    if (!target || !target->ducking) { setError(tr("This track has no generated ducking that follows its triggers.")); return false; }
    if (target->volumeAutomation != target->ducking->generated) {
        setError(tr("The ducking points were edited by hand. Remove them in Automation instead.")); return false;
    }
    if (const auto deferred = deferAutomationEdit([this, track] { return removeGeneratedDucking(track); })) return *deferred;
    target = m_project.track(session::TrackId(track));
    if (!target || !target->ducking || target->volumeAutomation != target->ducking->generated) {
        setError(tr("The ducking changed. Try again.")); return false;
    }
    const auto before = captureEditState();
    target->volumeAutomation = target->ducking->baseline;
    target->ducking.reset();
    m_duckingInputs.erase(session::TrackId(track));
    commit("remove generated ducking", session::TrackId(track), before);
    emit duckingChanged();
    return m_lastError.isEmpty();
}
