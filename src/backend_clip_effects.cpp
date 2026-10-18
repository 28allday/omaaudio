#include "backend.h"
#include "pluginmetadata.h"
#include "session/effect_racks.h"
#include "engine/clip_effect_runtime.h"

#include <QMetaObject>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <limits>

// Clip effects are rendered per shared region on worker threads and played as
// ordinary audio (render/clip_effects.h). Playback and every delivery, analysis
// and measurement job read the same rendering, so they always agree.

namespace {
constexpr std::size_t kRetainedClipEffectRenders = 24; // recent versions kept for Undo
bool contains(const std::vector<omawave::render::ClipEffectRegion>& regions,
              const omawave::render::ClipEffectRegion& region)
{
    return std::find(regions.begin(), regions.end(), region) != regions.end();
}
}

render::AppliedClipEffects Backend::heardProject()
{
    try {
        return render::applyClipEffects(m_project, m_clipEffectRenders, render::MissingClipEffects::StandIn);
    } catch (const std::exception&) {
        // An inconsistent region is never played dry; the edit that made it is
        // refused by validation. Leave its views silent until then.
        render::AppliedClipEffects silent;
        silent.project = m_project;
        for (auto& track : silent.project.tracks())
            std::erase_if(track.clips, [](const session::Clip& clip) { return bool(clip.rack); });
        return silent;
    }
}

void Backend::requestClipEffectRenders(const std::vector<render::ClipEffectRegion>& regions,
                                       const session::Project& project, bool retryFailed)
{
    if (m_shuttingDown) return;
    bool changed = false;
    std::shared_ptr<const session::Project> snapshot;
    for (const auto& region : regions) {
        if (retryFailed)
            changed = std::erase_if(m_clipEffectFailures, [&](const auto& failure) { return failure.first == region; }) || changed;
        if (m_clipEffectRenders.find(region)
            || std::any_of(m_clipEffectJobs.begin(), m_clipEffectJobs.end(), [&](const auto& job) { return job.region == region; })
            || std::any_of(m_clipEffectFailures.begin(), m_clipEffectFailures.end(), [&](const auto& failure) { return failure.first == region; }))
            continue;
        if (m_scanning) {
            // The catalogue is being read on a worker. Try again once it is ready.
            QTimer::singleShot(250, this, [this] { if (!m_shuttingDown) publishPlan(false, false); });
            break;
        }
        if (!m_lv2->scanned()) m_lv2->scan();
        auto features = std::make_shared<lv2::HostFeatures>(m_urids, project.sampleRate(), lv2::kMaxBlock);
        std::string error;
        std::shared_ptr<lv2::Chain> chain = render::buildClipEffectChain(*m_lv2, *features, region, error);
        if (!chain) {
            m_clipEffectFailures.emplace_back(region, QString::fromStdString(error));
            changed = true;
            continue;
        }
        if (!snapshot) snapshot = std::make_shared<const session::Project>(project);
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        m_clipEffectJobs.push_back({region, cancel});
        changed = true;
        m_clipEffectPool.start([this, region, chain, features, cancel, snapshot]() mutable {
            auto result = std::make_shared<render::ClipEffectRender>(
                render::renderClipEffectRegion(*snapshot, region, *chain, cancel.get()));
            snapshot.reset();
            // Plug-ins are destroyed on the main thread, which created them.
            QMetaObject::invokeMethod(this, [this, region, result, chain = std::move(chain),
                                             features = std::move(features)]() mutable {
                chain.reset();
                features.reset();
                finishClipEffectRender(region, std::move(result));
            }, Qt::QueuedConnection);
        });
    }
    if (changed) emit clipEffectsChanged();
}

void Backend::finishClipEffectRender(const render::ClipEffectRegion& region,
                                     std::shared_ptr<render::ClipEffectRender> result)
{
    std::erase_if(m_clipEffectJobs, [&](const auto& job) { return job.region == region; });
    if (m_shuttingDown) return;
    if (result->ok) {
        m_clipEffectRenders.insert(region, std::move(result->audio));
    } else if (!result->cancelled) {
        m_clipEffectFailures.emplace_back(region, QString::fromStdString(result->error));
    }
    std::vector<render::ClipEffectRegion> wanted;
    try { wanted = render::clipEffectRegions(m_project); } catch (const std::exception&) {}
    const bool audible = contains(wanted, region);
    for (const auto& waiter : m_clipEffectWaiters)
        wanted.insert(wanted.end(), waiter.regions.begin(), waiter.regions.end());
    m_clipEffectRenders.trim(wanted, kRetainedClipEffectRenders);
    if (audible && result->ok) publishPlan(false, true);
    resumeClipEffectWaiters();
    emit clipEffectsChanged();
}

