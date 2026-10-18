#include "backend.h"
#include "session/effect_racks.h"
#include <QScopeGuard>
#include <algorithm>
#include <cmath>

using namespace omawave;

namespace {
struct FxAutomationTarget {
    std::uint32_t slot = 0, port = 0;
    std::size_t actual = SIZE_MAX;
};
std::optional<FxAutomationTarget> fxAutomationTarget(const QString& key)
{
    const auto fields = key.split(':');
    if ((fields.size() != 3 && fields.size() != 4) || fields[0] != "fx") return {};
    bool slotOk = false, portOk = false, actualOk = true;
    FxAutomationTarget target{fields[1].toUInt(&slotOk), fields[2].toUInt(&portOk)};
    if (fields.size() == 4) target.actual = fields[3].toUInt(&actualOk);
    if (!slotOk || !portOk || !actualOk || (fields.size() == 4 && target.actual > 1)) return {};
    return target;
}

bool automationEpochMatches(const host::ParameterTime& time, std::uint32_t published,
                            const std::optional<std::uint32_t>& pendingPlay)
{
    if (pendingPlay && time.playbackEpoch == *pendingPlay) return false;
    if (time.playbackEpoch == published) return true;
    // Play takes effect before the first processor runs, but the meter publishes
    // its epoch at callback completion. Process events already acknowledge that
    // queued Play; rejecting them here would permanently drain its first edits.
    // Main-thread controller timestamps still require the published epoch.
    return time.processSequence && pendingPlay
        && std::uint32_t(time.playbackEpoch - published) < 0x80000000u
        && std::uint32_t(time.playbackEpoch - *pendingPlay) < 0x80000000u;
}
}

QString Backend::automationLiveStatus() const
{
    if (m_automationMode == "read") return tr("Read — existing automation plays back.");
    if (m_automationSuspended) return tr("%1 armed — press Play to start another pass.").arg(m_automationMode);
    if (m_automationMode == "touch" && !m_automationTouch) return tr("Touch — hold the live control to write.");
    if (m_automationMode == "latch" && !m_automationLatched) return tr("Latch — touch the control; writing continues until Stop.");
    return m_automationPass ? tr("Writing %1 — Stop finishes one Undo step.").arg(m_automationDescriptor.value("label").toString())
                            : tr("%1 armed — press Play.").arg(m_automationMode);
}

void Backend::setAutomationReturnSeconds(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 5) {
        setError(tr("Touch return must be between 0 and 5 seconds."));
        return;
    }
    if (seconds == m_automationReturnSeconds) return;
    m_automationReturnSeconds = seconds;
    // The audio owner captures the duration on release. Publishing the policy
    // changes future returns without restarting a pass or an in-flight ramp.
    if (processOwnedAutomation() && !m_automationSuspended) publishAutomationPlan();
    emit automationReturnChanged();
}

session::AutomationCurve* Backend::liveAutomationCurve(bool create)
{
    const auto target = chainTarget(m_automationTrack);
    if (!target.inserts) return nullptr;
    auto* track = m_project.track(target.track);
    if (m_automationKey == "volume") return track ? &track->volumeAutomation : &m_project.masterVolumeAutomation();
    if (m_automationKey == "pan") return track ? &track->panAutomation : nullptr;
    const auto fx = fxAutomationTarget(m_automationKey);
    if (!fx || fx->slot >= target.inserts->size()) return nullptr;
    const auto slot = fx->slot, port = fx->port;
    auto& insert = (*target.inserts)[slot];
    if (create && !insert.control(port)) insert.setControl(port, m_automationDescriptor.value("base").toDouble());
    for (auto& control : insert.controls) if (control.port == port) {
        // An absent override inherits the shared curve. Callers must materialize
        // the selected instance only after validating the complete candidate.
        if (fx->actual != SIZE_MAX && control.instanceAutomation[fx->actual])
            return &control.instanceAutomation[fx->actual]->curve;
        return &control.automation;
    }
    return nullptr;
}

session::SessionState Backend::captureEditState()
{
    finishAutomationRecording();
    if (m_automationMode != "read") { m_automationMode = "read"; emit automationLiveChanged(); }
    return session::captureState(m_project);
}

std::optional<bool> Backend::deferAutomationEdit(std::function<bool()> edit, std::function<void()> abandoned, bool keepCurrentView)
{
    if (m_applyingAutomationEdit || m_applyingPluginEdit) return {};
    const bool pluginBoundary = m_pluginEditCapturing || !m_pluginEdits.empty();
    if (!m_automationFinishPending && !m_automationEditQueueActive && !m_automationCancelPending) {
        if (m_automationSuspended && !m_automationPass) return {};
        if (!processOwnedAutomation()) return {};
    }
    // These are command inputs, not references to the GUI at eventual execution.
    // Restore newer user navigation/clipboard changes after the accepted edit.
    struct View {
        session::TrackId focus, anchor;
        session::Selection selection;
        std::vector<session::TrackId> tracks;
        std::vector<session::ClipAddress> clips;
        bool operator==(const View&) const = default;
    };
    struct Clipboard {
        std::vector<session::Clip> clips;
        session::FramePos frames;
        int channel;
        std::optional<session::MultitrackClipboard> multitrack;
        bool operator==(const Clipboard&) const = default;
    };
    const auto view = [this] { return View{m_focused, m_trackSelectionAnchor, m_selection, m_selectedTracks, m_selectedClips}; };
    const auto clipboard = [this] { return Clipboard{m_clipboard, m_clipboardFrames, m_clipboardChannel, m_multitrackClipboard}; };
    const auto setView = [this](const View& value) {
        m_focused = value.focus; m_trackSelectionAnchor = value.anchor; m_selection = value.selection;
        m_selectedTracks = value.tracks; m_selectedClips = value.clips;
    };
    const auto setClipboard = [this](const Clipboard& value) {
        m_clipboard = value.clips; m_clipboardFrames = value.frames; m_clipboardChannel = value.channel;
        m_multitrackClipboard = value.multitrack;
    };
    auto result = std::make_shared<std::optional<bool>>();
    auto accepted = [this, edit = std::move(edit), result, view, clipboard, setView, setClipboard,
        acceptedView = view(), acceptedClipboard = clipboard(), selectionRevision = m_trackSelectionRevision,
        ripple = m_ripple, playhead = m_playhead, stamp = automationStamp(), generation = m_sessionGeneration, keepCurrentView] {
        if (generation != m_sessionGeneration) { *result = false; return false; }
        const auto liveView = view(); const auto liveClipboard = clipboard();
        const auto liveRevision = m_trackSelectionRevision;
        const auto liveRipple = m_ripple; const auto livePlayhead = m_playhead;
        const auto previousStamp = std::exchange(m_automationEditAcceptedStamp, stamp);
        const auto previousApplying = std::exchange(m_applyingAutomationEdit, true);
        const auto restore = qScopeGuard([&] {
            m_applyingAutomationEdit = previousApplying; m_automationEditAcceptedStamp = previousStamp;
            m_ripple = liveRipple;
            // A deferred New creates its own view and clipboard. Never restore
            // navigation or source references from the outgoing document.
            if (generation != m_sessionGeneration) return;
            m_playhead = livePlayhead;
            if (liveClipboard != acceptedClipboard) setClipboard(liveClipboard);
            if (!keepCurrentView && liveView != acceptedView) {
                setView(liveView);
                m_trackSelectionRevision = std::max(liveRevision, m_trackSelectionRevision) + 1;
                clampSelection(); rebuildTracks(); emit selectionChanged();
            } else m_trackSelectionRevision = std::max(liveRevision, m_trackSelectionRevision);
        });
        // Captured clip-address edits can remap the current navigation themselves.
        // Restoring its old descriptors afterwards would clear the user's selection.
        if (!keepCurrentView) { setView(acceptedView); m_trackSelectionRevision = selectionRevision; }
        setClipboard(acceptedClipboard); m_ripple = ripple; m_playhead = playhead;
        *result = edit(); return **result;
    };
    std::vector<session::SourceId> sources;
    for (const auto& clip : m_clipboard) sources.push_back(clip.source);
    if (m_multitrackClipboard) for (const auto& [id, uuid] : m_multitrackClipboard->sources) {
        (void)uuid; sources.push_back(id);
    }
    m_deferredAutomationEdits.push_back({std::move(accepted), [result, abandoned = std::move(abandoned)] {
        *result = false; if (abandoned) abandoned();
    }, std::move(sources)});
    const bool alreadyQueued = std::exchange(m_automationEditQueueActive, true);
    if (!m_automationFinishPending && !alreadyQueued && !pluginBoundary && !m_automationCancelPending) finishAutomationRecording();
    applyDeferredAutomationEdits();
    return result->value_or(true); // synchronous result, or accepted pending ownership
}

bool Backend::automationStampCurrent(const QString& stamp) const
{
    return stamp == automationStamp() || (m_applyingAutomationEdit && stamp == m_automationEditAcceptedStamp);
}

