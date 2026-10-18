#include "backend.h"
#include "shortname.h"
#include "session/effect_racks.h"
#include "storagepaths.h"

#include <QSet>
#include <cmath>
#include <limits>

namespace {
bool number(const QVariantMap& settings, const QString& key, double fallback, double minimum,
            double maximum, double& result, bool integral = false)
{
    if (!settings.contains(key)) { result = fallback; return true; }
    const auto value = settings.value(key); const auto type = value.metaType().id();
    if (type != QMetaType::Int && type != QMetaType::UInt && type != QMetaType::LongLong
        && type != QMetaType::ULongLong && type != QMetaType::Double && type != QMetaType::Float) return false;
    result = value.toDouble();
    return std::isfinite(result) && result >= minimum && result <= maximum && (!integral || std::floor(result) == result);
}
bool flag(const QVariantMap& settings, const QString& key, bool fallback, bool& value)
{
    if (!settings.contains(key)) { value = fallback; return true; }
    if (settings.value(key).metaType().id() != QMetaType::Bool) return false;
    value = settings.value(key).toBool(); return true;
}
bool text(const QVariantMap& settings, const QString& key, const QString& fallback, QString& value)
{
    if (!settings.contains(key)) { value = fallback; return true; }
    if (settings.value(key).metaType().id() != QMetaType::QString) return false;
    value = settings.value(key).toString(); return !value.contains(QChar::Null);
}

// Full-file jobs have no timeline. Preserve opaque settings on an unautomated
// instance, but explicitly restore the base on every automated instance. A
// shared fromState flag alone cannot express an automated left processor and an
// independently edited, unautomated right processor.
QVariantList staticBatchInserts(std::vector<session::Insert>& inserts)
{
    std::erase_if(inserts, [](const auto& insert) { return insert.bypass; });
    QVariantList captured;
    for (auto& insert : inserts) {
        QVariantList controls;
        for (auto& control : insert.controls) {
            std::array<std::optional<session::InstanceAutomation>, 2> bases{};
            QVariantList displayBases;
            for (std::size_t actual = 0; actual < bases.size(); ++actual) {
                if (control.automationAt(actual).active()
                    || (!control.fromState && control.instanceAutomation[actual]))
                    bases[actual] = session::InstanceAutomation{control.automationBase(actual), {}};
                displayBases.push_back(bases[actual] ? QVariant(bases[actual]->base) : QVariant::fromValue(nullptr));
            }
            control.automation = {};
            control.instanceAutomation = std::move(bases);
            controls.push_back(QVariantMap{{"port", control.port}, {"value", control.value},
                {"instanceBases", displayBases}});
        }
        captured.push_back(QVariantMap{{"uri", QString::fromStdString(insert.uri)}, {"controls", controls},
            {"legacyClapControls", insert.legacyClapControls}});
    }
    return captured;
}
}

QVariantList Backend::fileBatchJobs() const { return m_fileBatch ? m_fileBatch->jobs() : QVariantList{}; }
bool Backend::fileBatchBusy() const { return m_fileBatch && m_fileBatch->busy(); }
QVariantMap Backend::fileBatchStatus() const
{
    return m_fileBatch ? m_fileBatch->status()
        : QVariantMap{{"available", false}, {"error", tr("The file queue is unavailable.")}};
}

void Backend::initialiseFileBatch()
{
    m_fileBatch = std::make_unique<FileBatchProcessor>(m_urids,
        [this](const auto& inserts, lv2::HostFeatures& features, unsigned channels, std::string& problem) {
            // Restored jobs need no open project or live rack. Discover effects
            // only after explicit processing/resume, never while reading a queue.
            if (!inserts.empty() && !m_lv2->scanned()) m_lv2->scan();
            auto chain = std::make_unique<lv2::Chain>(channels, lv2::kMaxBlock, host::ProcessingMode::Offline);
            for (const auto& insert : inserts) {
                if (!chain->append(*m_lv2, features, insert, problem)) return std::unique_ptr<lv2::Chain>{};
                const auto slot = chain->size() - 1;
                for (const auto& control : insert.controls)
                    for (std::size_t actual = 0; actual < chain->instanceCount(slot); ++actual)
                        if (actual < control.instanceAutomation.size() && control.instanceAutomation[actual])
                            chain->setControl(slot, control.port, control.instanceAutomation[actual]->base, UINT64_MAX, actual);
            }
            return chain;
        }, [this] { emit fileBatchChanged(); }, this,
        omaaudio::storage::locations().stateDir + QStringLiteral("/file-batch"));
}

