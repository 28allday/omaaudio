#include "backend.h"
#include "engine/stream_cache.h"
#include "engine/elastic_runtime.h"

#include <algorithm>

std::vector<session::SourceId> Backend::protectedSourceIds() const
{
    std::vector<session::SourceId> ids;
    const auto retain = [&](const session::SessionState& state) {
        for (const auto& track : state.tracks) for (const auto& clip : track.clips) ids.push_back(clip.source);
    };
    for (const auto& command : m_undo.commands()) { retain(command.before); retain(command.after); }
    for (const auto& clip : m_clipboard) ids.push_back(clip.source);
    if (m_multitrackClipboard) for (const auto& [id, uuid] : m_multitrackClipboard->sources) { (void)uuid; ids.push_back(id); }
    for (const auto& edit : m_deferredAutomationEdits) ids.insert(ids.end(), edit.sources.begin(), edit.sources.end());
    std::sort(ids.begin(), ids.end()); ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

bool Backend::elasticPreparing() const {
    return m_currentPlan && m_currentPlan->elasticRuntime && m_currentPlan->elasticRuntime->preparing();
}
bool Backend::elasticFailed() const {
    return m_currentPlan && m_currentPlan->elasticRuntime && m_currentPlan->elasticRuntime->failed();
}
qulonglong Backend::streamCacheMisses() const
{
    return m_currentPlan && m_currentPlan->streamCache ? m_currentPlan->streamCache->misses() : 0;
}
qulonglong Backend::streamReadErrors() const
{
    return m_currentPlan && m_currentPlan->streamCache ? m_currentPlan->streamCache->readErrors() : 0;
}

QVariantMap Backend::storageUsage() const
{
    QVariantMap out{{"stamp", automationStamp()}};
    try {
        const auto live = m_project.usedSources();
        const auto needed = m_project.usedSources(protectedSourceIds());
        int sources = 0, unused = 0, retained = 0, disk = 0;
        qulonglong memory = 0, backing = 0, unusedBytes = 0;
        QVariantList rows;
        for (const auto& source : m_project.sources()) {
            if (!source) continue;
            const auto id = std::size_t(source->id);
            const qulonglong bytes = source->audio.frames() * source->audio.channels() * sizeof(float);
            const bool stored = bool(source->audio.diskStorage());
            ++sources; if (stored) { ++disk; backing += bytes; }
            memory += source->audio.memoryBytes();
            if (!needed[id]) { ++unused; unusedBytes += bytes; }
            else if (!live[id]) ++retained;
            rows.push_back(QVariantMap{{"name", QString::fromStdString(source->name)},
                {"uuid", QString::fromStdString(source->uuid.toString())}, {"bytes", bytes}, {"disk", stored},
                {"use", live[id] ? "session" : needed[id] ? "undo / clipboard" : "unused"}});
        }
        out.insert("sources", sources); out.insert("unused", unused); out.insert("retained", retained);
        out.insert("diskSources", disk); out.insert("memoryBytes", memory); out.insert("backingBytes", backing);
        out.insert("unusedBytes", unusedBytes); out.insert("rows", rows);
        out.insert("cacheBytes", qulonglong(m_currentPlan && m_currentPlan->streamCache ? m_currentPlan->streamCache->config().bytes() : 0));
        out.insert("droppedRequests", qulonglong(m_currentPlan && m_currentPlan->streamCache ? m_currentPlan->streamCache->droppedRequests() : 0));
    } catch (const std::exception& e) { out.insert("error", QString::fromUtf8(e.what())); }
    return out;
}

void Backend::pruneUnusedSources(const QString& stamp) { startStorage(StorageKind::Prune, stamp); }
void Backend::inspectUnusedAudio() { startStorage(StorageKind::Inspect); }
void Backend::removeUnusedAudio(const QString& stamp) { startStorage(StorageKind::Remove, stamp); }

void Backend::startStorage(StorageKind kind, const QString& stamp)
{
    if (deferAutomationEdit([this, kind, stamp] { startStorage(kind, stamp); return true; })) return;
    const auto refuse = [&](const QString& text) { setError(text); m_storageMessage = text; emit storageChanged(); };
    if (recording()) { refuse("stop recording before managing source storage"); return; }
    if (storageBusy()) { refuse("source storage is already working"); return; }
    (void)captureEditState();
    if (!stamp.isEmpty() && !automationStampCurrent(stamp)) { refuse("the storage target changed; refresh the preview"); return; }
    if (kind != StorageKind::Prune && m_projectPath.isEmpty()) { refuse("save the project before inspecting its bundle audio"); return; }
    if (kind == StorageKind::Remove && (m_dirty || stamp != m_storagePreviewStamp || m_cleanupPreview.empty())) {
        refuse("save any changes and inspect unused audio before removing files"); return;
    }
    StorageRequest request;
    request.kind = kind; request.generation = m_sessionGeneration; request.revision = m_editRevision;
    request.path = m_projectPath; request.protectedIds = protectedSourceIds();
    for (const auto& source : m_project.sources()) if (source) request.protectedUuids.push_back(source->uuid.toString());
    if (kind == StorageKind::Remove) request.files = m_cleanupPreview;
    m_storageRequest = std::move(request);
    m_storageMessage = kind == StorageKind::Prune ? "Checking source dependencies…"
        : kind == StorageKind::Inspect ? "Inspecting saved and recovery audio…" : "Removing previewed unused audio…";
    setError({}); emit storageChanged(); pumpStorage();
}

void Backend::cancelStorage()
{
    const bool queued = m_storageRequest.has_value();
    m_storageRequest.reset(); m_cancelStorage.store(true, std::memory_order_relaxed);
    if (queued) m_storageMessage = "Storage operation cancelled.";
    emit storageChanged();
}

void Backend::pumpStorage()
{
    if (m_shuttingDown || m_storageActive || !m_storageRequest || m_loading || m_saving
        || m_busy.load(std::memory_order_acquire)) return;
    auto request = std::move(*m_storageRequest); m_storageRequest.reset();
    if (request.generation != m_sessionGeneration || request.revision != m_editRevision || request.path != m_projectPath) {
        m_storageMessage = "Storage operation discarded because the session changed.";
        setError(m_storageMessage); emit storageChanged(); return;
    }
    auto snapshot = m_project;
    joinWorker(); m_busy.store(true, std::memory_order_release);
    m_cancelStorage.store(false, std::memory_order_relaxed); m_storageActive = true; emit storageChanged();
    m_worker = std::thread([this, request = std::move(request), snapshot = std::move(snapshot)]() mutable {
        StorageJob job; job.request = request;
        try {
            if (m_cancelStorage.load(std::memory_order_relaxed)) job.cancelled = true;
            else if (request.kind == StorageKind::Prune) {
                job.pruned = snapshot.pruneSources(request.protectedIds);
                if (job.pruned) job.project = std::move(snapshot);
            } else if (request.kind == StorageKind::Inspect) {
                job.preview = io::inspectUnusedAudio(request.path.toStdString(), request.protectedUuids);
                job.error = QString::fromStdString(job.preview.error);
            } else {
                job.cleanup = io::cleanupUnusedAudio(request.path.toStdString(), request.files,
                    request.protectedUuids, &m_cancelStorage);
                job.error = QString::fromStdString(job.cleanup.error);
            }
        } catch (const std::exception& e) { job.error = QString::fromUtf8(e.what()); }
        job.cancelled = job.cancelled || m_cancelStorage.load(std::memory_order_relaxed);
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_pendingStorage.push_back(std::move(job)); m_finishedJobs.push_back(Job::Storage);
        }
        m_busy.store(false, std::memory_order_release); emit workerFinished();
    });
}

