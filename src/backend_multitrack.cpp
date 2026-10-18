#include "backend.h"
#include <QScopeGuard>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace {
bool selectionIndex(const QVariant& value, int& number)
{
    const auto type = value.metaType().id();
    const bool numeric = type == QMetaType::Int || type == QMetaType::UInt || type == QMetaType::LongLong
        || type == QMetaType::ULongLong || type == QMetaType::Double;
    const double raw = value.toDouble();
    if (!numeric || !std::isfinite(raw) || raw < 0 || raw > std::numeric_limits<int>::max() || std::floor(raw) != raw)
        return false;
    number = int(raw); return true;
}
}

bool Backend::selectedClipsCurrent() const
{
    return std::all_of(m_selectedClips.begin(), m_selectedClips.end(), [this](const auto& address) {
        const auto* track = m_project.track(address.track);
        return track && address.index < track->clips.size() && track->clips[address.index].sameDescriptor(address.expected);
    });
}

QVariantList Backend::selectedClips() const
{
    QVariantList clips;
    for (const auto& address : m_selectedClips)
        clips.push_back(QVariantMap{{"track", int(address.track)}, {"clip", int(address.index)},
            {"start", qint64(address.expected.timelineStart)}, {"end", qint64(address.expected.timelineEnd())},
            {"group", address.expected.group == session::kInvalidClipGroup ? -1 : int(address.expected.group)}});
    return clips;
}

void Backend::applyClipSelection(std::vector<session::ClipAddress> clips, session::TrackId focus, std::size_t focusedClip)
{
    m_selectedClips = std::move(clips); m_selectedTracks.clear();
    if (m_project.track(focus)) m_focused = focus;
    std::sort(m_selectedClips.begin(), m_selectedClips.end(), [this](const auto& a, const auto& b) {
        const auto ai = m_project.trackIndex(a.track), bi = m_project.trackIndex(b.track);
        return ai == bi ? a.index < b.index : ai < bi;
    });
    for (const auto& clip : m_selectedClips)
        if (m_selectedTracks.empty() || m_selectedTracks.back() != clip.track) m_selectedTracks.push_back(clip.track);
    auto anchor = std::find_if(m_selectedClips.begin(), m_selectedClips.end(), [&](const auto& clip) {
        return clip.track == focus && clip.index == focusedClip;
    });
    if (anchor == m_selectedClips.end() && !m_selectedClips.empty()) anchor = m_selectedClips.begin();
    if (anchor != m_selectedClips.end()) {
        m_focused = anchor->track;
        m_selection = {anchor->expected.timelineStart, anchor->expected.timelineEnd(), m_focused};
    } else m_selection = {0, 0, m_focused};
    m_trackSelectionAnchor = m_focused; ++m_trackSelectionRevision;
    emit selectionChanged(); emit tracksChanged(); markSpectralStale();
}

bool Backend::setSelectedClips(const QVariantList& values, const QString& stamp)
{
    if (!stamp.isEmpty() && !automationStampCurrent(stamp)) { setError(tr("The clip selection target changed.")); return false; }
    std::vector<session::ClipAddress> clips;
    std::set<std::pair<int, int>> seen;
    for (const auto& value : values) {
        const auto map = value.toMap(); int id = -1, index = -1;
        if (value.metaType().id() != QMetaType::QVariantMap || map.size() != 2
            || !selectionIndex(map.value("track"), id) || !selectionIndex(map.value("clip"), index)
            || !seen.emplace(id, index).second) {
            setError(tr("Choose each clip once, using its track and clip indices.")); return false;
        }
        const auto* track = m_project.track(session::TrackId(id));
        if (!track || std::size_t(index) >= track->clips.size()) { setError(tr("A selected clip no longer exists.")); return false; }
        clips.push_back({track->id, std::size_t(index), track->clips[std::size_t(index)]});
    }
    if (!clips.empty()) {
        const auto captured = session::copySelectedClips(m_project, clips);
        if (!captured.clipboard) { setError(QString::fromStdString(captured.error)); return false; }
    }
    const auto focus = clips.empty() ? m_focused : clips.back().track;
    const auto index = clips.empty() ? 0 : clips.back().index;
    if (m_clipGroupsLinked && !clips.empty()) {
        auto expanded = session::expandClipGroups(m_project, clips);
        if (!expanded.error.empty()) { setError(QString::fromStdString(expanded.error)); return false; }
        clips = std::move(expanded.clips);
    }
    applyClipSelection(std::move(clips), focus, index); setError({}); return true;
}

