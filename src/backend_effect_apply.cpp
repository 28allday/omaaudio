#include "backend.h"
#include "session/edits.h"
#include "session/effect_racks.h"
#include "session/render.h"

#include <QScopeGuard>

// Apply an effect to the selection, the way the Effects menu does on the Waveform
// page. The preview is the effect itself, first in the focused track's chain, so
// it is heard (and adjusted, in the rack's controls or its own window) exactly as
// the applied result will sound. It goes on through the ordinary edits, Undo
// entries and all; Apply and Cancel then walk the history back to where it stood
// and drop the walked-back entries, so the preview never shows in it. Apply
// captures the effect's settings first and writes it into the selection's audio
// as a sample edit: one Undo step, "apply <effect>".

QVariantMap Backend::effectPreview() const
{
    if (!m_effectPreview || m_effectPreview->generation != m_sessionGeneration)
        return {{"active", false}, {"applying", false}};
    const auto& p = *m_effectPreview;
    const auto key = session::EffectOwner::track(p.track);
    int slot = -1;
    if (const auto* inserts = session::findEffectRack(m_project, key))
        for (std::size_t i = 0; i < inserts->size(); ++i)
            if ((*inserts)[i].identity == p.identity) slot = static_cast<int>(i);
    return {{"active", true}, {"applying", m_effectPreviewApplying},
            {"track", static_cast<int>(p.track)}, {"slot", slot}, {"uri", p.uri},
            {"name", QString::fromStdString(p.name)},
            {"start", static_cast<qint64>(p.selection.start)}, {"end", static_cast<qint64>(p.selection.end)}};
}

bool Backend::beginEffectPreview(const QString &uri)
{
    if (m_effectPreview && m_effectPreview->generation == m_sessionGeneration) {
        setError(tr("Apply or cancel the effect being previewed first."));
        return false;
    }
    m_effectPreview.reset();
    if (recording()) { setError(tr("Stop recording before applying an effect.")); return false; }
    if (sampleEditing()) { setError(tr("Wait for the current audio edit to finish.")); return false; }
    const auto track = m_focused;
    if (!m_project.track(track) || m_selection.empty() || m_selection.track != track) {
        setError(tr("Select a time range on a track first: drag across its waveform."));
        return false;
    }
    const auto key = session::EffectOwner::track(track);
    const auto* rack = session::findEffectRack(m_project, key);
    const std::size_t before = rack ? rack->size() : 0;
    EffectPreview preview{.track = track, .selection = m_selection, .uri = uri, .name = {}, .identity = {},
                          .cursor = m_undo.cursor(), .serial = 0, .dirty = m_dirty, .generation = m_sessionGeneration};
    if (preview.cursor > 0) preview.serial = m_undo.commands()[preview.cursor - 1].serial;
    if (!addPlugin(uri, static_cast<int>(track)))
        return false;
    rack = session::findEffectRack(m_project, key);
    if (!rack || rack->size() != before + 1 || rack->back().uri != uri.toStdString()) {
        setError(tr("The effect could not be put on the track for a preview. Try again."));
        return false;
    }
    preview.identity = rack->back().identity;
    if (const auto* info = m_lv2->find(uri.toStdString())) preview.name = info->name;
    if (preview.name.empty()) preview.name = uri.toStdString();
    // First in the chain: the applied audio is processed before the track's
    // other effects, so that is where the preview must sit to sound the same.
    if (before > 0) movePlugin(static_cast<int>(before), 0, static_cast<int>(track));
    m_effectPreview = std::move(preview);
    emit effectPreviewChanged();
    return true;
}

void Backend::rollBackEffectPreview(std::function<void(const EffectPreview&)> then)
{
    const auto apply = [this, then] {
        if (!m_effectPreview) return false;
        const auto preview = *m_effectPreview;
        m_effectPreview.reset();
        m_effectPreviewApplying = false;
        if (preview.generation == m_sessionGeneration) {
            std::optional<std::size_t> target;
            if (!preview.serial) target = 0;
            else {
                const auto& commands = m_undo.commands();
                for (std::size_t i = 0; i < commands.size(); ++i)
                    if (commands[i].serial == preview.serial) target = i + 1;
            }
            if (target && *target <= m_undo.cursor()) {
                // As jumpHistory: this boundary owns the whole walk back.
                const bool applying = m_applyingPluginEdit;
                m_applyingPluginEdit = true;
                const auto restore = qScopeGuard([this, applying] { m_applyingPluginEdit = applying; });
                while (m_undo.cursor() > *target && m_undo.canUndo()) undo();
                m_undo.discardRedo();
                m_dirty = preview.dirty;
            } else {
                // The history from before the preview was trimmed away while it was
                // open; take the effect off as an ordinary, undoable edit instead.
                const int slot = pluginSlot(session::EffectOwner::track(preview.track), preview.identity);
                if (slot >= 0) removePlugin(slot, static_cast<int>(preview.track));
            }
            if (m_project.track(preview.track))
                select(preview.selection.start, preview.selection.end, false, static_cast<int>(preview.track));
            emit timelineChanged();
        }
        emit effectPreviewChanged();
        if (then && preview.generation == m_sessionGeneration) then(preview);
        return true;
    };
    if (deferAutomationEdit(apply)) return;
    if (deferPluginEdit(apply, {}, true)) return;
    apply();
}

void Backend::cancelEffectPreview()
{
    rollBackEffectPreview({});
}