void Backend::applyDeferredAutomationEdits()
{
    if (m_applyingAutomationEdit || m_automationFinishOwned || m_finishingAutomation) return;
    while (!m_automationFinishPending && !m_automationCancelPending && !m_automationFinishFailed && !m_deferredAutomationEdits.empty()
           && !m_pluginEditCapturing && m_pluginEdits.empty()) {
        auto edit = std::move(m_deferredAutomationEdits.front());
        m_deferredAutomationEdits.pop_front();
        (void)edit.apply();
    }
    if (m_deferredAutomationEdits.empty() && !m_pluginEditCapturing && m_pluginEdits.empty())
        m_automationEditQueueActive = false;
}

void Backend::discardDeferredAutomationEdits()
{
    auto edits = std::move(m_deferredAutomationEdits);
    m_deferredAutomationEdits.clear(); m_automationEditQueueActive = false;
    for (auto& edit : edits) if (edit.abandoned) edit.abandoned();
}

void Backend::requestAutomationFinish(bool cancel)
{
    if (m_automationFinishPending) return;
    m_automationFinishPending = true; m_automationFinishFailed = false;
    m_automationFinishCancel = cancel;
    const auto generation = m_sessionGeneration;
    const auto capture = [this, generation] {
        if (generation != m_sessionGeneration || !m_automationFinishPending) return;
        requestDeliverySnapshot({}, true, [this, generation] {
            return generation != m_sessionGeneration;
        }, [this, generation](DeliverySnapshot, QString error, bool cancelled) {
            // An old session's completion must not clear a newer request/queue.
            if (generation != m_sessionGeneration) return;
            m_automationFinishPending = false; m_automationFinishCancel = false;
            if (cancelled) { discardDeferredAutomationEdits(); return; }
            if (!error.isEmpty()) {
                m_automationFinishFailed = true;
                const bool editsPending = !m_deferredAutomationEdits.empty();
                setError(error); // dependent job completions retain this cause
                discardDeferredAutomationEdits();
                setError(editsPending ? error + tr(" Pending edits were not applied.") : error);
                pumpPluginEdits();
                return;
            }
            setError({});
            pumpPluginEdits();
            applyDeferredAutomationEdits();
        }, true, true);
    };
    // Native settings notifications can end a pass from inside the notification
    // consumer. Finish draining it before capture can service/flush that plug-in.
    if (m_consumingPluginNotifications) QTimer::singleShot(0, this, capture);
    else capture();
}

bool Backend::configureAutomation(int track, const QString& key, const QString& mode)
{
    if (m_automationCancelPending) {
        setError(tr("Wait for the cancelled automation pass to finish restoring its effect settings."));
        return false;
    }
    if (mode != "read" && mode != "write" && mode != "touch" && mode != "latch") return false;
    if (const auto deferred = deferAutomationEdit([this, track, key, mode] {
        return configureAutomation(track, key, mode);
    })) return *deferred;
    finishAutomationRecording();
    QVariantMap descriptor;
    const auto target = chainTarget(track);
    for (const auto& entry : automationTargets(track))
        if (entry.toMap().value("key").toString() == key) { descriptor = entry.toMap(); break; }
    if (mode != "read" && (descriptor.isEmpty() || !descriptor.value("available").toBool()
        || !descriptor.value("recordable", true).toBool())) {
        setError(descriptor.value("independent").toBool() ? tr("Choose Left or Right to record this effect's independent curves.")
            : tr("Choose an available automation parameter.")); return false;
    }
    m_automationTrack = target.target;
    m_automationKey = key; m_automationDescriptor = descriptor;
    m_automationMode = mode; m_automationTouch = false; m_automationLatched = false;
    m_automationManualTouch = m_automationManualLatched = false;
    m_automationAppliedManualTouch = false;
    ++m_automationRunToken;
    m_automationArmRevision = m_automationManualRevision;
    m_nativeAutomationTouches.clear(); m_nativeAutomationGroups.clear();
    m_automationSuspended = false;
    if (processManualAutomation() && !m_manualAutomation) m_manualAutomation = std::make_shared<engine::AutomationRecorder>();
    const auto* curve = liveAutomationCurve();
    const auto base = descriptor.value("base", 1).toDouble();
    m_automationValue = curve ? curve->preciseValueAt(m_meters.position(), base) : base;
    m_automationManualValue = m_automationValue;
    prepareInstanceAutomation();
    setError({});
    if (m_meters.playing() && mode == "write" && !processOwnedAutomation()) sampleAutomation(m_meters.position());
    else publishAutomationPlan();
    emit automationLiveChanged(); return true;
}

void Backend::prepareInstanceAutomation()
{
    m_automationInstances = false;
    const auto fx = fxAutomationTarget(m_automationKey);
    auto* chain = processAutomationChain();
    if (!chain || !fx) return;
    const auto slot = fx->slot, port = fx->port;
    if (!chain->processAutomationSpans(slot) || chain->instanceCount(slot) != 2) return;
    const auto target = chainTarget(m_automationTrack);
    const auto* control = (*target.inserts)[slot].control(port);
    for (std::size_t actual = 0; actual < m_automationInstanceRead.size(); ++actual) {
        if (control && control->instanceAutomation[actual]) m_automationInstanceRead[actual] = *control->instanceAutomation[actual];
        else m_automationInstanceRead[actual] = {
            control && control->automation.hasData() ? control->value : chain->controlValue(slot, port, actual),
            control ? control->automation : session::AutomationCurve{}};
    }
    m_automationInstances = true;
}

void Backend::publishAutomationPlan(bool audibleChange)
{
    // A bounded notification batch can contain many values at the same frame.
    // Retain every curve edit, but publish only its final plan to the callback.
    if (m_consumingPluginNotifications) {
        m_automationPlanPending = true;
        m_automationPlanAudibleChange = m_automationPlanAudibleChange || audibleChange;
        return;
    }
    publishPlan(true, audibleChange);
    emit automationLiveChanged();
}

lv2::Chain* Backend::processAutomationChain()
{
    const auto fx = fxAutomationTarget(m_automationKey);
    if (m_automationMode == "read" || !fx) return nullptr;
    const auto target = chainTarget(m_automationTrack);
    if (target.target == kMasterTarget && m_previewBefore) return nullptr;
    const auto slot = fx->slot;
    if (!target.inserts || slot >= target.inserts->size() || (*target.inserts)[slot].bypass) return nullptr;
    const auto chain = m_chains.find(target.key);
    return chain != m_chains.end() && chain->second && chain->second->processParameterEvents(slot)
        ? chain->second.get() : nullptr;
}

void Backend::sampleProcessAutomation(const host::ParameterTime& time)
{
    if (m_automationPass && m_automationPass->followingRead) return;
    if (!time.valid || !time.playing || !time.processSequence || time.document != m_sessionGeneration
        || !automationEpochMatches(time, m_meters.playbackState().epoch, m_automationPlayEpoch)) return;
    // This snapshot preceded queue consumption. An in-flight callback can have
    // delivered a newer native event since then; never move it backwards.
    if (m_automationPass && time.processSequence < m_automationPass->processSequence) return;
    const auto wraps = m_automationPass && time.loopIteration > m_automationPass->processLoopIteration
        ? time.loopIteration - m_automationPass->processLoopIteration : 0;
    sampleAutomation(time.frame, wraps);
    if (m_automationPass) {
        m_automationPass->processSequence = time.processSequence;
        m_automationPass->processLoopIteration = time.loopIteration;
    }
}

