// Tidy pauses: find the gaps between words on the selection, a track or every
// voice track, then shorten, remove or mark them, or split the speech at them.
//
// Finding reads the tracks' own clips (without clip effects, before the faders),
// in ten-second blocks, as 10 ms window levels; render::findPauses does the rest.
// With several voice tracks a pause is where all of them are quiet at once.
// Nothing changes until applyPauses, which is one Undo step. Shortening and
// removing take time out of the tracks looked at (and, if asked, the other
// non-music tracks); music tracks never move. Each join gets a few milliseconds
// of fade either side: a join is always in quiet audio, so a dip there is not
// heard, and it stops a click. Markers move with the voices.
#include "backend.h"
#include "render/pauses.h"
#include "session/edits.h"
#include "session/render.h"

#include <algorithm>
#include <cmath>

namespace {

using omawave::session::FramePos;

bool isVoice(omawave::session::TrackRole role)
{
    using omawave::session::TrackRole;
    return role == TrackRole::Dialogue || role == TrackRole::Host || role == TrackRole::Guest;
}

}  // namespace

QVariantMap Backend::findPauses(const QVariantMap& spec)
{
    if (m_loading || recording() || sampleEditing() || m_repairing || m_recipeRunner) {
        m_pauseFind.reset();
        emit pausesChanged();
        setError(tr("Finish recording or the current edit first."));
        return {{"valid", false}, {"reason", m_lastError}};
    }
    return scanPauses(spec);
}

QVariantMap Backend::scanPauses(const QVariantMap& spec)
{
    const auto fail = [this](const QString& reason) {
        m_pauseFind.reset();
        emit pausesChanged();
        setError(reason);
        return QVariantMap{{"valid", false}, {"reason", reason}};
    };
    const auto scope = spec.value("scope", "track").toString();
    PauseFind find;
    find.scope = scope;
    if (scope == QLatin1String("voices")) {
        for (const auto& t : m_project.tracks())
            if (isVoice(t.role) && !t.clips.empty()) find.tracks.push_back(t.id);
        if (find.tracks.empty()) return fail(tr("No track is a voice track. Set a track's kind to Voice in the Sound tab."));
        find.start = 0;
        for (const auto id : find.tracks) find.end = std::max(find.end, m_project.track(id)->length());
    } else {
        const auto* track = m_project.track(m_focused);
        if (!track || track->clips.empty()) return fail(tr("Click a track with audio first."));
        find.tracks = {track->id};
        if (scope == QLatin1String("selection")) {
            if (m_selection.empty() || m_selection.track != m_focused)
                return fail(tr("Select a time range on a track first: drag across its waveform."));
            find.start = m_selection.start; find.end = m_selection.end;
        } else if (scope == QLatin1String("track")) {
            find.start = 0; find.end = track->length();
        } else return fail(tr("Look in the selection, this track or every voice track."));
        // withVoices: the other voice tracks are looked at too, so a pause is where
        // they are all quiet and moving them never cuts their words.
        if (spec.value("withVoices", false).toBool())
            for (const auto& t : m_project.tracks())
                if (t.id != track->id && isVoice(t.role) && !t.clips.empty()) find.tracks.push_back(t.id);
    }
    const auto number = [&](const char* key, double fallback, double low, double high) {
        bool ok = false;
        const double v = spec.value(QLatin1String(key)).toDouble(&ok);
        return ok && std::isfinite(v) ? std::clamp(v, low, high) : fallback;
    };
    find.autoThreshold = spec.value("auto", true).toBool();
    find.minSeconds = number("minSeconds", 0.5, 0.05, 30);
    find.paddingSeconds = number("paddingSeconds", 0.1, 0, 2);
    find.keepBreaths = spec.value("keepBreaths", true).toBool();

    // The levels: the loudest track in each window.
    const std::uint32_t rate = m_project.sampleRate();
    const std::uint32_t window = std::max<std::uint32_t>(1, rate / 100);
    const FramePos block = FramePos(window) * 1000;          // ten seconds at any rate
    std::vector<float> levels;
    for (std::size_t n = 0; n < find.tracks.size(); ++n) {
        const auto* track = m_project.track(find.tracks[n]);
        const auto channels = std::uint32_t(std::max(1, trackChannels(*track)));
        std::vector<float> mine;
        for (FramePos at = find.start; at < find.end; at += block) {
            const auto to = std::min(find.end, at + block);
            auto clips = omawave::session::edits::copyRange(track->clips, at, to);
            for (auto& clip : clips) { clip.rack.reset(); clip.effectOffset = 0; }
            auto audio = omawave::session::renderClipRange(m_project, clips, 0, to - at, channels);
            audio.resize(channels, std::size_t(to - at));
            std::vector<const float*> planes;
            for (std::uint32_t c = 0; c < channels; ++c) planes.push_back(audio.plane(c).data());
            omawave::render::windowLevels(planes.data(), channels, std::size_t(to - at), window, mine);
        }
        if (n == 0) levels = std::move(mine);
        else for (std::size_t i = 0; i < std::min(levels.size(), mine.size()); ++i) levels[i] = std::max(levels[i], mine[i]);
    }
    omawave::render::PauseSettings settings;
    settings.autoThreshold = find.autoThreshold;
    settings.thresholdDb = number("thresholdDb", -44, -90, 0);
    settings.minSeconds = find.minSeconds;
    settings.keepBreaths = find.keepBreaths;
    const auto scan = omawave::render::findPauses(levels, find.start, find.end, window, rate, settings);
    find.pauses = scan.pauses;
    find.kept.assign(find.pauses.size(), false);
    find.thresholdDb = scan.thresholdDb;
    find.noiseFloorDb = scan.noiseFloorDb;
    find.stamp = automationStamp();
    m_pauseFind = std::move(find);
    setError({});
    emit pausesChanged();
    return pauses();
}

