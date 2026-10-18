#include "filebatch.h"
#include "filebatch_store.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <limits>

using namespace omawave;
namespace {
QString localPath(const QString& value)
{
    if (value.isEmpty() || value.contains(QChar::Null)) return {};
    const QUrl url(value, QUrl::StrictMode);
    QString path = value;
    if (value.startsWith("file:", Qt::CaseInsensitive)) {
        if (!url.isValid() || !url.isLocalFile()
            || (!url.host().isEmpty() && url.host().compare("localhost", Qt::CaseInsensitive) != 0)
            || !url.userInfo().isEmpty() || url.port(-1) != -1 || url.hasQuery() || url.hasFragment()) return {};
        auto local = url; local.setHost({}); path = local.toLocalFile();
    } else if (!url.scheme().isEmpty() && value.contains("://")) return {};
    if (path.isEmpty() || path.contains(QChar::Null)) return {};
    // Lexical cleanup of link/../file changes the file that the user selected.
    // Let the filesystem resolve the components, as the command-line opener does.
    return QDir::isAbsolutePath(path) ? path : QDir::currentPath() + '/' + path;
}
bool exists(const QString& path) { const QFileInfo info(path); return info.exists() || info.isSymLink(); }
bool reserved(const QString& state) { return state == "paused" || state == "preparing" || state == "queued" || state == "reading" || state == "processing"; }
bool pending(const QString& state) { return state == "preparing" || state == "queued" || state == "reading" || state == "processing"; }
QVariantMap loudness(const engine::LoudnessReport& report)
{
    return {{"integrated", double(report.integrated)}, {"range", double(report.range)},
        {"truePeak", double(report.truePeak)}, {"momentaryMax", double(report.momentaryMax)},
        {"shortTermMax", double(report.shortTermMax)}, {"frames", qint64(report.frames)}};
}
}

FileBatchProcessor::FileBatchProcessor(lv2::UridMap& urids, ChainBuilder builder,
                                     std::function<void()> changed, QObject* parent, QString storageRoot)
    : QObject(parent), m_store(std::make_unique<FileBatchStore>(std::move(storageRoot))), m_urids(urids), m_builder(std::move(builder)), m_changed(std::move(changed))
{
    m_store->load(m_jobs, m_nextJob, m_nextBatch);
    m_timer.setInterval(100);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        if (auto* job = find(m_active)) {
            const auto next = m_progress.load(std::memory_order_relaxed);
            if (next != job->progress) { job->progress = next; notify(); }
        }
    });
}
FileBatchProcessor::~FileBatchProcessor() { shutdown(); }
void FileBatchProcessor::notify() { if (!m_closing && m_changed) m_changed(); }
FileBatchProcessor::Job* FileBatchProcessor::find(int id)
{
    const auto found = std::find_if(m_jobs.begin(), m_jobs.end(), [id](const auto& job) { return job.id == id; });
    return found == m_jobs.end() ? nullptr : &*found;
}
bool FileBatchProcessor::busy() const
{
    return std::any_of(m_jobs.begin(), m_jobs.end(), [](const auto& job) { return pending(job.state); });
}

QVariantMap FileBatchProcessor::status() const { return m_store->status(); }
bool FileBatchProcessor::available(QString& error) const
{
    error.clear();
    if (m_closing) error = tr("The application is closing.");
    else if (!status().value("available").toBool()) error = status().value("error").toString();
    return error.isEmpty();
}
void FileBatchProcessor::persistenceFailed()
{
    for (auto& job : m_jobs) {
        if (job.id == m_active || !pending(job.state)) continue;
        job.state = job.options->effectsCaptured ? "paused" : "failed";
        job.message = tr("Queue storage failed. Reopen OmaAudio before resuming captured jobs; queue uncaptured files again.");
    }
    notify();
}
bool FileBatchProcessor::persist(const std::vector<Job>& jobs, int nextJob, int nextBatch, QString& error)
{
    if (m_store->save(jobs, nextJob, nextBatch, error)) return true;
    if (!status().value("available").toBool()) persistenceFailed();
    return false;
}