void Backend::cancelClipEffectRenders(bool all)
{
    std::vector<render::ClipEffectRegion> wanted;
    if (!all) {
        try { wanted = render::clipEffectRegions(m_project); } catch (const std::exception&) {}
        for (const auto& waiter : m_clipEffectWaiters)
            wanted.insert(wanted.end(), waiter.regions.begin(), waiter.regions.end());
    }
    for (const auto& job : m_clipEffectJobs)
        if (all || !contains(wanted, job.region)) job.cancel->store(true, std::memory_order_relaxed);
}

void Backend::awaitClipEffects(DeliverySnapshot snapshot,
                               std::function<void(DeliverySnapshot, QString, bool)> complete)
{
    std::vector<render::ClipEffectRegion> regions;
    try {
        regions = render::clipEffectRegions(snapshot.project);
    } catch (const std::exception& error) {
        complete(std::move(snapshot), tr("Clip effects are inconsistent: %1").arg(QString::fromUtf8(error.what())), false);
        return;
    }
    if (regions.empty()) {
        // Bypassed racks still need their views made dry for every reader.
        snapshot.project = render::applyClipEffects(snapshot.project, m_clipEffectRenders).project;
        complete(std::move(snapshot), {}, false);
        return;
    }
    m_clipEffectWaiters.push_back({std::move(snapshot), std::move(complete), regions});
    requestClipEffectRenders(regions, m_clipEffectWaiters.back().snapshot.project, true);
    resumeClipEffectWaiters();
}

void Backend::resumeClipEffectWaiters()
{
    // Completions may start jobs that queue further waiters; take them first.
    std::vector<std::tuple<DeliverySnapshot, std::function<void(DeliverySnapshot, QString, bool)>, QString, bool>> ready;
    for (auto it = m_clipEffectWaiters.begin(); it != m_clipEffectWaiters.end();) {
        auto& waiter = *it;
        QString error;
        bool stopped = m_shuttingDown || (waiter.snapshot.cancelled && waiter.snapshot.cancelled());
        bool waiting = false;
        if (!stopped) for (const auto& region : waiter.regions) {
            const auto failed = std::find_if(m_clipEffectFailures.begin(), m_clipEffectFailures.end(),
                [&](const auto& failure) { return failure.first == region; });
            if (failed != m_clipEffectFailures.end()) {
                error = tr("A clip effect could not be rendered: %1").arg(failed->second);
                break;
            }
            if (!m_clipEffectRenders.find(region)) waiting = true;
        }
        if (!stopped && error.isEmpty() && waiting) {
            // A render cancelled because the live project moved on is restarted.
            std::vector<render::ClipEffectRegion> missing;
            for (const auto& region : waiter.regions) if (!m_clipEffectRenders.find(region)) missing.push_back(region);
            const bool running = std::all_of(missing.begin(), missing.end(), [&](const auto& region) {
                return std::any_of(m_clipEffectJobs.begin(), m_clipEffectJobs.end(), [&](const auto& job) {
                    return job.region == region && !job.cancel->load(std::memory_order_relaxed); });
            });
            if (!running) requestClipEffectRenders(missing, waiter.snapshot.project);
            ++it;
            continue;
        }
        if (!stopped && error.isEmpty()) {
            try {
                waiter.snapshot.project = render::applyClipEffects(waiter.snapshot.project, m_clipEffectRenders).project;
            } catch (const std::exception& failure) {
                error = tr("Clip effects could not be applied: %1").arg(QString::fromUtf8(failure.what()));
            }
        }
        ready.emplace_back(std::move(waiter.snapshot), std::move(waiter.complete), error, stopped);
        it = m_clipEffectWaiters.erase(it);
    }
    for (auto& [snapshot, complete, error, stopped] : ready)
        complete(std::move(snapshot), error, stopped);
}

QString Backend::clipEffectsProblem() const
{
    std::vector<render::ClipEffectRegion> current;
    try { current = render::clipEffectRegions(m_project); } catch (const std::exception&) {
        return tr("A clip effect region does not match its clip.");
    }
    for (const auto& [region, message] : m_clipEffectFailures)
        if (contains(current, region)) return message;
    return {};
}

