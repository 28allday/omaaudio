#include "backend.h"
#include "session/effect_racks.h"

#include <QScopeGuard>
#include <QDateTime>
#include <algorithm>

const std::vector<session::Insert>* Backend::chainInserts(session::EffectOwner key)
{
    return session::findEffectRack(m_project, key);
}

void Backend::establishPluginHistory(session::EffectOwner key, const std::shared_ptr<lv2::Chain>& chain)
{
    auto edit = session::prepareEffectRackEdit(m_project, key);
    if (!chain || !edit) { m_nativeHistory.erase(key); return; }
    auto runtime = preparePluginHistory(chain, edit->inserts, false);
    std::string error;
    if (!session::replaceEffectRack(m_project, *edit, error)) {
        setError(tr("Could not prepare effect history: %1").arg(QString::fromStdString(error)));
        return;
    }
    m_nativeHistory[key] = std::move(runtime);
}

std::shared_ptr<Backend::NativeChainHistory> Backend::preparePluginHistory(
    const std::shared_ptr<lv2::Chain>& chain, std::vector<session::Insert>& inserts, bool requireKnown)
{
    auto runtime = std::make_shared<NativeChainHistory>();
    runtime->processor = chain; runtime->generation = m_sessionGeneration;
    runtime->inserts.resize(inserts.size());
    // The candidate has warmed but has not been published to audio. Its initial
    // state belongs to this exact runtime, before any native gesture can arrive.
    for (std::size_t slot = 0; slot < inserts.size(); ++slot) {
        if (!chain->hasOpaqueState(slot)) continue;
        std::string reason;
        runtime->inserts[slot].baselineKnown = chain->captureInsertState(slot, inserts[slot], reason, true);
        if (runtime->inserts[slot].baselineKnown) {
            runtime->inserts[slot].verifiedState = inserts[slot].instanceState;
            runtime->inserts[slot].verifiedInputs = chain->capturedInputValues(slot);
        }
        if (!runtime->inserts[slot].baselineKnown) {
            setError(tr("Could not prepare Undo for this plug-in: %1").arg(QString::fromStdString(reason)));
            if (requireKnown) return {};
        }
    }
    (void)chain->consumeNotifications(); // initial capture output also predates publication
    return runtime;
}

void Backend::noteNativeParameter(session::EffectOwner key, std::size_t slot, std::size_t actual,
                                  const host::ParameterEvent& event)
{
    if (event.automationPass) {
        (void)recordNativeAutomation(key, slot, actual, event);
        return; // processing acknowledgements belong only to automation history
    }
    const auto found = m_nativeHistory.find(key);
    auto* inserts = chainInserts(key);
    if (found == m_nativeHistory.end() || !inserts || slot >= inserts->size()
        || slot >= found->second->inserts.size()) return;
    auto& state = found->second->inserts[slot];
    if (!state.before) state.before = (*inserts)[slot];
    const bool recorded = recordNativeAutomation(key, slot, actual, event);
    if (event.kind == host::ParameterEventKind::Value && !recorded) {
        // Value notifications own their edit even if a bounded earlier poll
        // already consumed the adapter's coalesced parameter-only flag.
        noteNativeState(key, slot);
        markDirty(true);
    }
    const bool groupBegin = event.kind == host::ParameterEventKind::GroupBegin;
    const bool groupEnd = event.kind == host::ParameterEventKind::GroupEnd;
    // UINT32_MAX is not a valid CLAP/VST3 parameter. Reserve it for the native
    // group, which keeps sequential parameter gestures in one history boundary.
    const auto gesture = std::pair{actual, groupBegin || groupEnd ? UINT32_MAX : event.id};
    if (event.kind == host::ParameterEventKind::Begin || groupBegin) {
        if (std::find(state.gestures.begin(), state.gestures.end(), gesture) == state.gestures.end())
            state.gestures.push_back(gesture);
    } else if (event.kind == host::ParameterEventKind::End || groupEnd) {
        std::erase(state.gestures, gesture);
        if (state.gestures.empty()) {
            if (state.dirty) ++found->second->revision;
            else state.before.reset(); // an empty gesture owns no earlier settings
        }
    }
}