void Backend::finishStorage(StorageJob job)
{
    m_storageActive = false;
    if (m_shuttingDown || job.request.generation != m_sessionGeneration) { emit storageChanged(); return; }
    const bool current = job.request.revision == m_editRevision && job.request.path == m_projectPath;
    if (job.request.kind == StorageKind::Remove) {
        m_cleanupPreview.clear(); m_unusedAudioFiles.clear(); m_storagePreviewStamp.clear();
        m_storageMessage = QStringLiteral("Removed %1 unused audio %2 (%3 MiB).%4")
            .arg(job.cleanup.removed).arg(job.cleanup.removed == 1 ? "file" : "files").arg(double(job.cleanup.bytes) / (1024 * 1024), 0, 'f', 2)
            .arg(job.cleanup.cancelled || job.cancelled ? " Remaining files were kept after cancellation." : "");
        if (!job.error.isEmpty()) { m_storageMessage += " " + job.error; setError(job.error); }
    } else if (!current) {
        m_storageMessage = "Storage result discarded because the session changed."; setError(m_storageMessage);
    } else if (job.cancelled) {
        m_storageMessage = "Storage operation cancelled. The session is unchanged.";
    } else if (!job.error.isEmpty()) {
        m_storageMessage = job.error; setError(job.error);
    } else if (job.request.kind == StorageKind::Prune) {
        if (job.project) {
            // Old plans may still borrow a source whose last history reference was
            // dropped. Retire the entire old pool and cache by the normal handshake.
            joinWorker();   // nothing on the worker may be reading the old project
            m_retiredProjects.push_back(std::move(m_project)); m_project = std::move(*job.project);
            m_resetStreaming = true; markDirty(); publishPlan(); publishTrackParams(); rebuildTracks();
            m_cleanupPreview.clear(); m_unusedAudioFiles.clear(); m_storagePreviewStamp.clear();
            emit timelineChanged();
        }
        m_storageMessage = job.pruned ? QStringLiteral("Released %1 unused sources. Undo and clipboard audio were retained. Save before cleaning bundle files.").arg(job.pruned)
            : QStringLiteral("Every source is still needed by the session, undo history or clipboard.");
    } else {
        m_cleanupPreview = std::move(job.preview.files); m_unusedAudioFiles.clear();
        for (const auto& file : m_cleanupPreview)
            m_unusedAudioFiles.push_back(QVariantMap{{"name", QString::fromStdString(file.name)}, {"bytes", qulonglong(file.bytes)}});
        m_storagePreviewStamp = automationStamp();
        m_storageMessage = QStringLiteral("Unreferenced bundle files: %1. Saved, recovery and open-session sources are protected.").arg(m_cleanupPreview.size());
    }
    emit storageChanged();
}
