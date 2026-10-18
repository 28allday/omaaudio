#include "backend.h"
#include "render/speech_align.h"
#include "render/remix.h"
#include "session/render.h"

#include <algorithm>
#include <limits>

// Automatic speech alignment: a second performance of the same words is re-timed
// to follow a reference clip, on a worker, and placed on a new track in one Undo
// step. The originals are left as they are.

namespace {
constexpr session::FramePos kMaxAlignSeconds = 10 * 60;
}

// The clip on `track` under the selection start or the playhead, else its only clip.
static int clipToAlign(const session::Project& project, session::TrackId track, session::FramePos at)
{
    const auto* lane = project.track(track);
    if (!lane) return -1;
    for (std::size_t i = 0; i < lane->clips.size(); ++i)
        if (lane->clips[i].covers(at)) return int(i);
    return lane->clips.size() == 1 ? 0 : -1;
}

QVariantMap Backend::speechAlignmentTarget(int referenceTrack, int alignTrack) const
{
    const auto at = m_selection.empty() ? m_playhead : m_selection.start;
    const auto describe = [&](int track) -> QVariantMap {
        const auto id = session::TrackId(track);
        const int clip = clipToAlign(m_project, id, at);
        if (clip < 0) return {{"valid", false}};
        const auto& c = m_project.track(id)->clips[std::size_t(clip)];
        const auto* source = m_project.source(c.source);
        return {{"valid", true}, {"clip", clip}, {"start", qint64(c.timelineStart)}, {"length", qint64(c.length)},
                {"name", source ? QString::fromStdString(source->name) : QString()}};
    };
    return {{"reference", describe(referenceTrack)}, {"align", describe(alignTrack)}, {"stamp", automationStamp()},
            {"running", m_aligningSpeech}, {"message", m_speechAlignMessage}};
}

bool Backend::alignSpeech(int referenceTrack, int alignTrack, double tightness, const QString& stamp)
{
    const auto reject = [this](const QString& message) {
        m_speechAlignMessage = message; setError(message); emit speechAlignmentChanged(); return false;
    };
    if (stamp.isEmpty() || stamp != automationStamp()) return reject(tr("The project changed. Choose the clips again."));
    if (m_aligningSpeech) return reject(tr("An alignment is already running."));
    if (recording() || loading() || sampleEditing() || m_repairing || storageBusy() || m_clipMove)
        return reject(tr("Finish recording or the current edit first."));
    if (referenceTrack == alignTrack) return reject(tr("Choose two different tracks: the reference and the one to align."));
    if (m_project.tracks().size() >= engine::Transport::kMaxTracks) return reject(tr("This session already has as many tracks as OmaAudio plays (%1). Remove one first.").arg(engine::Transport::kMaxTracks));
    const auto target = speechAlignmentTarget(referenceTrack, alignTrack);
    const auto reference = target.value("reference").toMap(), align = target.value("align").toMap();
    if (!reference.value("valid").toBool() || !align.value("valid").toBool())
        return reject(tr("Put the playhead over a clip on each track, or use tracks with one clip each."));
    const auto rate = m_project.sampleRate();
    if (reference.value("length").toLongLong() > kMaxAlignSeconds * rate || align.value("length").toLongLong() > kMaxAlignSeconds * rate)
        return reject(tr("Each clip can be at most 10 minutes long for alignment."));
    const auto heard = heardProject();
    if (!heard.missing.empty()) return reject(tr("Clip effects are still being prepared. Try again in a moment."));
    // Both clips as they play (gain, fades, envelopes and clip effects), alone.
    const auto render = [&](session::TrackId track, int clip) {
        const auto& original = m_project.track(track)->clips[std::size_t(clip)];
        const auto* lane = heard.project.track(track);
        for (const auto& view : lane->clips)
            if (view.timelineStart == original.timelineStart && view.length == original.length) {
                const auto* source = heard.project.source(view.source);
                const auto channels = std::min<std::uint32_t>(source ? source->audio.channels() : 1, 8);
                return session::renderClipRange(heard.project, {view}, view.timelineStart, view.timelineEnd(), channels);
            }
        throw std::runtime_error(tr("A clip is not audible yet. Try again in a moment.").toStdString());
    };
    session::AudioBuffer referenceAudio, alignAudio;
    try {
        referenceAudio = render(session::TrackId(referenceTrack), reference.value("clip").toInt());
        alignAudio = render(session::TrackId(alignTrack), align.value("clip").toInt());
    } catch (const std::exception& error) { return reject(QString::fromUtf8(error.what())); }

    setError({});
    m_aligningSpeech = true;
    m_speechAlignMessage = tr("Aligning…");
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    m_speechAlignCancel = cancel;
    const auto serial = ++m_speechAlignSerial;
    const auto generation = m_sessionGeneration;
    struct Request { session::TrackId alignTrack; session::FramePos start, length; QString name; QString stamp; };
    Request request{session::TrackId(alignTrack), reference.value("start").toLongLong(), reference.value("length").toLongLong(),
                    align.value("name").toString(), stamp};
    emit speechAlignmentChanged();
    m_clipEffectPool.start([this, referenceAudio = std::move(referenceAudio), alignAudio = std::move(alignAudio),
                            rate, tightness, cancel, serial, generation, request]() mutable {
        auto result = std::make_shared<render::SpeechAlignment>(render::alignSpeech(referenceAudio, alignAudio, rate, tightness, cancel.get()));
        referenceAudio = {}; alignAudio = {};
        QMetaObject::invokeMethod(this, [this, result, serial, generation, request]() mutable {
            if (serial != m_speechAlignSerial) return;
            m_aligningSpeech = false; m_speechAlignCancel.reset();
            scheduleMemoryRelease();   // hand the large working buffers back once idle
            const auto finish = [this](const QString& message, bool failed) {
                m_speechAlignMessage = message;
                if (failed) setError(message);
                emit speechAlignmentChanged();
            };
            if (m_shuttingDown || generation != m_sessionGeneration) return finish(tr("Alignment discarded: the session changed."), false);
            if (result->cancelled) return finish(tr("Alignment cancelled. Nothing was changed."), false);
            if (!result->ok) return finish(QString::fromStdString(result->error), true);
            if (request.stamp != automationStamp()) return finish(tr("The project changed during alignment. Align again."), true);
            const auto apply = [this, result, request, finish]() -> bool {
            if (request.stamp != automationStamp()) { finish(tr("The project changed during alignment. Align again."), true); return false; }
            if (m_project.tracks().size() >= engine::Transport::kMaxTracks) { finish(tr("This session already has as many tracks as OmaAudio plays (%1). Remove one first.").arg(engine::Transport::kMaxTracks), true); return false; }
            const auto before = captureEditState();
            auto audio = std::move(result->audio);
            if (audio.frames() * audio.channels() * sizeof(float) >= session::AudioBuffer::kDiskThreshold) audio.storeOnDisk();
            const auto label = (request.name.isEmpty() ? tr("Take") : request.name) + tr(" (aligned)");
            const auto source = m_project.addSource(label.toStdString(), {}, std::move(audio));
            const auto* original = m_project.track(request.alignTrack);
            const auto track = m_project.addTrack((original ? QString::fromStdString(original->name) + tr(" aligned") : label).toStdString());
            session::Clip clip;
            clip.source = source; clip.timelineStart = request.start; clip.length = request.length;
            m_project.track(track)->clips.push_back(clip);
            m_focused = track;
            commit("match speech timing", track, before);
            markPeaksDirty(track);
            pumpPeaks();
            finish(tr("Aligned onto a new track; the timing moved by up to %1 ms. Mute or remove the original when you are happy with it.")
                       .arg(qRound(result->largestShiftMs)), false);
            return true;
            };
            // During an automation pass the new track waits for the pass to end.
            if (deferAutomationEdit(apply)) return;
            apply();
        }, Qt::QueuedConnection);
    });
    return true;
}