void Backend::noteNativeState(session::EffectOwner key, std::size_t slot)
{
    if (!chainInserts(key)) return;
    // An independent settings edit must follow the automation pass in history.
    // Otherwise finishing the pass later includes this edit in its before/after
    // snapshots and Undo of automation can also discard the private settings.
    if (m_automationPass) {
        if (m_finishingAutomation) m_automationIndependentEdit = true;
        else finishAutomationRecording();
    }
    const auto found = m_nativeHistory.find(key);
    auto* inserts = chainInserts(key);
    if (found == m_nativeHistory.end() || !inserts || slot >= inserts->size()
        || slot >= found->second->inserts.size()) return;
    auto& state = found->second->inserts[slot];
    if (!state.before) state.before = (*inserts)[slot];
    state.dirty = true; ++state.revision; ++found->second->revision;
    state.nativeRevision = ++m_pluginSettingSequence;
}

void Backend::noteNativeOverflow(session::EffectOwner key, std::size_t slot)
{
    const auto found = m_nativeHistory.find(key);
    if (found == m_nativeHistory.end() || slot >= found->second->inserts.size()) return;
    // A missing End can never arrive later. Keep the last known baseline and
    // recover only after an exclusive capture succeeds; failure must retry.
    auto& state = found->second->inserts[slot];
    state.resyncPending = true;
    ++found->second->revision;
}

void Backend::noteNativeReadout(session::EffectOwner key, std::size_t slot)
{
    const auto found = m_nativeHistory.find(key);
    const auto chain = m_chains.find(key);
    auto* inserts = chainInserts(key);
    if (found == m_nativeHistory.end() || chain == m_chains.end() || !chain->second
        || !inserts || slot >= inserts->size() || slot >= found->second->inserts.size()
        || !chain->second->hasOpaqueState(slot)) return;
    auto& state = found->second->inserts[slot];
    if (!state.before) state.before = (*inserts)[slot];
    state.readoutPending = true;
    state.readoutRevision = ++m_editRevision;
    emit automationStampChanged();
    state.readoutNativeRevision = ++m_pluginSettingSequence;
    state.readoutAt = QDateTime::currentMSecsSinceEpoch();
    ++state.revision; ++found->second->revision;
    invalidateAnalysis(true);
    armAutosave();
}

bool Backend::hasUnverifiedPluginState() const
{
    for (const auto& [key, runtime] : m_nativeHistory) {
        (void)key;
        for (const auto& state : runtime->inserts) if (state.readoutPending) return true;
    }
    return false;
}

std::optional<std::vector<session::PluginState>> Backend::normalizeReadoutInputs(
    const session::Insert& captured, const std::vector<session::PluginState>& baseline,
    const lv2::Chain::InputValues& previous, const lv2::Chain::InputValues& current)
{
    // State blobs have no host-readable parameter layout. If known inputs moved,
    // reconstruct their effect on a PRIVATE candidate, never by loading old state
    // into the live processor. This also accounts for rendered automation.
    // Only an exact successful comparison can declare the readout unchanged.
    if (baseline.empty() || baseline.size() > 2 || previous.size() != baseline.size()
        || current.size() != baseline.size()) return {};
    try {
        auto expected = baseline;
        for (std::size_t actual = 0; actual < baseline.size(); ++actual) {
            if (previous[actual] == current[actual]) continue;
            if (previous[actual].size() != current[actual].size()) return {};
            session::Insert input;
            input.uri = captured.uri; input.legacyClapControls = captured.legacyClapControls;
            input.instanceState = {baseline[actual]};
            for (std::size_t port = 0; port < current[actual].size(); ++port) {
                const auto& before = previous[actual][port]; const auto& after = current[actual][port];
                if (before.first != after.first) return {};
                if (before.second != after.second)
                    input.controls.push_back({.port = after.first, .value = after.second});
            }
            lv2::Chain candidate(baseline.size() == 2 ? 1 : kOutputChannels, lv2::kMaxBlock);
            std::string error;
            if (!candidate.append(*m_lv2, hostFeatures(), input, error)) return {};
            std::vector<session::PluginState> state;
            if (!candidate.captureState(0, state, error) || state.size() != 1) return {};
            expected[actual] = std::move(state.front());
        }
        return expected;
    } catch (...) {
        // A comparison failure does not replace or mutate any live instance.
        // Retain a conservative real-state change; never discard uncertain data.
        return {};
    }
}