bool Backend::selectClipAt(int id, qint64 frame, bool toggle)
{
    const auto* track = m_project.track(session::TrackId(id));
    if (!track) { setError(tr("Choose an existing track.")); return false; }
    for (std::size_t index = 0; index < track->clips.size(); ++index) {
        if (!track->clips[index].covers(frame)) continue;
        auto selected = toggle && selectedClipsCurrent() ? m_selectedClips : std::vector<session::ClipAddress>{};
        std::vector<session::ClipAddress> unit{{track->id, index, track->clips[index]}};
        if (m_clipGroupsLinked) {
            auto expanded = session::expandClipGroups(m_project, unit);
            if (!expanded.error.empty()) { setError(QString::fromStdString(expanded.error)); return false; }
            unit = std::move(expanded.clips);
        }
        const bool remove = toggle && std::all_of(unit.begin(), unit.end(), [&](const auto& clip) {
            return std::find(selected.begin(), selected.end(), clip) != selected.end();
        });
        for (const auto& clip : unit) {
            const auto found = std::find(selected.begin(), selected.end(), clip);
            if (remove) { if (found != selected.end()) selected.erase(found); }
            else if (found == selected.end()) selected.push_back(clip);
        }
        applyClipSelection(std::move(selected), track->id, index); setError({}); return true;
    }
    if (!toggle) applyClipSelection({}, track->id, 0);
    return false;
}

QVariantList Backend::clipGroups() const
{
    QVariantList groups;
    for (const auto& group : m_project.clipGroups()) {
        QVariantList members;
        for (const auto& track : m_project.tracks())
            for (std::size_t i = 0; i < track.clips.size(); ++i)
                if (track.clips[i].group == group.id)
                    members.push_back(QVariantMap{{"track", int(track.id)}, {"clip", int(i)}});
        groups.push_back(QVariantMap{{"id", int(group.id)}, {"name", QString::fromStdString(group.name)},
            {"colour", QString::fromStdString(group.colour)}, {"memberCount", members.size()}, {"members", members}});
    }
    return groups;
}

void Backend::setClipGroupsLinked(bool linked)
{
    if (linked == m_clipGroupsLinked) return;
    if (linked && !m_selectedClips.empty()) {
        auto expanded = session::expandClipGroups(m_project, m_selectedClips);
        if (!expanded.error.empty()) { setError(QString::fromStdString(expanded.error)); return; }
        const auto anchor = std::find_if(m_selectedClips.begin(), m_selectedClips.end(), [this](const auto& clip) {
            return clip.track == m_focused && clip.expected.covers(m_selection.start);
        });
        const auto index = anchor == m_selectedClips.end() ? 0 : anchor->index;
        m_clipGroupsLinked = linked;
        applyClipSelection(std::move(expanded.clips), m_focused, index);
    } else {
        m_clipGroupsLinked = linked; ++m_trackSelectionRevision; emit selectionChanged();
    }
    cancelClipMove(); setError({});
}

bool Backend::createClipGroup(const QString& name, const QString& colour, const QString& stamp)
{
    if (!stamp.isEmpty() && !automationStampCurrent(stamp)) { setError(tr("The clip group target changed.")); return false; }
    return applyClipGroupEdit(ClipGroupEdit::Create, m_selectedClips, {}, name, colour);
}

bool Backend::updateClipGroup(int id, const QString& name, const QString& colour, const QString& stamp)
{
    if (!stamp.isEmpty() && !automationStampCurrent(stamp)) { setError(tr("The clip group target changed.")); return false; }
    const auto* group = id < 0 ? nullptr : m_project.clipGroup(session::ClipGroupId(id));
    if (!group) { setError(tr("Choose an existing clip group.")); return false; }
    return applyClipGroupEdit(ClipGroupEdit::Update, {}, *group, name, colour);
}