bool Backend::applyEffectPreview()
{
    if (!m_effectPreview || m_effectPreview->generation != m_sessionGeneration) {
        setError(tr("There is no effect preview to apply."));
        return false;
    }
    if (m_effectPreviewApplying) return false;
    if (sampleEditing()) { setError(tr("Wait for the current audio edit to finish.")); return false; }
    setError({});
    const auto key = session::EffectOwner::track(m_effectPreview->track);
    const auto identity = m_effectPreview->identity;
    const auto generation = m_sessionGeneration;
    m_effectPreviewApplying = true;
    emit effectPreviewChanged();
    // The effect's exact settings, including what only its own window holds.
    requestDeliverySnapshot(key, true,
        [this, generation] { return m_shuttingDown || generation != m_sessionGeneration || !m_effectPreview; },
        [this, key, identity](DeliverySnapshot snapshot, QString problem, bool cancelled) {
            std::optional<session::Insert> captured;
            if (problem.isEmpty() && !cancelled) {
                if (const auto* inserts = session::findEffectRack(snapshot.project, key))
                    for (const auto& insert : *inserts)
                        if (insert.identity == identity) captured = insert;
                if (!captured) problem = tr("The previewed effect is no longer on the track.");
            }
            if (cancelled || !problem.isEmpty() || !m_effectPreview) {
                m_effectPreviewApplying = false;
                if (!problem.isEmpty()) setError(problem);
                emit effectPreviewChanged();
                return;
            }
            // A preview heard bypassed (comparing before and after) still applies.
            captured->bypass = false;
            rollBackEffectPreview([this, insert = std::move(*captured)](const EffectPreview& preview) {
                startEffectApply(preview, insert);
            });
        });
    return true;
}

void Backend::startEffectApply(const EffectPreview& preview, session::Insert insert)
{
    if (m_focused != preview.track || m_selection.start != preview.selection.start
        || m_selection.end != preview.selection.end) {
        setError(tr("The selection changed, so the effect was not applied."));
        return;
    }
    auto features = std::make_shared<lv2::HostFeatures>(m_urids, m_project.sampleRate(), lv2::kMaxBlock);
    QString error;
    std::shared_ptr<lv2::Chain> chain = buildOfflineChain({insert}, *features, error);
    if (!chain) {
        setError(error.isEmpty() ? tr("The effect could not be prepared for applying.") : error);
        return;
    }
    if (!m_recipeApplying) {
        m_lastEffect = LastEffect{insert, preview.name};
        recordStep(effectStepFor(insert, preview.name));
    }
    m_stagedEffect = StagedEffect{std::move(features), std::move(chain), {std::move(insert)}, preview.name};
    startSampleEdit(SampleEditKind::ApplyEffect);
    emit effectPreviewChanged();
}

bool Backend::repeatLastEffect()
{
    // Kept across New and Open: applying the same effect to file after file is
    // what this is for.
    if (!m_lastEffect) {
        setError(tr("Apply an effect to a selection first; this applies it again.")); return false;
    }
    if (m_effectPreview && m_effectPreview->generation == m_sessionGeneration) {
        setError(tr("Apply or cancel the effect being previewed first.")); return false;
    }
    if (recording()) { setError(tr("Stop recording before applying an effect.")); return false; }
    if (sampleEditing()) { setError(tr("Wait for the current audio edit to finish.")); return false; }
    if (!m_project.track(m_focused) || m_selection.empty() || m_selection.track != m_focused) {
        setError(tr("Select a time range on a track first: drag across its waveform.")); return false;
    }
    setError({});
    auto insert = m_lastEffect->insert;
    insert.forkIdentity();
    EffectPreview target{.track = m_focused, .selection = m_selection, .uri = QString::fromStdString(insert.uri),
                         .name = m_lastEffect->name};
    startEffectApply(target, std::move(insert));
    return m_lastError.isEmpty();
}

QVariantList Backend::selectionPeaks(int track, qint64 start, qint64 end, int buckets) const
{
    const auto* t = m_project.track(session::TrackId(track));
    const auto rate = m_project.sampleRate();
    if (!t || end <= start || buckets < 1 || buckets > 4096 || rate == 0 || end - start > qint64(rate) * 1200) return {};
    const auto channels = std::uint32_t(std::max(1, trackChannels(*t)));
    std::vector<float> peaks(std::size_t(buckets), 0.0f);
    const double perBucket = double(end - start) / buckets;
    const session::FramePos block = session::FramePos(rate) * 10;
    for (session::FramePos at = start; at < end; at += block) {
        const auto to = std::min<session::FramePos>(end, at + block);
        auto clips = session::edits::copyRange(t->clips, at, to);
        for (auto& clip : clips) { clip.rack.reset(); clip.effectOffset = 0; }
        auto audio = session::renderClipRange(m_project, clips, 0, to - at, channels);
        audio.resize(channels, std::size_t(to - at));
        for (std::uint32_t c = 0; c < channels; ++c) {
            const auto& plane = audio.plane(c);
            for (std::size_t i = 0; i < plane.size(); ++i) {
                const auto b = std::min<std::size_t>(std::size_t(buckets - 1), std::size_t(double(at - start + session::FramePos(i)) / perBucket));
                peaks[b] = std::max(peaks[b], std::abs(plane[i]));
            }
        }
    }
    QVariantList out;
    out.reserve(buckets);
    for (const float v : peaks) out.append(double(std::min(1.0f, v)));
    return out;
}