void Backend::verifyReadoutState(session::EffectOwner key, const std::shared_ptr<lv2::Chain>& chain,
                                  std::size_t slot, const session::Insert& captured)
{
    const auto found = m_nativeHistory.find(key);
    const auto* inserts = chainInserts(key);
    if (found == m_nativeHistory.end() || found->second->processor.lock() != chain
        || found->second->generation != m_sessionGeneration || !inserts
        || slot >= inserts->size() || slot >= found->second->inserts.size()
        || (*inserts)[slot].identity != captured.identity) return;
    const auto runtime = found->second;
    auto& state = runtime->inserts[slot];
    auto inputs = chain->capturedInputValues(slot);
    bool changed = !state.verifiedState || *state.verifiedState != captured.instanceState;
    if (changed && state.readoutPending && !state.dirty && state.verifiedState
        && state.verifiedInputs != inputs) {
        const auto normalized = normalizeReadoutInputs(captured, *state.verifiedState, state.verifiedInputs, inputs);
        if (normalized) changed = *normalized != captured.instanceState;
        const auto current = m_nativeHistory.find(key);
        if (runtime->generation != m_sessionGeneration || current == m_nativeHistory.end()
            || current->second != runtime) return;
    }
    if (state.readoutPending && changed) {
        if (m_automationPass) {
            if (m_finishingAutomation) m_automationIndependentEdit = true;
            else finishAutomationRecording();
        }
        state.dirty = true;
        state.nativeRevision = std::max(state.nativeRevision, state.readoutNativeRevision);
        m_confirmedEditRevision = std::max(m_confirmedEditRevision, state.readoutRevision);
        if (state.readoutRevision > m_savedRevision) {
            const bool wasDirty = m_dirty;
            m_dirty = true; armAutosave();
            if (!wasDirty) emit timelineChanged();
        }
    }
    state.verifiedState = captured.instanceState;
    state.verifiedInputs = std::move(inputs);
    const bool verifiedReadout = std::exchange(state.readoutPending, false);
    if (!state.dirty && state.gestures.empty()) state.before.reset();
    if (verifiedReadout) queueAnalysisValidityRefresh();
}

void Backend::prepareDocumentChange(qulonglong request)
{
    m_documentChangeRequest = request;
    const auto generation = m_sessionGeneration;
    requestDeliverySnapshot({}, true, [this, generation, request] {
        return m_shuttingDown || generation != m_sessionGeneration || request != m_documentChangeRequest;
    }, [this, request, generation](DeliverySnapshot, QString error, bool cancelled) {
        if (request != m_documentChangeRequest) return;
        if (!error.isEmpty()) setError(error);
        // A failed comparison cannot authorize silently dropping live state.
        // The existing prompt still offers an explicit Save/Discard/Cancel choice.
        emit documentChangePrepared(request, !cancelled && generation == m_sessionGeneration,
                                    m_dirty || !error.isEmpty() || hasUnverifiedPluginState());
    }, true);
}