bool Backend::detachSelectedClips(const QString& stamp)
{
    if (!stamp.isEmpty() && !automationStampCurrent(stamp)) { setError(tr("The clip group target changed.")); return false; }
    return applyClipGroupEdit(ClipGroupEdit::Detach, m_selectedClips, {}, {}, {});
}

bool Backend::dissolveClipGroup(int id, const QString& stamp)
{
    if (!stamp.isEmpty() && !automationStampCurrent(stamp)) { setError(tr("The clip group target changed.")); return false; }
    const auto* group = id < 0 ? nullptr : m_project.clipGroup(session::ClipGroupId(id));
    if (!group) { setError(tr("Choose an existing clip group.")); return false; }
    return applyClipGroupEdit(ClipGroupEdit::Dissolve, {}, *group, {}, {});
}

bool Backend::applyClipGroupEdit(ClipGroupEdit operation, const std::vector<session::ClipAddress>& clips,
                                 const session::ClipGroup& expected, const QString& name, const QString& colour)
{
    if (m_shuttingDown || recording() || loading() || sampleEditing() || m_repairing || storageBusy() || m_clipMove) {
        setError(tr("Finish recording or the current clip edit first.")); return false;
    }
    if (operation == ClipGroupEdit::Create || operation == ClipGroupEdit::Update) {
        const session::ClipGroup candidate{session::ClipGroupId(0), name.toStdString(), colour.toStdString()};
        if (!candidate.valid()) {
            setError(tr("Enter a group name of up to 512 UTF-8 bytes without control characters, and a colour as #RRGGBB or leave it empty.")); return false;
        }
    }
    if (operation == ClipGroupEdit::Create || operation == ClipGroupEdit::Detach) {
        if (clips.size() < (operation == ClipGroupEdit::Create ? 2U : 1U)) {
            setError(operation == ClipGroupEdit::Create ? tr("Select at least two whole clips to group.") : tr("Select whole clips to detach.")); return false;
        }
        // This validates exactly the accepted addresses without expanding the scope.
        const auto selected = session::copySelectedClips(m_project, clips);
        if (!selected.clipboard) { setError(QString::fromStdString(selected.error)); return false; }
    } else {
        const auto* group = m_project.clipGroup(expected.id);
        if (!group || *group != expected) { setError(tr("The clip group changed. Reload it before applying.")); return false; }
    }
    if (const auto deferred = deferAutomationEdit([this, operation, clips, expected, name, colour] {
        return applyClipGroupEdit(operation, clips, expected, name, colour);
    })) return *deferred;
    const auto before = captureEditState();
    std::string error, label;
    switch (operation) {
    case ClipGroupEdit::Create: {
        const auto result = session::createClipGroup(m_project, clips, name.toStdString(), colour.toStdString());
        error = result.error; label = "group clips"; break;
    }
    case ClipGroupEdit::Update:
        *m_project.clipGroup(expected.id) = {expected.id, name.toStdString(), colour.toStdString()};
        label = "clip group properties"; break;
    case ClipGroupEdit::Detach:
        error = session::detachClipGroupMembers(m_project, clips).error; label = "detach clips from groups"; break;
    case ClipGroupEdit::Dissolve:
        (void)m_project.removeClipGroup(expected.id); label = "dissolve clip group"; break;
    }
    if (!error.empty()) { setError(QString::fromStdString(error)); return false; }
    // Identity participates in stale-address checks. Refresh only descriptors:
    // this metadata edit leaves focus, range and audio-derived views unchanged.
    for (auto& address : m_selectedClips) {
        const auto* track = m_project.track(address.track);
        if (track && address.index < track->clips.size()) address.expected = track->clips[address.index];
    }
    setError({}); commit(std::move(label), session::kInvalidTrack, before);
    ++m_trackSelectionRevision; emit selectionChanged(); emit tracksChanged();
    return m_lastError.isEmpty();
}

bool Backend::copyClipSelection()
{
    auto captured = session::copySelectedClips(m_project, m_selectedClips);
    if (!captured.clipboard) { setError(QString::fromStdString(captured.error)); return false; }
    m_multitrackClipboard = std::move(captured.clipboard);
    m_clipboard.clear(); m_clipboardFrames = 0; m_clipboardChannel = -1;
    setError({}); emit timelineChanged(); return true;
}