QVariantList FileBatchProcessor::preview(const QStringList& paths, const Options& options, QString& error)
{
    error.clear();
    if (paths.empty() || paths.size() > 512) { error = tr("Choose between 1 and 512 audio files."); return {}; }
    const auto directory = localPath(options.directory);
    const QFileInfo folder(directory);
    if (directory.isEmpty() || !folder.isDir()) { error = tr("Choose an existing output folder."); return {}; }
    if (options.suffix.contains('/') || options.suffix.contains('\\') || options.suffix.contains(QChar::Null)
        || options.suffix.size() > 128) { error = tr("Use a filename suffix without path separators, up to 128 characters."); return {}; }
    const auto canonical = folder.canonicalFilePath();
    if (canonical.isEmpty()) { error = tr("The output folder is no longer available."); return {}; }
    const QDir destination(canonical);
    const auto extension = QString::fromUtf8(render::containerExtension(options.output.container).data(),
                                             qsizetype(render::containerExtension(options.output.container).size()));
    QVariantList planned; QSet<QString> outputs;
    for (const auto& path : paths) {
        const auto input = localPath(path);
        const QFileInfo file(input);
        const auto name = file.completeBaseName() + options.suffix + "." + extension;
        const auto output = destination.filePath(name);
        QString problem;
        if (input.isEmpty() || !file.isFile()) problem = tr("Input file does not exist or is not a local regular file.");
        else if (name.toUtf8().size() > 255) problem = tr("The output filename is too long; shorten the suffix or input name.");
        else if (outputs.contains(output)) problem = tr("Another chosen file has the same output name.");
        else if (exists(output)) problem = tr("Output already exists. Choose another folder or suffix; files are never replaced.");
        outputs.insert(output);
        planned.push_back(QVariantMap{{"input", input.isEmpty() ? path : input}, {"output", output},
            {"valid", problem.isEmpty()}, {"message", problem}});
    }
    return planned;
}

int FileBatchProcessor::enqueue(const QStringList& paths, Options options, QString& error)
{
    if (!available(error)) return -1;
    if (paths.size() > 512 - qsizetype(m_jobs.size()) || m_nextBatch == std::numeric_limits<int>::max()
        || paths.size() > std::numeric_limits<int>::max() - m_nextJob) {
        error = tr("Clear finished batch rows before adding more files."); return -1;
    }
    const auto entries = preview(paths, options, error);
    if (!error.isEmpty()) return -1;
    options.output.replaceExisting = false; options.output.path.clear();
    options.directory = QFileInfo(localPath(options.directory)).canonicalFilePath();
    const auto settings = std::make_shared<const Options>(std::move(options));
    const int batch = m_nextBatch; int nextJob = m_nextJob; auto candidate = m_jobs;
    QSet<QString> outputs;
    for (const auto& job : m_jobs) if (reserved(job.state)) outputs.insert(job.output);
    for (const auto& entry : entries) {
        const auto map = entry.toMap(); Job job;
        job.id = nextJob++; job.batch = batch; job.options = settings;
        job.input = map.value("input").toString(); job.output = map.value("output").toString();
        job.message = map.value("message").toString();
        if (outputs.contains(job.output)) job.message = tr("A pending or paused batch reserves this output name.");
        if (!job.message.isEmpty()) job.state = "failed";
        else {
            if (!settings->effectsCaptured) job.state = "preparing";
            outputs.insert(job.output);
        }
        candidate.push_back(std::move(job));
    }
    if (!persist(candidate, nextJob, batch + 1, error)) return -1;
    m_jobs = std::move(candidate); m_nextJob = nextJob; ++m_nextBatch;
    notify(); QTimer::singleShot(0, this, [this] { pump(); }); return batch;
}

bool FileBatchProcessor::awaitingEffects(int batch) const
{
    return !m_closing && std::any_of(m_jobs.begin(), m_jobs.end(), [batch](const auto& job) {
        return job.batch == batch && job.state == "preparing";
    });
}