void Backend::sampleAutomation(qint64 frame, std::uint64_t loopWraps)
{
    if (m_automationUpdating || m_automationCancelPending || m_automationSuspended
        || m_automationMode == "read" || m_project.length() <= 0) return;
    frame = std::clamp<qint64>(frame, 0, m_project.length() - 1);
    if (!m_automationPass)
        m_automationPass = AutomationPass{session::captureState(m_project), m_sessionGeneration, -1, m_automationValue};
    if (m_automationPass->generation != m_sessionGeneration) { m_automationPass.reset(); return; }
    auto* original = liveAutomationCurve(true);
    if (!original) { m_automationSuspended = true; finishAutomationRecording(); return; }
    const auto fx = fxAutomationTarget(m_automationKey);
    std::optional<session::InstanceAutomation>* instance = nullptr;
    if (fx && fx->actual != SIZE_MAX) {
        const auto target = chainTarget(m_automationTrack);
        for (auto& control : (*target.inserts)[fx->slot].controls)
            if (control.port == fx->port) { instance = &control.instanceAutomation[fx->actual]; break; }
    }
    auto curve = *original;
    auto& pass = *m_automationPass;
    if (pass.lastFrame == frame && pass.lastValue == m_automationValue) return;
    const double base = m_automationDescriptor.value("base").toDouble();
    bool ok = true;
    if (pass.lastFrame >= 0 && (loopWraps || frame < pass.lastFrame)) {
        if (loopWraps > 1 && m_loopEnabled && m_loopEnd > m_loopStart)
            ok = curve.writeInterval(m_loopStart, m_loopEnd - 1, pass.lastValue, pass.lastValue, base);
        if (m_loopEnabled && m_loopEnd > pass.lastFrame)
            ok = ok && curve.writeInterval(pass.lastFrame, m_loopEnd - 1, pass.lastValue, pass.lastValue, base);
        pass.lastFrame = m_loopEnabled ? m_loopStart : frame;
    }
    if (pass.lastFrame >= 0 && frame > pass.lastFrame)
        ok = ok && curve.writeInterval(pass.lastFrame, frame - 1, pass.lastValue, pass.lastValue, base);
    ok = ok && curve.writeInterval(frame, frame, m_automationValue, m_automationValue, base);
    if (!ok || m_project.automationPointCount() - (instance && !*instance ? 0 : original->points.size())
        + curve.points.size() > session::kMaxProjectAutomationPoints) {
        // Keep everything recorded so far. No partially applied interval reaches the model.
        m_automationSuspended = true; finishAutomationRecording();
        setError(tr("Automation point limit reached. The recorded pass was kept.")); return;
    }
    if (instance) *instance = session::InstanceAutomation{base, std::move(curve)};
    else *original = std::move(curve);
    pass.lastFrame = frame; pass.lastValue = m_automationValue;
    m_automationUpdating = true;
    markDirty(); publishAutomationPlan();
    m_automationUpdating = false;
}

void Backend::pollAutomation()
{
    const auto playback = m_meters.playbackState();
    if (m_automationPlayEpoch) {
        // Do not commit the first point while Play is still queued. Its epoch is
        // published together with playing, including a take shorter than a poll.
        if (playback.epoch == *m_automationPlayEpoch) return;
        m_automationPlayEpoch.reset();
    }
    if (!playback.playing) {
        if (!processOwnedAutomation() && m_automationPass && !m_automationSuspended && (m_automationMode == "write" || m_automationTouch || m_automationLatched)
            && m_meters.position() > m_automationPass->lastFrame)
            sampleAutomation(std::max<qint64>(0, m_meters.position() - 1));
        if (m_automationPass && !m_automationFinishFailed) finishAutomationRecording();
        m_automationSuspended = false;
        return;
    }
    if (!m_nativeAutomationGroups.empty()) return; // a native group still owns its starting timestamp
    if (processOwnedAutomation()) return; // advanced from the snapshot frozen before native polling
    if (m_automationMode == "write" || m_automationTouch || m_automationLatched)
        sampleAutomation(m_meters.position());
}

void Backend::touchAutomation(bool pressed)
{
    if (m_automationMode == "read" || m_automationSuspended) return;
    m_automationManualTouch = pressed;
    if (pressed && m_automationMode == "latch") m_automationManualLatched = true;
    if (processOwnedAutomation() && m_meters.playing()) {
        if (processManualAutomation()) {
            m_automationTouch = pressed; m_automationLatched = m_automationManualLatched;
        }
        publishAutomationPlan(); return; // record when the callback applies it
    }
    if (!pressed && m_automationTouch && m_meters.playing()) sampleAutomation(m_meters.position());
    m_automationAppliedManualTouch = pressed;
    m_automationTouch = pressed || !m_nativeAutomationTouches.empty();
    if (pressed && m_automationMode == "latch") m_automationLatched = true;
    if (pressed && m_meters.playing()) sampleAutomation(m_meters.position());
    if (!pressed && m_automationMode == "touch") {
        if (m_automationPass) m_automationPass->lastFrame = -1;
        publishPlan(true);
    }
    if (processOwnedAutomation()) publishAutomationPlan();
    emit automationLiveChanged();
}

void Backend::writeAutomationValue(double value)
{
    if (!std::isfinite(value) || m_automationDescriptor.isEmpty()) return;
    if (m_automationFinishPending || m_automationCancelPending) {
        if (deferAutomationEdit([this, value] { writeAutomationValue(value); return true; })) return;
    }
    m_automationManualValue = std::clamp(value, m_automationDescriptor.value("minimum").toDouble(), m_automationDescriptor.value("maximum").toDouble());
    m_automationManualRevision = host::nextAutomationRevision();
    if (processOwnedAutomation() && m_meters.playing()) {
        if (!m_automationSuspended && (m_automationMode == "write" || m_automationManualTouch || m_automationManualLatched)) {
            if (!m_automationPass)
                m_automationPass = AutomationPass{session::captureState(m_project), m_sessionGeneration, -1, m_automationValue};
            const auto playback = m_meters.playbackState();
            m_automationPass->manualPending = ManualAutomationInput{m_automationManualValue, m_automationManualRevision,
                m_sessionGeneration, playback.epoch};
            markDirty();
        }
        publishAutomationPlan(); return;
    }
    m_automationValue = m_automationManualValue;
    if (m_meters.playing() && (m_automationMode == "write" || m_automationTouch || m_automationLatched))
        sampleAutomation(m_meters.position());
    if (processOwnedAutomation()) publishAutomationPlan();
    emit automationLiveChanged();
}

bool Backend::writeMatchingAutomation(int track, const QString& key, double value)
{
    if (m_automationMode == "read" || m_automationSuspended || m_automationFinishPending || key != m_automationKey
        || chainTarget(track).target != m_automationTrack || !m_meters.playing()) return false;
    if (m_automationMode == "touch" && !m_automationTouch && !m_automationManualTouch) return false;
    if (m_automationMode == "latch") {
        m_automationManualLatched = true;
        if (!processAutomationChain()) m_automationLatched = true;
    }
    writeAutomationValue(value); return true;
}

void Backend::automationGesture(int track, const QString& key, bool pressed)
{
    if (key == m_automationKey && chainTarget(track).target == m_automationTrack) touchAutomation(pressed);
}

bool Backend::recordAutomationRamp(const host::ParameterEvent& event, std::size_t actual)
{
    if (event.automationPass != m_automationRunToken || m_automationMode == "read" || m_automationSuspended
        || !event.recordable || !event.time.valid || !event.time.playing || event.time.document != m_sessionGeneration
        || !automationEpochMatches(event.time, m_meters.playbackState().epoch, m_automationPlayEpoch)) return false;
    if (!event.automationSpanFrames || !std::isfinite(event.value) || !std::isfinite(event.automationEndValue)
        || !std::isfinite(event.automationStep) || !event.time.processSequence
        || event.time.frame < 0 || event.time.frame >= m_project.length()) return false;
    if (!m_automationPass)
        m_automationPass = AutomationPass{session::captureState(m_project), m_sessionGeneration, -1, m_automationValue};
    auto& pass = *m_automationPass;
    if (actual >= pass.spans.size()) return false;
    auto& span = pass.spans[actual];
    if (event.time.processSequence < span.sequence) return false;
    auto* original = liveAutomationCurve(true);
    if (!original) return false;
    std::optional<session::InstanceAutomation>* instance = nullptr;
    const auto fx = fxAutomationTarget(m_automationKey);
    if (m_automationInstances || (fx && fx->actual != SIZE_MAX)) {
        const auto target = chainTarget(m_automationTrack);
        for (auto& control : (*target.inserts)[fx->slot].controls)
            if (control.port == fx->port) { instance = &control.instanceAutomation[actual]; break; }
        if (!instance) return false;
        if (*instance) original = &(*instance)->curve;
        else if (m_automationInstances) original = &m_automationInstanceRead[actual].curve;
    }
    const auto count = std::min<qint64>(event.automationSpanFrames, m_project.length() - event.time.frame);
    const auto grid = std::max(1u, event.automationGridFrames);
    const auto last = event.time.frame + count - 1;
    const double endValue = event.automationSpanFrames == 1 ? event.value
        : std::lerp(event.value, event.automationEndValue, double(count - 1) / (event.automationSpanFrames - 1));
    auto first = event.time.frame;
    auto firstValue = event.value;
    // Extend one stored straight segment instead of retaining two points
    // for every audio block. Never join across re-grabs, loops or instances.
    if (span.start >= 0 && first == span.end + 1
        && span.loop == event.time.loopIteration && span.step == event.automationStep && span.grid == grid
        && std::abs(event.value - (span.from + (first - span.start) * span.step))
            <= 1e-12 * std::max(1., std::abs(event.value))) {
        first = span.start; firstValue = span.from;
    }
    auto curve = *original;
    const double base = m_automationInstances ? m_automationInstanceRead[actual].base : m_automationDescriptor.value("base").toDouble();
    if (!curve.writeControlInterval(first, last, firstValue, endValue, base, grid)
        || m_project.automationPointCount() - (instance && !*instance ? 0 : original->points.size()) + curve.points.size() > session::kMaxProjectAutomationPoints) {
        m_automationSuspended = true; finishAutomationRecording();
        setError(tr("Automation point limit reached. The recorded pass was kept.")); return false;
    }
    if (instance) {
        *instance = session::InstanceAutomation{base, std::move(curve)};
        original = &(*instance)->curve;
    } else *original = std::move(curve);
    span = {event.time.processSequence, event.time.loopIteration, first, last, firstValue, event.automationStep, grid,
        std::max(span.acceptedRevision, event.automationRevision)};
    if (pass.pending[actual] && pass.pending[actual]->automationRevision <= span.acceptedRevision)
        pass.pending[actual].reset();
    pass.lastFrame = -1; pass.followingRead = true; pass.processSource = actual;
    pass.processSequence = event.time.processSequence; pass.processLoopIteration = event.time.loopIteration;
    m_automationValue = grid > 1 ? original->preciseValueAt(original->controlFrame(last, grid), base) : endValue;
    markDirty(); publishAutomationPlan();
    return true;
}