void Backend::deleteClipSelection(const std::string& label)
{
    if (deferAutomationEdit([this, label] { deleteClipSelection(label); return true; })) return;
    if (recording()) { setError(tr("Stop recording before editing clips.")); return; }
    const auto before = captureEditState();
    const auto result = session::deleteSelectedClips(m_project, m_selectedClips);
    if (!result.error.empty()) { setError(QString::fromStdString(result.error)); return; }
    if (!result.changed) return;
    m_selectedClips.clear(); ++m_trackSelectionRevision; m_selection.end = m_selection.start;
    commit(label, session::kInvalidTrack, before); setError({});
}

bool Backend::applyClipTranslation(const std::vector<session::ClipAddress>& clips, qint64 frames,
                                   int rows, session::TrackId focus, std::size_t focusedClip)
{
    if (const auto deferred = deferAutomationEdit([this, clips, frames, rows, focus, focusedClip] { return applyClipTranslation(clips, frames, rows, focus, focusedClip); })) return *deferred;
    if (recording()) { setError(tr("Stop recording before moving clips.")); return false; }
    // Own the selection before applyClipSelection replaces the member vector.
    const auto original = clips;
    const auto before = captureEditState();
    const auto result = session::moveSelectedClips(m_project, original, frames, rows);
    if (!result.error.empty()) { setError(QString::fromStdString(result.error)); return false; }
    if (!result.changed) { setError({}); return true; }
    std::vector<session::ClipAddress> selected;
    session::TrackId nextFocus = session::kInvalidTrack; std::size_t nextIndex = 0;
    for (const auto& address : original) {
        const auto target = std::size_t(std::int64_t(*m_project.trackIndex(address.track)) + rows);
        const auto& track = m_project.tracks()[target];
        const auto found = std::find_if(track.clips.begin(), track.clips.end(), [&](const auto& clip) {
            return clip.identity == address.expected.identity;
        });
        if (found == track.clips.end()) continue;
        const auto index = std::size_t(found - track.clips.begin());
        selected.push_back({track.id, index, *found});
        if (address.track == focus && address.index == focusedClip) { nextFocus = track.id; nextIndex = index; }
    }
    applyClipSelection(std::move(selected), nextFocus, nextIndex);
    commit(original.size() == 1 ? "move clip" : "move clips", original.size() == 1 ? m_focused : session::kInvalidTrack, before);
    setError({}); return true;
}

bool Backend::moveSelectedClips(qint64 frames, int rows, const QString& stamp)
{
    if (!stamp.isEmpty() && !automationStampCurrent(stamp)) { setError(tr("The selected clips changed.")); return false; }
    if (m_selectedClips.empty()) { setError(tr("Select whole clips to move first.")); return false; }
    const auto anchor = std::find_if(m_selectedClips.begin(), m_selectedClips.end(), [this](const auto& c) {
        return c.track == m_focused && c.expected.covers(m_selection.start);
    });
    const auto& clip = anchor == m_selectedClips.end() ? m_selectedClips.front() : *anchor;
    return applyClipTranslation(m_selectedClips, frames, rows, clip.track, clip.index);
}

std::vector<session::TrackId> Backend::selectedTrackIds() const
{
    std::vector<session::TrackId> ids;
    // Model order, even after a track reorder; stale IDs never select a new row.
    for (const auto& track : m_project.tracks())
        if (std::find(m_selectedTracks.begin(), m_selectedTracks.end(), track.id) != m_selectedTracks.end())
            ids.push_back(track.id);
    if (ids.empty() && m_project.track(m_focused)) ids.push_back(m_focused);
    return ids;
}

QVariantList Backend::selectedTracks() const
{
    QVariantList ids;
    for (const auto id : selectedTrackIds()) ids.push_back(int(id));
    return ids;
}

session::FramePos Backend::selectedTrackLength() const
{
    session::FramePos length = 0;
    for (const auto id : selectedTrackIds()) length = std::max(length, trackLength(id));
    return length;
}

void Backend::clearGroupSelection()
{
    m_selectedClips.clear();
    m_selectedTracks.clear(); m_trackSelectionAnchor = m_focused;
    m_selection.track = m_focused;
    ++m_trackSelectionRevision;
    clampSelection();
}