void Backend::cancelSpeechAlignment()
{
    if (!m_aligningSpeech) return;
    if (m_speechAlignCancel) m_speechAlignCancel->store(true, std::memory_order_relaxed);
    m_speechAlignMessage = tr("Cancelling…");
    emit speechAlignmentChanged();
}

// Remix: the clip under the playhead on the focused track (or its only clip) is
// re-arranged to a new duration on a worker and replaces the clip in place, one
// Undo step. The source audio is kept, so Undo restores the original exactly.

QVariantMap Backend::remixTarget() const
{
    const auto at = m_selection.empty() ? m_playhead : m_selection.start;
    const int clip = clipToAlign(m_project, m_focused, at);
    if (clip < 0) return {{"valid", false}, {"running", m_remixing}, {"message", m_remixMessage}};
    const auto& c = m_project.track(m_focused)->clips[std::size_t(clip)];
    const auto* source = m_project.source(c.source);
    const auto& clips = m_project.track(m_focused)->clips;
    session::FramePos room = std::numeric_limits<session::FramePos>::max();
    for (const auto& other : clips)
        if (other.timelineStart >= c.timelineEnd()) room = std::min(room, other.timelineStart - c.timelineStart);
    return {{"valid", true}, {"track", int(m_focused)}, {"clip", clip}, {"start", qint64(c.timelineStart)},
            {"length", qint64(c.length)}, {"room", qint64(room == std::numeric_limits<session::FramePos>::max() ? -1 : room)},
            {"name", source ? QString::fromStdString(source->name) : QString()}, {"stamp", automationStamp()},
            {"running", m_remixing}, {"message", m_remixMessage}};
}

