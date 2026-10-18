#include "backend.h"
#include "session/effect_racks.h"

#include <QScopeGuard>
#include <algorithm>
#include <exception>
#include <iterator>

bool Backend::deliveryBoundaryActive() const
{
    return m_pumpingDeliverySnapshots || !m_deliverySnapshots.empty();
}

bool Backend::hasPendingTimelineSnapshot() const
{
    return m_pumpingDeliverySnapshots
        || std::any_of(m_deliverySnapshots.begin(), m_deliverySnapshots.end(), [this](const auto& snapshot) {
            return snapshot.automationRun && snapshot.generation == m_sessionGeneration;
        });
}

void Backend::verifyLoadRevision(std::uint64_t generation, std::uint64_t revision,
                                 std::function<void(QString)> complete)
{
    const auto changed = tr("The session was edited while the file was opening; your edits were kept. Open the file again to replace it.");
    // Ordinary polling pauses during loading. Consume callback output before
    // deciding whether the outgoing session still matches the accepted request.
    consumePluginNotifications();
    if (m_confirmedEditRevision > revision) { complete(changed); return; }
    const bool flush = std::any_of(m_chains.begin(), m_chains.end(), [](const auto& entry) {
        return entry.second && entry.second->parameterFlushRequested();
    });
    if (!flush && (m_editRevision <= revision || !hasUnverifiedPluginState())) {
        // Older state was already covered by the user's replacement decision;
        // later observations that have verified equal are not new edits.
        complete({});
        return;
    }
    requestDeliverySnapshot({}, true, [this, generation] {
        return m_shuttingDown || generation != m_sessionGeneration || m_cancelLoad.load(std::memory_order_relaxed);
    }, [this, revision, changed, complete = std::move(complete)](DeliverySnapshot, QString error, bool cancelled) {
        if (!cancelled && error.isEmpty()) {
            // The handoff has been released. Any newer callback output must
            // remain conservative, including a readout not covered by capture.
            consumePluginNotifications();
            if (m_confirmedEditRevision > revision || hasUnverifiedPluginState()) error = changed;
        }
        complete(std::move(error));
    }, true);
}

void Backend::requestDeliverySnapshot(std::optional<session::EffectOwner> selectedChain, bool includeBypassed,
                                     std::function<bool()> cancelled,
                                     std::function<void(DeliverySnapshot, QString, bool)> complete,
                                     bool includeInputControls, bool finishAutomation, std::function<void()> owned, bool timeline)
{
    const bool ownerAvailable = !selectedChain || session::findEffectRack(m_project, *selectedChain);
    if (ownerAvailable) pollPluginNotifications();
    DeliverySnapshot request{.project = m_project, .view = currentView(),
        .generation = m_sessionGeneration, .revision = m_editRevision,
        .analysisEpoch = m_analysisEpoch,
        .automationRun = timeline && m_automationMode != "read" ? m_automationRunToken : 0,
        .manualAutomation = m_automationPass ? m_automationPass->manualPending : std::nullopt,
        .chains = {}, .includeBypassed = includeBypassed, .includeInputControls = includeInputControls,
        .finishAutomation = finishAutomation, .owned = std::move(owned), .commandsBefore = {},
        .cancelled = std::move(cancelled), .complete = std::move(complete)};
    if (!ownerAvailable) {
        auto done = std::move(request.complete);
        done(std::move(request), tr("The requested effect owner is unavailable."), false);
        return;
    }
    bool hasState = false;
    for (const auto& [key, chain] : m_chains) {
        if (!chain || (selectedChain && key != *selectedChain)) continue;
        const auto* inserts = session::findEffectRack(request.project, key);
        if (!inserts) continue;
        request.chains.push_back({key, chain});
        for (std::size_t slot = 0; slot < inserts->size(); ++slot)
            if (includeBypassed || !(*inserts)[slot].bypass) hasState = hasState || chain->hasOpaqueState(slot);
    }
    // Ordinary model-only jobs can begin immediately. A recording lane still
    // needs the final callback even if it has no opaque plug-in state.
    if ((!hasState && !request.automationRun && !finishAutomation) || recording()) {
        auto done = std::move(request.complete);
        const bool cancelledNow = request.cancelled && request.cancelled();
        done(std::move(request), hasState ? tr("Stop recording before capturing live effect settings for this job.") : QString{}, cancelledNow);
        return;
    }
    request.commandsBefore = std::move(m_pendingCommands);
    m_pendingCommands.clear();
    m_deliverySnapshots.push_back(std::move(request));
    if (m_commandRetryTimer) m_commandRetryTimer->stop();
    if (!m_deliveryAccess) {
        // A queued Save may already own the same handoff. Transfer its request;
        // a second ControlAccess would be invalid, not a second ownership lease.
        if (m_snapshotAccess) {
            m_deliveryAccess.emplace(std::move(*m_snapshotAccess));
            m_snapshotAccess.reset();
        } else m_deliveryAccess.emplace(m_transport.requestControl());
    }
    pumpDeliverySnapshots();
}