QVariantMap Backend::clipEffectsTarget(int track, int clip)
{
    const auto missing = [this] { return QVariantMap{{"valid", false},
        {"reason", tr("Choose a clip to see its effects.")}}; };
    if (track == -1 && clip == -1) {
        const auto reference = clipProperties();
        if (!reference.value("valid").toBool()) return missing();
        track = reference.value("track").toInt(); clip = reference.value("clip").toInt();
    }
    if (track < 0 || clip < 0) return missing();
    const auto* lane = m_project.track(session::TrackId(track));
    if (!lane || std::size_t(clip) >= lane->clips.size()) return missing();
    const auto& target = lane->clips[std::size_t(clip)];
    const auto* source = m_project.source(target.source);
    if (!source) return missing();
    const unsigned channels = std::max<unsigned>(1, source->audio.channels());

    QString status = QStringLiteral("none"), problem;
    int views = 0;
    if (target.rack) {
        for (const auto& t : m_project.tracks())
            for (const auto& c : t.clips) views += c.rack && c.rack->owner == target.rack->owner ? 1 : 0;
        if (!render::clipEffectsActive(*target.rack)) status = QStringLiteral("bypassed");
        else {
            const render::ClipEffectRegion region{target.rack, target.timelineStart - target.effectOffset, channels,
                                                  m_project.sampleRate(), m_project.musicalTime()};
            const auto failed = std::find_if(m_clipEffectFailures.begin(), m_clipEffectFailures.end(),
                [&](const auto& failure) { return failure.first == region; });
            if (m_clipEffectRenders.find(region)) status = QStringLiteral("ready");
            else if (failed != m_clipEffectFailures.end()) { status = QStringLiteral("failed"); problem = failed->second; }
            else status = QStringLiteral("rendering");
        }
    }

    QVariantList effects;
    if (target.rack) {
        if (!m_scanning && !m_lv2->scanned()) m_lv2->scan();
        for (std::size_t slot = 0; slot < target.rack->inserts.size(); ++slot) {
            const auto& insert = target.rack->inserts[slot];
            const auto* info = m_scanning ? nullptr : m_lv2->find(insert.uri);
            QVariantMap effect{{"slot", int(slot)}, {"uri", QString::fromStdString(insert.uri)},
                {"name", info ? QString::fromStdString(info->name) : QString::fromStdString(insert.uri)},
                {"bypass", insert.bypass}, {"available", info != nullptr},
                {"automationTiming", automationTiming(info)}};
            QVariantList controls;
            const auto key = std::make_pair(insert.uri, channels);
            auto described = m_clipEffectPorts.find(key);
            if (described == m_clipEffectPorts.end() && info && !m_scanning) {
                std::string error;
                auto probe = host::createInstance(*m_lv2, insert.uri, hostFeatures(), error, insert.legacyClapControls,
                    insert.instanceState.empty() ? nullptr : &insert.instanceState.front(), {channels});
                QVariantList ports;
                if (probe) {
                    const auto* plugin = m_lv2->lilvPlugin(insert.uri);
                    for (const lv2::ControlPort& port : probe->controls()) {
                        if (port.output) continue;
                        QVariantMap entry = pluginControlPresentation(m_lv2->raw(), plugin,
                            plugin ? lilv_plugin_get_port_by_index(plugin, port.index) : nullptr, port.minimum, port.maximum);
                        entry["index"] = qint64(port.index);
                        entry["symbol"] = QString::fromStdString(port.symbol);
                        entry["name"] = QString::fromStdString(port.name);
                        entry["min"] = double(port.minimum);
                        entry["max"] = double(port.maximum);
                        entry["defaultValue"] = double(port.defaultValue);
                        entry["toggled"] = port.toggled;
                        entry["integer"] = port.integer;
                        ports.append(entry);
                    }
                } else effect["problem"] = QString::fromStdString(error);
                described = m_clipEffectPorts.emplace(key, std::move(ports)).first;
            }
            if (described != m_clipEffectPorts.end()) for (auto port : described->second) {
                auto entry = port.toMap();
                const auto index = std::uint32_t(entry.value("index").toLongLong());
                const auto* stored = insert.control(index);
                entry["value"] = stored ? stored->value : entry.value("defaultValue").toDouble();
                QVariantList points;
                if (stored) for (const auto& point : stored->automation.points) {
                    // Effect curves belong to the region; show them in this view's frames.
                    points.append(QVariantMap{{"frame", qint64(point.frame - target.effectOffset)},
                        {"value", point.value}, {"interpolation", int(point.interpolation)}});
                }
                entry["automated"] = stored && stored->automation.hasData();
                entry["automationEnabled"] = stored && stored->automation.enabled;
                entry["points"] = points;
                controls.append(entry);
            }
            effect["controls"] = controls;
            effects.append(effect);
        }
    }
    return {{"valid", true}, {"reason", QString()}, {"track", track}, {"clip", clip},
        {"name", QString::fromStdString(source->name)}, {"trackName", QString::fromStdString(lane->name)},
        {"start", qint64(target.timelineStart)}, {"length", qint64(target.length)},
        {"channels", int(channels)}, {"views", views}, {"status", status}, {"problem", problem},
        {"stamp", automationStamp()}, {"effects", effects}};
}