void FileBatchProcessor::prepareEffects(int batch, Options options, const QString& error, bool cancelled)
{
    QString storageError;
    if (!available(storageError) || !awaitingEffects(batch)) return;
    const auto existing = std::find_if(m_jobs.begin(), m_jobs.end(), [batch](const auto& job) { return job.batch == batch; });
    // Capture updates effect state only; keep the already previewed export paths/settings.
    options.directory = existing->options->directory;
    options.suffix = existing->options->suffix;
    options.output = existing->options->output;
    options.effectsCaptured = error.isEmpty() && !cancelled;
    const auto captured = std::make_shared<const Options>(std::move(options));
    auto candidate = m_jobs;
    for (auto& job : candidate) {
        if (job.batch != batch) continue;
        if (captured->effectsCaptured) job.options = captured;
        if (job.state != "preparing") continue;
        job.state = cancelled ? "cancelled" : error.isEmpty() ? "queued" : "failed";
        job.cancelRequested = cancelled;
        job.message = cancelled ? tr("Cancelled before effect settings were captured.") : error;
    }
    if (!persist(candidate, m_nextJob, m_nextBatch, storageError)) {
        // Invalid captures cannot run, even when the previous journal remains healthy.
        for (auto& job : m_jobs) if (job.batch == batch && job.state == "preparing") {
            job.state = "failed"; job.message = storageError;
        }
        notify(); return;
    }
    m_jobs = std::move(candidate); notify();
    QTimer::singleShot(0, this, [this] { pump(); });
}

QVariantList FileBatchProcessor::jobs() const
{
    QVariantList rows;
    for (const auto& job : m_jobs)
        rows.push_back(QVariantMap{{"id", job.id}, {"batch", job.batch}, {"input", job.input}, {"output", job.output},
            {"state", job.state}, {"progress", job.progress}, {"message", job.message}, {"warning", job.warning},
            {"effects", job.options->effects}, {"settings", job.options->settings}, {"report", job.report}});
    return rows;
}

void FileBatchProcessor::pump()
{
    QString storageError;
    if (!available(storageError) || m_active >= 0) return;
    const auto found = std::find_if(m_jobs.begin(), m_jobs.end(), [](const auto& job) { return job.state == "queued"; });
    if (found == m_jobs.end()) { m_timer.stop(); return; }
    m_active = found->id;
    if (exists(found->output)) { complete("failed", tr("Output already exists; it was not replaced.")); return; }
    if (!QFileInfo(found->input).isFile()) { complete("failed", tr("Input file is no longer available.")); return; }
    found->state = "reading"; found->progress = 0; found->message.clear();
    if (!persist(m_jobs, m_nextJob, m_nextBatch, storageError)) {
        found->state = "paused"; found->message = storageError; m_active = -1; notify(); return;
    }
    m_cancel.store(false); m_progress.store(0); m_prepared = {}; m_result = {};
    auto output = found->options->output; output.path = found->output.toStdString(); output.replaceExisting = false;
    const auto input = found->input.toStdString();
    m_timer.start(); notify();
    try {
        m_worker = std::thread([this, input, output] {
            m_prepared = render::prepareBatchFile(input, output, &m_cancel, [this](float progress) { m_progress.store(.3f * progress); });
            QMetaObject::invokeMethod(this, [this] { finishRead(); }, Qt::QueuedConnection);
        });
    } catch (const std::exception& error) { complete("failed", QString::fromUtf8(error.what())); }
}