void Backend::requestTimelineSnapshot(std::function<bool()> cancelled,
                                     std::function<void(DeliverySnapshot, QString, bool)> complete,
                                     bool clipEffects)
{
    if (clipEffects) {
        // Every reader of this snapshot hears clip effects exactly as playback does.
        complete = [this, complete = std::move(complete)](DeliverySnapshot snapshot, QString error, bool stopped) mutable {
            if (stopped || !error.isEmpty()) { complete(std::move(snapshot), std::move(error), stopped); return; }
            awaitClipEffects(std::move(snapshot), std::move(complete));
        };
    }
    if ((m_automationFinishPending && m_automationFinishCancel) || m_automationCancelPending) {
        // This job belongs after the cancellation, including an asynchronous
        // native-state restore. Join the edit queue so later model changes stay
        // after this request, and report a failed restore instead of exporting
        // the retained pass. Copy callbacks so a non-deferred call can proceed.
        const auto retry = [this, cancelled, complete] {
            requestTimelineSnapshot(cancelled, complete, false); return true;
        };
        const auto abandoned = [this, cancelled, complete, rate = m_project.sampleRate(),
            generation = m_sessionGeneration, revision = m_editRevision, epoch = m_analysisEpoch] {
            DeliverySnapshot snapshot{.project = session::Project(rate), .view = {},
                .generation = generation, .revision = revision, .analysisEpoch = epoch,
                .manualAutomation = {}, .chains = {}, .owned = {}, .commandsBefore = {}, .cancelled = {}, .complete = {}};
            const bool stopped = generation != m_sessionGeneration || m_shuttingDown || (cancelled && cancelled());
            QString error;
            if (!stopped) {
                error = tr("The automation cancellation could not finish. This job was not started.");
                if (!lastError().isEmpty()) error += QLatin1Char(' ') + lastError();
            }
            complete(std::move(snapshot), std::move(error), stopped);
        };
        if (deferAutomationEdit(retry, abandoned)) return;
    }
    requestDeliverySnapshot({}, true, std::move(cancelled), std::move(complete), false, false, {}, true);
}

