#include "backend.h"

#include "session/edits.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace {
bool parseClipFadeShapes(const QVariantMap& values, std::optional<session::ClipFadeShapes>& shapes)
{
    if (values.isEmpty()) return true; // legacy calls retain each member's shape
    if (values.size() != 4) return false;
    const auto number = [&](const char* name, double low, double high, double& result) {
        const auto v = values.value(QLatin1String(name));
        const auto type = v.metaType().id();
        if (type != QMetaType::Int && type != QMetaType::UInt && type != QMetaType::LongLong
            && type != QMetaType::ULongLong && type != QMetaType::Float && type != QMetaType::Double) return false;
        result = v.toDouble(); return std::isfinite(result) && result >= low && result <= high;
    };
    double inShape = 0, outShape = 0, inCurve = 0, outCurve = 0;
    if (!number("fadeInShape", 0, 1, inShape) || std::floor(inShape) != inShape
        || !number("fadeOutShape", 0, 1, outShape) || std::floor(outShape) != outShape
        || !number("fadeInCurve", -1, 1, inCurve) || !number("fadeOutCurve", -1, 1, outCurve)) return false;
    shapes = session::ClipFadeShapes{session::FadeShape(int(inShape)), session::FadeShape(int(outShape)),
        float(inCurve), float(outCurve)};
    return true;
}
}

QVariantMap Backend::trackSourceInfo(int track) const
{
    if (track < -1) return {};
    const auto *t = m_project.track(trackArg(track));
    if (!t) return {};
    std::set<session::SourceId> references;
    for (const auto &clip : t->clips) references.insert(clip.source);
    std::set<std::uint32_t> channels, rates, originals, resampledFrom;
    int missing = 0;
    const session::AudioSource *single = nullptr;
    for (const auto id : references) {
        const auto *source = m_project.source(id);
        if (!source) { ++missing; continue; }
        single = source;
        const auto rate = source->sampleRate ? source->sampleRate : m_project.sampleRate();
        const auto original = source->originalSampleRate.value_or(rate);
        channels.insert(source->audio.channels()); rates.insert(rate); originals.insert(original);
        if (original != rate) resampledFrom.insert(original);
    }
    const auto list = [](const auto &values) {
        QVariantList result;
        for (const auto value : values) result.append(int(value));
        return result;
    };
    QVariantMap out{{"valid", true}, {"track", int(t->id)}, {"clipCount", qint64(t->clips.size())},
        {"sourceCount", qint64(references.size())}, {"missingSources", missing},
        {"channels", list(channels)}, {"sampleRates", list(rates)}, {"originalRates", list(originals)},
        {"resampledFrom", list(resampledFrom)}, {"projectRate", int(m_project.sampleRate())}};
    if (references.size() == 1 && single) {
        out["name"] = QString::fromStdString(single->name);
        out["path"] = QString::fromStdString(single->originalPath);
        out["sourceFrames"] = qint64(single->audio.frames());
        out["generated"] = single->originalPath.empty();
    }
    return out;
}