QVariantMap Backend::pauses() const
{
    if (!m_pauseFind) return {{"valid", false}};
    const auto& f = *m_pauseFind;
    const double rate = m_project.sampleRate();
    QVariantList list, ids, names;
    double seconds = 0;
    int kept = 0;
    for (std::size_t i = 0; i < f.pauses.size(); ++i) {
        const double length = double(f.pauses[i].second - f.pauses[i].first) / rate;
        seconds += length;
        kept += f.kept[i];
        list.append(QVariantMap{{"index", int(i)}, {"start", qint64(f.pauses[i].first)}, {"end", qint64(f.pauses[i].second)},
                                {"seconds", length}, {"kept", bool(f.kept[i])}});
    }
    for (const auto id : f.tracks) {
        ids.append(int(id));
        const auto* t = m_project.track(id);
        names.append(t ? QString::fromStdString(t->name) : QString());
    }
    return {{"valid", true}, {"stale", f.stamp != automationStamp()}, {"scope", f.scope}, {"tracks", ids}, {"trackNames", names},
            {"start", qint64(f.start)}, {"end", qint64(f.end)}, {"thresholdDb", f.thresholdDb}, {"noiseFloorDb", f.noiseFloorDb},
            {"auto", f.autoThreshold}, {"minSeconds", f.minSeconds}, {"paddingSeconds", f.paddingSeconds},
            {"keepBreaths", f.keepBreaths}, {"count", int(f.pauses.size())}, {"seconds", seconds}, {"kept", kept}, {"pauses", list}};
}

bool Backend::setPauseKept(int index, bool kept)
{
    if (!m_pauseFind || index < 0 || std::size_t(index) >= m_pauseFind->kept.size()) {
        setError(tr("There is no such pause.")); return false;
    }
    m_pauseFind->kept[std::size_t(index)] = kept;
    emit pausesChanged();
    return true;
}

void Backend::clearPauses()
{
    if (!m_pauseFind) return;
    m_pauseFind.reset();
    emit pausesChanged();
}

void Backend::clearPauseResult()
{
    if (m_pauseResult.isEmpty()) return;
    m_pauseResult.clear();
    emit pausesChanged();
}

Backend::PausePlan Backend::planPauses(const QVariantMap& action) const
{
    PausePlan plan;
    if (!m_pauseFind) { plan.reason = tr("Find the pauses first."); return plan; }
    const auto& f = *m_pauseFind;
    if (f.stamp != automationStamp()) { plan.reason = tr("The project changed. Find pauses again."); return plan; }
    const auto then = action.value("then", "shorten").toString();
    const double rate = m_project.sampleRate();
    const auto frames = [&](double seconds) { return FramePos(std::llround(seconds * rate)); };
    const FramePos pad = frames(f.paddingSeconds);
    const FramePos keep = then == QLatin1String("remove") ? 2 * pad
        : std::max(2 * pad, frames(std::clamp(action.value("shortenTo", 0.4).toDouble(), 0.0, 30.0)));
    for (std::size_t i = 0; i < f.pauses.size(); ++i) {
        if (f.kept[i]) continue;
        const auto [a, b] = f.pauses[i];
        if (then == QLatin1String("shorten") || then == QLatin1String("remove")) {
            if (b - a <= keep) continue;                 // already this short
            const FramePos from = a + keep / 2, to = b - (keep - keep / 2);
            plan.removals.emplace_back(from, to);
            plan.removed += to - from;
        } else if (then != QLatin1String("mark") && then != QLatin1String("split")) {
            plan.reason = tr("Choose to shorten, remove, mark or split."); return plan;
        }
        plan.changed.push_back(i);
    }
    // What moves: the tracks looked at, and with moveOthers every other track but
    // music (and buses, which hold no audio). Music plays on.
    plan.moving = f.tracks;
    if (action.value("moveOthers", true).toBool())
        for (const auto& t : m_project.tracks())
            if (std::find(plan.moving.begin(), plan.moving.end(), t.id) == plan.moving.end() && !t.clips.empty()
                && t.role != omawave::session::TrackRole::Music)
                plan.moving.push_back(t.id);
    for (const auto id : f.tracks)
        if (const auto* t = m_project.track(id)) plan.lengthBefore = std::max(plan.lengthBefore, t->length());
    return plan;
}