bool Backend::replaceClipRack(int track, int clip, const QString& stamp, const std::string& label,
                              const std::function<bool(std::vector<session::Insert>&, QString&)>& change)
{
    const auto reject = [this](const QString& message) { setError(message); return false; };
    if (stamp.isEmpty() || stamp != automationStamp())
        return reject(tr("The clip or project changed. Reload its effects before editing."));
    if (m_shuttingDown || recording() || loading() || sampleEditing() || m_repairing || storageBusy() || m_clipMove)
        return reject(tr("Finish recording or the current clip edit first."));
    auto* lane = m_project.track(session::TrackId(track));
    if (!lane || clip < 0 || std::size_t(clip) >= lane->clips.size())
        return reject(tr("Choose an existing clip for its effects."));
    const auto& current = lane->clips[std::size_t(clip)];
    if (!current.hasValidEffects()) return reject(tr("The clip's effect region is invalid."));
    std::vector<session::Insert> inserts = current.rack ? current.rack->inserts : std::vector<session::Insert>{};
    QString error;
    if (!change(inserts, error)) return reject(error);
    if (current.rack && session::sameInsertDescriptors(current.rack->inserts, inserts)) { setError({}); return true; }
    if (!current.rack && inserts.empty()) { setError({}); return true; }
    if (const auto deferred = deferAutomationEdit([this, track, clip, stamp, label, change] {
            return replaceClipRack(track, clip, stamp, label, change);
        })) return *deferred;
    setError({});
    const auto before = captureEditState();
    auto& installed = m_project.track(session::TrackId(track))->clips[std::size_t(clip)];
    if (!installed.rack) {
        try {
            installed.rack = session::makeClipRack(installed, std::move(inserts));
            installed.effectOffset = 0;
        } catch (const std::exception& failure) {
            return reject(QString::fromUtf8(failure.what()));
        }
    } else if (inserts.empty()) {
        // No effects left: every view of the region plays its own audio again.
        const auto owner = installed.rack->owner;
        for (auto& t : m_project.tracks()) for (auto& c : t.clips)
            if (c.rack && c.rack->owner == owner) { c.rack.reset(); c.effectOffset = 0; }
    } else {
        auto edit = session::prepareEffectRackEdit(m_project, installed.rack->owner);
        if (!edit) return reject(tr("The clip's effects changed. Reload them before editing."));
        edit->inserts = std::move(inserts);
        std::string reason;
        if (!session::replaceEffectRack(m_project, *edit, reason)) {
            session::restoreState(m_project, before);
            return reject(QString::fromStdString(reason));
        }
    }
    for (auto& selected : m_selectedClips) {
        const auto* t = m_project.track(selected.track);
        if (t && selected.index < t->clips.size() && t->clips[selected.index].identity == selected.expected.identity)
            selected.expected = t->clips[selected.index];
    }
    commit(label, session::TrackId(track), before);
    emit clipEffectsChanged();
    return m_lastError.isEmpty();
}