void Backend::prepareSessionImport(ProjectLoadResult result)
{
    auto pending = std::make_shared<ProjectLoadResult>(std::move(result));
    const auto generation = pending->request.sessionGeneration;
    const auto runtimes = m_nativeHistory;
    std::unordered_map<session::EffectOwner, std::vector<std::uint64_t>> hostRevisions;
    for (const auto& [key, runtime] : runtimes)
        for (const auto& slot : runtime->inserts) hostRevisions[key].push_back(slot.hostRevision);
    requestDeliverySnapshot({}, true, [this, generation] {
        return m_shuttingDown || generation != m_sessionGeneration || m_cancelLoad.load(std::memory_order_relaxed);
    }, [this, pending, generation, runtimes, hostRevisions](DeliverySnapshot snapshot, QString error, bool cancelled) {
        for (const auto& [key, runtime] : runtimes) {
            if (cancelled || !error.isEmpty() || m_shuttingDown || generation != m_sessionGeneration
                || m_cancelLoad.load(std::memory_order_relaxed)) break;
            // Capture and each installation can deliver native gestures through
            // interface notifications. A newer pass must get its Undo entry
            // before any remaining settings baseline is consumed. The final
            // load boundary closes that pass and rejects the stale import;
            // ordinary history polling then installs the retained settings.
            consumePluginNotifications();
            if (generation != m_sessionGeneration || m_cancelLoad.load(std::memory_order_relaxed)
                || pending->request.hostRevision != m_hostEditRevision
                || m_automationPass || m_automationFinishPending) break;
            installNativeHistory(key, runtime, snapshot, hostRevisions.at(key), true);
        }
        finishProjectLoad(std::move(*pending), true, std::move(error));
        pumpWorkerJobs();
    }, true);
}

void Backend::pumpNativeHistory()
{
    if (m_shuttingDown || m_loading || recording() || deliveryBoundaryActive() || m_pluginEditCapturing
        || m_automationFinishPending || (m_automationFinishFailed && m_automationPass)) return;
    // A settings notification can request finalization while its consumer is
    // still draining. Its deferred capture must close automation history first.
    // A failed finish retains that boundary until an explicit retry/cancel; a
    // successful capture on another rack must not put settings history ahead
    // of it or restart the failing capture from every background poll.
    // Callbacks may install state and notify QML. Iterate a stable list rather
    // than retaining map iterators across synchronous snapshot completion.
    std::vector<std::pair<session::EffectOwner, std::shared_ptr<NativeChainHistory>>> pending;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    for (const auto& [key, runtime] : m_nativeHistory) {
        // New notifications still update the pending state, but must not bypass
        // a failed capture's backoff. Otherwise an active plug-in can force a
        // failing exclusive capture on every poll. Explicit Save/rack boundaries
        // have their own snapshot requests and remain available during this wait.
        if (runtime->capturing || now < runtime->retryAt
            || (runtime->attempted == runtime->revision && runtime->retryAt == 0)) continue;
        const bool ready = std::any_of(runtime->inserts.begin(), runtime->inserts.end(), [this, now](const auto& slot) {
            // Coalesce continuous readouts while playing or monitoring. Quiet
            // stopped values get one comparison; explicit boundaries and the
            // ordinary autosave cadence verify continuous persistent changes.
            return slot.resyncPending || (slot.dirty && slot.gestures.empty())
                || (slot.readoutPending && !playing() && now - slot.readoutAt >= 250);
        });
        if (ready) pending.emplace_back(key, runtime);
    }
    for (const auto& [key, runtime] : pending) {
        // An earlier synchronous capture in this list may have requested (or
        // failed) finalization after the list was collected.
        if (m_automationFinishPending || (m_automationFinishFailed && m_automationPass)) break;
        runtime->capturing = true; runtime->attempted = runtime->revision;
        std::vector<std::uint64_t> hostRevisions;
        for (const auto& slot : runtime->inserts) hostRevisions.push_back(slot.hostRevision);
        requestDeliverySnapshot(key, true,
            [this, key, runtime] {
                const auto current = m_chains.find(key);
                return m_shuttingDown || runtime->generation != m_sessionGeneration
                    || current == m_chains.end() || current->second != runtime->processor.lock();
            },
            [this, key, runtime, hostRevisions](DeliverySnapshot snapshot, QString error, bool cancelled) {
                runtime->capturing = false;
                if (!cancelled && error.isEmpty()) {
                    runtime->failures = 0; runtime->retryAt = 0;
                    // Only a settings installation needs a history boundary.
                    // An unchanged meter comparison must leave a newer pass
                    // running, even when it began after capture was accepted.
                    const bool installsSettings = std::any_of(runtime->inserts.begin(), runtime->inserts.end(), [](const auto& slot) {
                        return slot.resyncPending || (slot.dirty && slot.gestures.empty());
                    });
                    if (installsSettings && m_automationPass && !m_automationFinishPending && !m_automationFinishFailed)
                        finishAutomationRecording();
                    if (m_automationFinishPending || (m_automationFinishFailed && m_automationPass)) {
                        // Verifying a persistent readout can end an automation
                        // pass from inside this capture. Keep its old baseline
                        // until that pass has its Undo entry, then recapture.
                        runtime->attempted = 0;
                        return;
                    }
                    installNativeHistory(key, runtime, snapshot, hostRevisions);
                } else if (!cancelled) {
                    runtime->retryAt = QDateTime::currentMSecsSinceEpoch() + (250 << std::min(runtime->failures, 4U));
                    runtime->failures = std::min(runtime->failures + 1, 4U);
                    setError(error);
                }
            }, true);
    }
}