void Backend::pumpDeliverySnapshots()
{
    if (m_pumpingDeliverySnapshots || m_deliverySnapshots.empty()) return;
    m_pumpingDeliverySnapshots = true;
    struct Completed { DeliverySnapshot request; QString error; bool cancelled = false; };
    std::vector<Completed> completed;
    const auto finish = qScopeGuard([&] {
        if (m_deliverySnapshots.empty()) {
            m_deliveryAccess.reset();
            if (m_deliveryTimer) m_deliveryTimer->stop();
        } else {
            if (!m_deliveryTimer) {
                m_deliveryTimer = new QTimer(this);
                m_deliveryTimer->setSingleShot(true);
                m_deliveryTimer->setInterval(1);
                connect(m_deliveryTimer, &QTimer::timeout, this, &Backend::pumpDeliverySnapshots);
            }
            m_deliveryTimer->start();
        }
        m_pumpingDeliverySnapshots = false;
        flushPendingCommands();
        // No completion may reenter construction, enqueue a command or start a
        // worker while an old processor is being inspected above.
        for (auto& result : completed) {
            auto done = std::move(result.request.complete);
            done(std::move(result.request), std::move(result.error), result.cancelled);
        }
    });
    // Cancellation cannot wait for a stuck callback. Keep command order while
    // removing cancelled boundaries, then release ownership if none remain.
    while (!m_deliverySnapshots.empty()) {
        auto& first = m_deliverySnapshots.front();
        if (!first.cancelled || !first.cancelled()) break;
        auto request = std::move(first);
        m_deliverySnapshots.pop_front();
        auto& next = m_deliverySnapshots.empty() ? m_pendingCommands : m_deliverySnapshots.front().commandsBefore;
        request.commandsBefore.insert(request.commandsBefore.end(), next.begin(), next.end());
        next = std::move(request.commandsBefore);
        completed.push_back({std::move(request), {}, true});
    }
    if (m_deliverySnapshots.empty() || !m_deliveryAccess || !m_deliveryAccess->ready()) return;

    // One snapshot per event-loop turn bounds queue work. The callback remains
    // excluded between boundaries; commands are applied in their accepted order.
    auto request = std::move(m_deliverySnapshots.front());
    m_deliverySnapshots.pop_front();
    (void)m_deliveryAccess->drainCommands();
    for (const auto& command : request.commandsBefore) {
        if (!m_commands.push(command)) {
            (void)m_deliveryAccess->drainCommands();
            const bool pushed = m_commands.push(command);
            Q_ASSERT(pushed);
            (void)pushed;
        }
    }
    (void)m_deliveryAccess->drainCommands();
    request.commandsBefore.clear();
    QString error;
    const auto beforeRevision = m_editRevision;
    const auto beforeEpoch = m_analysisEpoch;
    try {
        for (auto& selected : request.chains) {
            auto edit = session::prepareEffectRackEdit(request.project, selected.key);
            if (!edit) {
                error = tr("The captured effect owner is unavailable.");
                break;
            }
            auto* inserts = &edit->inserts;
            for (std::size_t slot = 0; slot < inserts->size(); ++slot) {
                if ((!request.includeBypassed && (*inserts)[slot].bypass) || !selected.processor->hasOpaqueState(slot)) continue;
                std::string reason;
                if (!selected.processor->captureInsertState(slot, (*inserts)[slot], reason, request.includeInputControls)) {
                    error = tr("Could not capture plug-in state for %1 (insert %2): %3")
                        .arg(QString::fromStdString((*inserts)[slot].uri)).arg(slot + 1).arg(QString::fromStdString(reason));
                    break;
                }
                consumePluginNotifications();
                verifyReadoutState(selected.key, selected.processor, slot, (*inserts)[slot]);
            }
            if (!error.isEmpty()) break;
            std::string reason;
            if (!session::replaceEffectRack(request.project, *edit, reason)) {
                error = tr("Could not capture effect settings: %1").arg(QString::fromStdString(reason));
                break;
            }
        }
    } catch (const std::exception& exception) {
        error = tr("Could not capture effect settings: %1").arg(QString::fromUtf8(exception.what()));
    }
    consumePluginNotifications();
    if (error.isEmpty() && request.automationRun) captureTimelineAutomation(request, error);
    if (request.owned && error.isEmpty() && request.generation == m_sessionGeneration) request.owned();
    if (request.finishAutomation && error.isEmpty() && request.generation == m_sessionGeneration) {
        // This boundary owns the processor after its last real callback and the
        // state capture's parameter flush. Resolve future input before closing
        // its history entry. Subsequent model edits are still queued on the GUI.
        m_automationFinishOwned = true;
        const auto release = qScopeGuard([this] { m_automationFinishOwned = false; });
        if (capturePendingAutomation(error, request.manualAutomation)) finishAutomationRecording(m_automationFinishCancel);
    }
    if (request.generation == m_sessionGeneration && request.revision == beforeRevision
        && request.analysisEpoch == beforeEpoch) {
        request.revision = m_editRevision;
        request.analysisEpoch = m_analysisEpoch;
    }
    request.chains.clear(); // all processor ownership is released on the GUI thread
    completed.push_back({std::move(request), std::move(error), false});
}
