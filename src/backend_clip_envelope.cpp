#include "backend.h"
#include "session/clip_envelope.h"

#include <algorithm>
#include <cmath>
#include <limits>

QVariantMap Backend::clipEnvelopeTarget(int track, int clip) const
{
    const auto missing = [this] { return QVariantMap{{"valid", false},
        {"reason", tr("Choose a clip to edit its envelope.")}}; };
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
    const auto describe = [&](const QString& key, const QString& label,
                              const session::AutomationCurve& curve, double low, double high,
                              double base, bool available) {
        QVariantList points;
        for (const auto& point : curve.points) {
            QVariantMap entry{{"frame", qint64(point.frame - target.envelopeOffset)},
                {"value", point.value}, {"interpolation", int(point.interpolation)}};
            if (point.interpolation == session::AutomationInterpolation::Smooth) {
                entry.insert("smoothStart", point.smoothStart); entry.insert("smoothEnd", point.smoothEnd);
            }
            points.append(entry);
        }
        return QVariantMap{{"key", key}, {"label", label}, {"enabled", curve.enabled},
            {"hold", curve.hold}, {"minimum", low}, {"maximum", high}, {"base", base},
            {"factor", 100}, {"available", available}, {"points", points}};
    };
    return {{"valid", true}, {"reason", QString()}, {"track", track}, {"clip", clip},
        {"name", QString::fromStdString(source->name)}, {"trackName", QString::fromStdString(lane->name)},
        {"start", qint64(target.timelineStart)}, {"length", qint64(target.length)},
        {"stamp", automationStamp()}, {"curves", QVariantList{
            describe("volume", tr("Volume (%)"), target.volumeEnvelope, 0, 4, 1, true),
            describe("pan", tr("Stereo balance (−100 left / 100 right)"), target.panEnvelope,
                     -1, 1, 0, source->audio.channels() >= 2)}}};
}

bool Backend::editClipEnvelope(int track, int clip, const QString& key, const QString& operation,
                              qint64 frame, qint64 to, double value, const QString& stamp)
{
    const auto reject = [this](const QString& message) { setError(message); return false; };
    if (stamp.isEmpty() || stamp != automationStamp())
        return reject(tr("The clip or project changed. Reload its envelope before editing."));
    const auto target = clipEnvelopeTarget(track, clip);
    if (!target.value("valid").toBool() || track < 0 || clip < 0)
        return reject(tr("Choose an existing clip to edit its envelope."));
    if ((key != "volume" && key != "pan") || !std::isfinite(value))
        return reject(tr("Choose Volume or Stereo balance and a finite value."));
    const auto& original = m_project.track(session::TrackId(track))->clips[std::size_t(clip)];
    const auto* source = m_project.source(original.source);
    if (key == "pan" && (!source || source->audio.channels() < 2))
        return reject(tr("Clip balance needs a stereo source. Use track pan for mono audio."));
    if (!original.hasValidEnvelopes())
        return reject(tr("The clip has invalid envelope data."));
    auto replacement = original;
    auto& curve = key == "volume" ? replacement.volumeEnvelope : replacement.panEnvelope;
    const auto local = [&](qint64 position) { return position >= 0 && position <= original.length
        && position <= std::numeric_limits<session::FramePos>::max() - original.envelopeOffset; };
    const auto pointAt = [&](qint64 position) {
        return std::find_if(curve.points.begin(), curve.points.end(), [&](const auto& point) {
            return point.frame == original.envelopeOffset + position;
        });
    };
    if (operation == "point") {
        if (!local(to) || frame < -1 || (frame >= 0 && !local(frame))
            || value < (key == "volume" ? 0 : -1) || value > (key == "volume" ? 4 : 1))
            return reject(tr("The point must be within the clip and the parameter's range."));
        if (frame >= 0) {
            const auto old = pointAt(frame);
            if (old == curve.points.end()) return reject(tr("The selected envelope point no longer exists."));
            if (frame == to) old->value = value;
            else {
                if (pointAt(to) != curve.points.end()) return reject(tr("An envelope point already occupies that time."));
                auto moved = *old; moved.frame = original.envelopeOffset + to; moved.value = value;
                curve.points.erase(old);
                // Existing knots retime their adjacent segments without slicing
                // either neighbour's retained Smooth phase window.
                const auto at = std::lower_bound(curve.points.begin(), curve.points.end(), moved.frame,
                    [](const auto& point, session::FramePos position) { return point.frame < position; });
                curve.points.insert(at, moved);
            }
        } else if (!curve.setPoint(original.envelopeOffset + to, value) && pointAt(to) == curve.points.end())
            return reject(tr("The envelope has reached its point limit."));
        curve.enabled = true;
    } else if (operation == "delete") {
        if (!local(frame) || !curve.erasePoint(original.envelopeOffset + frame))
            return reject(tr("Choose an envelope point within the clip to remove."));
    } else if (operation == "interpolation") {
        if (!local(frame) || value != std::trunc(value) || value < 0 || value > 3)
            return reject(tr("Choose Curve default, Linear, Hold or Smooth for a point within the clip."));
        const auto point = pointAt(frame);
        if (point == curve.points.end()) return reject(tr("The selected envelope point no longer exists."));
        const auto mode = static_cast<session::AutomationInterpolation>(int(value));
        if (point->interpolation != mode) {
            point->interpolation = mode; point->smoothStart = 0; point->smoothEnd = 1;
        }
    } else if (operation == "enabled" || operation == "hold") {
        if (value != 0 && value != 1) return reject(tr("Use an explicit off or on value for this envelope setting."));
        if (operation == "enabled") curve.enabled = value == 1;
        else curve.hold = value == 1;
    } else if (operation == "clear") {
        curve = {};
    } else return reject(tr("Unknown clip envelope operation."));
    if (!replacement.volumeEnvelope.hasData() && !replacement.panEnvelope.hasData()) replacement.envelopeOffset = 0;
    return applyClipEnvelopeEdit({session::TrackId(track), std::size_t(clip), original}, replacement, stamp);
}