void Backend::installNativeHistory(session::EffectOwner key, const std::shared_ptr<NativeChainHistory>& runtime,
                                   const DeliverySnapshot& snapshot, const std::vector<std::uint64_t>& hostRevisions,
                                   bool finishGesture)
{
    const auto current = m_chains.find(key);
    const auto* liveInserts = chainInserts(key);
    const auto* captured = session::findEffectRack(snapshot.project, key);
    if (snapshot.generation != m_sessionGeneration || current == m_chains.end()
        || current->second != runtime->processor.lock() || !liveInserts || !captured) return;
    auto before = session::captureState(m_project);
    auto edit = session::prepareEffectRackEdit(m_project, key);
    auto beforeEdit = session::prepareEffectRackEdit(before, key);
    if (!edit || !beforeEdit) return;
    auto* inserts = &edit->inserts;
    auto* beforeInserts = &beforeEdit->inserts;
    // Prepare history/cache updates alongside the model. A refused owner
    // replacement must leave notifications pending for a later valid capture.
    auto preparedHistory = runtime->inserts;
    const auto cached = m_chainSpecs.find(key);
    auto preparedCache = cached == m_chainSpecs.end()
        ? std::optional<std::vector<session::Insert>>{}
        : std::optional{cached->second};
    // Only effect settings participate in this transaction. Name, clip, marker,
    // routing and automation edits made during the gesture stay in BOTH sides.
    const auto settings = [](session::Insert& destination, const session::Insert& source) {
        const auto live = destination.controls;
        destination.instanceState = source.instanceState;
        destination.controls = source.controls;
        destination.settingsRevision = source.settingsRevision;
        for (const auto& control : live) {
            auto stored = std::find_if(destination.controls.begin(), destination.controls.end(), [&](const auto& item) {
                return item.port == control.port;
            });
            if (stored != destination.controls.end()) {
                stored->automation = control.automation;
                stored->instanceAutomation = control.instanceAutomation;
                if (control.hasAutomationData()) {
                    stored->value = control.value; stored->revision = std::max(stored->revision, control.revision);
                }
            } else if (control.hasAutomationData()) destination.controls.push_back(control);
        }
        for (const auto& control : destination.controls)
            destination.settingsRevision = std::max(destination.settingsRevision, control.revision);
    };
    bool changed = false, nativeEdit = false, recoveredEdit = false;
    for (std::size_t slot = 0; slot < inserts->size() && slot < runtime->inserts.size() && slot < captured->size(); ++slot) {
        auto& state = preparedHistory[slot];
        if ((!finishGesture && !state.resyncPending && (!state.dirty || !state.gestures.empty()))
            || (state.before && (*inserts)[slot].identity != state.before->identity)
            || (*inserts)[slot].identity != (*captured)[slot].identity) continue;
        const bool undoable = state.dirty && state.before && state.baselineKnown;
        recoveredEdit = recoveredEdit || (undoable && state.resyncPending);
        if (undoable) settings((*beforeInserts)[slot], *state.before);
        settings((*inserts)[slot], (*captured)[slot]);
        if (state.dirty) {
            (*inserts)[slot].settingsRevision = std::max((*inserts)[slot].settingsRevision, state.nativeRevision);
            for (auto& control : (*inserts)[slot].controls) control.revision = std::max(control.revision, state.nativeRevision);
        }
        // Commands accepted after this capture boundary have not run in the
        // captured processor. Preserve them as explicit model overrides, even
        // if their representative value happens to equal the captured value.
        for (const auto& control : state.hostControls)
            if (slot < hostRevisions.size() && control.revision > hostRevisions[slot])
                (*inserts)[slot].setControl(control.port, control.value, control.settingsRevision);
        if (!undoable) settings((*beforeInserts)[slot], (*inserts)[slot]);
        if (preparedCache && slot < preparedCache->size())
            settings((*preparedCache)[slot], (*inserts)[slot]);
        state.before.reset(); state.dirty = false; state.baselineKnown = true;
        if (finishGesture || state.resyncPending) state.gestures.clear();
        state.resyncPending = false;
        changed = true; nativeEdit = nativeEdit || undoable;
    }
    if (!changed) { runtime->attempted = runtime->revision; return; }
    std::string error;
    if (!session::replaceEffectRack(before, *beforeEdit, error)
        || !session::replaceEffectRack(m_project, *edit, error)) {
        setError(tr("Could not install captured effect history: %1").arg(QString::fromStdString(error)));
        return;
    }
    runtime->inserts = std::move(preparedHistory);
    runtime->attempted = runtime->revision;
    if (preparedCache) cached->second = std::move(*preparedCache);
    const auto historyVersion = m_undo.version();
    if (nativeEdit) (void)m_undo.push(recoveredEdit ? "recover plug-in settings" : "change plug-in settings",
                      key.trackId(),
                      std::move(before), session::captureState(m_project));
    advanceQueuedHistory(historyVersion);
    // The native notification already marked this edit dirty/revised. Recording
    // its coherent model representation must not turn a matching Save dirty again.
    refreshPluginValues(); rebuildTracks(false); emit timelineChanged();
}