void FileBatchProcessor::finishRead()
{
    if (m_closing) return;
    if (m_worker.joinable()) m_worker.join();
    auto* job = find(m_active); if (!job) return;
    if (m_cancel.load() || m_prepared.cancelled) { complete("cancelled", tr("Cancelled; no output was written.")); return; }
    if (!m_prepared.ok) { complete("failed", QString::fromStdString(m_prepared.error)); return; }
    job->warning = QString::fromStdString(m_prepared.warning);
    try {
        if (!job->options->inserts.empty()) {
            m_features = std::make_unique<lv2::HostFeatures>(m_urids, m_prepared.sampleRate, lv2::kMaxBlock);
            std::string error;
            m_chain = m_builder(job->options->inserts, *m_features, m_prepared.audio.channels(), error);
            if (!error.empty() || !m_chain) {
                complete("failed", QString::fromStdString(error.empty() ? "Could not create the captured effect chain." : error)); return;
            }
        }
        job->state = "processing"; job->progress = .3f;
        QString storageError;
        if (!persist(m_jobs, m_nextJob, m_nextBatch, storageError)) { complete("paused", storageError); return; }
        m_progress.store(.3f); notify();
        auto prepared = std::move(m_prepared);
        m_worker = std::thread([this, prepared = std::move(prepared)]() mutable {
            try {
                if (m_chain && !m_chain->warmUp(&m_cancel)) {
                    m_result.cancelled = m_cancel.load();
                    m_result.error = m_result.cancelled ? "Effect warm-up cancelled." : "Effect warm-up failed.";
                } else {
                    auto rendered = render::renderBus(std::move(prepared.audio), 1, m_chain.get(), &m_cancel,
                        [this](float progress) { m_progress.store(.3f + .3f * progress); });
                    if (!rendered.ok) { m_result.error = rendered.error; m_result.cancelled = rendered.cancelled; }
                    else m_result = render::exportMix(rendered.audio, prepared.sampleRate, prepared.output, &m_cancel,
                        [this](float progress) { m_progress.store(.6f + .4f * progress); });
                }
            } catch (const std::exception& error) { m_result.error = error.what(); m_result.cancelled = m_cancel.load(); }
            QMetaObject::invokeMethod(this, [this] { finishWrite(); }, Qt::QueuedConnection);
        });
    } catch (const std::exception& error) { complete("failed", QString::fromUtf8(error.what())); }
}

void FileBatchProcessor::applyWriteResult(Job& job)
{
    // Atomic installation is success even when cancellation arrived afterward.
    if (m_result.ok) {
        const auto& report = m_result.report;
        job.report = {{"frames", qint64(report.frames)}, {"sampleRate", int(report.sampleRate)},
            {"channels", int(report.channels)}, {"container", QString::fromStdString(report.container)},
            {"appliedGainDb", report.appliedGainDb}, {"ceilingLimited", report.ceilingLimited},
            {"measured", loudness(report.measured)}, {"delivered", loudness(report.delivered)},
            {"metadataWritten", report.metadataWritten}, {"markersWritten", int(report.markersWritten)}};
        if (!m_result.warning.empty()) {
            if (!job.warning.isEmpty()) job.warning += "; ";
            job.warning += QString::fromStdString(m_result.warning);
        }
        job.progress = 1; job.state = "done"; job.message = tr("Written to %1").arg(job.output);
    } else if (m_result.cancelled || m_cancel.load()) {
        job.state = "cancelled"; job.message = tr("Cancelled; no output was written.");
    } else { job.state = "failed"; job.message = QString::fromStdString(m_result.error); }
}
void FileBatchProcessor::finishWrite()
{
    if (m_closing) return;
    if (m_worker.joinable()) m_worker.join();
    auto* job = find(m_active); if (!job) return;
    applyWriteResult(*job); complete(job->state, job->message);
}

void FileBatchProcessor::complete(QString state, QString message)
{
    if (auto* job = find(m_active)) { job->state = std::move(state); job->message = std::move(message); }
    QString error; persist(m_jobs, m_nextJob, m_nextBatch, error);
    m_chain.reset(); m_features.reset(); m_prepared = {}; m_result = {};
    m_active = -1; notify(); QTimer::singleShot(0, this, [this] { pump(); });
}

void FileBatchProcessor::cancel(int id, int batch)
{
    if (m_closing) return;
    auto candidate = m_jobs; bool changed = false;
    for (auto& job : candidate) {
        if ((id >= 0 && job.id != id) || (batch >= 0 && job.batch != batch) || !reserved(job.state)) continue;
        changed = true; job.cancelRequested = true;
        if (job.id == m_active) m_cancel.store(true);
        else { job.state = "cancelled"; job.message = tr("Cancelled before it started."); }
    }
    if (!changed) return;
    QString error;
    // Even if storage has failed, honour cancellation in this live owner.
    persist(candidate, m_nextJob, m_nextBatch, error);
    m_jobs = std::move(candidate);
    if (!status().value("available").toBool()) persistenceFailed();
    notify();
}