bool Backend::addClipEffect(int track, int clip, const QString& uri, const QString& stamp)
{
    if (m_scanning) { setError(tr("Wait until the effect catalogue has been read.")); return false; }
    if (m_pluginLibrary.disabled().contains(uri)) {
        setError(tr("This effect is disabled for new inserts. Enable it in the Plug-in Manager."));
        return false;
    }
    if (!m_lv2->scanned()) m_lv2->scan();
    const auto* info = m_lv2->find(uri.toStdString());
    if (!info) { setError(tr("Effect not installed: %1").arg(uri)); return false; }
    if (!info->usable) {
        setError(tr("%1 cannot be used here: %2").arg(QString::fromStdString(info->name),
                                                     QString::fromStdString(info->unusableReason)));
        return false;
    }
    const auto* lane = m_project.track(session::TrackId(track));
    const auto* source = lane && clip >= 0 && std::size_t(clip) < lane->clips.size()
        ? m_project.source(lane->clips[std::size_t(clip)].source) : nullptr;
    const auto channels = source ? std::max(1u, source->audio.channels()) : 1u;
    // A mono effect runs once per channel; a stereo-only one makes a mono clip stereo.
    if (!info->fits(channels) && !info->fits(1) && !(channels == 1 && info->fits(2))) {
        setError(tr("%1 does not fit this clip's %2 channels.").arg(QString::fromStdString(info->name)).arg(channels));
        return false;
    }
    const bool added = replaceClipRack(track, clip, stamp, "add clip effect " + info->name,
        [&](std::vector<session::Insert>& inserts, QString& error) {
            if (inserts.size() >= engine::ClipEffectRuntime::maxSlots) { error = tr("This clip has the maximum number of effects."); return false; }
            session::Insert insert; insert.uri = uri.toStdString();
            inserts.push_back(std::move(insert));
            return true;
        });
    if (added) {
        if (const auto error = m_pluginLibrary.remember(uri); error.isEmpty()) emit pluginLibraryChanged();
    }
    return added;
}

bool Backend::addEffectToSelectedClips(const QString& uri, const QString& stamp)
{
    if (m_selectedClips.empty()) { setError(tr("Select one or more clips first.")); return false; }
    // Each clip keeps its own region; equal settings are not shared processing.
    const auto targets = m_selectedClips;
    for (const auto& selected : targets) {
        const auto* t = m_project.track(selected.track);
        if (!t || selected.index >= t->clips.size()) continue;
        if (!addClipEffect(int(selected.track), int(selected.index), uri, automationStamp())) return false;
    }
    (void)stamp;
    return true;
}

bool Backend::removeClipEffect(int track, int clip, int slot, const QString& stamp)
{
    return replaceClipRack(track, clip, stamp, "remove clip effect",
        [&](std::vector<session::Insert>& inserts, QString& error) {
            if (slot < 0 || std::size_t(slot) >= inserts.size()) { error = tr("That clip effect no longer exists."); return false; }
            inserts.erase(inserts.begin() + slot);
            return true;
        });
}

bool Backend::moveClipEffect(int track, int clip, int from, int to, const QString& stamp)
{
    return replaceClipRack(track, clip, stamp, "move clip effect",
        [&](std::vector<session::Insert>& inserts, QString& error) {
            if (from < 0 || to < 0 || std::size_t(from) >= inserts.size() || std::size_t(to) >= inserts.size()) {
                error = tr("That clip effect position does not exist."); return false;
            }
            auto moved = std::move(inserts[std::size_t(from)]);
            inserts.erase(inserts.begin() + from);
            inserts.insert(inserts.begin() + to, std::move(moved));
            return true;
        });
}

bool Backend::setClipEffectBypass(int track, int clip, int slot, bool bypass, const QString& stamp)
{
    return replaceClipRack(track, clip, stamp, bypass ? "bypass clip effect" : "enable clip effect",
        [&](std::vector<session::Insert>& inserts, QString& error) {
            if (slot < 0 || std::size_t(slot) >= inserts.size()) { error = tr("That clip effect no longer exists."); return false; }
            inserts[std::size_t(slot)].bypass = bypass;
            return true;
        });
}

bool Backend::setClipEffectControl(int track, int clip, int slot, qint64 port, double value, const QString& stamp)
{
    if (!std::isfinite(value) || port < 0 || port > std::numeric_limits<std::uint32_t>::max()) {
        setError(tr("Enter a finite value for an existing control.")); return false;
    }
    return replaceClipRack(track, clip, stamp, "clip effect control",
        [&](std::vector<session::Insert>& inserts, QString& error) {
            if (slot < 0 || std::size_t(slot) >= inserts.size()) { error = tr("That clip effect no longer exists."); return false; }
            inserts[std::size_t(slot)].setControl(std::uint32_t(port), value);
            return true;
        });
}