int Backend::pluginSlot(session::EffectOwner key, const session::InsertIdentity& identity)
{
    const auto* inserts = chainInserts(key);
    if (inserts) for (std::size_t slot = 0; slot < inserts->size(); ++slot)
        if ((*inserts)[slot].identity == identity) return static_cast<int>(slot);
    return -1;
}

void Backend::advanceQueuedHistory(std::uint64_t from)
{
    for (auto& edit : m_pluginEdits)
        if (edit.historyVersion && *edit.historyVersion == from) edit.historyVersion = m_undo.version();
}

std::optional<bool> Backend::deferPluginEdit(std::function<bool()> edit, std::function<void()> abandoned,
                                          bool protectHistory, std::function<void()> owned)
{
    if (m_applyingPluginEdit) return {};
    if (!m_finishingAutomation && !m_automationFinishOwned) {
        // Native restarts and internal callers need the same history boundary
        // as GUI rack edits. Cancellation's owned restore is already part of
        // finalization and must not wait behind itself.
        const auto apply = [this, edit, abandoned, protectHistory, owned] {
            const auto deferred = deferPluginEdit(edit, abandoned, protectHistory, owned);
            return deferred ? *deferred : edit();
        };
        if (const auto deferred = deferAutomationEdit(apply, abandoned)) return *deferred;
    }
    bool hasState = false;
    for (const auto& [key, chain] : m_chains) {
        const auto* inserts = chainInserts(key);
        if (chain && inserts) for (std::size_t slot = 0; slot < inserts->size(); ++slot)
            hasState = hasState || chain->hasOpaqueState(slot);
    }
    if (!hasState && !m_pluginEditCapturing && m_pluginEdits.empty()) return {};
    if (m_shuttingDown || m_loading || recording()) {
        setError(tr("Finish recording or loading before changing track effects or their history."));
        if (abandoned) abandoned();
        return false;
    }
    auto result = std::make_shared<std::optional<bool>>();
    m_pluginEdits.push_back({[edit = std::move(edit), result] { *result = edit(); return **result; },
                            std::move(abandoned), protectHistory ? std::optional(m_undo.version()) : std::nullopt,
                            std::move(owned)});
    pumpPluginEdits();
    // A pending operation has been accepted, not yet applied. Existing stopped
    // and between-block calls still return their synchronous result.
    return result->value_or(true);
}