bool Backend::pendingManualAutomation() const
{
    if (!m_automationPass || !m_automationPass->manualPending) return false;
    const auto fx = fxAutomationTarget(m_automationKey);
    const auto first = fx && fx->actual != SIZE_MAX ? fx->actual : 0;
    const auto count = m_automationInstances && fx && fx->actual == SIZE_MAX ? 2 : 1;
    for (auto actual = first; actual < first + count; ++actual)
        if (m_automationPass->manualPending->revision > m_automationPass->spans[actual].acceptedRevision) return true;
    return false;
}

void Backend::acknowledgeManualAutomation(std::size_t actual, std::uint64_t revision)
{
    if (!m_automationPass || actual >= m_automationPass->spans.size()) return;
    auto& accepted = m_automationPass->spans[actual].acceptedRevision;
    accepted = std::max(accepted, revision);
}

bool Backend::capturePendingManualAutomationInto(session::Project& candidate,
    const std::optional<ManualAutomationInput>& manual, QString& error, bool& changed)
{
    changed = false;
    if (!manual || !m_automationPass) return true;
    const auto& pass = *m_automationPass;
    const auto fx = fxAutomationTarget(m_automationKey);
    const auto first = fx && fx->actual != SIZE_MAX ? fx->actual : 0;
    const auto count = m_automationInstances && fx && fx->actual == SIZE_MAX ? 2 : 1;
    for (auto actual = first; actual < first + count; ++actual) {
        if (manual->revision <= pass.spans[actual].acceptedRevision
            || (pass.pending[actual] && manual->revision < pass.pending[actual]->automationRevision)) continue;
        const auto trackId = m_automationTrack == kMasterTarget ? std::optional<session::TrackId>{}
            : std::optional{static_cast<session::TrackId>(m_automationTrack)};
        auto* access = m_deliveryAccess ? &*m_deliveryAccess : m_snapshotAccess ? &*m_snapshotAccess : nullptr;
        const auto time = access ? access->automationInputTime(trackId, fx.has_value()) : host::ParameterTime{};
        if (!time.valid || time.document != pass.generation || time.document != manual->generation
            || time.playbackEpoch != manual->playbackEpoch || time.frame < 0 || time.frame > candidate.length()) {
            error = tr("The pending automation move could not be placed at its audio input. The pass was kept; try again after processing.");
            return false;
        }
        auto* track = trackId ? candidate.track(*trackId) : nullptr;
        if (trackId && !track) { error = tr("The automation track is no longer available."); return false; }
        session::AutomationCurve* curve = nullptr;
        const auto base = m_automationInstances ? m_automationInstanceRead[actual].base : m_automationDescriptor.value("base").toDouble();
        if (m_automationKey == "volume") curve = track ? &track->volumeAutomation : &candidate.masterVolumeAutomation();
        else if (m_automationKey == "pan" && track) curve = &track->panAutomation;
        else if (fx) {
            auto& rack = track ? track->inserts : candidate.masterInserts();
            if (fx->slot >= rack.size()) { error = tr("The automation effect is no longer available."); return false; }
            auto& insert = rack[fx->slot];
            if (!insert.control(fx->port)) insert.setControl(fx->port, base);
            auto& control = *std::find_if(insert.controls.begin(), insert.controls.end(), [&](const auto& c) { return c.port == fx->port; });
            if (m_automationInstances || fx->actual != SIZE_MAX) {
                if (!control.instanceAutomation[actual]) control.instanceAutomation[actual] = session::InstanceAutomation{base, control.automationAt(actual)};
                curve = &control.instanceAutomation[actual]->curve;
            } else curve = &control.automation;
        }
        if (!curve) { error = tr("The automation parameter is no longer available."); return false; }
        const auto before = *curve;
        if (!curve->writeControlInterval(time.frame, time.frame, manual->value, manual->value, base, 1)) {
            error = tr("Automation point limit reached. The pending pass was kept."); return false;
        }
        changed = changed || *curve != before;
    }
    return true;
}

bool Backend::capturePendingAutomation(QString& error, const std::optional<ManualAutomationInput>& manual)
{
    if (!m_automationPass || (m_automationSuspended && !m_automationFinishOwned)) return true;
    auto candidate = m_project;
    bool manualChanged = false;
    if (!capturePendingManualAutomationInto(candidate, manual, error, manualChanged)) return false;
    const auto fx = fxAutomationTarget(m_automationKey);
    auto* chain = processAutomationChain();
    auto& pass = *m_automationPass;
    bool changed = false;
    if (fx && chain && chain->processAutomationSpans(fx->slot)) {
        auto& rack = m_automationTrack == kMasterTarget ? candidate.masterInserts()
            : candidate.track(static_cast<session::TrackId>(m_automationTrack))->inserts;
        if (!capturePendingAutomationInto(rack[fx->slot], error, changed, manual ? manual->revision : 0)) return false;
    }
    if (!changed && !manualChanged) return true;
    if (candidate.automationPointCount() > session::kMaxProjectAutomationPoints) {
        error = tr("Project has reached its automation point limit. The pending pass was kept."); return false;
    }
    m_project = std::move(candidate);
    pass.pending = {};
    pass.pendingInputs = {};
    markDirty(); publishAutomationPlan();
    return true;
}

bool Backend::capturePendingAutomationInto(session::Insert& candidate, QString& error, bool& changed, std::uint64_t manualRevision)
{
    changed = false;
    if (!m_automationPass) return true;
    const auto fx = fxAutomationTarget(m_automationKey);
    auto* chain = processAutomationChain();
    if (!fx || !chain || !chain->processAutomationSpans(fx->slot)) return true;
    const auto& pass = *m_automationPass;
    if (!candidate.control(fx->port)) candidate.setControl(fx->port, m_automationDescriptor.value("base").toDouble());
    auto& control = *std::find_if(candidate.controls.begin(), candidate.controls.end(), [&](const auto& c) { return c.port == fx->port; });
    for (std::size_t actual = 0; actual < pass.pending.size(); ++actual) {
        const auto& pending = pass.pending[actual];
        if (!pending || pending->automationRevision < manualRevision) continue;
        // A failed cancellation can resume Read before the user chooses Keep.
        // Its exclusive capture established the accepted value. Place a retained
        // receipt at the current input boundary, never over Read already heard
        // while the user was deciding whether to retry Cancel or keep the edit.
        auto input = pass.pendingInputs[actual];
        if (input && input->revision == pending->automationRevision)
            input->time = chain->nextInputTime(fx->slot, actual);
        else
            input = chain->unrenderedParameter(fx->slot, fx->port, actual);
        if (!input || input->revision != pending->automationRevision || !input->time.valid
            || input->time.document != pass.generation || input->time.playbackEpoch != pending->time.playbackEpoch
            || input->time.frame < 0 || input->time.frame > m_project.length()) {
            error = tr("The plug-in's pending automation input could not be confirmed. The pass was kept; try again after processing.");
            return false;
        }
        const bool individual = m_automationInstances || fx->actual != SIZE_MAX;
        const double base = m_automationInstances ? m_automationInstanceRead[actual].base
            : m_automationDescriptor.value("base").toDouble();
        auto curve = individual ? control.automationAt(actual) : control.automation;
        if (!curve.writeControlInterval(input->time.frame, input->time.frame, input->value, input->value, base, 1)) {
            error = tr("Automation point limit reached. The pending pass was kept."); return false;
        }
        if (individual) control.instanceAutomation[actual] = session::InstanceAutomation{base, std::move(curve)};
        else control.automation = std::move(curve);
        changed = true;
    }
    return true;
}