bool Backend::remixClip(double targetSeconds, const QString& stamp)
{
    const auto reject = [this](const QString& message) {
        m_remixMessage = message; setError(message); emit remixChanged(); return false;
    };
    if (stamp.isEmpty() || stamp != automationStamp()) return reject(tr("The project changed. Choose the clip again."));
    if (m_remixing) return reject(tr("Music is already being fitted."));
    if (recording() || loading() || sampleEditing() || m_repairing || storageBusy() || m_clipMove)
        return reject(tr("Finish recording or the current edit first."));
    const auto target = remixTarget();
    if (!target.value("valid").toBool()) return reject(tr("Put the playhead over the music clip to fit."));
    const auto rate = m_project.sampleRate();
    const auto targetFrames = session::FramePos(std::llround(targetSeconds * rate));
    if (!std::isfinite(targetSeconds) || targetFrames < session::FramePos(rate) * 5)
        return reject(tr("Choose a duration of at least 5 seconds."));
    const auto room = target.value("room").toLongLong();
    if (room >= 0 && targetFrames > room)
        return reject(tr("The next clip on this track starts too soon for that duration. Move it later first."));
    const auto heard = heardProject();
    if (!heard.missing.empty()) return reject(tr("Clip effects are still being prepared. Try again in a moment."));
    const auto track = session::TrackId(target.value("track").toInt());
    const int clip = target.value("clip").toInt();
    const auto& original = m_project.track(track)->clips[std::size_t(clip)];
    session::AudioBuffer music;
    try {
        for (const auto& view : heard.project.track(track)->clips)
            if (view.timelineStart == original.timelineStart && view.length == original.length) {
                const auto* source = heard.project.source(view.source);
                music = session::renderClipRange(heard.project, {view}, view.timelineStart, view.timelineEnd(),
                                                 std::min<std::uint32_t>(source ? source->audio.channels() : 1, 8));
            }
    } catch (const std::exception& error) { return reject(QString::fromUtf8(error.what())); }
    if (music.empty()) return reject(tr("This clip is not audible yet. Try again in a moment."));

    setError({});
    m_remixing = true;
    m_remixMessage = tr("Finding passages to join…");
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    m_remixCancel = cancel;
    const auto serial = ++m_remixSerial;
    const auto generation = m_sessionGeneration;
    const auto start = original.timelineStart;
    const auto name = target.value("name").toString();
    emit remixChanged();
    m_clipEffectPool.start([this, music = std::move(music), rate, targetFrames, cancel, serial, generation, stamp, track, clip, start, name]() mutable {
        render::RemixSettings settings;
        settings.targetFrames = targetFrames;
        auto result = std::make_shared<render::Remix>(render::remixToDuration(music, rate, settings, cancel.get()));
        music = {};
        QMetaObject::invokeMethod(this, [this, result, serial, generation, stamp, track, clip, start, name]() mutable {
            if (serial != m_remixSerial) return;
            m_remixing = false; m_remixCancel.reset();
            scheduleMemoryRelease();   // hand the large working buffers back once idle
            const auto finish = [this](const QString& message, bool failed) {
                m_remixMessage = message;
                if (failed) setError(message);
                emit remixChanged();
            };
            if (m_shuttingDown || generation != m_sessionGeneration) return finish(tr("Fitting discarded: the session changed."), false);
            if (result->cancelled) return finish(tr("Fitting cancelled. Nothing was changed."), false);
            if (!result->ok) return finish(QString::fromStdString(result->error), true);
            const auto apply = [this, result, stamp, track, clip, start, name, finish]() -> bool {
                if (stamp != automationStamp()) { finish(tr("The project changed while fitting. Fit the music again."), true); return false; }
                const auto before = captureEditState();
                auto audio = std::move(result->audio);
                const auto frames = session::FramePos(audio.frames());
                if (audio.frames() * audio.channels() * sizeof(float) >= session::AudioBuffer::kDiskThreshold) audio.storeOnDisk();
                const auto source = m_project.addSource((name + tr(" (fitted)")).toStdString(), {}, std::move(audio));
                auto& replaced = m_project.track(track)->clips[std::size_t(clip)];
                session::Clip remixed;
                remixed.source = source; remixed.timelineStart = start; remixed.length = frames;
                remixed.gain = 1;   // the rendering already carries the clip's gain, fades and effects
                replaced = remixed;
                commit("fit music to length", track, before);
                markPeaksDirty(track);
                pumpPeaks();
                finish(result->seams == 0 ? tr("The clip is already close to that duration; it was left as it is.")
                       : tr("Remixed with %n join(s). Listen to each join; Undo restores the original.", "", int(result->seams)), false);
                return true;
            };
            if (deferAutomationEdit(apply)) return;
            apply();
        }, Qt::QueuedConnection);
    });
    return true;
}

void Backend::cancelRemix()
{
    if (!m_remixing) return;
    if (m_remixCancel) m_remixCancel->store(true, std::memory_order_relaxed);
    m_remixMessage = tr("Cancelling…");
    emit remixChanged();
}