void Backend::cancelPluginEdits()
{
    ++m_pluginEditSequence;
    auto edits = std::move(m_pluginEdits); m_pluginEdits.clear();
    for (auto& edit : edits) if (edit.abandoned) edit.abandoned();
    pumpDeliverySnapshots();
}

void Backend::pumpPluginEdits()
{
    if (m_pluginEditCapturing || m_pluginEdits.empty() || m_shuttingDown || m_loading
        || m_automationFinishPending) return;
    if (m_automationFinishFailed && m_automationPass) {
        // An older rack capture may have found a newer pass at completion.
        // Its finalization failed; discard accepted dependent edits rather
        // than silently replaying them on a later retry.
        cancelPluginEdits();
        setError(lastError() + tr(" Pending effect edits were not applied."));
        return;
    }
    m_pluginEditCapturing = true;
    const auto generation = m_sessionGeneration;
    const auto sequence = m_pluginEditSequence;
    const auto owned = m_pluginEdits.front().owned;
    const auto runtimes = m_nativeHistory;
    std::unordered_map<session::EffectOwner, std::vector<std::uint64_t>> hostRevisions;
    for (const auto& [key, runtime] : runtimes)
        for (const auto& slot : runtime->inserts) hostRevisions[key].push_back(slot.hostRevision);
    requestDeliverySnapshot({}, true, [this, generation, sequence] {
        return m_shuttingDown || generation != m_sessionGeneration || sequence != m_pluginEditSequence;
    }, [this, generation, sequence, runtimes, hostRevisions](DeliverySnapshot snapshot, QString error, bool cancelled) {
        const auto finished = qScopeGuard([this] {
            m_pluginEditCapturing = false;
            if (!m_pluginEdits.empty()) QTimer::singleShot(0, this, &Backend::pumpPluginEdits);
            else if (m_automationEditQueueActive) QTimer::singleShot(0, this, &Backend::applyDeferredAutomationEdits);
        });
        if (cancelled || generation != m_sessionGeneration || sequence != m_pluginEditSequence || m_pluginEdits.empty()) return;
        if (!error.isEmpty()) {
            auto edit = std::move(m_pluginEdits.front()); m_pluginEdits.pop_front();
            setError(error);
            if (edit.abandoned) edit.abandoned();
            return;
        }
        const auto waitingForAutomation = [this, generation, sequence] {
            const auto obsolete = [this, generation, sequence] {
                return generation != m_sessionGeneration || sequence != m_pluginEditSequence || m_pluginEdits.empty();
            };
            if (obsolete()) return true;
            // A signal handler can enqueue a native gesture without polling
            // it. Claim that input before deciding that no pass needs closing.
            consumePluginNotifications();
            if (obsolete()) return true;
            if (m_automationPass && !m_automationFinishPending && !m_automationFinishFailed)
                finishAutomationRecording();
            return obsolete() || m_automationFinishPending || (m_automationFinishFailed && m_automationPass);
        };
        // Arming and native gestures can occur after request acceptance. Keep
        // the original edit (including its history version) in the queue while
        // this newer pass closes, then recapture before replay. Installation
        // notifies QML, so recheck between racks and before applying the edit.
        for (const auto& [key, runtime] : runtimes) {
            if (waitingForAutomation()) return;
            installNativeHistory(key, runtime, snapshot, hostRevisions.at(key), true);
        }
        if (waitingForAutomation()) return;
        auto edit = std::move(m_pluginEdits.front()); m_pluginEdits.pop_front();
        const bool historyCurrent = !edit.historyVersion || *edit.historyVersion == m_undo.version();
        if (!historyCurrent) {
            setError(tr("The edit history changed while waiting for the plug-in. Try the history action again."));
            if (edit.abandoned) edit.abandoned();
            return;
        }
        const auto historyVersion = m_undo.version();
        m_applyingPluginEdit = true;
        const auto applied = qScopeGuard([this] { m_applyingPluginEdit = false; });
        (void)edit.apply();
        advanceQueuedHistory(historyVersion);
    }, true, false, owned);
}