QVariantMap Backend::clipProperties() const
{
    const auto *track = m_project.track(m_focused);
    if (!track) return {};
    const auto at = m_selection.empty() ? m_playhead : m_selection.start;
    for (std::size_t index = 0; index < track->clips.size(); ++index) {
        const auto &clip = track->clips[index];
        if (!clip.covers(at)) continue;
        const auto *source = m_project.source(clip.source);
        if (!source) return {};
        const auto rate = source->sampleRate ? source->sampleRate : m_project.sampleRate();
        const bool wholeSelection = selectedClipsCurrent() && std::any_of(m_selectedClips.begin(), m_selectedClips.end(),
            [&](const auto& address) { return address.track == track->id && address.index == index; });
        auto shortest = clip.length;
        if (wholeSelection) for (const auto& address : m_selectedClips) shortest = std::min(shortest, address.expected.length);
        return {{"valid", true}, {"track", int(track->id)}, {"clip", int(index)},
                {"stamp", automationStamp()}, {"name", QString::fromStdString(source->name)},
                {"selectionStamp", wholeSelection ? clipPropertySelectionStamp() : QString()},
                {"selectedCount", wholeSelection ? qint64(m_selectedClips.size()) : 0}, {"shortestLength", qint64(shortest)},
                {"path", QString::fromStdString(source->originalPath)},
                {"group", clip.group == session::kInvalidClipGroup ? -1 : int(clip.group)},
                {"sourceId", QString::fromStdString(source->uuid.toString())},
                {"channels", int(source->audio.channels())}, {"sampleRate", int(rate)},
                {"originalSampleRate", int(source->originalSampleRate.value_or(rate))},
                {"sourceFrames", qint64(source->audio.frames())},
                {"sourceOffset", qint64(clip.sourceOffset)},
                {"start", qint64(clip.timelineStart)}, {"end", qint64(clip.timelineEnd())},
                {"length", qint64(clip.length)}, {"gainDb", dbFromLinear(clip.gain)},
                {"fadeIn", qint64(std::clamp<session::FramePos>(clip.fadeIn - clip.fadeInOffset, 0, clip.length))},
                {"fadeOut", qint64(std::clamp<session::FramePos>(clip.fadeOut - clip.fadeOutOffset, 0, clip.length))},
                {"inheritedFades", clip.fadeInOffset != 0 || clip.fadeOutOffset != 0},
                {"fadeInShape", int(clip.fadeInShape)}, {"fadeOutShape", int(clip.fadeOutShape)},
                {"fadeInCurve", double(clip.fadeInCurve)}, {"fadeOutCurve", double(clip.fadeOutCurve)},
                {"elastic", clip.elastic.has_value()},
                {"semitones", clip.elastic ? double(clip.elastic->semitones) : 0.0},
                {"durationRatio", clip.elastic ? double(clip.elastic->outputFrames) / clip.elastic->inputFrames : 1.0},
                {"crossfade", source->crossfade.has_value()}};
    }
    return {};
}

QString Backend::clipPropertySelectionStamp() const
{
    return automationStamp() + "/" + QString::number(m_trackSelectionRevision);
}

bool Backend::applySelectedClipProperties(const QString& stamp, double gainDb, bool relativeGain,
                                          bool replaceFades, qint64 fadeIn, qint64 fadeOut, bool proportionalFades,
                                          const QVariantMap& fadeShapes)
{
    const auto target = clipProperties();
    if (stamp.isEmpty() || stamp != target.value("selectionStamp").toString()) {
        setError(tr("The clip selection changed. Reload its properties before applying.")); return false;
    }
    std::optional<session::ClipFadeShapes> shapes;
    if (!parseClipFadeShapes(fadeShapes, shapes)) {
        setError(tr("Choose Linear or Smooth fades with curve amounts from −100% to +100%.")); return false;
    }
    const auto clips = m_selectedClips;
    return applyClipPropertyEdit(clips, {gainDb, relativeGain, replaceFades, fadeIn, fadeOut,
        proportionalFades ? target.value("length").toLongLong() : 0, shapes});
}

bool Backend::applyClipPropertyEdit(const std::vector<session::ClipAddress>& clips, const session::ClipPropertyEdit& edit)
{
    if (m_shuttingDown || recording() || loading() || sampleEditing() || m_repairing || storageBusy() || m_clipMove) {
        setError(tr("Finish recording or the current clip edit first.")); return false;
    }
    // Preflight all members before accepting an automation boundary. A rejected
    // member cannot partially change the others or end a pending automation pass.
    auto candidate = m_project;
    const auto checked = session::editSelectedClipProperties(candidate, clips, edit);
    if (!checked.error.empty()) { setError(QString::fromStdString(checked.error)); return false; }
    setError({});
    if (!checked.changed) return true;
    if (const auto deferred = deferAutomationEdit([this, clips, edit] { return applyClipPropertyEdit(clips, edit); }, {}, true)) return *deferred;
    const auto before = captureEditState();
    // Capture can update effect state. Install only the validated descriptors,
    // retaining current inserts, automation and all unrelated project state.
    for (const auto& address : clips) {
        const auto* live = m_project.track(address.track);
        if (!live || address.index >= live->clips.size()
            || live->clips[address.index].identity != address.expected.identity) {
            setError(tr("A clip changed while preparing its properties."));
            return false;
        }
    }
    for (const auto& address : clips) {
        auto& live = m_project.track(address.track)->clips[address.index];
        const auto& prepared = candidate.track(address.track)->clips[address.index];
        live.gain = prepared.gain;
        live.fadeIn = prepared.fadeIn; live.fadeOut = prepared.fadeOut;
        live.fadeInOffset = prepared.fadeInOffset; live.fadeOutOffset = prepared.fadeOutOffset;
        live.fadeInShape = prepared.fadeInShape; live.fadeOutShape = prepared.fadeOutShape;
        live.fadeInCurve = prepared.fadeInCurve; live.fadeOutCurve = prepared.fadeOutCurve;
    }
    for (auto& address : m_selectedClips) {
        const auto* track = m_project.track(address.track);
        if (track && address.index < track->clips.size()) address.expected = track->clips[address.index];
    }
    commit("selected clip properties", session::kInvalidTrack, before);
    ++m_trackSelectionRevision; emit selectionChanged(); emit tracksChanged();
    return m_lastError.isEmpty();
}