bool Backend::applyClipEnvelopeEdit(const session::ClipAddress& target,
                                   const session::Clip& replacement, const QString& stamp)
{
    const auto finished = [&](bool success, const QString& message = {}) {
        setError(message);
        emit clipEnvelopeEditFinished(int(target.track), int(target.index), stamp, success, message);
        return success;
    };
    if (m_shuttingDown || recording() || loading() || sampleEditing() || m_repairing || storageBusy() || m_clipMove)
        return finished(false, tr("Finish recording or the current clip edit first."));
    const auto* track = m_project.track(target.track);
    if (!track || target.index >= track->clips.size() || !track->clips[target.index].sameDescriptor(target.expected))
        return finished(false, tr("The clip changed while the envelope edit waited. Reload its envelope."));
    const auto count = [](const auto& clip) { return clip.volumeEnvelope.points.size() + clip.panEnvelope.points.size(); };
    if (!replacement.hasValidEnvelopes()
        || m_project.automationPointCount() - count(target.expected) + count(replacement) > session::kMaxProjectAutomationPoints)
        return finished(false, tr("The envelope is invalid or the project has reached its point limit."));
    if (target.expected == replacement) return finished(true);
    setError({});
    if (const auto deferred = deferAutomationEdit([this, target, replacement, stamp] {
            return applyClipEnvelopeEdit(target, replacement, stamp);
        }, [this, target, stamp] {
            emit clipEnvelopeEditFinished(int(target.track), int(target.index), stamp, false,
                tr("The clip envelope edit was cancelled."));
        }, true)) return *deferred;
    const auto before = captureEditState();
    const auto selectedBefore = m_selectedClips;
    auto* currentTrack = m_project.track(target.track);
    if (!currentTrack || target.index >= currentTrack->clips.size()
        || currentTrack->clips[target.index].identity != target.expected.identity)
        return finished(false, tr("The clip changed while preparing its envelope edit."));
    auto& installed = currentTrack->clips[target.index];
    // The ownership boundary may have captured newer native rack settings.
    // This operation owns only the clip envelopes, never its captured rack.
    installed.volumeEnvelope = replacement.volumeEnvelope;
    installed.panEnvelope = replacement.panEnvelope;
    installed.envelopeOffset = replacement.envelopeOffset;
    for (auto& selected : m_selectedClips) if (selected == target) selected.expected = installed;
    commit("clip envelope", target.track, before);
    if (!m_lastError.isEmpty()) {
        m_selectedClips = selectedBefore;
        return finished(false, m_lastError);
    }
    ++m_trackSelectionRevision; emit selectionChanged();
    return finished(true);
}