bool Backend::fileBatchOptions(const QVariantMap& settings, FileBatchProcessor::Options& options, QString& error) const
{
    const QSet<QString> allowed{"directory", "suffix", "format", "bitDepth", "sampleRate", "channels", "dither",
        "bitrateKbps", "normalise", "targetLufs", "ceilingDbTp", "peakTargetDb", "includeMetadata", "chain", "track", "stamp", "recipe"};
    for (auto it = settings.begin(); it != settings.end(); ++it) if (!allowed.contains(it.key())) {
        error = tr("Unknown batch setting: %1").arg(it.key()); return false;
    }
    QString format, normalise, chain, stamp; double bits = 0, rate = 0, channels = 0, bitrate = 0, lufs = 0, ceiling = 0, peak = 0, trackId = 0;
    bool metadata = false;
    if (!text(settings, "directory", {}, options.directory) || !text(settings, "suffix", "-processed", options.suffix)
        || !text(settings, "format", "wav", format) || !text(settings, "normalise", "none", normalise)
        || !text(settings, "chain", "none", chain) || !text(settings, "stamp", {}, stamp)
        || !number(settings, "bitDepth", 24, 16, 32, bits, true)
        || !number(settings, "sampleRate", 0, 0, 192000, rate, true) || (rate != 0 && rate < 8000)
        || !number(settings, "channels", 0, 0, 2, channels, true)
        || !number(settings, "bitrateKbps", 0, 0, 320, bitrate, true) || (bitrate != 0 && bitrate < 64)
        || !number(settings, "targetLufs", -16, -70, 0, lufs)
        || !number(settings, "ceilingDbTp", -1, -24, 0, ceiling)
        || !number(settings, "peakTargetDb", -.3, -90, 0, peak)
        || !number(settings, "track", m_focused == session::kInvalidTrack ? -1 : int(m_focused), -1,
                   std::numeric_limits<int>::max(), trackId, true)
        || !flag(settings, "dither", true, options.output.dither)
        || !flag(settings, "includeMetadata", true, metadata)) {
        error = tr("Invalid batch settings. Use valid numbers, text and boolean values for the listed controls."); return false;
    }
    if (!stamp.isEmpty() && stamp != automationStamp()) { error = tr("The effect-chain target changed. Review the batch settings again."); return false; }
    const auto container = render::containerFromId(format.toStdString());
    const auto depth = render::sampleFormatFromBits(int(bits));
    if (!container || !depth || (normalise != "none" && normalise != "peak" && normalise != "loudness")
        || (chain != "none" && chain != "track" && chain != "master" && chain != "recipe")) {
        error = tr("Choose a supported format, bit depth, level mode and effect-chain source."); return false;
    }
    options.output.container = *container; options.output.sampleFormat = *depth;
    options.output.sampleRate = unsigned(rate); options.output.channels = unsigned(channels);
    options.output.bitrateKbps = unsigned(bitrate); options.output.targetLufs = float(lufs);
    options.output.ceilingDbTp = float(ceiling); options.output.peakTargetDb = float(peak);
    options.output.normalise = normalise == "peak" ? render::Normalise::Peak
        : normalise == "loudness" ? render::Normalise::Loudness : render::Normalise::None;
    options.output.writeMetadata = options.output.writeMarkers = metadata;
    options.output.replaceExisting = false;
    if (chain == "track") {
        const auto* track = m_project.track(session::TrackId(int(trackId)));
        if (!track) { error = tr("Choose an existing track for the effect settings."); return false; }
        if (track->routing.sidechain != session::kInvalidTrack) {
            error = tr("This chain uses another track as a sidechain. Choose a chain that processes each file independently."); return false;
        }
        options.inserts = track->inserts; options.effects = tr("Track: %1").arg(shortName(QString::fromStdString(track->name)));
    } else if (chain == "master") { options.inserts = m_project.masterInserts(); options.effects = tr("Master inserts"); }
    else if (chain == "recipe") {
        // A recipe's effects become the chain; Loudness to target becomes the
        // level the file is delivered at (always last). Steps that change a
        // project's clips (Room echo, clicks, fades, Shorten pauses) are not done on
        // files.
        const auto chosen = recipe(settings.value("recipe").toString());
        if (chosen.isEmpty()) { error = tr("Choose a recipe."); return false; }
        QStringList skipped;
        auto* self = const_cast<Backend*>(this);       // reads the catalogue and probes effects only
        for (const auto& value : chosen.value("steps").toList()) {
            const auto step = value.toMap();
            if (!step.value("on", true).toBool()) continue;
            const auto kind = step.value("kind").toString();
            if (kind == QLatin1String("loudness")) {
                normalise = QStringLiteral("loudness");
                options.output.normalise = render::Normalise::Loudness;
                lufs = std::clamp(step.value("lufs").toDouble(), -70.0, 0.0);
                options.output.targetLufs = float(lufs);
                continue;
            }
            if (kind == QLatin1String("voiceCleanup")) {
                if (int(rate) != 48000) { skipped << tr("%1 (needs 48 kHz output)").arg(stepTitle(step)); continue; }
                QString reason;
                if (auto insert = self->recipeInsert(QStringLiteral("urn:omaaudio:denoise"), {{"amount", std::clamp(step.value("amount").toDouble(), 0.0, 100.0)}}, reason))
                    options.inserts.push_back(std::move(*insert));
                else skipped << stepTitle(step);
                continue;
            }
            if (kind == QLatin1String("limit")) {
                // Held again after the level is set, so the delivered peaks stay below it.
                ceiling = std::min(ceiling, std::clamp(step.value("ceiling").toDouble(), -24.0, 0.0));
                options.output.ceilingDbTp = float(ceiling);
            }
            session::Insert insert;
            QString name;
            const auto reason = self->recipeStepInsert(step, insert, name);
            if (reason.isEmpty() && !name.isEmpty()) options.inserts.push_back(std::move(insert));
            else skipped << stepTitle(step);
        }
        options.effects = tr("Recipe: %1").arg(chosen.value("name").toString());
        if (!skipped.isEmpty()) options.effects += tr(" · not done on files: %1").arg(skipped.join(QStringLiteral(", ")));
    }
    else options.effects = tr("No effects");
    // Static settings only: timeline automation/routing belong to the open session.
    const auto captured = staticBatchInserts(options.inserts);
    if (chain != "none") options.effects += tr(" · %1 active effects").arg(options.inserts.size());
    options.settings = {{"format", format}, {"bitDepth", int(bits)}, {"sampleRate", int(rate)}, {"channels", int(channels)},
        {"dither", options.output.dither}, {"bitrateKbps", int(bitrate)}, {"normalise", normalise},
        {"targetLufs", lufs}, {"ceilingDbTp", ceiling}, {"peakTargetDb", peak}, {"includeMetadata", metadata},
        {"chain", chain}, {"effects", options.effects}, {"inserts", captured}};
    if (chain == "recipe") options.settings["recipe"] = settings.value("recipe");
    return true;
}