bool Backend::applyClipProperties(int track, int index, const QString &stamp,
                                  double gainDb, bool replaceFades,
                                  qint64 fadeIn, qint64 fadeOut, const QVariantMap& fadeShapes)
{
    std::optional<session::ClipFadeShapes> shapes;
    if (!parseClipFadeShapes(fadeShapes, shapes)) {
        setError(tr("Choose Linear or Smooth fades with curve amounts from −100% to +100%.")); return false;
    }
    const auto target = clipProperties();
    if (target.isEmpty() || !automationStampCurrent(stamp) || track != target.value("track").toInt()
        || index != target.value("clip").toInt()) {
        setError(tr("The selected clip changed. Reload its properties before applying."));
        return false;
    }
    const auto& clip = m_project.track(session::TrackId(track))->clips[std::size_t(index)];
    return applySingleClipProperties({session::TrackId(track), std::size_t(index), clip}, gainDb,
        replaceFades, fadeIn, fadeOut, shapes);
}

bool Backend::setClipFades(int track, int index, qint64 fadeIn, qint64 fadeOut)
{
    const auto* owner = m_project.track(session::TrackId(track));
    if (!owner || index < 0 || std::size_t(index) >= owner->clips.size()) {
        setError(tr("That clip is no longer on the track.")); return false;
    }
    const auto& clip = owner->clips[std::size_t(index)];
    const qint64 length = qint64(clip.length);
    const qint64 in = std::clamp<qint64>(fadeIn, 0, length);
    const qint64 out = std::clamp<qint64>(fadeOut, 0, length - in);
    return applySingleClipProperties({session::TrackId(track), std::size_t(index), clip}, dbFromLinear(clip.gain),
        true, in, out, std::nullopt);
}

bool Backend::setClipGainDb(int track, int index, double gainDb)
{
    const auto* owner = m_project.track(session::TrackId(track));
    if (!owner || index < 0 || std::size_t(index) >= owner->clips.size()) {
        setError(tr("That clip is no longer on the track.")); return false;
    }
    const auto& clip = owner->clips[std::size_t(index)];
    return applySingleClipProperties({session::TrackId(track), std::size_t(index), clip}, gainDb,
        false, 0, 0, std::nullopt);
}

// The shortest a dragged edge leaves a clip: 10 ms, or what it already is.
static qint64 shortestTrim(qint64 rate, qint64 length) { return std::min<qint64>(length, std::max<qint64>(1, rate / 100)); }

QVariantMap Backend::clipTrimLimits(int track, int index) const
{
    const auto* owner = m_project.track(session::TrackId(track));
    if (!owner || index < 0 || std::size_t(index) >= owner->clips.size()) return {{"valid", false}};
    const auto limits = session::edits::clipTrimLimits(m_project, session::TrackId(track), std::size_t(index));
    const auto& clip = owner->clips[std::size_t(index)];
    return {{"valid", limits.valid}, {"earliestStart", qint64(limits.earliestStart)}, {"latestEnd", qint64(limits.latestEnd)},
            {"minimumLength", shortestTrim(m_project.sampleRate(), qint64(clip.length))},
            {"start", qint64(clip.timelineStart)}, {"end", qint64(clip.timelineEnd())}};
}