bool Backend::captureTimelineAutomation(DeliverySnapshot& snapshot, QString& error)
{
    if (snapshot.generation != m_sessionGeneration || snapshot.automationRun != m_automationRunToken
        || !m_automationPass || m_automationPass->generation != snapshot.generation) return true;
    // Preserve the accepted document, including unrelated earlier curves. Only
    // the recording lane can gain receipts while this boundary waits for audio.
    auto* track = m_automationTrack == kMasterTarget ? nullptr
        : snapshot.project.track(static_cast<session::TrackId>(m_automationTrack));
    const auto* current = m_automationTrack == kMasterTarget ? nullptr
        : m_project.track(static_cast<session::TrackId>(m_automationTrack));
    if (m_automationTrack != kMasterTarget && (!track || !current)) return true;
    if (m_automationKey == "volume") {
        if (track) track->volumeAutomation = current->volumeAutomation;
        else snapshot.project.masterVolumeAutomation() = m_project.masterVolumeAutomation();
    } else if (m_automationKey == "pan" && track) {
        track->panAutomation = current->panAutomation;
    } else if (const auto fx = fxAutomationTarget(m_automationKey)) {
        auto& rack = track ? track->inserts : snapshot.project.masterInserts();
        const auto& liveRack = current ? current->inserts : m_project.masterInserts();
        if (fx->slot >= rack.size() || fx->slot >= liveRack.size()
            || rack[fx->slot].identity != liveRack[fx->slot].identity) return true;
        const auto* source = liveRack[fx->slot].control(fx->port);
        auto candidate = rack[fx->slot];
        if (source) {
            if (!candidate.control(fx->port)) candidate.setControl(fx->port, source->value);
            for (auto& control : candidate.controls) if (control.port == fx->port) {
                control.value = source->value;
                control.automation = source->automation;
                control.instanceAutomation = source->instanceAutomation;
            }
        }
        bool changed = false;
        if (!capturePendingAutomationInto(candidate, error, changed, snapshot.manualAutomation ? snapshot.manualAutomation->revision : 0)) return false;
        // Capture into the job's copy without closing the live pass, consuming
        // its pending receipt or adding an Undo entry merely for delivery.
        rack[fx->slot] = std::move(candidate);
    }
    bool manualChanged = false;
    if (!capturePendingManualAutomationInto(snapshot.project, snapshot.manualAutomation, error, manualChanged)) return false;
    if (snapshot.project.automationPointCount() > session::kMaxProjectAutomationPoints) {
        error = tr("Project has reached its automation point limit. The delivery was not started.");
        return false;
    }
    return true;
}

bool Backend::processFaderAutomation() const
{
    return m_automationMode != "read" && (m_automationKey == "volume" || m_automationKey == "pan")
        && !(m_automationTrack == kMasterTarget && m_previewBefore);
}

lv2::Chain* Backend::processBlockAutomationChain() const
{
    const auto fx = fxAutomationTarget(m_automationKey);
    if (m_automationMode == "read" || !fx
        || (m_automationTrack == kMasterTarget && m_previewBefore)) return nullptr;
    const auto key = m_automationTrack == kMasterTarget ? kMasterChainKey
        : session::EffectOwner::track(static_cast<session::TrackId>(m_automationTrack));
    const auto chain = m_chains.find(key);
    const auto slot = fx->slot;
    const auto* track = m_project.track(static_cast<session::TrackId>(m_automationTrack));
    const auto* inserts = m_automationTrack == kMasterTarget ? &m_project.masterInserts() : track ? &track->inserts : nullptr;
    if (!inserts || slot >= inserts->size() || (*inserts)[slot].bypass || !m_lv2) return nullptr;
    const auto* info = m_lv2->find((*inserts)[slot].uri);
    return chain != m_chains.end() && chain->second && chain->second->hasProcessor(slot)
        && info && info->format == host::Format::Lv2
        ? chain->second.get() : nullptr;
}

void Backend::consumeManualAutomation()
{
    if (!m_manualAutomation) return;
    host::ParameterEvent event;
    const auto fx = fxAutomationTarget(m_automationKey);
    const auto control = processFaderAutomation() ? (m_automationKey == "pan" ? 1u : 0u)
        : fx ? fx->port : UINT32_MAX;
    for (unsigned count = 0; count < 1024 && m_manualAutomation->next(event); ++count)
        if (processManualAutomation() && event.control == control)
            (void)recordAutomationRamp(event, fx && fx->actual != SIZE_MAX ? fx->actual : 0);
    if (const auto pass = m_manualAutomation->takeOverflow(); pass && pass == m_automationRunToken) {
        m_automationSuspended = true; finishAutomationRecording();
        setError(tr("Automation feedback could not keep up. The recorded pass was kept; recording has stopped."));
    }
}

bool Backend::recordNativeAutomation(session::EffectOwner key, std::size_t slot, std::size_t actual,
                                     const host::ParameterEvent& event)
{
    if (!chainInserts(key)) return false;
    const bool processing = event.automationPass != 0;
    const bool manual = processing && event.automationFeedback == host::AutomationFeedback::Manual;
    const auto fx = fxAutomationTarget(m_automationKey);
    if (!fx || fx->slot != slot || (fx->actual != SIZE_MAX && fx->actual != actual)) return false;
    if (processing && (event.automationPass != m_automationRunToken
        || (manual && fx->actual == SIZE_MAX && actual != 0))) return false;
    if (m_automationMode == "read" || m_automationSuspended
        || (!event.recordable && event.kind != host::ParameterEventKind::End)
        || !event.time.valid || !event.time.playing || event.time.document != m_sessionGeneration
        || !automationEpochMatches(event.time, m_meters.playbackState().epoch, m_automationPlayEpoch)
        || chainTarget(m_automationTrack).key != key) return false;
    const auto group = std::pair{actual, event.group};
    if (event.kind == host::ParameterEventKind::GroupBegin) {
        if (std::find(m_nativeAutomationGroups.begin(), m_nativeAutomationGroups.end(), group) == m_nativeAutomationGroups.end())
            m_nativeAutomationGroups.push_back(group);
        return false;
    }
    if (event.kind == host::ParameterEventKind::GroupEnd) {
        std::erase(m_nativeAutomationGroups, group); return false;
    }
    if (fx->port != event.control) return false;
    const auto chain = m_chains.find(key);
    const auto* ports = chain != m_chains.end() && chain->second ? chain->second->controls(slot) : nullptr;
    if (!ports || std::none_of(ports->begin(), ports->end(), [&](const auto& port) {
        return port.index == event.control && !port.output && port.automatable;
    })) return false;
    if (processing && event.automationFeedback == host::AutomationFeedback::Ramp)
        return recordAutomationRamp(event, actual);
    const auto touching = std::find(m_nativeAutomationTouches.begin(), m_nativeAutomationTouches.end(), actual);
    if (!processing && chain->second->processAutomationSpans(slot) && processAutomationChain() == chain->second.get()) {
        // UI callbacks still own native gesture/group history, with their
        // original timestamps. The processor alone acknowledges audible spans.
        // Claim accepted Values here so they cannot become a separate settings
        // edit while waiting for their first processing callback.
        if (event.kind == host::ParameterEventKind::Begin || event.kind == host::ParameterEventKind::Value) {
            if (!std::isfinite(event.value)) return false;
            if (!m_automationPass)
                m_automationPass = AutomationPass{session::captureState(m_project), m_sessionGeneration, -1, m_automationValue};
            m_automationPass->followingRead = true;
            if (event.kind == host::ParameterEventKind::Value && actual < m_automationPass->pending.size()
                && event.automationRevision > m_automationPass->spans[actual].acceptedRevision)
                m_automationPass->pending[actual] = event;
            if (event.kind == host::ParameterEventKind::Begin && touching == m_nativeAutomationTouches.end())
                m_nativeAutomationTouches.push_back(actual);
            if (m_automationMode == "latch") m_automationLatched = true;
        } else if (event.kind == host::ParameterEventKind::End) std::erase(m_nativeAutomationTouches, actual);
        m_automationTouch = m_automationManualTouch || !m_nativeAutomationTouches.empty();
        return event.kind == host::ParameterEventKind::Value;
    }
    const auto sample = [&] {
        if (event.time.processSequence) sampleProcessAutomation(event.time);
        else sampleAutomation(event.time.frame);
    };
    const bool resumesRead = processing && event.automationFeedback == host::AutomationFeedback::Read;
    const bool endsProcessing = processing && event.automationFeedback == host::AutomationFeedback::End;
    if (resumesRead) {
        if (m_automationPass && m_automationPass->processSource == actual) {
            if (event.time.frame > 0) {
                auto time = event.time; --time.frame; sampleProcessAutomation(time);
            }
            if (m_automationPass) {
                m_automationPass->rampStart = -1;
                m_automationPass->lastFrame = -1;
                m_automationPass->followingRead = true;
            }
        }
        return false;
    }
    const auto startSpan = [&] {
        if (m_automationPass) {
            m_automationPass->rampStart = -1;
            m_automationPass->followingRead = false;
            m_automationPass->processSource = actual;
        }
    };
    const auto rememberSource = [&] {
        if (m_automationPass) m_automationPass->processSource = actual;
    };
    if (event.kind == host::ParameterEventKind::Begin) {
        startSpan();
        if (manual) m_automationAppliedManualTouch = true;
        else if (touching == m_nativeAutomationTouches.end()) m_nativeAutomationTouches.push_back(actual);
        m_automationTouch = true;
        if (m_automationMode == "latch") m_automationLatched = true;
        m_automationValue = std::clamp(event.value, m_automationDescriptor.value("minimum").toDouble(),
                                      m_automationDescriptor.value("maximum").toDouble());
        sample(); rememberSource();
    } else if (event.kind == host::ParameterEventKind::Value) {
        if (!std::isfinite(event.value) || (m_automationMode == "touch"
            && (manual ? !m_automationAppliedManualTouch : touching == m_nativeAutomationTouches.end()))) return false;
        if (manual) acknowledgeManualAutomation(actual, event.automationRevision);
        startSpan();
        if (m_automationMode == "latch") m_automationLatched = true;
        m_automationValue = std::clamp(event.value, m_automationDescriptor.value("minimum").toDouble(),
                                      m_automationDescriptor.value("maximum").toDouble());
        sample(); rememberSource();
        return m_automationPass.has_value() && !m_automationSuspended;
    } else if (event.kind == host::ParameterEventKind::End
               && (manual ? m_automationAppliedManualTouch : touching != m_nativeAutomationTouches.end())) {
        // DONT_RECORD suppresses writing this event, not the release of an
        // already-recorded gesture. Otherwise polling keeps extending Touch
        // after the audio owner has resumed the original playback curve.
        if ((manual || (endsProcessing && event.recordable)) && event.time.frame > 0) {
            auto time = event.time; --time.frame; sampleProcessAutomation(time);
        } else if (!manual && event.recordable) sample();
        if (event.automationEndDeferred) return false;
        if (manual) m_automationAppliedManualTouch = false;
        else std::erase(m_nativeAutomationTouches, actual);
        m_automationTouch = m_automationAppliedManualTouch || !m_nativeAutomationTouches.empty();
        if (m_automationMode == "touch" && !m_automationTouch) {
            if (m_automationPass) m_automationPass->lastFrame = -1;
            publishAutomationPlan();
        }
    }
    if (!m_consumingPluginNotifications) emit automationLiveChanged();
    return false;
}