QVariantMap Backend::pausePlan(const QVariantMap& action) const
{
    const auto plan = planPauses(action);
    if (!plan.reason.isEmpty()) return {{"valid", false}, {"reason", plan.reason}};
    const auto then = action.value("then", "shorten").toString();
    const auto& f = *m_pauseFind;
    const double rate = m_project.sampleRate();
    QVariantList joins, changed, lengths;
    if (!plan.removals.empty()) for (const auto& r : plan.removals) joins.append(qint64(r.first));
    else for (const auto i : plan.changed) joins.append(qint64((f.pauses[i].first + f.pauses[i].second) / 2));
    for (std::size_t n = 0; n < plan.changed.size(); ++n) {
        const auto [a, b] = f.pauses[plan.changed[n]];
        const auto after = (b - a) - (plan.removals.empty() ? 0 : plan.removals[n].second - plan.removals[n].first);
        changed.append(int(plan.changed[n]));
        lengths.append(double(after) / rate);
    }
    return {{"valid", true}, {"changes", int(plan.changed.size())}, {"changed", changed}, {"lengths", lengths},
            {"lengthBefore", qint64(plan.lengthBefore)},
            {"lengthAfter", qint64(plan.lengthBefore - plan.removed)},
            {"removedSeconds", double(plan.removed) / m_project.sampleRate()}, {"joins", joins}};
}

bool Backend::applyPauses(const QVariantMap& action)
{
    if (const auto deferred = deferAutomationEdit([this, action] { return applyPauses(action); })) return *deferred;
    if (m_loading || recording() || automationWriting() || sampleEditing() || m_repairing || m_recipeRunner || m_clipMove) {
        setError(tr("Finish recording or the current edit first.")); return false;
    }
    const double minSeconds = m_pauseFind ? m_pauseFind->minSeconds : 0.5;
    if (!tidyPauses(action)) return false;
    // Record steps: shortening or removing becomes a Shorten pauses step (Remove is
    // shortening to nothing but the air each side).
    const auto then = action.value("then", "shorten").toString();
    if (then == QLatin1String("shorten") || then == QLatin1String("remove"))
        recordStep({{"kind", "pauses"}, {"on", true},
                    {"shortenTo", then == QLatin1String("remove") ? 0.0 : std::clamp(action.value("shortenTo", 0.4).toDouble(), 0.0, 3.0)},
                    {"minSeconds", minSeconds}, {"moveOthers", action.value("moveOthers", true).toBool() ? 1 : 0}});
    return true;
}