bool Backend::setSelectedTracks(const QVariantList& values)
{
    std::vector<session::TrackId> ids;
    for (const auto& value : values) {
        const auto type = value.metaType().id();
        const bool numeric = type == QMetaType::Int || type == QMetaType::UInt || type == QMetaType::LongLong
            || type == QMetaType::ULongLong || type == QMetaType::Double;
        const auto number = value.toDouble();
        if (!numeric || !std::isfinite(number) || number < 0 || number > std::numeric_limits<int>::max()
            || std::floor(number) != number) { setError(tr("Choose existing track IDs.")); return false; }
        const auto id = session::TrackId(int(number));
        if (!m_project.track(id) || std::find(ids.begin(), ids.end(), id) != ids.end()) {
            setError(tr("Choose each existing track once.")); return false;
        }
        ids.push_back(id);
    }
    if (ids.empty()) { clearGroupSelection(); setError({}); return true; }
    m_selectedClips.clear(); m_selectedTracks = std::move(ids);
    m_trackSelectionAnchor = m_selectedTracks.front();
    if (std::find(m_selectedTracks.begin(), m_selectedTracks.end(), m_focused) == m_selectedTracks.end())
        m_focused = m_selectedTracks.front();
    m_selection.track = m_focused; m_selection.channel = -1;
    ++m_trackSelectionRevision;
    clampSelection(); emit tracksChanged(); markSpectralStale(); setError({});
    return true;
}

void Backend::selectTrack(int track, bool toggle, bool extend)
{
    const auto id = session::TrackId(track);
    const auto index = m_project.trackIndex(id);
    if (!index) { setError(tr("Choose an existing track.")); return; }
    const auto anchor = m_project.trackIndex(m_trackSelectionAnchor);
    QVariantList ids;
    if (extend && anchor) {
        if (toggle) ids = selectedTracks();
        for (auto i = std::min(*index, *anchor); i <= std::max(*index, *anchor); ++i) {
            const auto next = int(m_project.tracks()[i].id);
            if (!ids.contains(next)) ids.push_back(next);
        }
    } else if (toggle) {
        ids = selectedTracks();
        if (ids.contains(track)) ids.removeAll(track); else ids.push_back(track);
        // There is always a focused lane; toggling the last lane keeps it selected.
        if (ids.empty()) ids.push_back(track);
    } else ids.push_back(track);
    const auto previousAnchor = m_trackSelectionAnchor;
    if (!setSelectedTracks(ids)) return;
    if (ids.contains(track)) m_focused = id;
    if (extend && anchor) m_trackSelectionAnchor = previousAnchor;
    else m_trackSelectionAnchor = id;
    m_selection.track = m_focused;
    emit selectionChanged(); emit tracksChanged();
}

void Backend::focusSelectedTrack(int track)
{
    const auto ids = selectedTrackIds();
    const auto id = session::TrackId(track);
    if (std::find(ids.begin(), ids.end(), id) == ids.end()) { setFocusedTrack(track); return; }
    if (m_focused == id) return;
    if (!m_selectedClips.empty()) {
        const auto clip = std::find_if(m_selectedClips.begin(), m_selectedClips.end(), [&](const auto& c) { return c.track == id; });
        if (clip != m_selectedClips.end()) { applyClipSelection(m_selectedClips, id, clip->index); return; }
    }
    m_focused = id; m_selection.track = id; m_selection.channel = -1;
    ++m_trackSelectionRevision;
    emit selectionChanged(); emit tracksChanged(); markSpectralStale();
}

bool Backend::copyTrackSelection()
{
    auto captured = session::copyTrackRange(m_project, selectedTrackIds(), m_selection.start, m_selection.end);
    if (!captured.clipboard) { setError(QString::fromStdString(captured.error)); return false; }
    m_multitrackClipboard = std::move(captured.clipboard);
    m_clipboard.clear(); m_clipboardFrames = 0; m_clipboardChannel = -1;
    setError({}); emit timelineChanged();
    return true;
}