void Backend::finishAutomationRecording(bool cancel)
{
    if (!m_automationFinishOwned && m_automationFinishPending) {
        m_automationFinishCancel = m_automationFinishCancel || cancel;
        return;
    }
    if (m_automationUpdating || m_finishingAutomation) return;
    // Cancel removes the live pass, but an earlier accepted delivery still owns
    // its final callback and pending input. Retire that boundary before undoing
    // the pass. Without such a job, the existing discard path stays immediate.
    if ((!cancel || hasPendingTimelineSnapshot()) && !m_automationFinishOwned) {
        if (processOwnedAutomation()) {
            // The raw notification queue can contain a complete gesture even
            // when no real processing acknowledgement has arrived yet.
            m_finishingAutomation = true;
            consumePluginNotifications();
            m_finishingAutomation = false;
            // The first recorded segment may still belong to an in-flight
            // callback. Waiting only after receiving a point loses that whole
            // first gesture when an edit or session replacement arrives now.
            if (m_automationPass || (!m_automationSuspended && (m_meters.playing() || m_automationPlayEpoch))) {
                requestAutomationFinish(cancel); return;
            }
        }
    }
    m_finishingAutomation = true;
    m_automationIndependentEdit = false;
    const auto finishing = qScopeGuard([this] {
        m_finishingAutomation = false;
        if (m_automationEditQueueActive && !m_automationFinishPending)
            QTimer::singleShot(0, this, &Backend::applyDeferredAutomationEdits);
    });
    // Stop, Seek and another edit are boundaries even when the regular poll has
    // not run yet. Consume native values while their original mode is still armed.
    consumePluginNotifications();
    // An independent settings edit already ended this pass, even if its dirty
    // notification was waiting when Cancel was clicked. Keep both Undo steps.
    if (m_automationIndependentEdit) cancel = false;
    m_automationPlayEpoch.reset();
    const bool engaged = m_automationMode == "write" || m_automationTouch || m_automationLatched;
    if (!processOwnedAutomation() && m_automationPass && engaged && m_meters.playing() && !m_automationSuspended && !cancel)
        sampleAutomation(m_meters.position());
    m_automationSuspended = true; m_automationTouch = false; m_automationLatched = false;
    m_automationArmRevision = m_automationManualRevision;
    m_automationManualTouch = m_automationManualLatched = false;
    m_automationAppliedManualTouch = false;
    m_nativeAutomationTouches.clear(); m_nativeAutomationGroups.clear();
    // A plug-in cancellation first needs an exclusive snapshot of its accepted
    // input. Removing the recording lane here resets its input to the shared
    // base before that snapshot, losing the very edit a failed restore must keep.
    // Retiring an untouched Touch/Latch lane changes callback ownership only.
    // Keep valid audio analyses usable across that boundary. Any recorded pass,
    // engaged gesture or native notification still invalidates them normally.
    if (processOwnedAutomation() && !(cancel && m_automationKey.startsWith("fx:")))
        publishAutomationPlan(m_automationPass.has_value() || engaged);
    if (!m_automationPass) return;
    auto pass = std::move(*m_automationPass); m_automationPass.reset();
    if (pass.generation != m_sessionGeneration) return;
    if (cancel) {
        if (m_automationKey.startsWith("fx:")) {
            const auto key = chainTarget(m_automationTrack).key;
            cancelPluginAutomation(std::move(pass), key);
            return;
        }
        session::restoreState(m_project, pass.before);
    }
    else (void)m_undo.push("record automation", chainTarget(m_automationTrack).track, pass.before, session::captureState(m_project));
    markDirty(); publishPlan(true); rebuildTracks();
    emit timelineChanged(); emit automationLiveChanged();
}

void Backend::cancelPluginAutomation(AutomationPass pass, session::EffectOwner key)
{
    if (!chainInserts(key)) return;
    // Replacement warm-up must not wait on a processor still inside the audio
    // callback. Use the same asynchronous ownership boundary as rack edits.
    // Detach this pass now; newer host controls are independent of the cancelled
    // recording and must survive when the restore eventually runs.
    m_automationCancelPending = true;
    auto request = std::make_shared<AutomationPass>(std::move(pass));
    const auto from = session::captureState(m_project);
    const auto rackOf = [key](session::SessionState& state) -> std::vector<session::Insert>* {
        return session::findLegacyEffectRack(state, key);
    };
    const auto restore = [this, request, from, rackOf] {
        auto current = session::captureState(m_project);
        auto merged = session::stateForPluginAwareEdit(current, from, request->before);
        // Only the affected rack belongs to this cancellation. Unrelated edits
        // accepted while ownership was pending remain in the current session.
        if (auto* rack = rackOf(current))
            if (auto* original = rackOf(merged)) *rack = std::move(*original);
        return current;
    };
    const auto retain = [this, request, restore] {
        m_automationCancelPending = false;
        if (request->generation != m_sessionGeneration) return;
        request->before = restore();
        m_automationPass = *request; // suspended; retry cancellation or keep it
        // Jobs and edits accepted after Cancel depend on its restored state.
        // A failed restore must not silently run them against the kept pass.
        if (!m_deferredAutomationEdits.empty()) {
            const auto error = lastError();
            discardDeferredAutomationEdits();
            setError(error + tr(" Pending edits were not applied."));
        }
        emit automationLiveChanged();
    };
    const auto apply = [this, request, key, rackOf, restore, retain] {
        if (request->generation != m_sessionGeneration) { m_automationCancelPending = false; return false; }
        // The owned capture has now preserved unrendered input for a retry.
        // End the suspended recording lane even if preparing its replacement
        // fails; the retained live processor must resume Read, not a return ramp.
        publishAutomationPlan();
        auto restored = restore();
        auto* rack = rackOf(restored);
        // Resetting the old render plan knows only a shared logical base value.
        // A private, transactionally prepared chain restores independent mono
        // inputs. Failure retains the current processor and the recorded pass.
        if (!rack || !rebuildChainFor(key, rack, true)) { retain(); return false; }
        session::restoreState(m_project, restored);
        m_automationCancelPending = false;
        // Cancellation also resolves a pass retained by a failed final capture.
        // Leaving that failure latched would strand subsequent model/history
        // edits once stopped polling clears m_automationSuspended.
        m_automationFinishFailed = false;
        setError({});
        markDirty(); publishPlan(true); rebuildTracks();
        emit timelineChanged(); emit automationLiveChanged();
        return true;
    };
    const auto capture = [this, request, key] {
        const auto fx = fxAutomationTarget(m_automationKey);
        const auto chain = m_chains.find(key);
        if (!fx || chain == m_chains.end() || !chain->second) return;
        for (std::size_t actual = 0; actual < request->pending.size(); ++actual) {
            const auto& pending = request->pending[actual];
            if (!pending) continue;
            const auto input = chain->second->unrenderedParameter(fx->slot, fx->port, actual);
            if (input && input->revision == pending->automationRevision) request->pendingInputs[actual] = input;
        }
    };
    if (!deferPluginEdit(apply, retain, false, capture)) (void)apply();
}