QString FileBatchProcessor::retryProblem(const Job& job) const
{
    if (!job.options->effectsCaptured)
        return tr("This batch did not capture its effect settings. Queue it again from the session you want.");
    if (localPath(job.input).isEmpty() || !QFileInfo(job.input).isFile())
        return tr("Restore the local input file before retrying it.");
    const QFileInfo directory(job.options->directory), output(job.output);
    const auto extension = render::containerExtension(job.options->output.container);
    const auto expected = QDir(job.options->directory).filePath(QFileInfo(job.input).completeBaseName()
        + job.options->suffix + "." + QString::fromUtf8(extension.data(), qsizetype(extension.size())));
    if (!directory.isDir() || directory.canonicalFilePath() != job.options->directory || job.output != expected
        || output.fileName().toUtf8().size() > 255 || job.output == job.input)
        return tr("The captured output location is unavailable or invalid. Queue the file again with a new destination.");
    if (exists(job.output)) return tr("Output already exists; choose a new batch output instead of replacing it.");
    for (const auto& other : m_jobs) if (other.id != job.id && reserved(other.state) && other.output == job.output)
        return tr("A pending or paused batch reserves this output name.");
    return {};
}
bool FileBatchProcessor::retry(int id, QString& error)
{
    if (!available(error)) return false;
    const auto* job = find(id);
    if (!job || pending(job->state)) { error = tr("Choose a finished or paused batch row to retry."); return false; }
    error = retryProblem(*job); if (!error.isEmpty()) return false;
    auto candidate = m_jobs;
    for (auto& row : candidate) if (row.id == id) {
        row.state = "queued"; row.progress = 0; row.cancelRequested = false;
        row.message.clear(); row.warning.clear(); row.report.clear();
    }
    if (!persist(candidate, m_nextJob, m_nextBatch, error)) return false;
    m_jobs = std::move(candidate); notify(); QTimer::singleShot(0, this, [this] { pump(); }); return true;
}
bool FileBatchProcessor::resume(int id, QString& error)
{
    if (!available(error)) return false;
    if (id < -1) { error = tr("Choose a paused batch row to resume."); return false; }
    auto candidate = m_jobs; bool changed = false;
    for (auto& job : candidate) {
        if (id >= 0 && job.id != id) continue;
        if (job.state != "paused") {
            if (id >= 0) { error = tr("Choose a paused batch row to resume."); return false; }
            continue;
        }
        error = retryProblem(job); if (!error.isEmpty()) return false;
        changed = true; job.state = "queued"; job.progress = 0; job.cancelRequested = false;
        job.message.clear(); job.warning.clear(); job.report.clear();
    }
    if (!changed) { error = tr("There are no paused batch rows to resume."); return false; }
    if (!persist(candidate, m_nextJob, m_nextBatch, error)) return false;
    m_jobs = std::move(candidate); notify(); QTimer::singleShot(0, this, [this] { pump(); }); return true;
}
void FileBatchProcessor::clearFinished()
{
    QString error; if (!available(error)) return;
    auto candidate = m_jobs;
    std::erase_if(candidate, [](const auto& job) { return !reserved(job.state); });
    if (candidate.size() == m_jobs.size() || !persist(candidate, m_nextJob, m_nextBatch, error)) return;
    m_jobs = std::move(candidate); notify();
}
bool FileBatchProcessor::resetQueue(QString& error)
{
    if (m_active >= 0 || m_worker.joinable()) { error = tr("Wait for the current file to finish or cancel it first."); return false; }
    if (!m_store->reset(error)) return false;
    m_jobs.clear(); m_nextJob = 1; m_nextBatch = 1;
    notify();
    return true;
}
void FileBatchProcessor::shutdown()
{
    if (m_closing) return;
    m_closing = true; m_timer.stop(); m_cancel.store(true);
    if (m_worker.joinable()) m_worker.join();
    if (auto* job = find(m_active); job && m_result.ok) applyWriteResult(*job);
    for (auto& job : m_jobs) if (pending(job.state)) {
        if (job.cancelRequested) { job.state = "cancelled"; job.message = tr("Cancelled before the queue closed."); }
        else if (!job.options->effectsCaptured) { job.state = "failed"; job.message = tr("Effect capture did not finish. Queue this file again from the intended session."); }
        else { job.state = "paused"; job.message = tr("Paused when OmaAudio closed. Resume to continue with captured settings."); }
        job.progress = 0;
    }
    QString error; persist(m_jobs, m_nextJob, m_nextBatch, error);
    m_chain.reset(); m_features.reset(); m_prepared = {}; m_result = {}; m_active = -1;
}