bool Backend::trimClip(int track, int index, qint64 start, qint64 end)
{
    if (m_shuttingDown || recording() || loading() || sampleEditing() || m_repairing || storageBusy() || m_clipMove) {
        setError(tr("Finish recording or the current clip edit first.")); return false;
    }
    const auto id = session::TrackId(track);
    const auto* owner = m_project.track(id);
    if (!owner || index < 0 || std::size_t(index) >= owner->clips.size()) {
        setError(tr("That clip is no longer on the track.")); return false;
    }
    const auto& clip = owner->clips[std::size_t(index)];
    const auto limits = session::edits::clipTrimLimits(m_project, id, std::size_t(index));
    if (!limits.valid) { setError(tr("This clip can't be trimmed here: change a crossfade with its own edges.")); return false; }
    if (start < limits.earliestStart || end > limits.latestEnd) {
        setError(tr("A clip's edge can go no further than its audio, its prepared effects or the next clip.")); return false;
    }
    if (end - start < shortestTrim(m_project.sampleRate(), qint64(clip.length))) {
        setError(tr("A trimmed clip must be at least 10 ms long.")); return false;
    }
    if (start == clip.timelineStart && end == clip.timelineEnd()) { setError({}); return true; }
    if (const auto deferred = deferAutomationEdit([this, track, index, start, end] { return trimClip(track, index, start, end); }))
        return *deferred;
    const auto before = captureEditState();
    if (!session::edits::trimClip(m_project, id, std::size_t(index), start, end)) {
        setError(tr("That trim would leave the clip's fades, envelope or effects out of step.")); return false;
    }
    setError({});
    commit("trim clip", id, before);
    return true;
}

bool Backend::applySingleClipProperties(const session::ClipAddress& target, double gainDb, bool replaceFades,
                                        qint64 fadeIn, qint64 fadeOut, const std::optional<session::ClipFadeShapes>& shapes)
{
    if (m_shuttingDown || recording() || loading() || sampleEditing() || m_repairing || storageBusy() || m_clipMove) {
        setError(tr("Finish recording or the current clip edit first.")); return false;
    }
    const auto* track = m_project.track(target.track);
    if (!track || target.index >= track->clips.size() || !track->clips[target.index].sameDescriptor(target.expected)) {
        setError(tr("The selected clip changed. Reload its properties before applying.")); return false;
    }
    auto clip = target.expected;
    if (!std::isfinite(gainDb) || gainDb < -80 || gainDb > 48
        || (replaceFades && (fadeIn < 0 || fadeOut < 0 || fadeIn > clip.length || fadeOut > clip.length))) {
        setError(tr("Choose gain from -80 to +48 dB and fades within the clip's length."));
        return false;
    }
    setError({});
    // An unchanged display value must not round-trip the stored float needlessly.
    if (std::abs(gainDb - dbFromLinear(clip.gain)) > 0.000001)
        clip.gain = linearFromDb(gainDb);
    if (replaceFades) {
        clip.fadeIn = fadeIn;
        clip.fadeOut = fadeOut;
        clip.fadeInOffset = 0;
        clip.fadeOutOffset = 0;
        if (shapes) {
            clip.fadeInShape = shapes->fadeInShape; clip.fadeOutShape = shapes->fadeOutShape;
            clip.fadeInCurve = shapes->fadeInCurve; clip.fadeOutCurve = shapes->fadeOutCurve;
        }
    }
    if (clip == target.expected) return true;
    // Own the exact descriptor, independently of later selection/navigation.
    if (const auto deferred = deferAutomationEdit([this, target, gainDb, replaceFades, fadeIn, fadeOut, shapes] {
        return applySingleClipProperties(target, gainDb, replaceFades, fadeIn, fadeOut, shapes);
    }, {}, true)) return *deferred;
    const auto before = captureEditState();
    auto* current = m_project.track(target.track);
    if (!current || target.index >= current->clips.size()
        || current->clips[target.index].identity != target.expected.identity) {
        setError(tr("The selected clip changed. Reload its properties before applying.")); return false;
    }
    // Capturing live plug-in state may replace the immutable rack. Apply only
    // these properties to the still-current view, retaining that captured state.
    auto& installed = current->clips[target.index];
    installed.gain = clip.gain;
    installed.fadeIn = clip.fadeIn; installed.fadeOut = clip.fadeOut;
    installed.fadeInOffset = clip.fadeInOffset; installed.fadeOutOffset = clip.fadeOutOffset;
    installed.fadeInShape = clip.fadeInShape; installed.fadeOutShape = clip.fadeOutShape;
    installed.fadeInCurve = clip.fadeInCurve; installed.fadeOutCurve = clip.fadeOutCurve;
    for (auto& address : m_selectedClips) if (address == target) address.expected = installed;
    commit("clip properties", target.track, before);
    ++m_trackSelectionRevision; emit selectionChanged();
    return m_lastError.isEmpty();
}