void Backend::previewAutomation(engine::RenderPlan& plan) const
{
    session::AutomationCurve* curve = nullptr;
    engine::ControlAutomations* controls = nullptr;
    if (m_automationTrack == -2) {
        if (m_automationKey == "volume") curve = &plan.masterVolumeAutomation;
        controls = &plan.masterControls;
    } else for (auto& track : plan.tracks) if (static_cast<int>(track.id) == m_automationTrack) {
        if (m_automationKey == "volume") curve = &track.volumeAutomation;
        if (m_automationKey == "pan") curve = &track.panAutomation;
        controls = &track.controls; break;
    }
    if (processFaderAutomation() && !m_automationSuspended && m_manualAutomation) {
        engine::RecordingLane* lane = nullptr;
        if (m_automationTrack == kMasterTarget && m_automationKey == "volume") lane = &plan.masterVolumeRecording;
        else for (auto& track : plan.tracks) if (static_cast<int>(track.id) == m_automationTrack) {
            lane = m_automationKey == "pan" ? &track.panRecording : &track.volumeRecording; break;
        }
        if (lane) {
            lane->runtime = m_manualAutomation; lane->base = m_automationDescriptor.value("base").toDouble();
            lane->armRevision = m_automationArmRevision;
            lane->policy = {m_automationKey == "pan" ? 1u : 0u,
                m_automationMode == "write" ? host::AutomationMode::Write
                    : m_automationMode == "touch" ? host::AutomationMode::Touch : host::AutomationMode::Latch,
                m_automationRunToken, m_automationManualRevision, m_automationManualValue,
                m_automationManualTouch || m_automationManualLatched,
                static_cast<std::uint32_t>(std::llround(std::clamp(m_automationReturnSeconds, 0., 5.) * m_project.sampleRate())),
                m_automationArmRevision, pendingManualAutomation()};
        }
        return;
    }
    const auto fx = fxAutomationTarget(m_automationKey);
    if (!m_automationSuspended && m_automationMode != "read" && controls && fx) {
        const auto slot = fx->slot, port = fx->port;
        const auto key = m_automationTrack == kMasterTarget ? kMasterChainKey
            : session::EffectOwner::track(static_cast<session::TrackId>(m_automationTrack));
        const auto chain = m_chains.find(key);
        if (chain != m_chains.end() && chain->second
            && (chain->second->processParameterEvents(slot) || processBlockAutomationChain())
            && !(m_automationTrack == kMasterTarget && m_previewBefore)) {
            if (m_automationInstances) {
                // A native gesture belongs to one actual processor. Both sides
                // retain their own pre-pass Read curve and base throughout plan
                // publications, including a peer which has not been touched.
                std::erase_if(*controls, [&](const auto& c) { return c.slot == slot && c.port == port; });
                for (std::size_t actual = 0; actual < m_automationInstanceRead.size(); ++actual) {
                    const auto& read = m_automationInstanceRead[actual];
                    controls->push_back({slot, port, read.base, read.curve});
                    auto& control = controls->back(); control.actual = actual;
                    if (fx->actual != SIZE_MAX && actual != fx->actual) continue;
                    control.recording = {port, m_automationMode == "write" ? host::AutomationMode::Write
                        : m_automationMode == "touch" ? host::AutomationMode::Touch : host::AutomationMode::Latch,
                        m_automationRunToken, m_automationManualRevision, m_automationManualValue,
                        m_automationManualTouch || m_automationManualLatched,
                        static_cast<std::uint32_t>(std::llround(std::clamp(m_automationReturnSeconds, 0., 5.) * m_project.sampleRate())),
                        m_automationArmRevision, pendingManualAutomation()};
                }
                return;
            }
            if (fx->actual != SIZE_MAX) {
                const auto shared = std::find_if(controls->begin(), controls->end(), [&](const auto& c) {
                    return c.slot == slot && c.port == port && c.actual == SIZE_MAX;
                });
                if (shared != controls->end()) {
                    auto common = *shared; controls->erase(shared);
                    for (std::size_t actual = 0; actual < 2; ++actual) { common.actual = actual; controls->push_back(common); }
                }
            }
            auto control = std::find_if(controls->begin(), controls->end(), [&](const auto& c) {
                return c.slot == slot && c.port == port && c.actual == fx->actual;
            });
            if (control == controls->end()) {
                controls->push_back({slot, port, m_automationDescriptor.value("base").toDouble(), {}});
                control = std::prev(controls->end());
                control->actual = fx->actual;
            }
            control->recording = {port, m_automationMode == "write" ? host::AutomationMode::Write
                : m_automationMode == "touch" ? host::AutomationMode::Touch : host::AutomationMode::Latch,
                m_automationRunToken, m_automationManualRevision, m_automationManualValue,
                m_automationManualTouch || m_automationManualLatched,
                static_cast<std::uint32_t>(std::llround(std::clamp(m_automationReturnSeconds, 0., 5.) * m_project.sampleRate())),
                m_automationArmRevision, pendingManualAutomation()};
            if (processBlockAutomationChain()) {
                control->runtime = m_manualAutomation;
                control->armRevision = m_automationArmRevision;
            }
            // Audio-owned native gestures decide when playback is overridden.
            // Keep the unmodified future curve available for Touch release.
            return;
        }
    }
    if (!m_automationPass || m_automationSuspended || m_automationPass->lastFrame < 0
        || (m_automationMode != "write" && !m_automationTouch && !m_automationLatched)) return;
    if (!curve && controls && fx) for (auto& c : *controls)
        if (c.slot == fx->slot && c.port == fx->port && c.actual == fx->actual) { curve = &c.curve; break; }
    if (!curve) return;
    const auto frame = m_automationPass->lastFrame;
    // Split before discarding the right endpoint: a Smooth prefix needs its
    // original phase window even when the plan holds the last recorded value.
    if (!curve->setPoint(frame, m_automationValue)
        && std::none_of(curve->points.begin(), curve->points.end(), [=](const auto& point) { return point.frame == frame; })) return;
    std::erase_if(curve->points, [=](const auto& point) { return point.frame > frame; });
    curve->enabled = true;
}

QString Backend::automationStamp() const
{
    return QString::number(m_sessionGeneration) + QLatin1Char(':') + QString::number(m_editRevision);
}