bool Backend::tidyPauses(const QVariantMap& action)
{
    const auto plan = planPauses(action);
    if (!plan.reason.isEmpty()) { setError(plan.reason); return false; }
    if (plan.changed.empty()) { setError(tr("No pause is long enough to change.")); return false; }
    const auto& f = *m_pauseFind;
    const auto then = action.value("then", "shorten").toString();
    const double rate = m_project.sampleRate();
    const int count = int(plan.changed.size());
    const auto before = captureEditState();
    QVariantList joins;
    QString label;
    int moved = 0;
    const double smoothMs = std::clamp(action.value("smoothMs", 10.0).toDouble(), 0.0, 200.0);

    if (then == QLatin1String("shorten") || then == QLatin1String("remove")) {
        if (!omawave::session::edits::removeRangesOnTracks(m_project, plan.moving, plan.removals, true)) {
            setError(tr("These pauses cannot be taken out: an automation limit would be passed.")); return false;
        }
        // Where each join is now, and a short fade either side of it.
        const auto half = FramePos(std::llround(smoothMs / 2000 * rate));
        FramePos gone = 0;
        for (const auto& [from, to] : plan.removals) {
            const FramePos join = from - gone;
            gone += to - from;
            joins.append(qint64(join));
            if (half <= 0) continue;
            for (const auto id : plan.moving) {
                auto* track = m_project.track(id);
                if (!track) continue;
                omawave::session::edits::applyFadeOut(track->clips, std::max<FramePos>(0, join - half), join);
                omawave::session::edits::applyFadeIn(track->clips, join, join + half);
            }
        }
        moved = int(plan.moving.size());
        label = then == QLatin1String("shorten") ? tr("shorten %1 pauses").arg(count) : tr("remove %1 pauses").arg(count);
    } else if (then == QLatin1String("mark")) {
        const auto as = action.value("markAs", "marker").toString();
        int n = 0;
        const auto add = [&](FramePos at, FramePos length, const QString& name) {
            omawave::session::Marker marker;
            marker.name = name.toStdString();
            marker.position = at;
            marker.length = length;
            marker.type = length > 0 ? omawave::session::MarkerType::Range : omawave::session::MarkerType::Cue;
            m_project.addMarker(marker);
            joins.append(qint64(at));
        };
        if (as == QLatin1String("speech")) {
            FramePos from = f.start;
            for (const auto i : plan.changed) {
                const auto [a, b] = f.pauses[i];
                if (a > from) add(from, a - from, tr("Speech %1").arg(++n));
                from = b;
            }
            if (f.end > from) add(from, f.end - from, tr("Speech %1").arg(++n));
        } else {
            for (const auto i : plan.changed) {
                const auto [a, b] = f.pauses[i];
                if (as == QLatin1String("marker")) add((a + b) / 2, 0, tr("Pause %1").arg(++n));
                else add(a, b - a, tr("Pause %1").arg(++n));
            }
        }
        label = tr("mark %1 pauses").arg(count);
    } else {
        // Split: each stretch of speech its own clip, the pauses left empty.
        const auto pad = FramePos(std::llround(f.paddingSeconds * rate));
        for (const auto id : f.tracks) {
            auto* track = m_project.track(id);
            if (!track) continue;
            for (const auto i : plan.changed) {
                const auto [a, b] = f.pauses[i];
                if (b - a > 2 * pad) omawave::session::edits::clearRange(track->clips, a + pad, b - pad);
            }
        }
        for (const auto i : plan.changed) joins.append(qint64(f.pauses[i].first));
        moved = 0;
        label = tr("split at %1 pauses").arg(count);
    }

    const int kept = int(std::count(f.kept.begin(), f.kept.end(), true));
    const int total = int(f.pauses.size());
    const auto lengthBefore = plan.lengthBefore;
    setError({});
    commit(label.toStdString(), m_focused, before);
    if (!m_lastError.isEmpty()) return false;
    m_pauseFind.reset();
    m_pauseResult = {{"then", then}, {"changed", count}, {"kept", kept}, {"total", total},
        {"lengthBefore", qint64(lengthBefore)}, {"lengthAfter", qint64(lengthBefore - plan.removed)},
        {"removedSeconds", double(plan.removed) / rate}, {"smoothMs", smoothMs}, {"moved", moved},
        {"joins", joins}, {"undoLabel", label}};
    setError({});
    emit pausesChanged();
    return true;
}

// A recipe's Shorten pauses step, on the range the recipe runs on (already
// selected): find the pauses there, shorten them, and report. Done at once, so the
// runner's next tick finds the history changed (or not) and moves on. The range the
// later steps work on ends earlier by what was taken out.
void Backend::runPauseStep(const QVariantMap& step)
{
    auto& run = *m_recipeRunner;
    const auto value = [&](const char* key, double fallback, double low, double high) {
        bool ok = false;
        const double v = step.value(QLatin1String(key)).toDouble(&ok);
        return ok && std::isfinite(v) ? std::clamp(v, low, high) : fallback;
    };
    const bool moveOthers = value("moveOthers", 0, 0, 1) >= 0.5;
    const auto found = scanPauses({{"scope", "selection"}, {"auto", true}, {"minSeconds", value("minSeconds", 0.5, 0.2, 5)},
                                   {"keepBreaths", true}, {"withVoices", moveOthers}});
    if (!found.value("valid").toBool()) { endRecipeStep(false, {}, found.value("reason").toString()); return; }
    const QVariantMap action{{"then", "shorten"}, {"shortenTo", value("shortenTo", 0.4, 0, 3)}, {"moveOthers", moveOthers}, {"smoothMs", 10}};
    const auto plan = planPauses(action);
    if (plan.changed.empty()) {
        clearPauses();
        endRecipeStep(true, tr("No long pauses"), {});
        return;
    }
    if (!tidyPauses(action)) { clearPauses(); endRecipeStep(false, {}, m_lastError); return; }
    const double removed = double(plan.removed) / m_project.sampleRate();
    run.stepResult = plan.changed.size() == 1 ? tr("1 pause, %1 s shorter").arg(removed, 0, 'f', 1)
                                              : tr("%1 pauses, %2 s shorter").arg(plan.changed.size()).arg(removed, 0, 'f', 1);
    run.end = std::max(run.start, run.end - plan.removed);
    if (run.target == QLatin1String("selection") && !run.selectionBefore.empty())
        run.selectionBefore.end = std::max(run.selectionBefore.start, run.selectionBefore.end - plan.removed);
    clearPauseResult();
    setError({});
}