void Backend::deleteTrackSelection(bool ripple, const std::string& label)
{
    if (deferAutomationEdit([this, ripple, label] { deleteTrackSelection(ripple, label); return true; })) return;
    if (recording()) { setError(tr("Stop recording before editing tracks.")); return; }
    const auto before = captureEditState();
    const auto result = session::deleteTrackRange(m_project, selectedTrackIds(), m_selection.start, m_selection.end, ripple);
    if (!result.error.empty()) { setError(QString::fromStdString(result.error)); return; }
    if (ripple) m_selection.end = m_selection.start;
    if (result.changed) commit(label, session::kInvalidTrack, before);
    setError({}); emit selectionChanged();
}

void Backend::startMultitrackPaste(qint64 frame, bool newTracks)
{
    if (deferAutomationEdit([this, frame, newTracks] { startMultitrackPaste(frame, newTracks); return true; })) return;
    if (!m_multitrackClipboard) return;
    if (recording()) { setError(tr("Stop recording before pasting tracks.")); return; }
    if (sampleEditing()) { setError(tr("Finish or cancel the current sample edit first.")); return; }
    if (!newTracks && m_selection.channel >= 0) {
        if (m_multitrackClipboard->tracks.size() == 1 && m_multitrackClipboard->tracks.front().clips.size() == 1) {
            startChannelEdit(session::ChannelEdit::Paste, 1, frame); return;
        }
        setError(tr("Choose All channels to paste a group of whole clips or multiple tracks.")); return;
    }
    const auto index = newTracks ? std::optional<std::size_t>(m_project.tracks().size()) : m_project.trackIndex(m_focused);
    if (!index) { setError(tr("Focus the first destination track.")); return; }
    const auto at = newTracks ? 0 : frame < 0 ? m_playhead : frame;
    if (at < 0 || at > std::numeric_limits<qint64>::max() - m_multitrackClipboard->frames
        || *index >= engine::Transport::kMaxTracks
        || m_multitrackClipboard->tracks.back().offset >= engine::Transport::kMaxTracks - *index) {
        setError(tr("The paste exceeds the available tracks or timeline.")); return;
    }
    (void)captureEditState();
    SampleEditRequest request;
    request.kind = SampleEditKind::MultitrackPaste;
    request.track = m_focused; request.selection = m_selection;
    request.generation = m_sessionGeneration; request.revision = m_editRevision;
    request.clipIndex = *index; request.rangeStart = at;
    request.multitrack = m_multitrackClipboard;
    request.ripple = m_ripple && !newTracks; request.allTracks = true;
    request.selectionRevision = m_trackSelectionRevision;
    request.label = newTracks ? tr("paste to new tracks") : request.ripple ? tr("ripple paste tracks") : tr("paste tracks");
    m_sampleEditLabel = request.label; m_sampleEditRequest = std::move(request);
    setError({}); emit sampleEditChanged(); pumpSampleEdit();
}

QVariantMap Backend::selectedStretchTarget() const
{
    QVariantMap out{{"count", qint64(m_selectedClips.size())}, {"stamp", clipPropertySelectionStamp()},
        {"valid", false}, {"liveValid", false}, {"anyElastic", false},
        {"start", qint64(0)}, {"length", qint64(0)}, {"minimum", qint64(0)}, {"maximum", qint64(0)}};
    if (m_selectedClips.empty() || !selectedClipsCurrent()) {
        out["reason"] = out["liveReason"] = tr("Select whole clips to change their duration."); return out;
    }
    qint64 start = std::numeric_limits<qint64>::max(), end = 0;
    for (const auto& address : m_selectedClips) {
        start = std::min(start, qint64(address.expected.timelineStart));
        end = std::max(end, qint64(address.expected.timelineEnd()));
        if (address.expected.elastic) out["anyElastic"] = true;
    }
    const auto length = end-start;
    out["start"] = start; out["length"] = length;
    out["minimum"] = length/4 + (length%4 != 0);
    out["maximum"] = std::min(std::numeric_limits<qint64>::max()-start,
        length > std::numeric_limits<qint64>::max()/4 ? std::numeric_limits<qint64>::max() : length*4);
    const auto rendered = session::planSelectedClipStretch(m_project, m_selectedClips, length, false);
    const auto live = session::planSelectedClipStretch(m_project, m_selectedClips, length, true);
    out["valid"] = rendered.error.empty(); out["reason"] = QString::fromStdString(rendered.error);
    out["liveValid"] = live.error.empty(); out["liveReason"] = QString::fromStdString(live.error);
    return out;
}