QVariantList Backend::automationTargets(int track)
{
    QVariantList result;
    const auto target = chainTarget(track);
    if (!target.inserts) return result;
    const auto descriptor = [](const QString& key, const QString& label, double minimum, double maximum,
                               double base, double factor, const session::AutomationCurve& curve,
                               bool available, const QString& timing) {
        QVariantList points;
        for (const auto& p : curve.points) {
            QVariantMap point{{"frame", static_cast<qint64>(p.frame)}, {"value", p.value}};
            if (p.interpolation != session::AutomationInterpolation::Default)
                point.insert("interpolation", static_cast<int>(p.interpolation));
            if (p.interpolation == session::AutomationInterpolation::Smooth) {
                point.insert("smoothStart", p.smoothStart); point.insert("smoothEnd", p.smoothEnd);
            }
            points.push_back(point);
        }
        return QVariantMap{{"key", key}, {"label", label}, {"minimum", minimum}, {"maximum", maximum},
            {"base", base}, {"factor", factor}, {"enabled", curve.enabled}, {"hold", curve.hold},
            {"points", points}, {"available", available}, {"timing", timing}};
    };
    const auto* t = m_project.track(target.track);
    result.push_back(descriptor("volume", tr("Volume (%)"), 0, 4, 1, 100,
        t ? t->volumeAutomation : m_project.masterVolumeAutomation(), true, "sample"));
    if (t) result.push_back(descriptor("pan", tr("Pan (−100 left / 100 right)"), -1, 1, t->pan, 100, t->panAutomation, true, "sample"));
    const auto built = m_chains.find(target.key);
    const auto* chain = built != m_chains.end() ? built->second.get() : nullptr;
    for (std::size_t slot = 0; slot < target.inserts->size(); ++slot) {
        const auto& insert = (*target.inserts)[slot];
        const auto* ports = chain ? chain->controls(slot) : nullptr;
        const QString timing = automationTiming(m_lv2->find(insert.uri));
        const bool dual = (chain && chain->instanceCount(slot) == 2) || insert.instanceState.size() == 2
            || std::any_of(insert.controls.begin(), insert.controls.end(), [](const auto& c) { return c.hasInstanceAutomation(); });
        const auto key = [&](std::uint32_t port) { return QStringLiteral("fx:%1:%2").arg(slot).arg(port); };
        const auto append = [&](std::uint32_t port, const QString& label, double minimum, double maximum,
                                double base, bool available, bool atomicValues, const QString& playbackTiming) {
            const auto* saved = insert.control(port);
            const auto common = saved ? saved->automation : session::AutomationCurve{};
            auto shared = descriptor(key(port), dual ? label + tr(" · Both channels") : label,
                minimum, maximum, base, 1, common, available, playbackTiming);
            QVariantList instances;
            if (dual) {
                for (std::size_t actual = 0; actual < 2; ++actual) {
                    const bool inherited = !saved || !saved->instanceAutomation[actual];
                    const double instanceBase = !inherited ? saved->automationBase(actual)
                        : saved && common.hasData() ? saved->value
                        : atomicValues && chain && actual < chain->instanceCount(slot) ? chain->controlValue(slot, port, actual) : base;
                    auto instance = descriptor(key(port) + QLatin1Char(':') + QString::number(actual),
                        label + (actual ? tr(" · Right") : tr(" · Left")), minimum, maximum, instanceBase, 1,
                        saved ? saved->automationAt(actual) : common, available, playbackTiming);
                    instance.insert("instance", static_cast<int>(actual)); instance.insert("inherited", inherited);
                    instance.insert("editable", true);
                    instance.insert("recordable", available && chain && actual < chain->instanceCount(slot));
                    instances.push_back(instance);
                }
                const bool independent = saved && saved->hasInstanceAutomation();
                shared.insert("independent", independent); shared.insert("editable", !independent);
                shared.insert("recordable", available && (!independent || (chain && chain->processAutomationSpans(slot) && !insert.bypass)));
                if (independent) {
                    shared.insert("points", QVariantList{});
                    shared.insert("lanes", instances);
                }
            }
            result.push_back(shared);
            result.append(instances);
        };
        if (ports) for (const auto& port : *ports) {
            if (port.output || !std::isfinite(port.minimum) || !std::isfinite(port.maximum) || port.maximum <= port.minimum) continue;
            const auto* saved = insert.control(port.index);
            const double base = saved ? saved->value : port.parameterId ? chain->controlValue(slot, port.index) : port.defaultValue;
            append(port.index, tr("Effect %1 · %2").arg(slot + 1).arg(QString::fromStdString(port.name)),
                port.minimum, port.maximum, base, port.automatable, port.parameterId.has_value(), timing);
        }
        for (const auto& control : insert.controls) {
            if (!control.hasAutomationData()) continue;
            if (std::any_of(result.begin(), result.end(), [&](const auto& entry) { return entry.toMap().value("key").toString() == key(control.port); })) continue;
            double minimum = std::min(0., control.value), maximum = std::max(1., control.value);
            const auto range = [&](const auto& curve, double base) {
                minimum = std::min(minimum, base); maximum = std::max(maximum, base);
                for (const auto& point : curve.points) { minimum = std::min(minimum, point.value); maximum = std::max(maximum, point.value); }
            };
            range(control.automation, control.value);
            for (const auto& instance : control.instanceAutomation) if (instance) range(instance->curve, instance->base);
            append(control.port, tr("Effect %1 · port %2 (unavailable)").arg(slot + 1).arg(control.port),
                minimum, maximum, control.value, false, false, {});
        }
    }
    return result;
}

bool Backend::editAutomation(const QString& key,const QString& operation,qint64 frame,qint64 to,
                              double value,const QString& stamp,int track)
{
    setError({});
    if (!stamp.isEmpty() && !automationStampCurrent(stamp)) {
        setError(tr("The project changed during this automation edit. Try again.")); return false;
    }
    std::function<bool()> edit = [this, key, operation, frame, to, value, stamp, track] {
        return editAutomation(key, operation, frame, to, value, stamp, track);
    };
    if (const auto deferred = deferAutomationEdit(std::move(edit))) return *deferred;
    const auto target=chainTarget(track);
    if (!target.inserts) return false;
    QVariantMap descriptor;
    for (const auto& entry:automationTargets(track))
        if (entry.toMap().value("key").toString()==key) {descriptor=entry.toMap();break;}
    if (descriptor.isEmpty()) return false;
    if (!descriptor.value("editable", true).toBool()) {
        setError(tr("Choose Left or Right to edit an independent curve.")); return false;
    }
    auto* t=m_project.track(target.track);
    session::AutomationCurve* original=nullptr;
    session::Insert* insert=nullptr;
    std::uint32_t port=0;
    const auto fx = fxAutomationTarget(key);
    bool ownsCurve = true;
    double base = descriptor.value("base").toDouble();
    if (key=="volume") original=t?&t->volumeAutomation:&m_project.masterVolumeAutomation();
    else if (key=="pan" && t) original=&t->panAutomation;
    else {
        if (!fx || fx->slot >= target.inserts->size()) return false;
        insert = &(*target.inserts)[fx->slot]; port = fx->port;
        if (fx->actual != SIZE_MAX) ownsCurve = false;
        for (auto& c : insert->controls) if (c.port == port) {
            if (fx->actual != SIZE_MAX && c.instanceAutomation[fx->actual]) {
                original = &c.instanceAutomation[fx->actual]->curve; ownsCurve = true;
            } else original = &c.automation;
            break;
        }
    }
    if (operation == "inherit") {
        if (!fx || fx->actual == SIZE_MAX || !ownsCurve) return false;
        const auto before = captureEditState();
        for (auto& c : insert->controls) if (c.port == port) c.instanceAutomation[fx->actual].reset();
        commit("inherit automation", target.track, before); return true;
    }
    session::AutomationCurve curve=original?*original:session::AutomationCurve{};
    if (operation=="point") {
        if (!descriptor.value("available").toBool() || !std::isfinite(value) || to<0
            || to>std::max<qint64>(0,m_project.length()) || value<descriptor.value("minimum").toDouble()
            || value>descriptor.value("maximum").toDouble()) {
            setError(tr("Automation point is outside the timeline or parameter range.")); return false;
        }
        if (!curve.valid(descriptor.value("minimum").toDouble(),descriptor.value("maximum").toDouble())) {
            setError(tr("The automation curve has invalid point data.")); return false;
        }
        if (frame>=0) {
            const auto point = std::find_if(curve.points.begin(), curve.points.end(), [=](const auto& p) { return p.frame == frame; });
            if (point == curve.points.end()) return false;
            if (frame==to) point->value=value;
            else {
                if (std::any_of(curve.points.begin(),curve.points.end(),[&](const auto& p){return p.frame==to;})) {
                    setError(tr("There is already an automation point at this frame.")); return false;
                }
                auto moved=*point; moved.frame=to; moved.value=value;
                curve.points.erase(point);
                // Retiming a knot keeps its neighbours' outgoing shape windows.
                // Only a newly inserted knot subdivides an existing segment.
                const auto at=std::lower_bound(curve.points.begin(),curve.points.end(),to,
                    [](const auto& p,qint64 position){return p.frame<position;});
                curve.points.insert(at,moved);
            }
        } else if (!curve.setPoint(to,value) && std::none_of(curve.points.begin(),curve.points.end(),
            [&](const auto& p){return p.frame==to;})) {
            setError(tr("Automation curve has reached its point limit.")); return false;
        }
        curve.enabled=true;
    } else if (operation=="delete") {
        if (!curve.erasePoint(frame)) return false;
    } else if (operation=="enabled") curve.enabled=value!=0;
    else if (operation=="hold") curve.hold=value!=0;
    else if (operation=="interpolation") {
        if (!std::isfinite(value) || value != std::trunc(value) || value < 0 || value > 3) {
            setError(tr("Choose Curve default, Linear, Hold or Smooth for the selected segment.")); return false;
        }
        const auto point = std::find_if(curve.points.begin(), curve.points.end(), [=](const auto& p) { return p.frame == frame; });
        if (point == curve.points.end()) { setError(tr("Select an automation point first.")); return false; }
        const auto mode = static_cast<session::AutomationInterpolation>(static_cast<int>(value));
        if (point->interpolation == mode) return false; // also keep an inherited instance lane inherited
        point->interpolation = mode; point->smoothStart = 0; point->smoothEnd = 1;
    }
    else if (operation=="clear") curve={};
    else return false;
    if (ownsCurve && original && curve==*original) return false;
    if (m_project.automationPointCount()-(ownsCurve && original ? original->points.size() : 0)+curve.points.size()
        >session::kMaxProjectAutomationPoints) {
        setError(tr("Project has reached its automation point limit.")); return false;
    }
    const auto before=captureEditState();
    if (insert && fx && fx->actual != SIZE_MAX) {
        if (!insert->control(port)) insert->setControl(port, base);
        for (auto& c : insert->controls) if (c.port == port) {
            c.instanceAutomation[fx->actual] = session::InstanceAutomation{base, std::move(curve)};
            break;
        }
    } else {
        if (!original) {
            insert->setControl(port, base);
            for (auto& c : insert->controls) if (c.port == port) { original = &c.automation; break; }
        }
        *original=std::move(curve);
    }
    commit("edit automation",target.track,before);
    return true;
}