bool Backend::editClipEffectAutomation(int track, int clip, int slot, qint64 port, const QString& operation,
                                       qint64 frame, qint64 to, double value, const QString& stamp)
{
    const auto* lane = m_project.track(session::TrackId(track));
    if (!lane || clip < 0 || std::size_t(clip) >= lane->clips.size()) {
        setError(tr("Choose an existing clip for its effects.")); return false;
    }
    const auto view = lane->clips[std::size_t(clip)];
    if (!std::isfinite(value) || port < 0 || port > std::numeric_limits<std::uint32_t>::max()) {
        setError(tr("Enter a finite value for an existing control.")); return false;
    }
    const auto local = [&](qint64 position) { return position >= 0 && position <= view.length; };
    return replaceClipRack(track, clip, stamp, "clip effect automation",
        [&](std::vector<session::Insert>& inserts, QString& error) {
            if (slot < 0 || std::size_t(slot) >= inserts.size()) { error = tr("That clip effect no longer exists."); return false; }
            auto& insert = inserts[std::size_t(slot)];
            if (!insert.control(std::uint32_t(port))) insert.setControl(std::uint32_t(port), value);
            auto control = std::find_if(insert.controls.begin(), insert.controls.end(),
                [&](const auto& c) { return c.port == std::uint32_t(port); });
            auto& curve = control->automation;
            // Region frames: a curve is shared by every view of the region.
            const auto at = [&](qint64 position) { return session::FramePos(view.effectOffset + position); };
            if (operation == "point") {
                if (!local(to) || frame < -1 || (frame >= 0 && !local(frame))) {
                    error = tr("The point must be within the clip."); return false;
                }
                if (frame >= 0 && frame != to && !curve.erasePoint(at(frame))) {
                    error = tr("The selected automation point no longer exists."); return false;
                }
                if (!curve.setPoint(at(to), value)) { error = tr("The automation has reached its point limit."); return false; }
                curve.enabled = true;
            } else if (operation == "delete") {
                if (!local(frame) || !curve.erasePoint(at(frame))) { error = tr("Choose an automation point within the clip to remove."); return false; }
            } else if (operation == "enabled") {
                if (value != 0 && value != 1) { error = tr("Use an explicit off or on value."); return false; }
                curve.enabled = value == 1;
            } else if (operation == "clear") {
                curve = {};
            } else { error = tr("Unknown clip effect automation operation."); return false; }
            return true;
        });
}

bool Backend::clearClipEffects(int track, int clip, const QString& stamp)
{
    return replaceClipRack(track, clip, stamp, "clear clip effects",
        [](std::vector<session::Insert>& inserts, QString&) { inserts.clear(); return true; });
}

bool Backend::bakeClipEffects(int track, int clip, const QString& stamp)
{
    const auto reject = [this](const QString& message) { setError(message); return false; };
    if (stamp.isEmpty() || stamp != automationStamp())
        return reject(tr("The clip or project changed. Reload its effects before editing."));
    if (m_shuttingDown || recording() || loading() || sampleEditing() || m_repairing || storageBusy() || m_clipMove)
        return reject(tr("Finish recording or the current clip edit first."));
    auto* lane = m_project.track(session::TrackId(track));
    if (!lane || clip < 0 || std::size_t(clip) >= lane->clips.size())
        return reject(tr("Choose an existing clip to bake its effects."));
    const auto view = lane->clips[std::size_t(clip)];
    if (!view.rack) return reject(tr("This clip has no effects to bake."));
    if (!render::clipEffectsActive(*view.rack)) return clearClipEffects(track, clip, stamp);
    const auto* source = m_project.source(view.source);
    const render::ClipEffectRegion region{view.rack, view.timelineStart - view.effectOffset,
        source ? std::max(1u, source->audio.channels()) : 1u, m_project.sampleRate(), m_project.musicalTime()};
    const auto* audio = m_clipEffectRenders.find(region);
    if (!audio) {
        requestClipEffectRenders({region}, m_project, true);
        return reject(tr("The clip's effects are still rendering. Bake again when they are ready."));
    }
    if (const auto deferred = deferAutomationEdit([this, track, clip, stamp] {
            return bakeClipEffects(track, clip, stamp);
        })) return *deferred;
    setError({});
    const auto before = captureEditState();
    const auto baked = m_project.addSource((source ? source->name : std::string("Clip")) + " (baked effects)",
                                           {}, *audio, std::nullopt, session::Uuid{}, std::nullopt, m_project.sampleRate());
    auto& installed = m_project.track(session::TrackId(track))->clips[std::size_t(clip)];
    installed.source = baked;
    installed.sourceOffset = installed.effectOffset;
    installed.elastic.reset();
    installed.rack.reset();
    installed.effectOffset = 0;
    installed.forkIdentity();
    commit("bake clip effects", session::TrackId(track), before);
    markPeaksDirty(session::TrackId(track));
    pumpPeaks();
    emit clipEffectsChanged();
    return m_lastError.isEmpty();
}