bool Backend::stretchSelectedClips(qint64 targetFrames, bool realtime, double pitchChange, const QString& stamp)
{
    if (stamp.isEmpty() || stamp != clipPropertySelectionStamp() || !selectedClipsCurrent()) {
        setError(tr("The clip selection changed. Reload Pitch and duration before applying.")); return false;
    }
    if (sampleEditing()) { setError(tr("Finish the current audio edit first.")); return false; }
    return applySelectedStretch(m_selectedClips, targetFrames, realtime, pitchChange, m_groupStretchCancelSerial);
}

void Backend::remapStretchedSelection(const std::vector<session::ClipAddress>& before,
                                      const std::vector<session::ClipAddress>& after)
{
    if (m_selectedClips.empty()) return; // preserve later time-range navigation
    std::vector<session::ClipAddress> selected;
    auto focus = m_focused; std::size_t focusIndex = 0;
    for (const auto& address : m_selectedClips) {
        session::ClipAddress next;
        const auto changed = std::find(before.begin(), before.end(), address);
        if (changed != before.end()) next = after[std::size_t(changed-before.begin())];
        else {
            const auto* track = m_project.track(address.track);
            if (!track) continue;
            const auto found = std::find_if(track->clips.begin(),track->clips.end(), [&](const auto& clip) {
                return clip.identity == address.expected.identity;
            });
            if (found == track->clips.end()) continue;
            next = {track->id, std::size_t(found-track->clips.begin()), *found};
        }
        if (address.track == m_focused && address.expected.covers(m_selection.start)) {
            focus = next.track; focusIndex = next.index;
        }
        selected.push_back(next);
    }
    applyClipSelection(std::move(selected), focus, focusIndex);
}

bool Backend::applySelectedStretch(const std::vector<session::ClipAddress>& addresses, qint64 frames,
                                    bool realtime, double pitchChange, std::uint64_t serial)
{
    if (serial != m_groupStretchCancelSerial) return false;
    m_groupStretchDeferred = false;
    const auto notify = qScopeGuard([this] { emit sampleEditChanged(); });
    if (m_shuttingDown || recording() || loading() || sampleEditing() || m_repairing || storageBusy() || m_clipMove) {
        setError(tr("Finish recording or the current clip edit first.")); return false;
    }
    const auto clips = addresses; // selection can be replaced during commit
    const auto plan = session::planSelectedClipStretch(m_project, clips, frames, realtime, pitchChange);
    if (!plan.error.empty()) { setError(QString::fromStdString(plan.error)); return false; }
    setError({});
    if (!plan.changed) return true;
    m_groupStretchDeferred = !realtime;
    if (const auto deferred = deferAutomationEdit([this, clips, frames, realtime, pitchChange, serial] {
        return applySelectedStretch(clips, frames, realtime, pitchChange, serial);
    }, [this] { m_groupStretchDeferred = false; emit sampleEditChanged(); }, true)) return *deferred;
    m_groupStretchDeferred = false;
    if (realtime) {
        auto result = session::stretchSelectedClips(m_project, clips, frames, true, pitchChange);
        if (!result.edit.error.empty()) { setError(QString::fromStdString(result.edit.error)); return false; }
        if (!result.edit.project) return true;
        const auto before = captureEditState();
        std::set<session::TrackId> installed;
        for (const auto& address : clips) if (installed.insert(address.track).second)
            m_project.track(address.track)->clips = std::move(result.edit.project->track(address.track)->clips);
        remapStretchedSelection(clips, result.clips);
        commit("selected clip pitch and duration", session::kInvalidTrack, before);
        return m_lastError.isEmpty();
    }
    (void)captureEditState();
    SampleEditRequest request;
    request.kind = SampleEditKind::GroupStretch; request.track = m_focused; request.selection = m_selection;
    request.generation = m_sessionGeneration; request.revision = m_editRevision;
    request.targetFrames = frames; request.clips = clips; request.selectionRevision = m_trackSelectionRevision;
    request.label = sampleEditName(request.kind); m_sampleEditLabel = request.label;
    m_sampleEditRequest = std::move(request);
    emit sampleEditChanged(); pumpSampleEdit(); return true;
}