QVariantMap Backend::previewFileBatch(const QStringList& paths, const QVariantMap& settings) const
{
    FileBatchProcessor::Options options; QString error;
    if (!fileBatchOptions(settings, options, error)) return {{"ok", false}, {"error", error}};
    const auto files = FileBatchProcessor::preview(paths, options, error);
    return {{"ok", error.isEmpty()}, {"error", error}, {"files", files}, {"effects", options.effects}, {"settings", options.settings}};
}

int Backend::queueFileBatch(const QStringList& paths, const QVariantMap& settings)
{
    if (m_shuttingDown) { setError(tr("The application is closing.")); return -1; }
    if (!m_fileBatch || !m_fileBatch->status().value("available").toBool()) {
        setError(fileBatchStatus().value("error").toString()); return -1;
    }
    FileBatchProcessor::Options options; QString error;
    if (!fileBatchOptions(settings, options, error)) { setError(error); return -1; }
    // Once scanned, the shared catalogue is never replaced. Instances keep that
    // world's descriptors alive, while private host features use each file's rate.
    if (!options.inserts.empty() && !m_lv2->scanned()) m_lv2->scan();
    // A track's or the master's live chain is captured with its plugin state; a
    // recipe's effects are made from its settings and need nothing captured.
    const bool capture = !options.inserts.empty() && settings.value("chain").toString() != QLatin1String("recipe");
    options.effectsCaptured = !capture;
    const int batch = m_fileBatch->enqueue(paths, options, error);
    setError(error);
    if (batch < 0 || !capture) return batch;
    const auto key = settings.value("chain").toString() == "master" ? kMasterChainKey
        : session::EffectOwner::track(static_cast<session::TrackId>(settings.value("track", int(m_focused)).toInt()));
    requestDeliverySnapshot(key, false,
        [this, batch] { return m_shuttingDown || !m_fileBatch || !m_fileBatch->awaitingEffects(batch); },
        [this, batch, key, options = std::move(options)](DeliverySnapshot snapshot, QString problem, bool cancelled) mutable {
            if (problem.isEmpty() && !cancelled) {
                const auto* inserts = session::findEffectRack(snapshot.project, key);
                if (!inserts) problem = tr("The captured batch effect chain is unavailable.");
                else {
                    options.inserts = *inserts;
                    options.settings["inserts"] = staticBatchInserts(options.inserts);
                }
            }
            if (!problem.isEmpty()) setError(problem);
            if (m_fileBatch) m_fileBatch->prepareEffects(batch, std::move(options), problem, cancelled);
        });
    return batch;
}
void Backend::cancelBatchFile(int id)
{
    if (id < 0) { setError(tr("Choose a valid batch file ID.")); return; }
    if (m_fileBatch) m_fileBatch->cancel(id);
    pumpDeliverySnapshots();
}
void Backend::cancelFileBatch(int batch)
{
    if (batch < -1) { setError(tr("Choose a valid batch ID, or -1 for every batch.")); return; }
    if (m_fileBatch) m_fileBatch->cancel(-1, batch);
    pumpDeliverySnapshots();
}
bool Backend::retryBatchFile(int id)
{
    QString error;
    if (!m_fileBatch) { setError(tr("There is no batch file to retry.")); return false; }
    const bool accepted = m_fileBatch->retry(id, error); setError(error); return accepted;
}
bool Backend::resumeBatchFile(int id)
{
    if (id < 0) { setError(tr("Choose a valid paused batch file ID.")); return false; }
    QString error;
    if (!m_fileBatch) { setError(tr("There is no batch file to resume.")); return false; }
    const bool accepted = m_fileBatch->resume(id, error); setError(error); return accepted;
}
bool Backend::resumeFileBatch()
{
    QString error;
    if (!m_fileBatch) { setError(tr("There is no batch file to resume.")); return false; }
    const bool accepted = m_fileBatch->resume(-1, error); setError(error); return accepted;
}
void Backend::clearFinishedBatchFiles()
{
    if (m_fileBatch) m_fileBatch->clearFinished();
    const auto status = fileBatchStatus();
    setError(status.value("available").toBool() ? QString{} : status.value("error").toString());
}
bool Backend::resetFileBatchQueue()
{
    if (!m_fileBatch) { setError(tr("There is no batch queue to reset.")); return false; }
    QString error;
    const bool reset = m_fileBatch->resetQueue(error);
    setError(error);
    emit fileBatchChanged();
    return reset;
}
