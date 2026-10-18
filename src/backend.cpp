#include "backend.h"
#include "session/effect_racks.h"
#include "resourcebudget.h"

#include <QElapsedTimer>
#include "audiocommand.h"
#include <set>
#include <QRegularExpression>
#include "storagepaths.h"
#include "session/voice_cleanup.h"
#include "manual.h"
#include "aafimport.h"
#include <QTextDocument>
#include <QTextBlock>
#include <QTextCursor>
#include <QStringConverter>
#include "pluginmetadata.h"
#include "effectmenu.h"
#include "plugincategory.h"

#include "io/audio_file.h"
#include "io/project_file.h"
#include "lv2/chain.h"
#include "lv2/instance.h"
#include <QCoreApplication>
#include "io/peak_builder.h"
#include "io/resampler.h"
#include "io/timeline_peaks.h"
#include "render/export.h"
#include "render/master_render.h"
#include "render/apply_effect.h"
#include "session/edits.h"
#include "session/state.h"
#include "session/zero_crossing.h"
#include "session/elastic.h"
#include "session/clip_reader.h"
#include "session/clip_envelope.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QStandardPaths>
#include <QScopeGuard>
#include <QTimer>
#include <stdexcept>

#include "spectral/harmonics.h"
#include "spectral/repair.h"
#include "session/edits.h"
#include "session/render.h"
#include "render/audio_store.h"
#include "session/mixdown.h"
#include <QUrl>
#include <QDateTime>
#include <QDirIterator>

#include <toml++/toml.hpp>

#include <algorithm>
#include <limits>
#include <csignal>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <span>
#include <string_view>
#include <filesystem>
#include <system_error>

namespace {

// Kept identical to what the engine binary used, because peak files, project bundles
// and the audio path all agree on them and a divergence would be silent.
constexpr std::uint32_t kDefaultRate = 48000;
constexpr std::uint32_t kDefaultQuantum = 256;      // proven xrun-free in spike A
constexpr std::uint32_t kSamplesPerPeak = 256;
constexpr int kSpectralDebounceMs = 400;
constexpr int kStatusIntervalMs = 33;               // ~30 Hz, which is what a playhead needs

session::AudioBuffer renderRange(const session::Project& project, session::TrackId id,
    session::FramePos from, session::FramePos to, std::uint32_t channels, const std::atomic<bool>* cancel = nullptr)
{
    const auto* track = project.track(id);
    if (!track || to <= from) return {};
    auto region = session::renderClipRange(project, session::edits::copyRange(track->clips, from, to),
        0, to - from, channels, cancel);
    if (cancel && cancel->load(std::memory_order_relaxed)) return {};
    region.resize(channels, std::size_t(to - from));
    for (const auto& clip : track->clips) {
        const auto* source = project.source(clip.source);
        if (source && source->audio.diskStorage()) source->audio.diskStorage()->releaseMappedPages();
    }
    return region;
}

std::string runtimeRoot()
{
    return omaaudio::storage::locations().runtimeDir.toStdString();
}

QString displayCacheTemplate()
{
    const auto root = QString::fromStdString(runtimeRoot());
    QDir().mkpath(root);
    return root + QStringLiteral("/display-XXXXXX");
}

} // namespace

Backend::Backend(QObject *parent)
    : QObject(parent),
      m_displayCache(displayCacheTemplate()),
      m_project(kDefaultRate),
      m_transport(m_commands, m_meters, kDefaultRate, &m_captureClock),
      m_output(m_transport, kDefaultRate, kOutputChannels, kDefaultQuantum, &m_captureClock)
{
    // OmaAudio's own effects (the live voice denoiser): an LV2 bundle beside the
    // binary in a build, under lib/omaaudio when installed, or where
    // OMAAUDIO_PLUGIN_BUNDLES points (colon-separated; the test suites).
    static const bool ownPlugins = [] {
        const auto folder = QCoreApplication::applicationDirPath();
        QStringList bundles = qEnvironmentVariable("OMAAUDIO_PLUGIN_BUNDLES").split(QLatin1Char(':'), Qt::SkipEmptyParts);
        bundles << folder + QStringLiteral("/lv2/omaaudio.lv2")
                << QDir::cleanPath(folder + QStringLiteral("/../lib/omaaudio/lv2/omaaudio.lv2"));
        for (const auto& bundle : bundles)
            if (QFileInfo(bundle).isDir()) lv2::World::addBundle(bundle.toStdString());
        return true;
    }();
    (void)ownPlugins;

    // The Effects menu follows the catalogue and the disabled list. The core set
    // ships in the resources; rebuilt only when the result differs, since the
    // catalogue signal also carries scanning state.
    const auto rebuildEffectMenu = [this] {
        static const CoreEffects core = loadCoreEffects(QStringLiteral(":/effects/core.toml"));
        QVariantMap menu = buildEffectMenu(m_plugins, m_pluginLibrary.disabled(), core);
        if (menu == m_effectMenu) return;
        m_effectMenu = std::move(menu);
        emit effectMenuChanged();
    };
    connect(this, &Backend::pluginsChanged, this, rebuildEffectMenu);
    connect(this, &Backend::pluginLibraryChanged, this, rebuildEffectMenu);

    // Track views follow the ids incrementally (one delegate per added track), and
    // each re-reads its track only when that track's published data changed.
    connect(this, &Backend::tracksChanged, this, [this] {
        std::vector<int> ids;
        ids.reserve(m_project.tracks().size());
        for (const auto& track : m_project.tracks()) ids.push_back(int(track.id));
        m_trackIdModel.setIds(ids);
        QVariantList changed;
        std::unordered_map<int, QVariantMap> published;
        published.reserve(m_tracks.size());
        for (const auto& entry : m_tracks) {
            const auto map = entry.toMap();
            const int id = map.value(QStringLiteral("id")).toInt();
            const auto previous = m_publishedTracks.find(id);
            if (previous == m_publishedTracks.end() || previous->second != map) changed.append(id);
            published.emplace(id, map);
        }
        m_publishedTracks = std::move(published);
        ++m_tracksRevision;
        QVariantList summaries;
        summaries.reserve(m_tracks.size());
        for (const auto& entry : m_tracks) {
            const auto map = entry.toMap();
            summaries.append(QVariantMap{{"id", map.value("id")}, {"name", map.value("name")},
                                         {"frames", map.value("frames")}, {"height", map.value("height")}});
        }
        m_trackSummaries = std::move(summaries);
        emit trackSummariesChanged();
        if (!changed.isEmpty()) emit trackDataChanged(changed);
    });
    // Queued on purpose: the worker thread emits it, and everything the slot touches
    // belongs to the GUI thread. This is the whole of the old server_.wake().
    m_systemMemoryMiB = omaaudio::resources::systemMemoryMiB();
    m_spectralStore = SpectralStore(omaaudio::storage::locations().cacheDir + QStringLiteral("/spectral"));
    m_storePool.setMaxThreadCount(1);
    // "Timecode at the video's rate" follows the picture: its rate when one is
    // read, its drop-frame choice when that is changed, plain time when it goes.
    connect(this, &Backend::videoChanged, this, [this] {
        if (m_timeDisplay == omaaudio::timeformat::videoKey()) emit timeDisplayChanged();
    });
    connect(this, &Backend::timelineChanged, this, &Backend::syncVideoReference);
    connect(this, &Backend::timelineChanged, this, &Backend::automationStampChanged);
    // Pauses found go stale when the project changes; tell their readers.
    connect(this, &Backend::automationStampChanged, this, [this] { if (m_pauseFind) emit pausesChanged(); });
    connect(this, &Backend::timelineChanged, this, [this] {
        if (m_displayMusicalTime == m_project.musicalTime()) return;
        m_displayMusicalTime = m_project.musicalTime();
        emit musicalTimeChanged();
        emit snappingChanged();
        if (m_timeDisplay == QLatin1String("barsbeats")) emit timeDisplayChanged();
    });
    m_videoTimer = new QTimer(this);
    m_videoTimer->setInterval(33);
    connect(m_videoTimer, &QTimer::timeout, this, &Backend::videoTick);
    connect(this, &Backend::workerFinished, this, &Backend::drainWorker,
            Qt::QueuedConnection);

    // The directory as well as the file: an editor that writes through a temp file and
    // renames it would otherwise take the watch with it on the first save.
    m_keymapWatcher = new QFileSystemWatcher(this);
    const QString keymap = userKeymapPath();
    m_keymapWatcher->addPath(QFileInfo(keymap).absolutePath());
    if (QFile::exists(keymap))
        m_keymapWatcher->addPath(keymap);
    connect(m_keymapWatcher, &QFileSystemWatcher::fileChanged, this, [this, keymap] {
        if (QFile::exists(keymap) && !m_keymapWatcher->files().contains(keymap))
            m_keymapWatcher->addPath(keymap);
        emit keymapChanged();
    });
    connect(m_keymapWatcher, &QFileSystemWatcher::directoryChanged, this, [this, keymap] {
        if (QFile::exists(keymap) && !m_keymapWatcher->files().contains(keymap))
            m_keymapWatcher->addPath(keymap);
        emit keymapChanged();
    });

    m_retirementPool.setMaxThreadCount(1);
    m_retirementPool.setExpiryTimeout(1000);

    auto *status = new QTimer(this);
    connect(status, &QTimer::timeout, this, &Backend::pollStatus);
    status->start(kStatusIntervalMs);

    // The spectral display rebuilds after a change, debounced: a spectrum is a full
    // pass over the take and a fader nudge is not a reason to run it twice.
    m_spectralTimer = new QTimer(this);
    m_spectralTimer->setSingleShot(true);
    m_spectralTimer->setInterval(kSpectralDebounceMs);
    connect(m_spectralTimer, &QTimer::timeout, this, &Backend::pumpSpectral);

    // Autosave (A22): armed by the first change after a write, fires once. If the
    // slot is busy when it fires it arms itself again rather than queueing behind
    // whatever holds it.
    m_autosaveTimer = new QTimer(this);
    m_autosaveTimer->setSingleShot(true);
    connect(m_autosaveTimer, &QTimer::timeout, this, [this] { autosave(/*synchronous=*/false); });
    m_instanceId = QString::fromStdString(session::Uuid::generate().toString());
    scanRecovery();
    scanRecordingRecovery();
    if (const auto error = m_pluginLibrary.load(); !error.isEmpty())
        setError(error);
    reloadRecentDocuments();
    initialiseFileBatch();
}

std::string Backend::runtimeDir() const
{
    return m_displayCache.path().toStdString();
}

Backend::~Backend()
{
    quiesceForShutdown();
    // Queued completions (a finished clip-effect render holds its effect chain)
    // can never run now. Drop them while the plugin world they free into still
    // exists; QObject's own destructor would otherwise free them after it.
    QCoreApplication::removePostedEvents(this);
    consumePluginNotifications();
    m_storePool.waitForDone();
    // A graphics checkpoint is an explicit handoff, including for a clean session.
    // Keep it intact until the next process adopts it.
    if (!m_graphicsCheckpoint) {
        if ((m_dirty || hasUnverifiedPluginState()) && m_loaded && m_autosaveSeconds > 0)
            autosave(/*synchronous=*/true);
        else if (!m_dirty && !hasUnverifiedPluginState())
            dropRecovery();
    }

    // The textures are ours and live in the runtime directory; leaving them behind
    // would accumulate a megabyte per take per session.
    for (auto &entry : m_peaks)
        removeTextures(entry.second);
    removeTextures(m_analysis.peaks);
    clearSpectral();
}

void Backend::quiesceForShutdown()
{
    if (m_shuttingDown)
        return;
    finishAutomationRecording();
    m_shuttingDown = true;
    m_cancelPluginScan.store(true, std::memory_order_relaxed);
    cancelClipEffectRenders(true);
    if (m_speechAlignCancel) m_speechAlignCancel->store(true, std::memory_order_relaxed);
    if (m_remixCancel) m_remixCancel->store(true, std::memory_order_relaxed);
    m_computerCheckCancel->store(true, std::memory_order_relaxed);
    m_clipEffectPool.waitForDone();
    resumeClipEffectWaiters();
    pruneEditors(true);
    cancelPluginEdits();
    if (m_fileBatch) m_fileBatch->shutdown();
    cancelCollection();
    if (m_collectThread.joinable()) m_collectThread.join();
    m_responseCancel.store(true, std::memory_order_relaxed);
    if (m_responseThread.joinable()) m_responseThread.join();
    m_cancelLoad.store(true, std::memory_order_relaxed);
    m_autosaveTimer->stop();
    m_videoTimer->stop();
    m_videoProbeCancel.store(true, std::memory_order_release);
    if (m_videoProbeThread.joinable()) m_videoProbeThread.join();
    m_videoDecoder.reset();
    m_spectralTimer->stop();
    if (m_inputScan) { m_inputScan->cancel(); m_inputScan = nullptr; }
    if (m_audioSetupScan) { m_audioSetupScan->cancel(); m_audioSetupScan = nullptr; }
    if (m_outputLinks) { m_outputLinks->cancel(); m_outputLinks = nullptr; }
    stopRecording();
    if (m_recording) m_recording->wait();
    for (auto& input : m_additionalRecordings) input->writer->wait();
    cancelSampleEdit();
    cancelStorage();
    cancelTrackLoudness(); cancelDucking();
    // Cancel measurement without erasing the existing print from recovery.
    ++m_noisePrintSequence;
    m_noisePrintRequest.reset();
    m_cancelNoisePrint.store(true, std::memory_order_relaxed);
    m_cancelRepair.store(true, std::memory_order_relaxed);
    m_cancelPitch.store(true, std::memory_order_relaxed);
    m_cancelPeaks.store(true, std::memory_order_relaxed);
    m_cancelDetect.store(true, std::memory_order_relaxed);
    if (m_activeRender < 0) m_cancelJob.store(true, std::memory_order_relaxed);
    // A queued state capture may be waiting for the current output block. Join
    // the callback before draining saves synchronously, without GUI polling.
    m_output.stop();
    m_ready = false;
    while (!m_deliverySnapshots.empty() && m_deliveryAccess && m_deliveryAccess->ready())
        pumpDeliverySnapshots();
    joinWorker();
    m_stagedEffect.reset();   // an effect prepared for applying, never started
    drainWorker();
    finishRecording();
    if (m_autosavePending) autosave(/*synchronous=*/true);
    // A Save accepted just before closing still has to reach disk. Background
    // display work is no longer scheduled while these deliberate writes drain.
    while (m_saving) {
        pumpSave();
        joinWorker();
        drainWorker();
    }
    m_retirementPool.waitForDone();
    for (QTimer *timer : findChildren<QTimer *>(QString(), Qt::FindDirectChildrenOnly))
        timer->stop();
}

bool Backend::start()
{
    std::string error;
    if (!m_output.start(error)) {
        setError(QString::fromStdString("no audio output: " + error));
        return false;
    }
    m_ready = true;
    emit readyChanged();
    return true;
}

bool Backend::commandBusy()
{
    bool completed = false;
    { const std::lock_guard<std::mutex> lock(m_mutex); completed = !m_finishedJobs.empty(); }
    const bool queuedRender = std::any_of(m_renders.begin(), m_renders.end(),
        [](const auto &job) { return job.state == QLatin1String("queued"); });
    return m_busy.load(std::memory_order_acquire) || completed || m_loading || m_saving
        || m_autosaving || m_scanning || m_analysing || rendering() || queuedRender || fileBatchBusy() || collecting() || deliveryBoundaryActive() || m_pluginEditCapturing || !m_pluginEdits.empty()
        || sampleEditing() || storageBusy() || m_repairing || m_detecting || noisePrintBuilding()
        || m_pitchTracking || m_measuringTrackLoudness || m_generatingDucking || m_videoProbing
        || m_recordStopping || m_automationFinishPending || (m_automationEditQueueActive && !m_automationFinishFailed)
        || audioSetupScanning() || audioLinkBusy() || scanningInputs() || m_spectralBuilding || m_effectPreviewApplying
        || (m_spectralWanted && m_spectralTimer->isActive()) || !m_dirtyPeaks.empty() || m_recipeRunner.has_value();
}

void Backend::synchronizeCommandState()
{
    // In an offline instance, the stopped transport still acknowledges seeks and
    // parameter commands. Hardware instances keep their callback as sole reader.
    pollStatus();
}

void Backend::cancelCommandWork()
{
    discardDeferredAutomationEdits();
    stopRecipe();
    m_cancelPluginScan.store(true, std::memory_order_relaxed);
    cancelPluginEdits();
    cancelLoad(); cancelSampleEdit(); cancelStorage(); cancelTrackLoudness(); cancelDucking();
    cancelFileBatch(); cancelCollection();
    cancelRepair(); cancelPitchTracking(); cancelAudioSetupScan();
    m_videoProbeCancel.store(true, std::memory_order_release);
    m_cancelDetect.store(true, std::memory_order_relaxed);
    m_autoHealPending = false;
    if (m_queuedDetect) { m_queuedDetect.reset(); m_detecting = false; emit clicksChanged(); }
    cancelRecordingInputScan(); cancelAudioLinks();
    m_cancelNoisePrint.store(true, std::memory_order_relaxed);
    m_noisePrintRequest.reset();
    m_cancelJob.store(true, std::memory_order_relaxed);
    for (const auto &job : m_renders) cancelRender(job.id);
    stop();
}

int Backend::sampleRate() const { return static_cast<int>(m_project.sampleRate()); }
int Backend::samplesPerPeak() const { return static_cast<int>(kSamplesPerPeak); }
int Backend::maxTracks() const { return static_cast<int>(engine::Transport::kMaxTracks); }

qint64 Backend::takeFrames() const { return m_project.length(); }

double Backend::durationSeconds() const
{
    const auto rate = static_cast<double>(m_project.sampleRate());
    return rate > 0.0 ? static_cast<double>(m_project.length()) / rate : 0.0;
}

double Backend::dbFromLinear(float linear)
{
    if (linear <= 0.0001f)
        return -80.0;
    return 20.0 * std::log10(static_cast<double>(linear));
}

float Backend::linearFromDb(double db)
{
    if (db <= -80.0)
        return 0.0f;
    return static_cast<float>(std::pow(10.0, db / 20.0));
}

void Backend::clearError() { setError({}); }

void Backend::setError(const QString &message)
{
    if (m_lastError == message)
        return;
    m_lastError = message;
    emit lastErrorChanged();
}

// --- loading -------------------------------------------------------------------------

void Backend::open(const QString &path) { beginLoad(path, /*replaceProject=*/true); }
void Backend::importFile(const QString &path) { beginLoad(path, /*replaceProject=*/false); }

bool Backend::tryImportFile(const QString &path, bool openWhenEmpty)
{
    if (m_shuttingDown || commandBusy()) return false;
    beginLoad(path, openWhenEmpty && m_project.tracks().empty());
    return m_loading;
}

void Backend::beginLoad(const QString &path, bool replaceProject)
{
    if (m_busy.load(std::memory_order_acquire) || m_loading || m_saving || m_projectLoadActive) {
        setError(QStringLiteral("still working on the previous file; wait for it to finish"));
        return;
    }
    joinWorker();
    m_busy.store(true, std::memory_order_release);

    setError({});
    m_loading = true;
    m_cancelLoad.store(false, std::memory_order_relaxed);
    emit loadingChanged();

    // A file: URL from a portal dialog, or a plain path from the command line.
    const QUrl url(path);
    const std::string local =
        (url.isLocalFile() ? url.toLocalFile() : path).toStdString();
    const std::uint32_t projectRate = m_project.sampleRate();
    const std::uint64_t generation = m_sessionGeneration;

    const std::uint64_t revision = m_editRevision;
    m_worker = std::thread([this, local, projectRate, replaceProject, generation, revision] {
        LoadedTake out;
        out.sessionGeneration = generation;
        out.editRevision = revision;
        out.projectRate = projectRate;
        out.path = local;
        out.name = std::filesystem::path(local).filename().string();
        out.replaceProject = replaceProject;

        try {
        io::AudioFile file = io::readAudioFile(local, &m_cancelLoad, {});
        if (!file.ok) {
            out.error = file.error;
        } else {
            out.warning = file.error;       // ok with a reservation
            out.formatName = file.formatName;
            out.metadata = std::move(file.metadata);
            out.originalRate = file.sampleRate;
            session::AudioBuffer audio = std::move(file.audio);
            if (file.sampleRate != projectRate) {
                // Section 2: resample on import, never mid-graph.
                const io::Resampler resampler(file.sampleRate, projectRate);
                audio = resampler.process(audio, &m_cancelLoad);
                out.resampled = true;
            }
            out.channels = audio.channels();
            out.audio = std::move(audio);
            out.ok = true;
        }

        } catch (const std::exception& e) { out.ok = false; out.audio = {}; out.error = e.what(); }
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_pendingLoad.push_back(std::move(out));
            m_finishedJobs.push_back(Job::Load);
        }
        m_busy.store(false, std::memory_order_release);
        emit workerFinished();
    });
}

void Backend::finishAudioLoad(LoadedTake take, bool verified, QString verificationError)
{
    if (take.sessionGeneration != m_sessionGeneration || m_shuttingDown) return;
    if (!verified && take.ok && take.replaceProject && !m_cancelLoad.load(std::memory_order_relaxed)) {
        const auto generation = take.sessionGeneration, revision = take.editRevision;
        verifyLoadRevision(generation, revision,
            [this, take = std::move(take)](QString error) mutable {
                finishAudioLoad(std::move(take), true, std::move(error));
                pumpWorkerJobs();
            });
        return;
    }
    // Keep loading true through the acceptance decision. A loadingChanged
    // observer must not start another document job before this one is finished.
    const auto finished = qScopeGuard([this, generation = take.sessionGeneration] {
        if (generation != m_sessionGeneration) return; // installation/reset notified already
        m_loading = false;
        emit loadingChanged();
    });
    if (m_cancelLoad.load(std::memory_order_relaxed))
        setError(QStringLiteral("Audio loading cancelled; the session is unchanged."));
    else if (!take.ok)
        setError(QString::fromStdString(take.error));
    else if (!verificationError.isEmpty())
        setError(verificationError);
    else if (!take.replaceProject && take.projectRate != m_project.sampleRate())
        setError(QStringLiteral("The project's sample rate changed while the file was loading. Import it again."));
    else
        installTake(std::move(take));
}

void Backend::cancelLoad()
{
    if (!m_loading) return;
    m_cancelLoad.store(true, std::memory_order_relaxed);
}

QVariantMap Backend::importSupport() const
{
    const io::ImportSupport support = io::importSupport();
    QVariantMap out;
    out["aac"] = support.aac;
    out["reason"] = QString::fromStdString(support.reason);
    return out;
}

void Backend::installTake(LoadedTake take)
{
    if (!take.replaceProject && !m_applyingAutomationEdit && !m_applyingPluginEdit) {
        auto pending = std::make_shared<LoadedTake>(std::move(take));
        if (deferAutomationEdit([this, pending] { installTake(std::move(*pending)); return true; })) return;
        take = std::move(*pending);
    }
    // Adding a source touches the deque the audio thread reads through the plan, so no
    // worker may be running while it happens.
    joinWorker();
    // A reservation about the file (truncated, damaged) reaches the shell after the
    // take is in place, so the message describes something that exists.
    if (take.ok && !take.warning.empty()) {
        QMetaObject::invokeMethod(this, [this, warning = QString::fromStdString(take.warning)] {
            emit importWarning(warning);
        }, Qt::QueuedConnection);
    }

    const std::uint32_t originalRate = take.originalRate;
    const bool resampled = take.resampled;
    const QString path = QString::fromStdString(take.path);
    const std::string name = take.name;
    const auto frames = static_cast<session::FramePos>(take.audio.frames());

    if (take.replaceProject) {
        resetSessionJobs();
        // The old project's audio is still being read by the audio thread through the
        // current plan, so it is retired rather than destroyed.
        dropRecovery();
        const std::uint32_t rate = m_project.sampleRate();
        m_retiredProjects.push_back(std::move(m_project));
        m_project = session::Project(rate);
        for (auto &entry : m_peaks)
            removeTextures(entry.second);
        m_peaks.clear();
        m_dirtyPeaks.clear();
        removeTextures(m_analysis.peaks);
        m_analysis = Analysis{};
        m_masterLevels.clear();
        m_masterReport.clear();
        m_shortTermHistory.clear();
        m_momentaryHistory.clear();
        m_analysisStale = true;
        emit analysisChanged();
        m_focused = session::kInvalidTrack;
        enqueueAudioCommand(engine::Command::resetLoudness());
        clearSpectral();
        m_bandLowHz = 0.0;
        m_bandHighHz = 0.0;
        clearNoisePrint();
        clearMask();
        m_soundChoices.clear();
        if (m_hearWithoutEffects) { m_hearWithoutEffects = false; emit previewChanged(); }
        // Everything else the old session had: its undo history (whose snapshots
        // name the old project's sources), its clipboard, its path (a Save would
        // have written this take over the old project), its dirtiness, its
        // running chains (a new project's track 0 would have picked up the old
        // track 0's plugins) and its bus trim.
        m_undo.clear();
        m_clipboard.clear(); m_clipboardFrames = 0; m_clipboardChannel = -1;
        m_multitrackClipboard.reset(); m_selectedTracks.clear(); m_selectedClips.clear(); m_trackSelectionAnchor = session::kInvalidTrack; m_clipGroupsLinked = true;
        ++m_trackSelectionRevision;
        m_projectPath.clear();
        m_dirty = false;
        m_confirmedEditRevision = m_savedRevision = m_editRevision;
        m_selection = session::Selection{};
        for (auto &entry : m_chains) {
            if (entry.second != nullptr)
                m_retiredChains.push_back(std::move(entry.second));
        }
        m_chains.clear();
        m_chainSpecs.clear();
        enqueueAudioCommand(engine::Command::setMasterTrim(1.0f));
        rebuildMaster();
        emit masterChanged();
        emit selectionChanged();
        m_previewMatched = false;
        m_previewMatchDb = 0.0;
        if (m_previewBefore) {
            m_previewBefore = false;
            pushPreview();
        }
        emit previewChanged();
    }

    if (m_project.tracks().size() >= engine::Transport::kMaxTracks) {
        setError(QStringLiteral("this session already has as many tracks as OmaAudio plays (%1); remove one to add another").arg(engine::Transport::kMaxTracks));
        return;
    }
    // A take added to a session is an edit like any other. Without its own step
    // in the history, undoing an earlier edit restored a state from before the
    // import and the new track went with it, beyond the reach of Redo.
    std::optional<session::SessionState> beforeImport;
    if (!take.replaceProject) beforeImport = captureEditState();
    // What the file said about itself follows it in, at the project's rate.
    session::AudioMetadata provenance = std::move(take.metadata);
    const std::uint32_t projectRate = m_project.sampleRate();
    if (resampled) {
        for (auto &marker : provenance.markers) {
            const auto end = session::rescaleFrames(marker.position + marker.length, originalRate, projectRate);
            marker.position = session::rescaleFrames(marker.position, originalRate, projectRate);
            marker.length = std::max<std::int64_t>(0, end - marker.position);
        }
        if (provenance.bext.present)
            provenance.bext.timeReference = session::rescaleFrames(provenance.bext.timeReference, originalRate, projectRate);
    }
    const bool timestamped = provenance.bext.present && provenance.bext.timeReference > 0;
    const session::SourceId id = m_project.addSource(
        take.name, take.path, std::move(take.audio),
        resampled ? std::optional<std::uint32_t>(originalRate) : std::nullopt,
        session::Uuid{}, std::nullopt, 0, std::move(provenance));

    const session::TrackId trackId = m_project.addTrack(name);
    session::Track *track = m_project.track(trackId);

    // Where it lands: the start (a new session always), the playhead, or its
    // recorded time against the project's reference. The first timestamped
    // import into a project without a reference sets the reference and lands at 0.
    session::FramePos start = 0;
    QString placementNote;
    if (!take.replaceProject) {
        if (m_importPlacement == QLatin1String("playhead")) start = m_playhead;
        else if (m_importPlacement == QLatin1String("timestamp")) {
            const auto *source = m_project.source(id);
            if (!timestamped || !source) {
                placementNote = QStringLiteral("%1 carries no recorded time; it was placed at the start").arg(QString::fromStdString(name));
            } else if (!m_project.metadata().timeReferenceSet) {
                m_project.metadata().timeReferenceSet = true;
                m_project.metadata().timeReference = source->provenance.bext.timeReference;
                placementNote = QStringLiteral("The project's time reference is now %1, from %2")
                                    .arg(formatTimeReference(source->provenance.bext.timeReference, static_cast<int>(projectRate)), QString::fromStdString(name));
                emit metadataChanged();
            } else {
                const std::int64_t offset = source->provenance.bext.timeReference - m_project.metadata().timeReference;
                if (offset < 0) placementNote = QStringLiteral("%1 was recorded %2 before the project's time reference; it was placed at the start")
                                                    .arg(QString::fromStdString(name), formatTimeReference(-offset, static_cast<int>(projectRate)));
                else start = static_cast<session::FramePos>(offset);
            }
        }
    }
    if (!placementNote.isEmpty()) {
        QMetaObject::invokeMethod(this, [this, placementNote] { emit importWarning(placementNote); }, Qt::QueuedConnection);
    }

    session::Clip whole;
    whole.source = id;
    whole.timelineStart = start;
    whole.sourceOffset = 0;
    whole.length = frames;
    track->clips = {whole};

    m_focused = trackId;
    clearGroupSelection();
    m_loaded = true;
    m_takePath = path;
    m_takeName = QString::fromStdString(name);
    m_takeChannels = static_cast<int>(take.channels);
    m_originalRate = static_cast<int>(originalRate);
    m_resampled = resampled;

    publishPlan();
    publishTrackParams();
    markPeaksDirty(trackId);
    pumpPeaks();
    rebuildTracks();
    markSpectralStale();
    if (take.replaceProject) {
        rebuildMarkers();
        emit projectSampleSettingsChanged();
    } else {
        if (beforeImport)
            (void)m_undo.push("import audio", trackId, *beforeImport, session::captureState(m_project));
        markDirty();            // a take added to a session is a change to it
    }

    rememberRecording(path);
    rememberDocument(path, false);
    emit takeChanged();
    emit timelineChanged();
}

// --- the plan ------------------------------------------------------------------------

void Backend::publishPlan(bool curvesOnly, bool audibleChange)
{
    refreshLoop();
    // Clip effects play from their renderings; a region still rendering is silent.
    // Without effects, the clips play as they are.
    const auto heard = m_hearWithoutEffects ? [this] {
        render::AppliedClipEffects dry;
        dry.project = m_project;
        for (auto& track : dry.project.tracks())
            for (auto& clip : track.clips) { clip.rack.reset(); clip.effectOffset = 0; }
        return dry;
    }() : heardProject();
    auto plan = std::make_shared<engine::RenderPlan>(
        engine::buildRenderPlan(heard.project, kOutputChannels, m_currentPlan.get()));
    plan->document = m_sessionGeneration;
    previewAutomation(*plan);

    // Only the armed interval is silent in the playback plan during capture.
    // Borrow audio from the live project, not a temporary Project's source objects.
    if (m_recording && m_recordPlacement.generation == m_sessionGeneration) {
        for (auto& pt : plan->tracks) {
            const bool extra = std::any_of(m_additionalRecordings.begin(), m_additionalRecordings.end(),
                [&](const auto& input) { return pt.id == input->placement.target; });
            if (pt.id != m_recordPlacement.target && !extra) continue;
            auto clips = heard.project.track(pt.id)->clips;
            const auto end = m_recordPlacement.passLength > 0 ? m_recordPlacement.start + m_recordPlacement.passLength : m_recordPlacement.limit > 0
                ? m_recordPlacement.start + m_recordPlacement.limit : m_project.length();
            session::edits::clearRange(clips, m_recordPlacement.start, end);
            auto muted = engine::buildPlanTrack(heard.project, clips);
            pt.clips = std::move(muted.clips); pt.length = muted.length;
        }
    }

    // Chains are attached rather than built here. Rebuilding one means
    // instantiating plugins, and publishing a plan happens on every edit: an
    // instance must outlive a cut, or every keystroke would reload the rack and
    // lose whatever state the plugins had settled into.
    for (engine::PlanTrack &pt : plan->tracks) {
        const auto it = m_chains.find(session::EffectOwner::track(pt.id));
        if (!m_hearWithoutEffects && it != m_chains.end() && it->second != nullptr && !it->second->empty())
            pt.chain = it->second.get();
    }
    if (const auto master = m_chains.find(kMasterChainKey);
        !m_hearWithoutEffects && master != m_chains.end() && master->second != nullptr && !master->second->empty()) {
        plan->master = master->second.get();
    }

    engine::prepareStreaming(*plan, m_resetStreaming ? nullptr : m_currentPlan.get(), playbackCacheBytes());
    m_resetStreaming = false;
    engine::prefetchStreaming(*plan, m_playhead, false);
    if (m_loopEnabled) engine::prefetchStreaming(*plan, m_loopStart, false, false);

    ++m_planGeneration;
    // The audio thread borrows a raw pointer to this plan. Previous ones are kept alive
    // until it acknowledges a later generation, so a plan can never be freed while
    // still in use.
    m_retiring.push_back(m_currentPlan);
    m_currentPlan = std::move(plan);

    enqueueAudioCommand(engine::Command::setPlan(m_currentPlan.get(), m_planGeneration, curvesOnly));

    collectRetiredPlans();
    if (audibleChange) invalidateAnalysis();
    cancelClipEffectRenders(false);
    if (!heard.missing.empty()) requestClipEffectRenders(heard.missing, m_project);
    if (!curvesOnly && audibleChange) scheduleDuckingFollow();
}

void Backend::collectRetiredPlans()
{
    // An open effect window lets go of a processor before it can be freed.
    pruneEditors();
    if (!m_ready) flushStoppedCommands();
    // Anything the audio thread has moved past is provably unreferenced -- both the
    // plans and, critically, the audio those plans pointed into.
    if (m_meters.acknowledgedPlan() >= m_planGeneration) {
        if (m_retiredProjects.empty()) {
            m_retiring.clear();
        } else {
            // These plans are no longer borrowed by the audio callback. Keep the
            // old projects with them until their I/O caches stop, then release the
            // large unlinked source files on a worker. Plugin cleanup stays here.
            m_retirementPool.start([plans = std::move(m_retiring),
                                    projects = std::move(m_retiredProjects)]() mutable {
                plans.clear();
                projects.clear();
            });
            m_retiring.clear();
            m_retiredProjects.clear();
        }
        m_retiredChains.clear();
    }
}

void Backend::publishTrackParams()
{
    const bool anySolo = m_project.anySoloed();
    const bool routed = m_currentPlan && m_currentPlan->routing.active;
    for (const session::Track &t : m_project.tracks()) {
        enqueueAudioCommand(engine::Command::setTrackParams(
            t.id, routed ? t.gain : t.effectiveGain(anySolo), session::panGainsFor(t.pan)));
    }
    invalidateAnalysis();
}

// A history jump or control burst can fill the audio queue before the next
// callback. Retain overflow on the GUI thread and preserve order: resending only
// a final plan loses Stop/Seek and plugin values, and can apply controls to the
// wrong chain across a rebuild. Plan retirement still waits for acknowledgement
// of the newest generation, including plans waiting in this overflow.
void Backend::enqueueAudioCommand(const engine::Command &command)
{
    if (deliveryBoundaryActive()) { m_pendingCommands.push_back(command); return; }
    if (m_pendingCommands.empty() && m_commands.push(command)) return;
    m_pendingCommands.push_back(command);
    flushPendingCommands();
}

void Backend::flushPendingCommands()
{
    if (deliveryBoundaryActive()) return;
    while (!m_pendingCommands.empty() && m_commands.push(m_pendingCommands.front()))
        m_pendingCommands.pop_front();
    if (m_pendingCommands.empty()) {
        if (m_commandRetryTimer) m_commandRetryTimer->stop();
        return;
    }
    if (m_shuttingDown) return;
    if (!m_commandRetryTimer) {
        m_commandRetryTimer = new QTimer(this);
        m_commandRetryTimer->setSingleShot(true);
        m_commandRetryTimer->setInterval(30);
        connect(m_commandRetryTimer, &QTimer::timeout, this, &Backend::flushPendingCommands);
    }
    if (!m_commandRetryTimer->isActive()) m_commandRetryTimer->start();
}

void Backend::flushStoppedCommands()
{
    // A stopped output makes pending delivery ownership ready. Finish its
    // accepted boundaries before any later commands or rate reconfiguration.
    while (!m_pumpingDeliverySnapshots && !m_deliverySnapshots.empty()
           && m_deliveryAccess && m_deliveryAccess->ready()) pumpDeliverySnapshots();
    // No audio consumer is running. Drain every batch before retirement or a
    // rate change; an older deferred Play must not arrive after configureStopped.
    do {
        m_transport.flushStopped();
        flushPendingCommands();
    } while (!m_commands.empty());
}

void Backend::consumePluginNotifications()
{
    if (m_consumingPluginNotifications) return;
    m_consumingPluginNotifications = true;
    const auto finished = qScopeGuard([this] {
        m_consumingPluginNotifications = false;
        if (std::exchange(m_automationPlanPending, false))
            publishAutomationPlan(std::exchange(m_automationPlanAudibleChange, false));
    });
    // Only current chains belong to this session. Retired/offline chains never
    // publish dirty flags into a new session or a replacement processor.
    consumeManualAutomation();
    bool dirty = false, textChanged = false;
    std::uint32_t diagnostics = 0;
    for (auto& [key, chain] : m_chains) {
        if (!chain) continue;
        const auto completed = chain->processedTime();
        struct Context { Backend* backend; session::EffectOwner key; } context{this, key};
        const auto changes = chain->consumeNotifications(
            [](void* opaque, std::size_t slot, std::size_t actual, const host::ParameterEvent& event) noexcept {
                auto& owner = *static_cast<Context*>(opaque);
                owner.backend->noteNativeParameter(owner.key, slot, actual, event);
            }, &context, [](void* opaque, std::size_t slot, std::size_t) noexcept {
                auto& owner = *static_cast<Context*>(opaque);
                owner.backend->noteNativeState(owner.key, slot);
            }, [](void* opaque, std::size_t slot, std::size_t) noexcept {
                auto& owner = *static_cast<Context*>(opaque);
                owner.backend->noteNativeOverflow(owner.key, slot);
            }, [](void* opaque, std::size_t slot, std::size_t) noexcept {
                auto& owner = *static_cast<Context*>(opaque);
                owner.backend->noteNativeReadout(owner.key, slot);
            }, true, m_finishingAutomation);
        if (processAutomationChain() == chain.get() && !m_automationSuspended
            && !chain->processAutomationSpans(m_automationKey.section(':', 1, 1).toUInt())
            && (m_meters.playing() || m_automationPass)
            && (m_automationMode == "write" || m_automationTouch || m_automationLatched))
            sampleProcessAutomation(completed);
        dirty = dirty || changes.dirty;
        textChanged = textChanged || changes.textChanged;
        diagnostics |= changes.newDiagnostics;
    }
    // Native parameter edits either enter the armed automation curve or retain
    // their settings-history boundary. Independent/hidden state still requires
    // coherent capture; no representative model knob is echoed over mono state.
    if (dirty) markDirty(true);
    if (textChanged) { ++m_pluginTextRevision; emit pluginTextChanged(); }
    if (diagnostics & host::OutputOverflow)
        setError(tr("A plug-in's change queue overflowed. Its current state can still be saved, but some change notifications were lost."));
    else if (diagnostics)
        setError(tr("A plug-in reported invalid change notifications or failed its main-thread callback."));
}

void Backend::pollPluginNotifications()
{
    if (m_shuttingDown || m_loading || m_pollingPluginNotifications) return;
    if (deliveryBoundaryActive()) { consumePluginNotifications(); return; }
    m_pollingPluginNotifications = true;
    const auto finished = qScopeGuard([this] { m_pollingPluginNotifications = false; });
    bool flush = false;
    for (auto& [key, chain] : m_chains) {
        (void)key;
        if (!chain) continue;
        chain->serviceMainThread();
        flush = flush || chain->parameterFlushRequested();
    }
    QString error;
    if (flush && !recording() && !m_snapshotAccess) {
        // One nonblocking attempt per tick. A callback already in progress keeps
        // its ownership; cancellation immediately releases this request. Playing
        // effects can deliver via process(), while stopped/bypassed effects get
        // their requested flush when a between-block handoff succeeds.
        auto access = m_transport.requestControl();
        if (access.ready()) {
            do { (void)access.drainCommands(); flushPendingCommands(); } while (!m_commands.empty());
            for (auto& [key, chain] : m_chains) {
                (void)key;
                if (!chain) continue;
                std::string reason;
                if (!chain->flushParameterChanges(reason) && error.isEmpty())
                    error = tr("Could not service plug-in parameter changes: %1").arg(QString::fromStdString(reason));
            }
        }
    }
    consumePluginNotifications();
    if (!error.isEmpty()) setError(error);
    pumpNativeHistory();
}

void Backend::honourPluginRestarts()
{
    if (m_shuttingDown || m_loading || m_recording) return;
    std::vector<session::EffectOwner> keys, refresh;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const auto &entry : m_chains) {
        if (entry.second == nullptr || !entry.second->restartRequested()) continue;
        // A plugin that asks again on every activation is not obeyed in a loop.
        auto &last = m_chainRestartedAt[entry.first];
        if (last != 0 && now - last < 5000) continue;
        if (entry.second->canRefreshConfiguration()) refresh.push_back(entry.first);
        else { last = now; keys.push_back(entry.first); }
    }
    bool refreshed = false;
    if (!refresh.empty() && !deliveryBoundaryActive() && !m_snapshotAccess) {
        // Never wait for an in-flight callback. A failed ownership attempt is
        // retried next tick, without consuming the plug-in's restart throttle.
        auto access = m_transport.requestControl();
        if (access.ready()) {
            do { (void)access.drainCommands(); flushPendingCommands(); } while (!m_commands.empty());
            for (const auto key : refresh) {
                const auto chain = m_chains.find(key);
                if (chain == m_chains.end() || !chain->second || !chain->second->restartRequested()) continue;
                m_chainRestartedAt[key] = now;
                std::string error;
                if (!chain->second->refreshConfiguration(error))
                    setError(tr("Could not refresh the live effect configuration: %1").arg(QString::fromStdString(error)));
                refreshed = true; // A partial refresh can change latency too.
            }
        }
    }
    if (refreshed) {
        publishPlan(); rebuildTracks(); rebuildMaster(); emit masterChanged();
        if (m_pluginPortsSlot >= 0 && std::find(refresh.begin(), refresh.end(), chainTarget(m_pluginPortsTarget).key) != refresh.end())
            requestPluginPorts(m_pluginPortsSlot, m_pluginPortsTarget);
        consumePluginNotifications();
    }
    if (keys.empty()) return;
    const auto restart = [this, keys] {
        bool replaced = false;
        for (const session::EffectOwner key : keys) {
            const auto running = m_chains.find(key);
            if (running == m_chains.end() || !running->second || !running->second->restartRequested()) continue;
            auto* inserts = session::findLegacyEffectRack(m_project, key);
            if (!inserts) continue;
            replaced = rebuildChainFor(key, inserts, true) || replaced;
        }
        if (replaced) { publishPlan(); rebuildTracks(); rebuildMaster(); emit masterChanged(); }
        return true;
    };
    if (deferPluginEdit(restart)) return;
    (void)restart();
}

bool Backend::finishedJobsWaiting()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return !m_finishedJobs.empty();
}

// --- what QML reads ------------------------------------------------------------------

void Backend::rebuildTracks(bool notifyUnchanged)
{
    const bool anySolo = m_project.anySoloed();
    QVariantList list;

    for (const session::Track &t : m_project.tracks()) {
        QVariantMap entry;
        entry["id"] = static_cast<int>(t.id);
        entry["name"] = QString::fromStdString(t.name);
        entry["role"] = QString::fromUtf8(session::roleName(t.role).data(),
                                          static_cast<int>(session::roleName(t.role).size()));
        entry["color"] = QString::fromStdString(t.colour);
        entry["height"] = QString::fromUtf8(session::heightName(t.height).data(),
                                            static_cast<int>(session::heightName(t.height).size()));
        entry["gainDb"] = dbFromLinear(t.gain);
        entry["pan"] = static_cast<double>(t.pan);
        int automated = t.volumeAutomation.active() + t.panAutomation.active();
        for (const auto& insert : t.inserts) for (const auto& c : insert.controls) automated += c.automationActive();
        entry["automationCount"] = automated;
        entry["muted"] = t.muted;
        entry["soloed"] = t.soloed;
        entry["silenced"] = m_currentPlan && m_currentPlan->routing.active ? t.muted : t.effectiveGain(anySolo) == 0.0f;
        auto routing = routingTarget(static_cast<int>(t.id)); routing.remove("stamp"); entry["routing"] = routing;
        entry["frames"] = static_cast<qint64>(t.length());
        entry["clipCount"] = static_cast<int>(t.clips.size());

        // The take's own channel count -- the widest source on the track. Peaks are
        // built at the output's two channels whatever the source is, so this is
        // what says whether the Waveform page draws one lane or two.
        entry["channels"] = trackChannels(t);

        // The clips themselves, for the lane to label and outline. Descriptors
        // only: a heavily edited take has thousands, and each is a few numbers.
        QVariantList clips;
        for (const session::Clip &c : t.clips) {
            const session::AudioSource *source = m_project.source(c.source);
            QVariantMap clip;
            clip["start"] = static_cast<qint64>(c.timelineStart);
            clip["length"] = static_cast<qint64>(c.length);
            clip["sourceOffset"] = static_cast<qint64>(c.sourceOffset);
            clip["elastic"] = c.elastic.has_value();
            clip["semitones"] = c.elastic ? c.elastic->semitones : 0.0f;
            clip["durationRatio"] = c.elastic ? double(c.elastic->outputFrames)/c.elastic->inputFrames : 1.0;
            clip["end"] = static_cast<qint64>(c.timelineEnd());
            clip["name"] = source != nullptr ? QString::fromStdString(source->name)
                                             : QString();
            const auto* group = m_project.clipGroup(c.group);
            clip["group"] = group ? int(group->id) : -1;
            clip["groupName"] = group ? QString::fromStdString(group->name) : QString();
            clip["groupColour"] = group ? QString::fromStdString(group->colour) : QString();
            clip["gainDb"] = dbFromLinear(c.gain);
            clip["fadeIn"] = static_cast<qint64>(c.fadeIn);
            clip["fadeOut"] = static_cast<qint64>(c.fadeOut);
            clip["fadeInOffset"] = static_cast<qint64>(c.fadeInOffset);
            clip["fadeOutOffset"] = static_cast<qint64>(c.fadeOutOffset);
            clip["fadeInShape"] = int(c.fadeInShape); clip["fadeOutShape"] = int(c.fadeOutShape);
            clip["fadeInCurve"] = double(c.fadeInCurve); clip["fadeOutCurve"] = double(c.fadeOutCurve);
            clip["volumeEnvelopePoints"] = int(c.volumeEnvelope.points.size());
            clip["panEnvelopePoints"] = int(c.panEnvelope.points.size());
            clip["volumeEnvelopeEnabled"] = c.volumeEnvelope.active();
            clip["panEnvelopeEnabled"] = c.panEnvelope.active() && source && source->audio.channels() >= 2;
            clip["index"] = static_cast<int>(&c - t.clips.data());
            if (source && source->crossfade) {
                auto target = crossfadeTarget(static_cast<int>(t.id), static_cast<int>(&c - t.clips.data()));
                target.remove("stamp");
                clip["crossfade"] = target;
            }
            clips.append(clip);
        }
        entry["clips"] = clips;

        // The chain, named but not described: port descriptors are asked for on
        // demand, because tracks rebuilds on every edit and one LSP plugin has
        // 1030 ports.
        QVariantList inserts;
        for (std::size_t j = 0; j < t.inserts.size(); ++j) {
            const session::Insert &insert = t.inserts[j];
            const lv2::PluginInfo *info = m_lv2->find(insert.uri);
            QVariantMap slot;
            slot["slot"] = static_cast<int>(j);
            slot["uri"] = QString::fromStdString(insert.uri);
            slot["name"] = QString::fromStdString(info != nullptr ? info->name
                                                                  : insert.uri);
            slot["bypass"] = insert.bypass;
            // False for a plugin this box does not have. The slot is kept and
            // reported either way, so the rack can grey it rather than lose it.
            slot["available"] = info != nullptr && info->usable;
            slot["timing"] = automationTiming(info);
            inserts.append(slot);
        }
        entry["inserts"] = inserts;

        const auto chainIt = m_chains.find(session::EffectOwner::track(t.id));
        entry["latency"] = static_cast<qint64>(
            chainIt != m_chains.end() && chainIt->second != nullptr
                ? chainIt->second->latencyFrames()
                : 0);

        QVariantList levels;
        const auto it = m_peaks.find(static_cast<std::uint32_t>(t.id));
        if (it != m_peaks.end() && it->second.ok) {
            const PeakSet &set = it->second;
            for (std::size_t level = 0; level < set.textures.size(); ++level) {
                const io::PeakTexture &tex = set.textures[level];
                if (!tex.ok)
                    continue;
                QVariantMap l;
                // CPU rendering and cleanup use the filename; Qt Quick images
                // use a URL that preserves filename punctuation literally.
                l["texture"] = QString::fromStdString(tex.path);
                l["asset"] = set.cache ? set.cache->token() : QString{};
                l["textureUrl"] = set.cache ? set.cache->textureUrl(QString::fromStdString(tex.path)) : QUrl{};
                l["peakCount"] = static_cast<int>(tex.peakCount);
                l["samplesPerPeak"] =
                    static_cast<int>(set.pyramid.levels[level].samplesPerPeak);
                l["textureWidth"] = static_cast<int>(tex.width);
                l["textureHeight"] = static_cast<int>(tex.height);
                l["channels"] = static_cast<int>(tex.channels);
                levels.append(l);
            }
        }
        entry["levels"] = levels;
        list.append(entry);
    }

    if (notifyUnchanged || m_tracks != list) {
        m_tracks = std::move(list);
        emit tracksChanged();
    }
    rebuildMaster();
}

// A fader changes three display fields, not the arrangement. Reusing the
// existing descriptors avoids rebuilding every clip, texture and effect on
// each pointer movement in a long multitrack session.
void Backend::refreshTrackMixControls(int track)
{
    const auto *t = m_project.track(static_cast<session::TrackId>(track));
    const int index = trackIndexById(track);
    if (!t || index < 0) {
        rebuildTracks();
        return;
    }
    QVariantMap entry = m_tracks.at(index).toMap();
    entry["gainDb"] = dbFromLinear(t->gain);
    entry["pan"] = static_cast<double>(t->pan);
    entry["silenced"] = m_currentPlan && m_currentPlan->routing.active
        ? t->muted : t->effectiveGain(m_project.anySoloed()) == 0.0f;
    m_tracks[index] = entry;
    emit tracksChanged();
}

void Backend::setFocusedTrack(int id)
{
    const auto wanted = static_cast<session::TrackId>(id);
    if (m_project.track(wanted) == nullptr) return;
    const bool grouped = !m_selectedTracks.empty() || !m_selectedClips.empty();
    m_selectedClips.clear();
    m_selectedTracks.clear(); m_trackSelectionAnchor = wanted; ++m_trackSelectionRevision;
    if (m_focused == wanted) { if (grouped) clampSelection(); return; }
    m_focused = wanted;
    m_selection.channel = -1; emit selectionChanged();
    emit tracksChanged();
    markSpectralStale();
}

// --- transport -----------------------------------------------------------------------

void Backend::play() { startPlayback(1); }
void Backend::startPlayback(int speed) {
    if (recording()) return;
    if (m_automationFinishPending && deferAutomationEdit([this, speed] { startPlayback(speed); return true; })) return;
    m_automationPlayEpoch = m_meters.playbackState().epoch;
    m_automationSuspended = false;
    ++m_automationRunToken; m_automationManualValue = m_automationValue;
    prepareInstanceAutomation();
    if (m_automationMode == "write" && processManualAutomation() && !m_automationPass)
        m_automationPass = AutomationPass{session::captureState(m_project), m_sessionGeneration, -1, m_automationValue};
    if (m_automationMode == "write" && m_project.length() > 0 && !processOwnedAutomation())
        sampleAutomation(m_meters.position() >= m_project.length() ? 0 : m_meters.position());
    else if (processOwnedAutomation()) publishAutomationPlan();
    if (m_currentPlan) engine::prefetchStreaming(*m_currentPlan, m_playhead >= m_project.length() && speed > 0 ? 0 : m_playhead, false);
    // The speed first: Play from the end rewinds only going forwards.
    m_playSpeed = speed;
    enqueueAudioCommand(engine::Command::setSpeed(speed));
    enqueueAudioCommand(engine::Command::play());
}
bool Backend::shuttle(int speed)
{
    speed = std::clamp(speed, -engine::Transport::kMaxSpeed, engine::Transport::kMaxSpeed);
    if (speed == 0) speed = 1;
    if (recording()) { setError(tr("Recording plays at normal speed.")); return false; }
    if (speed != 1 && m_automationMode != QLatin1String("read")) {
        setError(tr("Faster and backwards playback are off while automation is being written. Set automation to Read."));
        return false;
    }
    if (!m_playing) { startPlayback(speed); emit statusChanged(); return true; }
    m_playSpeed = speed;
    enqueueAudioCommand(engine::Command::setSpeed(speed));
    emit statusChanged();
    return true;
}
void Backend::stop() { finishAutomationRecording(); stopRecording(); enqueueAudioCommand(engine::Command::stop()); }
void Backend::togglePlay() { (m_playing || recording()) ? stop() : play(); }

void Backend::seekFrames(qint64 frame)
{
    if (recording()) return;
    if (deferAutomationEdit([this, frame] { seekFrames(frame); return true; })) return;
    finishAutomationRecording();
    if (m_currentPlan) engine::prefetchStreaming(*m_currentPlan, std::max<qint64>(0, frame), false);
    enqueueAudioCommand(engine::Command::seek(std::max<qint64>(0, frame)));
}

void Backend::setGainDb(double db)
{
    // dB converts here rather than on the audio thread, so the callback never runs a
    // std::pow.
    m_masterGainDb = std::clamp(db, -80.0, 12.0);
    enqueueAudioCommand(engine::Command::setGain(linearFromDb(m_masterGainDb)));
    emit masterGainChanged();
}

void Backend::setMetronome(bool on)
{
    if (m_metronome == on) return;
    m_metronome = on;
    enqueueAudioCommand(engine::Command::setMetronome(m_metronome, float(linearFromDb(m_metronomeLevelDb))));
    emit metronomeChanged();
}

void Backend::setMetronomeLevelDb(double db)
{
    db = std::clamp(db, -40.0, 0.0);
    if (m_metronomeLevelDb == db) return;
    m_metronomeLevelDb = db;
    enqueueAudioCommand(engine::Command::setMetronome(m_metronome, float(linearFromDb(m_metronomeLevelDb))));
    emit metronomeChanged();
}

void Backend::setTrackGainDb(int track, double db)
{
    session::Track *t = m_project.track(static_cast<session::TrackId>(track));
    if (t == nullptr)
        return;
    const float gain = linearFromDb(db);
    if (writeMatchingAutomation(track, "volume", gain / std::max(1e-12f, t->gain))) return;
    if (deferAutomationEdit([this, track, db] { setTrackGainDb(track, db); return true; })) return;
    finishAutomationRecording();
    if (t->gain == gain) return;
    t->gain = gain;
    publishTrackParams();
    refreshTrackMixControls(track);
    markDirty();                // not an undo step, but a change to save
}

void Backend::setTrackPan(int track, double pan)
{
    session::Track *t = m_project.track(static_cast<session::TrackId>(track));
    if (t == nullptr)
        return;
    if (writeMatchingAutomation(track, "pan", pan)) return;
    if (deferAutomationEdit([this, track, pan] { setTrackPan(track, pan); return true; })) return;
    finishAutomationRecording();
    const float clamped = std::clamp(static_cast<float>(pan), -1.0f, 1.0f);
    if (t->pan == clamped) return;
    t->pan = clamped;
    publishTrackParams();
    refreshTrackMixControls(track);
    markDirty();
}

void Backend::setTrackMute(int track, bool on)
{
    if (deferAutomationEdit([this, track, on] { setTrackMute(track, on); return true; })) return;
    finishAutomationRecording();
    session::Track *t = m_project.track(static_cast<session::TrackId>(track));
    if (t == nullptr)
        return;
    t->muted = on;
    if (m_currentPlan && m_currentPlan->routing.active) publishPlan(true);
    publishTrackParams();
    rebuildTracks();
    markDirty();
}

void Backend::setTrackSolo(int track, bool on)
{
    if (deferAutomationEdit([this, track, on] { setTrackSolo(track, on); return true; })) return;
    finishAutomationRecording();
    session::Track *t = m_project.track(static_cast<session::TrackId>(track));
    if (t == nullptr)
        return;
    t->soloed = on;
    if (m_currentPlan && m_currentPlan->routing.active) publishPlan(true);
    publishTrackParams();
    rebuildTracks();
    markDirty();
}

// --- workers -------------------------------------------------------------------------

void Backend::joinWorker()
{
    if (m_worker.joinable())
        m_worker.join();
    // Offline instances are created/activated here and destroyed here. The
    // worker may process them, but CLAP destruction belongs to the main thread.
    m_offlineChains.clear();
    m_offlineFeatures.reset();
}

void Backend::markPeaksDirty(session::TrackId track)
{
    if (track == session::kInvalidTrack)
        return;
    if (std::find(m_dirtyPeaks.begin(), m_dirtyPeaks.end(), track) == m_dirtyPeaks.end())
        m_dirtyPeaks.push_back(track);
}

void Backend::pumpPeaks()
{
    if (m_shuttingDown || m_saving || m_loading || m_pitchRequest || m_dirtyPeaks.empty()
        || m_busy.load(std::memory_order_acquire))
        return;

    const session::TrackId track = m_dirtyPeaks.front();
    m_dirtyPeaks.pop_front();

    const session::Track *source = m_project.track(track);
    if (source == nullptr) {
        pumpPeaks();            // removed under us; try the next one
        return;
    }

    joinWorker();
    m_cancelPeaks.store(false, std::memory_order_relaxed);
    m_busy.store(true, std::memory_order_release);

    const std::vector<session::Clip> snapshot = source->clips;
    const std::uint32_t sequence = ++m_peakSequence;
    const std::uint64_t generation = m_sessionGeneration;

    m_worker = std::thread([this, snapshot, sequence, track, generation] {
        // Reads m_project for its audio sources only. Sources are never added or
        // removed while a worker runs -- installTake joins first -- and clip lists are
        // copied into `snapshot` rather than read from here.
        PeakSet result = buildPeaks(snapshot, sequence, track);
        result.sessionGeneration = generation;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_pendingPeaks.push_back(std::move(result));
            m_finishedJobs.push_back(Job::Peaks);
        }
        m_busy.store(false, std::memory_order_release);
        emit workerFinished();
    });
}

Backend::PeakSet Backend::buildPeaks(const std::vector<session::Clip> &clips,
                                     std::uint32_t sequence,
                                     session::TrackId track) const
{
    PeakSet out;
    out.track = track;

    io::PeakLevel base;
    try { base = io::buildTimelinePeaks(m_project, clips, kSamplesPerPeak, kOutputChannels, &m_cancelPeaks); }
    catch (const std::exception& e) { out.error = QString::fromUtf8(e.what()); return out; }
    if (base.empty()) {
        // An empty track is a valid state, not a failure: its lane draws nothing.
        out.ok = true;
        return out;
    }

    if (!m_displayCache.isValid()) {
        out.error = tr("Could not create a private graphics cache: %1").arg(m_displayCache.errorString());
        return out;
    }
    if (m_cancelPeaks.load(std::memory_order_relaxed)) return out;
    out.pyramid = io::pyramidFromLevel(std::move(base));
    if (m_cancelPeaks.load(std::memory_order_relaxed)) return out;

    out.ok = writePeakTextures(out, QStringLiteral("peaks-%1-%2")
        .arg(static_cast<std::uint32_t>(track)).arg(sequence));
    return out;
}

bool Backend::writePeakTextures(PeakSet &set, const QString &stem) const
{
    QStringList paths;
    // Only unpublished files are removed directly. Once adopted, the final
    // producer/reader lease owns cleanup, including discarded worker results.
    const auto cleanup = qScopeGuard([&] {
        if (!set.cache) for (const auto &path : paths) QFile::remove(path);
    });
    try {
        set.textures.reserve(set.pyramid.size());
        for (std::size_t level = 0; level < set.pyramid.size(); ++level) {
            const auto path = QDir(m_displayCache.path()).filePath(stem + QStringLiteral("-%1.png").arg(level));
            paths.append(path);
            auto texture = io::writePeakTexture(set.pyramid.levels[level], path.toStdString());
            if (!texture.ok) { set.error = QString::fromStdString(texture.error); return false; }
            set.textures.push_back(std::move(texture));
        }
        set.cache = m_displayCache.adopt(paths, set.error);
        return bool(set.cache);
    } catch (const std::exception &e) {
        set.error = QString::fromUtf8(e.what());
        return false;
    }
}

void Backend::removeTextures(PeakSet &set)
{
    set.cache.reset();
}

void Backend::installPeaks(PeakSet set)
{
    if (set.sessionGeneration != m_sessionGeneration) {
        removeTextures(set);
        return;
    }
    if (!set.ok) { if (!set.error.isEmpty()) setError(set.error); return; }
    const auto key = static_cast<std::uint32_t>(set.track);
    if (const auto old = m_peaks.find(key); old != m_peaks.end())
        removeTextures(old->second);
    m_peaks[key] = std::move(set);
    rebuildTracks();
}

void Backend::drainWorker()
{
    // Release completed offline plugins on their creation thread promptly, even
    // when no next background job is queued. A later job already joined the old
    // worker before starting, so never wait here while that next job is busy.
    if (!m_offlineChains.empty() && !m_busy.load(std::memory_order_acquire))
        joinWorker();
    scheduleMemoryRelease();
    // One queued signal per finished job, but a drain may find more than one
    // waiting (see m_finishedJobs), so it takes them all, in order.
    for (;;) {
    Job job = Job::None;
    LoadedTake take;
    PeakSet peaks;
    Analysis analysis;
    RenderOutcome render;
    SpectralResult spectralResult;
    RepairResult repair;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_finishedJobs.empty())
            return;
        job = m_finishedJobs.front();
        m_finishedJobs.pop_front();
        const auto take_ = [](auto &queue, auto &into) {
            if (queue.empty())
                return;
            into = std::move(queue.front());
            queue.pop_front();
        };
        if (job == Job::Spectral)
            take_(m_pendingSpectral, spectralResult);
        else if (job == Job::Repair)
            take_(m_pendingRepair, repair);
        else if (job == Job::Load)
            take_(m_pendingLoad, take);
        else if (job == Job::Peaks)
            take_(m_pendingPeaks, peaks);
        else if (job == Job::Analyse)
            take_(m_pendingAnalysis, analysis);
        else if (job == Job::Render)
            take_(m_pendingRender, render);
    }

    switch (job) {
    case Job::Load:
        finishAudioLoad(std::move(take));
        break;

    case Job::Peaks:
        installPeaks(std::move(peaks));
        pumpPeaks();            // whatever else went dirty while that one ran
        break;

    case Job::Save: {
        SaveJob job;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_pendingSave.empty()) {
                job = std::move(m_pendingSave.front());
                m_pendingSave.pop_front();
            }
        }
        m_saveActive = false;
        m_saving = !m_saveRequests.empty();
        if (job.ok) rememberDocument(QString::fromStdString(job.path), true);
        if (job.sessionGeneration != m_sessionGeneration) {
            // The write belongs to the old session. Its file is valid, but its
            // path and clean flag must never become those of the new session.
            emit timelineChanged();
            break;
        }
        if (!job.ok) {
            setError(QString::fromStdString(job.error));
        } else {
            // Native output may have arrived since capture without a status tick.
            // Observe it before deciding whether this older write makes us clean.
            consumePluginNotifications();
            // The recovery layer was for exactly this: under the old path, or in
            // the scratch bundle. Gone before the path moves.
            dropRecovery();
            retireRecordings(job.sessionGeneration, job.revision);
            m_projectPath = QString::fromStdString(job.path);
            m_savedRevision = job.revision;
            m_dirty = m_confirmedEditRevision > job.revision;
            if (m_dirty || hasUnverifiedPluginState())
                armAutosave();
            m_takeName = projectName();
            emit takeChanged();
        }
        emit timelineChanged();
        break;
    }

    case Job::Scan: {
        PluginCatalogueResult catalogue;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_pendingCatalogues.empty()) {
                catalogue = std::move(m_pendingCatalogues.front());
                m_pendingCatalogues.pop_front();
            }
        }
        m_scanning = false;
        if (catalogue.cancelled || m_cancelPluginScan.load(std::memory_order_relaxed) || m_shuttingDown) {
            if (!m_shuttingDown) setError(tr("Plug-in discovery cancelled; the previous catalogue was kept."));
            emit pluginsChanged();
            break;
        }
        acceptPluginCatalogue(std::move(catalogue));
        publishPluginCatalogue();
        break;
    }

    case Job::Analyse:
        installAnalysis(std::move(analysis));
        pumpPeaks();
        break;

    case Job::Render:
        finishRender(std::move(render));
        pumpPeaks();
        break;

    case Job::Spectral:
        installSpectral(std::move(spectralResult));
        pumpPeaks();
        break;

    case Job::Repair:
        finishRepair(std::move(repair));
        pumpPeaks();
        break;

    case Job::Detect: {
        DetectResult job;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_pendingDetect.empty()) {
                job = std::move(m_pendingDetect.front());
                m_pendingDetect.pop_front();
            }
        }
        finishDetect(std::move(job));
        pumpPeaks();
        break;
    }

    case Job::Autosave: {
        SaveJob job;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_pendingAutosave.empty()) {
                job = std::move(m_pendingAutosave.front());
                m_pendingAutosave.pop_front();
            }
        }
        finishAutosave(std::move(job));
        pumpPeaks();
        break;
    }

    case Job::LoadProject: {
        ProjectLoadResult result;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_pendingProjectLoad.empty()) {
                result = std::move(m_pendingProjectLoad.front());
                m_pendingProjectLoad.pop_front();
            }
        }
        finishProjectLoad(std::move(result));
        break;
    }
    case Job::NoisePrint: {
        NoisePrintJob result;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_pendingNoisePrint.empty()) {
                result = std::move(m_pendingNoisePrint.front()); m_pendingNoisePrint.pop_front();
            }
        }
        finishNoisePrint(std::move(result)); break;
    }
    case Job::Storage: {
        StorageJob result;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_pendingStorage.empty()) {
                result = std::move(m_pendingStorage.front()); m_pendingStorage.pop_front();
            }
        }
        finishStorage(std::move(result)); break;
    }
    case Job::SampleEdit: {
        SampleEditJob result;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_pendingSampleEdit.empty()) {
                result = std::move(m_pendingSampleEdit.front());
                m_pendingSampleEdit.pop_front();
            }
        }
        finishSampleEdit(std::move(result));
        break;
    }
    case Job::Pitch: {
        PitchResult result;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_pendingPitch.empty()) {
                result = std::move(m_pendingPitch.front());
                m_pendingPitch.pop_front();
            }
        }
        finishPitch(std::move(result));
        break;
    }
    case Job::TrackLoudness: {
        TrackLoudnessJob result;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_pendingTrackLoudness.empty()) {
                result = std::move(m_pendingTrackLoudness.front());
                m_pendingTrackLoudness.pop_front();
            }
        }
        finishTrackLoudness(std::move(result));
        break;
    }
    case Job::Ducking: {
        DuckingJob result;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_pendingDucking.empty()) {
                result = std::move(m_pendingDucking.front());
                m_pendingDucking.pop_front();
            }
        }
        finishDucking(std::move(result));
        break;
    }
    case Job::None:
        break;
    }

    pumpWorkerJobs();
    }
}

void Backend::pumpWorkerJobs()
{
    // A worker or an asynchronous load verification has released the next job.
    pumpSave();
    pumpProjectLoad();
    pumpSampleEdit();
    pumpStorage();
    pumpNoisePrint();
    pumpPitch();
    pumpPeaks();
    pumpRenders();
    pumpRepair();
    pumpDetect();
    if (m_spectralWanted && m_spectralStale && !m_spectralBuilding)
        m_spectralTimer->start();
}

void Backend::pollStatus()
{
    pollPluginNotifications();
    pollAutomation();
    pollRecording();
    collectRetiredPlans();

    // Each field is published independently and deliberately lossily, so these can come
    // from adjacent callbacks. That is fine for a playhead and a meter, and buying
    // consistency would mean a lock on the audio thread.
    const auto position = static_cast<qint64>(m_meters.position());
    const bool playing = m_meters.playing();
    const auto left = static_cast<double>(m_meters.peakLeft());
    const auto right = static_cast<double>(m_meters.peakRight());

    // The per-track slots, in the order the running plan was published. The plan
    // the audio thread holds may be one behind for a tick; a meter drawn on the
    // wrong lane for 33 ms is not worth a lock.
    const std::size_t count = m_currentPlan ? m_currentPlan->tracks.size() : 0;
    std::vector<float> meters;
    meters.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        meters.push_back(m_meters.trackPeakLeft(i));
        meters.push_back(m_meters.trackPeakRight(i));
    }
    const bool metersMoved = meters != m_lastMeters;

    pollLoudness();
    if (m_generatingDucking) {
        const auto progress = static_cast<double>(m_jobProgress.load(std::memory_order_relaxed));
        if (progress != m_duckingProgress) {
            m_duckingProgress = progress;
            emit duckingChanged();
        }
    }
    if (m_measuringTrackLoudness) {
        const auto progress = static_cast<double>(m_jobProgress.load(std::memory_order_relaxed));
        if (progress != m_trackLoudnessProgress) {
            m_trackLoudnessProgress = progress;
            emit trackLoudnessChanged();
        }
    }
    if (m_spectralBuilding) {
        const auto progress = static_cast<double>(m_jobProgress.load(std::memory_order_relaxed));
        if (progress != m_spectralProgress) {
            m_spectralProgress = progress;
            emit spectralProgressChanged();
        }
    }
    if (m_activeRender >= 0) {
        const auto progress = static_cast<double>(m_jobProgress.load(std::memory_order_relaxed));
        if (progress != m_renderProgress) {
            m_renderProgress = progress;
            if (RenderJob *job = renderJob(m_activeRender))
                job->progress = progress;
            emit renderProgressChanged();
        }
    }

    honourPluginRestarts();
    const auto streamMisses = streamCacheMisses(), streamErrors = streamReadErrors();
    const bool preparing = elasticPreparing(), failed = elasticFailed();
    const bool elasticChanged = preparing != m_lastElasticPreparing || failed != m_lastElasticFailed;
    m_lastElasticPreparing = preparing; m_lastElasticFailed = failed;
    const bool streamChanged = elasticChanged || streamMisses != m_lastStreamMisses || streamErrors != m_lastStreamReadErrors;
    m_lastStreamMisses = streamMisses; m_lastStreamReadErrors = streamErrors;
    if (position == m_playhead && playing == m_playing
        && left == m_peakLeft && right == m_peakRight && !metersMoved && !streamChanged) {
        return;
    }

    if (metersMoved) {
        m_lastMeters = std::move(meters);
        QVariantList list;
        m_meterById.clear();
        for (std::size_t i = 0; i < count; ++i) {
            QVariantMap entry;
            const int id = static_cast<int>(m_currentPlan->tracks[i].id);
            const double left = m_lastMeters[i * 2], right = m_lastMeters[i * 2 + 1];
            entry["id"] = id;
            entry["left"] = left;
            entry["right"] = right;
            list.append(entry);
            m_meterById[id] = {left, right};
        }
        m_trackMeters = list;
        ++m_meterRevision;
    }

    m_playhead = position;
    if (m_playing && !playing) m_playSpeed = 1;
    m_playing = playing;
    m_peakLeft = left;
    m_peakRight = right;
    emit statusChanged();
}

QVariantMap Backend::trackMeter(int id) const
{
    if (const auto found = m_meterById.find(id); found != m_meterById.end())
        return {{"id", id}, {"left", found->second.first}, {"right", found->second.second}};
    QVariantMap silent;
    silent["id"] = id;
    silent["left"] = 0.0;
    silent["right"] = 0.0;
    return silent;
}

// --- selection -------------------------------------------------------------------

session::FramePos Backend::trackLength(session::TrackId id) const noexcept
{
    const session::Track *track = m_project.track(id);
    return track == nullptr ? 0 : track->length();
}

void Backend::clampSelection()
{
    if (!selectedClipsCurrent()) { m_selectedClips.clear(); m_selectedTracks.clear(); ++m_trackSelectionRevision; }
    std::erase_if(m_selectedTracks, [this](auto id) { return !m_project.track(id); });
    if (!m_selectedTracks.empty() && std::find(m_selectedTracks.begin(), m_selectedTracks.end(), m_focused) == m_selectedTracks.end()) {
        m_selectedClips.clear(); m_selectedTracks.clear(); m_trackSelectionAnchor = m_focused;
        m_selection.track = m_focused; m_selection.channel = -1; ++m_trackSelectionRevision;
    }
    if (m_project.track(m_selection.track) == nullptr)
        m_selection.track = m_focused;
    const int channel = m_selection.channel;
    m_selection = session::Selection::normalised(m_selection.start, m_selection.end,
                                                 selectedTrackIds().size() > 1 ? selectedTrackLength() : trackLength(m_selection.track),
                                                 m_selection.track);
    const auto* selected = m_project.track(m_selection.track);
    m_selection.channel = selectedTrackIds().size() == 1 && selected && channel < trackChannels(*selected) ? channel : -1;
    emit selectionChanged();
}


session::FramePos Backend::snapToZeroCrossing(session::FramePos frame) const
{
    const session::Track *track = m_project.track(m_focused);
    if (track == nullptr)
        return frame;

    for (const session::Clip &c : track->clips) {
        if (!c.covers(frame))
            continue;
        const session::AudioSource *source = m_project.source(c.source);
        if (source == nullptr)
            break;

        if (c.elastic) {
            const auto from = std::max(c.timelineStart,frame-4800), to = std::min(c.timelineEnd(),frame+4801);
            try {
                const auto audio = session::renderClipRange(m_project,{c},from,to,source->audio.channels());
                return from+session::nearestZeroCrossing(audio,frame-from,4800,std::uint32_t(std::max(0,m_selection.channel)));
            } catch (const std::exception&) { return frame; }
        }
        const session::FramePos inSource = c.sourceOffset + (frame - c.timelineStart);
        const session::FramePos snapped =
            session::nearestZeroCrossing(source->audio, inSource, 4800, std::uint32_t(std::max(0, m_selection.track == m_focused ? m_selection.channel : -1)));
        const session::FramePos backOnTimeline =
            c.timelineStart + (snapped - c.sourceOffset);

        // Refuse to snap across a clip boundary: the neighbouring audio is unrelated,
        // so a crossing there means nothing.
        if (backOnTimeline >= c.timelineStart && backOnTimeline <= c.timelineEnd())
            return backOnTimeline;
        break;
    }
    return frame;
}

void Backend::select(qint64 start, qint64 end, bool snap, int track)
{
    if (!m_selectedClips.empty()) { m_selectedClips.clear(); ++m_trackSelectionRevision; }
    if (track >= 0) focusSelectedTrack(track);
    session::Selection next = session::Selection::normalised(
        start, end, selectedTrackLength(), m_focused);
    if (m_selection.track == m_focused && selectedTrackIds().size() == 1) next.channel = m_selection.channel;

    if (snap && m_snapping && !next.empty()) {
        // Section 5 wants selections on zero crossings, so the exact-frame case is the
        // one that has to ask for it. Phase 1 adds the clip edges, markers and the
        // playhead in front of that -- see snapFrame.
        next.start = snapFrame(next.start);
        next.end = snapFrame(next.end);
        // A nearby marker or a musical grid point can lie beyond this track.
        // Selection-based edits must keep both edges inside the available audio.
        next.start = std::clamp<qint64>(next.start, 0, selectedTrackLength());
        next.end = std::clamp<qint64>(next.end, 0, selectedTrackLength());
        if (next.start > next.end)
            std::swap(next.start, next.end);
    }

    m_selection = next;
    emit selectionChanged();
}

void Backend::selectAll()
{
    if (!m_selectedClips.empty()) { m_selectedClips.clear(); ++m_trackSelectionRevision; }
    m_selection = session::Selection{
        .start = 0, .end = selectedTrackLength(), .track = m_focused, .channel = m_selection.channel};
    emit selectionChanged();
}

void Backend::clearSelection()
{
    if (!m_selectedClips.empty()) { m_selectedClips.clear(); ++m_trackSelectionRevision; }
    m_selection = session::Selection{.start = 0, .end = 0, .track = m_focused, .channel = m_selection.channel};
    emit selectionChanged();
}

// --- tracks ----------------------------------------------------------------------

session::TrackId Backend::trackArg(int track) const
{
    if (track >= 0)
        return static_cast<session::TrackId>(track);
    return m_focused;
}

session::Track *Backend::focusedTrackPtr() { return m_project.track(m_focused); }

std::vector<session::Clip> *Backend::focusedClips()
{
    session::Track *track = focusedTrackPtr();
    return track == nullptr ? nullptr : &track->clips;
}

void Backend::addTrack()
{
    if (deferAutomationEdit([this] { addTrack(); return true; })) return;
    if (m_project.tracks().size() >= engine::Transport::kMaxTracks) {
        setError(QStringLiteral("this session already has as many tracks as OmaAudio plays (%1); remove one to add another").arg(engine::Transport::kMaxTracks));
        return;
    }
    const session::SessionState before = captureEditState();
    const session::TrackId id =
        m_project.addTrack("track " + std::to_string(m_project.tracks().size() + 1));
    m_focused = id;
    commit("add track", id, before);
}

void Backend::removeTrack(int track)
{
    if (deferAutomationEdit([this, track] { removeTrack(track); return true; })) return;
    const session::TrackId id = trackArg(track);
    if (m_project.track(id) == nullptr)
        return;
    if (deferPluginEdit([this, id] { removeTrack(static_cast<int>(id)); return true; })) return;
    const session::SessionState before = captureEditState();
    m_project.removeTrack(id);
    rebuildChain(id);
    if (m_project.track(m_focused) == nullptr) {
        m_focused = m_project.tracks().empty() ? session::kInvalidTrack
                                               : m_project.tracks().front().id;
    }
    commit("remove track", m_focused, before);
}

void Backend::removeSelectedTracks()
{
    if (deferAutomationEdit([this] { removeSelectedTracks(); return true; })) return;
    removeTrackIds(selectedTrackIds());
}

void Backend::removeTrackIds(const std::vector<session::TrackId>& wanted)
{
    std::vector<session::TrackId> ids;
    for (const auto id : wanted) if (m_project.track(id)) ids.push_back(id);
    if (ids.empty()) return;
    if (ids.size() == 1) { removeTrack(static_cast<int>(ids.front())); return; }
    if (deferPluginEdit([this, ids] { removeTrackIds(ids); return true; })) return;
    const session::SessionState before = captureEditState();
    // Focus moves to the track that followed the first one removed, else the one before.
    const auto first = m_project.trackIndex(ids.front()).value_or(0);
    for (const auto id : ids) { m_project.removeTrack(id); rebuildChain(id); }
    m_selectedTracks.clear(); m_selectedClips.clear();
    const auto& tracks = m_project.tracks();
    m_focused = tracks.empty() ? session::kInvalidTrack : tracks[std::min<std::size_t>(first, tracks.size() - 1)].id;
    m_trackSelectionAnchor = m_focused;
    commit("remove tracks", m_focused, before);
}

void Backend::renameTrack(int track, const QString &name)
{
    if (deferAutomationEdit([this, track, name] { renameTrack(track, name); return true; })) return;
    session::Track *t = m_project.track(trackArg(track));
    if (t == nullptr)
        return;
    const session::SessionState before = captureEditState();
    t->name = name.toStdString();
    commit("rename track", t->id, before);
}

void Backend::moveTrack(int track, int toIndex)
{
    if (deferAutomationEdit([this, track, toIndex] { moveTrack(track, toIndex); return true; })) return;
    // A destination index, as Project::moveTrack takes. This used to be named
    // `delta` and refused 0 as "no move" -- so a track could be moved down but
    // never back to the top. The project already answers a same-index move with
    // nothing. Found by the shell suite.
    const session::TrackId id = trackArg(track);
    if (m_project.track(id) == nullptr || toIndex < 0)
        return;
    const int current = trackIndexById(track);
    if (current == toIndex)
        return;
    const session::SessionState before = captureEditState();
    if (!m_project.moveTrack(id, static_cast<std::size_t>(toIndex)))
        return;
    commit("move track", id, before);
}

void Backend::focusTrack(int track) { setFocusedTrack(track); }

void Backend::focusNextTrack(int delta)
{
    const auto &tracks = m_project.tracks();
    if (tracks.empty())
        return;
    int index = 0;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (tracks[i].id == m_focused)
            index = static_cast<int>(i);
    }
    index = std::clamp(index + delta, 0, static_cast<int>(tracks.size()) - 1);
    setFocusedTrack(static_cast<int>(tracks[static_cast<std::size_t>(index)].id));
    clampSelection();
}

void Backend::toggleMute(int track)
{
    session::Track *t = m_project.track(trackArg(track));
    if (t != nullptr)
        setTrackMute(static_cast<int>(t->id), !t->muted);
}

void Backend::toggleSolo(int track)
{
    session::Track *t = m_project.track(trackArg(track));
    if (t != nullptr)
        setTrackSolo(static_cast<int>(t->id), !t->soloed);
}

// --- editing ---------------------------------------------------------------------

void Backend::markDirty(bool pluginNotification)
{
    m_confirmedEditRevision = ++m_editRevision;
    if (!pluginNotification) m_hostEditRevision = m_editRevision;
    emit automationStampChanged();
    invalidateAnalysis();
    armAutosave();
    if (m_dirty)
        return;
    m_dirty = true;
    emit timelineChanged();
}

// Waveforms and spectral data depend on immutable clip sources and their
// placement/transforms, not track names, routing, effects or automation. Include
// removed ids too so changing focus away from a deleted track invalidates its view.
std::vector<session::TrackId> Backend::changedWaveforms(const session::SessionState &state) const
{
    std::vector<session::TrackId> changed;
    const bool rateChanged = state.sampleRate && state.sampleRate != m_project.sampleRate();
    for (const auto &track : m_project.tracks()) {
        const auto previous = std::find_if(state.tracks.begin(), state.tracks.end(),
            [&](const auto &entry) { return entry.id == track.id; });
        const auto sameAudio = [](const session::Clip& a, const session::Clip& b) { return a.sameAudio(b); };
        if (rateChanged || previous == state.tracks.end()
            || !std::equal(previous->clips.begin(), previous->clips.end(), track.clips.begin(), track.clips.end(), sameAudio))
            changed.push_back(track.id);
    }
    for (const auto &track : state.tracks) {
        if (!m_project.track(track.id)) changed.push_back(track.id);
    }
    return changed;
}

// The one path every clip-list edit takes, so undo can never be forgotten.
void Backend::commit(std::string label, session::TrackId track,
                     const session::SessionState &before)
{
    if (const auto error = session::validateElastic(m_project); !error.empty()) {
        session::restoreState(m_project,before); setError(QString::fromStdString(error)); return;
    }
    if (const auto error = session::validateClipEnvelopes(m_project); !error.empty()) {
        session::restoreState(m_project,before); setError(QString::fromStdString(error)); return;
    }
    const auto waveforms = changedWaveforms(before);
    session::SessionState after = session::captureState(m_project);
    if (!m_undo.push(std::move(label), track, before, std::move(after))) {
        emit timelineChanged();     // nothing actually changed
        return;
    }
    m_dirty = true;
    armAutosave();
    m_hostEditRevision = m_confirmedEditRevision = ++m_editRevision;
    publishPlan();
    publishTrackParams();
    clampSelection();
    for (const auto id : waveforms) markPeaksDirty(id);
    pumpPeaks();
    rebuildTracks();
    rebuildMarkers();
    if (m_spectralTrack != m_focused
        || std::find(waveforms.begin(), waveforms.end(), m_focused) != waveforms.end())
        markSpectralStale();
    emit timelineChanged();
}

// Undo and redo restore whole snapshots, which can change any number of tracks.
void Backend::installState(const session::SessionState &state, session::TrackId focus)
{
    const auto waveforms = changedWaveforms(state);
    const bool groupsChanged = m_project.clipGroups() != state.clipGroups;
    const auto viewedInsert = [&]() -> const session::Insert * {
        const auto target = chainTarget(m_pluginPortsTarget);
        return target.inserts && m_pluginPortsSlot >= 0
            && static_cast<std::size_t>(m_pluginPortsSlot) < target.inserts->size()
            ? &(*target.inserts)[static_cast<std::size_t>(m_pluginPortsSlot)] : nullptr;
    };
    const auto *previousInsert = viewedInsert();
    const std::string previousUri = previousInsert ? previousInsert->uri : std::string{};
    const auto oldFocus = m_focused;
    const auto oldRate = m_project.sampleRate();
    const auto oldBits = m_project.defaultExportBits();
    const bool restart = prepareProjectRate(state.sampleRate ? state.sampleRate : oldRate);
    session::restoreState(m_project, state);
    rescaleProjectView(oldRate, true);
    // Set focus before notifying QML or clamping the selection to that track.
    if (m_project.track(focus)) m_focused = focus;
    if (m_project.track(m_focused) == nullptr) {
        m_focused = m_project.tracks().empty() ? session::kInvalidTrack
                                               : m_project.tracks().front().id;
    }
    rebuildAllChains();
    publishPlan();
    publishTrackParams();
    enqueueAudioCommand(engine::Command::setMasterTrim(m_project.masterTrim()));
    enqueueAudioCommand(engine::Command::resetLoudness());
    finishProjectRate(restart, oldRate, oldBits);
    clampSelection();
    for (const auto id : waveforms) markPeaksDirty(id);
    pumpPeaks();
    // Undo restores effect values too. Reuse the displayed port metadata for the
    // same plugin, and only notify when a value changed. Re-probing an unchanged
    // plugin would instantiate it and rebuild its controls on every history step.
    if (m_pluginPortsSlot >= 0) {
        const auto *insert = viewedInsert();
        if (!insert || insert->uri != previousUri || oldRate != m_project.sampleRate()) {
            requestPluginPorts(m_pluginPortsSlot, m_pluginPortsTarget);
        } else {
            bool changed = false;
            for (auto &entry : m_pluginPorts) {
                auto port = entry.toMap();
                const auto *stored = insert->control(port.value("index").toUInt());
                const double value = stored ? stored->value : port.value("defaultValue").toDouble();
                if (port.value("value").toDouble() == value) continue;
                port["value"] = value;
                entry = std::move(port);
                changed = true;
            }
            if (changed) emit pluginPortsChanged();
        }
    }
    // A point move can leave every descriptor identical. Avoid re-converting all
    // clips/textures into JavaScript objects when only timeline data changed.
    rebuildTracks(m_focused != oldFocus || groupsChanged);
    rebuildMarkers();
    if (m_focused != oldFocus || m_spectralTrack != m_focused
        || std::find(waveforms.begin(), waveforms.end(), m_focused) != waveforms.end())
        markSpectralStale();
    // History restores a curve together with the triggers it was made from.
    rememberDuckingInputs();
}

void Backend::undo()
{
    if (deferAutomationEdit([this] { undo(); return true; })) return;
    if (deferPluginEdit([this] { undo(); return true; }, {}, true)) return;
    finishAutomationRecording();
    const session::EditCommand *command = m_undo.undo();
    if (command != nullptr) {
        installState(session::stateForPluginAwareEdit(session::captureState(m_project), command->after, command->before), command->track);
        m_dirty = true;
        armAutosave();
        m_hostEditRevision = m_confirmedEditRevision = ++m_editRevision;
    }
    emit timelineChanged();
}

void Backend::redo()
{
    if (deferAutomationEdit([this] { redo(); return true; })) return;
    if (deferPluginEdit([this] { redo(); return true; }, {}, true)) return;
    finishAutomationRecording();
    const session::EditCommand *command = m_undo.redo();
    if (command != nullptr) {
        installState(session::stateForPluginAwareEdit(session::captureState(m_project), command->before, command->after), command->track);
        m_dirty = true;
        armAutosave();
        m_hostEditRevision = m_confirmedEditRevision = ++m_editRevision;
    }
    emit timelineChanged();
}

QString Backend::undoLabel() const
{
    return QString::fromStdString(m_undo.undoLabel());
}

void Backend::split(qint64 frame)
{
    if (deferAutomationEdit([this, frame] { split(frame); return true; })) return;
    std::vector<session::Clip> *clips = focusedClips();
    if (clips == nullptr || clips->empty())
        return;
    const session::SessionState before = captureEditState();
    session::edits::splitAt(*clips, frame < 0 ? m_selection.start : frame);
    commit("split", m_focused, before);
}

void Backend::deleteSelection()
{
    if (deferAutomationEdit([this] { deleteSelection(); return true; })) return;
    if (!m_selectedClips.empty()) { deleteClipSelection("delete clips"); return; }
    // Tracks picked by their headers, with no time range: Delete removes those tracks.
    if (m_selection.empty() && !m_selectedTracks.empty()) { removeTrackIds(selectedTrackIds()); return; }
    if (selectedTrackIds().size() > 1) { deleteTrackSelection(m_ripple, m_ripple ? "ripple delete tracks" : "delete tracks"); return; }
    std::vector<session::Clip> *clips = focusedClips();
    if (clips == nullptr || clips->empty())
        return;
    if (m_selection.empty()) {
        setError(QStringLiteral("delete needs a selection: drag across a lane to select a time range first"));
        return;
    }
    if (m_selection.channel >= 0) { startChannelEdit(session::ChannelEdit::Silence); return; }
    const session::SessionState before = captureEditState();
    if (m_ripple) {
        // Ripple mode: the gap closes and what followed moves up. The selection
        // then refers to nothing, so it collapses to its start.
        session::edits::removeRange(*clips, m_selection.start, m_selection.end);
        m_selection.end = m_selection.start;
        commit("ripple delete", m_focused, before);
        return;
    }
    // Leaves the gap (A13): later clips stay put and the range goes silent. The
    // selection is deliberately kept -- it now frames the gap, which is exactly what
    // "delete, close the gap" wants next.
    session::edits::clearRange(*clips, m_selection.start, m_selection.end);
    commit("delete", m_focused, before);
}

void Backend::deleteClosingGap()
{
    if (deferAutomationEdit([this] { deleteClosingGap(); return true; })) return;
    if (!m_selectedClips.empty()) { setError(tr("Select a time range for this command. Delete removes whole clips without moving other audio.")); return; }
    if (selectedTrackIds().size() > 1) { deleteTrackSelection(true, "close gap on tracks"); return; }
    if (m_selection.channel >= 0) { setError(QStringLiteral("choose All channels for this whole-track edit")); return; }
    std::vector<session::Clip> *clips = focusedClips();
    if (clips == nullptr || clips->empty())
        return;
    if (m_selection.empty()) {
        setError(QStringLiteral("close the gap needs a selection: drag across a lane to select a time range first"));
        return;
    }
    const session::SessionState before = captureEditState();
    session::edits::removeRange(*clips, m_selection.start, m_selection.end);
    // The selection no longer refers to anything: its content is gone and what
    // followed has moved into its place.
    m_selection.end = m_selection.start;
    commit("close gap", m_focused, before);
}

void Backend::deleteTimeAllTracks()
{
    if (deferAutomationEdit([this] { deleteTimeAllTracks(); return true; })) return;
    if (!m_selectedClips.empty()) { setError(tr("Select a time range for this command. Delete removes whole clips without moving other audio.")); return; }
    if (m_selection.empty()) {
        setError(QStringLiteral("delete time across all tracks needs a selection: drag across a lane to select a time range first"));
        return;
    }

    const session::SessionState before = captureEditState();
    if (!session::edits::removeRangeAllTracks(m_project, m_selection.start, m_selection.end)) {
        setError(QStringLiteral("Cannot delete this time range: automation point limit reached or empty range"));
        return;
    }
    m_selection.end = m_selection.start;
    commit("delete time across all tracks", session::kInvalidTrack, before);
}

void Backend::cut()
{
    if (deferAutomationEdit([this] { cut(); return true; })) return;
    if (!m_selectedClips.empty()) { if (copyClipSelection()) deleteClipSelection("cut clips"); return; }
    if (selectedTrackIds().size() > 1) { if (copyTrackSelection()) deleteTrackSelection(m_ripple, "cut tracks"); return; }
    std::vector<session::Clip> *clips = focusedClips();
    if (clips == nullptr || clips->empty() || m_selection.empty()) {
        setError(QStringLiteral("cut needs a selection: drag across a lane to select a time range first"));
        return;
    }
    if (m_selection.channel < 0 && std::any_of(clips->begin(), clips->end(), [this](const auto& clip) {
            return clip.group != session::kInvalidClipGroup && clip.intersects(m_selection.start, m_selection.end);
        })) {
        if (copyTrackSelection()) deleteTrackSelection(m_ripple, "cut grouped range");
        return;
    }
    const session::SessionState before = captureEditState();
    m_multitrackClipboard.reset();
    m_clipboard = session::edits::copyRange(*clips, m_selection.start, m_selection.end);
    // A single-channel clipboard carries samples, not whole-clip membership.
    for (auto& clip : m_clipboard) clip.group = session::kInvalidClipGroup;
    m_clipboardFrames = m_selection.length(); m_clipboardChannel = m_selection.channel;
    // Cut follows the ripple switch the way delete does: with it off the range goes
    // silent and everything else keeps its place, with it on the gap closes.
    if (m_selection.channel >= 0) { startChannelEdit(session::ChannelEdit::Silence); emit timelineChanged(); return; }
    if (m_ripple) {
        session::edits::removeRange(*clips, m_selection.start, m_selection.end);
        m_selection.end = m_selection.start;
    } else {
        session::edits::clearRange(*clips, m_selection.start, m_selection.end);
    }
    commit("cut", m_focused, before);
}

void Backend::copy()
{
    if (!m_selectedClips.empty()) { (void)copyClipSelection(); return; }
    if (selectedTrackIds().size() > 1) { (void)copyTrackSelection(); return; }
    std::vector<session::Clip> *clips = focusedClips();
    if (clips == nullptr || m_selection.empty()) {
        setError(QStringLiteral("copy needs a selection: drag across a lane to select a time range first"));
        return;
    }
    if (m_selection.channel < 0 && std::any_of(clips->begin(), clips->end(), [this](const auto& clip) {
            return clip.group != session::kInvalidClipGroup && clip.intersects(m_selection.start, m_selection.end);
        })) { (void)copyTrackSelection(); return; }
    m_multitrackClipboard.reset();
    m_clipboard = session::edits::copyRange(*clips, m_selection.start, m_selection.end);
    for (auto& clip : m_clipboard) clip.group = session::kInvalidClipGroup;
    m_clipboardFrames = m_selection.length(); m_clipboardChannel = m_selection.channel;
    emit timelineChanged();         // clipboard state changed, nothing else did
}

void Backend::paste(qint64 frame)
{
    if (deferAutomationEdit([this, frame] { paste(frame); return true; })) return;
    if (m_multitrackClipboard) { startMultitrackPaste(frame, false); return; }
    if (m_selection.channel >= 0 || m_clipboardChannel >= 0
        || (m_clipboardFrames > 0 && m_clipboardFrames != session::edits::totalLength(m_clipboard))) {
        startChannelEdit(session::ChannelEdit::Paste, 1, frame); return;
    }
    std::vector<session::Clip> *clips = focusedClips();
    if (clips == nullptr)
        return;
    if (m_clipboard.empty()) {
        setError(QStringLiteral("nothing to paste: copy or cut a selection first"));
        return;
    }
    const session::SessionState before = captureEditState();
    const session::FramePos at = frame < 0 ? m_playhead : frame;
    if (m_ripple)
        session::edits::insertAt(*clips, m_clipboard, at);
    else
        session::edits::overwriteAt(*clips, m_clipboard, at);
    commit(m_ripple ? "ripple paste" : "paste", m_focused, before);
}

void Backend::trim()
{
    if (deferAutomationEdit([this] { trim(); return true; })) return;
    if (m_selection.channel >= 0) { setError(QStringLiteral("choose All channels for this whole-track edit")); return; }
    std::vector<session::Clip> *clips = focusedClips();
    if (clips == nullptr || clips->empty() || m_selection.empty()) {
        setError(QStringLiteral("trim needs a selection: drag across a lane to select a time range first"));
        return;
    }
    const session::SessionState before = captureEditState();
    session::edits::trimToRange(*clips, m_selection.start, m_selection.end);
    m_selection.end = m_selection.start = 0;
    commit("trim", m_focused, before);
}

void Backend::gainSelection(double db)
{
    if (deferAutomationEdit([this, db] { gainSelection(db); return true; })) return;
    std::vector<session::Clip> *clips = focusedClips();
    if (clips == nullptr || clips->empty() || m_selection.empty()) {
        setError(QStringLiteral("gain needs a selection: drag across a lane to select a time range first"));
        return;
    }
    if (m_selection.channel >= 0) { startChannelEdit(session::ChannelEdit::Gain, linearFromDb(db)); return; }
    const session::SessionState before = captureEditState();
    session::edits::applyGain(*clips, m_selection.start, m_selection.end,
                              linearFromDb(db));
    commit("gain", m_focused, before);
}

void Backend::silenceSelection()
{
    if (deferAutomationEdit([this] { silenceSelection(); return true; })) return;
    std::vector<session::Clip> *clips = focusedClips();
    if (clips == nullptr || clips->empty())
        return;
    if (m_selection.empty()) {
        setError(QStringLiteral("silence needs a selection: drag across a lane to select a time range first"));
        return;
    }
    if (m_selection.channel >= 0) { startChannelEdit(session::ChannelEdit::Silence); return; }
    const session::SessionState before = captureEditState();
    session::edits::clearRange(*clips, m_selection.start, m_selection.end);
    commit("silence", m_focused, before);
}

double Backend::selectionPeak() const
{
    const session::Track *track = m_project.track(m_focused);
    if (track == nullptr || m_selection.empty())
        return 0.0;

    float peak = 0.0f;
    try {
        session::ClipReader reader(m_project);
        for (const auto& clip : track->clips) {
            const auto from = std::max(clip.timelineStart,m_selection.start), to = std::min(clip.timelineEnd(),m_selection.end);
            for (auto at = from; at < to;) {
                const auto count = std::uint32_t(std::min<session::FramePos>(session::ClipReader::kBlock,to-at));
                reader.read(clip,at-clip.timelineStart,count);
                for (unsigned c = 0; c < reader.channels(); ++c) {
                    if (m_selection.channel >= 0 && int(c) != m_selection.channel) continue;
                    for (unsigned f = 0; f < count; ++f) peak = std::max(peak,std::abs(reader.plane(c)[f]*clip.channelEnvelopeAt(at-clip.timelineStart+f,c,reader.channels())));
                }
                at += count;
            }
        }
    } catch (const std::exception&) { return 0; }

    return static_cast<double>(peak);
}

void Backend::normalizeSelection(double targetDb)
{
    if (deferAutomationEdit([this, targetDb] { normalizeSelection(targetDb); return true; })) return;
    std::vector<session::Clip> *clips = focusedClips();
    if (clips == nullptr || clips->empty() || m_selection.empty()) {
        setError(QStringLiteral("normalize needs a selection: drag across a lane to select a time range first"));
        return;
    }
    if (m_selection.channel >= 0) { startChannelEdit(session::ChannelEdit::Normalize, linearFromDb(std::min(targetDb, 0.0))); return; }
    for (const auto& clip : *clips) {
        const auto* source = m_project.source(clip.source);
        if (source && (clip.elastic || source->audio.diskStorage()) && clip.intersects(m_selection.start, m_selection.end)) {
            session::ChannelEditSpec spec; spec.operation = session::ChannelEdit::Normalize;
            spec.gain = linearFromDb(std::min(targetDb, 0.0));
            startSampleEdit(SampleEditKind::Normalize, 0, 0, 0, 0, std::move(spec)); return;
        }
    }
    const double peak = selectionPeak();
    if (peak <= 0.0) {
        setError(QStringLiteral("the selection is silent; nothing to normalize"));
        return;
    }
    const double target = static_cast<double>(linearFromDb(std::min(targetDb, 0.0)));
    const auto factor = static_cast<float>(target / peak);
    const session::SessionState before = captureEditState();
    session::edits::applyGain(*clips, m_selection.start, m_selection.end, factor);
    commit("normalize", m_focused, before);
}

void Backend::pasteToNewTrack()
{
    if (deferAutomationEdit([this] { pasteToNewTrack(); return true; })) return;
    if (m_multitrackClipboard) { startMultitrackPaste(0, true); return; }
    if (m_clipboardChannel >= 0 || (m_clipboardFrames > 0 && m_clipboardFrames != session::edits::totalLength(m_clipboard))) {
        startChannelEdit(session::ChannelEdit::Paste, 1, 0, 1, true); return;
    }
    if (m_clipboard.empty()) {
        setError(QStringLiteral("nothing to paste: copy or cut a selection first"));
        return;
    }
    if (m_project.tracks().size() >= engine::Transport::kMaxTracks) {
        setError(QStringLiteral("this session already has as many tracks as OmaAudio plays (%1); remove one to add another").arg(engine::Transport::kMaxTracks));
        return;
    }
    const session::SessionState before = captureEditState();
    const session::TrackId id =
        m_project.addTrack("pasted " + std::to_string(m_project.tracks().size() + 1));
    session::Track *track = m_project.track(id);
    session::edits::insertAt(track->clips, m_clipboard, 0);
    m_focused = id;
    m_selection = session::Selection{.start = 0, .end = track->length(), .track = id};
    commit("paste to new track", id, before);
}

void Backend::fadeIn()
{
    if (deferAutomationEdit([this] { fadeIn(); return true; })) return;
    std::vector<session::Clip> *clips = focusedClips();
    if (clips == nullptr || clips->empty() || m_selection.empty()) {
        setError(QStringLiteral("fade needs a selection: drag across a lane to select a time range first"));
        return;
    }
    if (m_selection.channel >= 0) { startChannelEdit(session::ChannelEdit::FadeIn); return; }
    const session::SessionState before = captureEditState();
    session::edits::applyFadeIn(*clips, m_selection.start, m_selection.end);
    commit("fadeIn", m_focused, before);
    recordStep({{"kind", "fadeIn"}, {"on", true}, {"seconds", double(m_selection.end - m_selection.start) / m_project.sampleRate()}});
}

void Backend::fadeOut()
{
    if (deferAutomationEdit([this] { fadeOut(); return true; })) return;
    std::vector<session::Clip> *clips = focusedClips();
    if (clips == nullptr || clips->empty() || m_selection.empty()) {
        setError(QStringLiteral("fade needs a selection: drag across a lane to select a time range first"));
        return;
    }
    if (m_selection.channel >= 0) { startChannelEdit(session::ChannelEdit::FadeOut); return; }
    const session::SessionState before = captureEditState();
    session::edits::applyFadeOut(*clips, m_selection.start, m_selection.end);
    commit("fadeOut", m_focused, before);
    recordStep({{"kind", "fadeOut"}, {"on", true}, {"seconds", double(m_selection.end - m_selection.start) / m_project.sampleRate()}});
}

// --- projects --------------------------------------------------------------------

QString Backend::projectName() const
{
    if (m_projectPath.isEmpty())
        return {};
    QString name = QFileInfo(m_projectPath).fileName();
    // Projects end in .omaaudio; those saved before the rename end in .omawave.
    for (const auto suffix : {QLatin1String(".omaaudio"), QLatin1String(".omawave")})
        if (name.endsWith(suffix)) { name.chop(suffix.size()); break; }
    return name;
}

void Backend::save()
{
    if (m_projectPath.isEmpty()) {
        // The engine says it needs a path rather than guessing one.
        emit pathNeeded();
        return;
    }
    saveAs(m_projectPath);
}

void Backend::saveAs(const QString &path)
{
    if (m_loading) {
        setError(QStringLiteral("wait until the file has finished opening before saving"));
        return;
    }
    const QUrl url(path);
    QString local = url.isLocalFile() ? url.toLocalFile() : path;
    if (local.isEmpty()) {
        setError(QStringLiteral("saving needs a project path: use Save As to choose one"));
        return;
    }
    // A new project is a .omaaudio folder; one saved before the rename keeps its
    // .omawave name when saved in place, so nothing is renamed behind anyone's back.
    if (!local.endsWith(QLatin1String(".omaaudio")) && !local.endsWith(QLatin1String(".omawave")))
        local += QLatin1String(".omaaudio");
    if (m_saveRequests.empty() || m_saveRequests.back() != local)
        m_saveRequests.push_back(local);
    m_saving = true;
    emit timelineChanged();
    pumpSave();
}

void Backend::cancelSnapshot()
{
    m_snapshotAccess.reset();
    if (m_snapshotRetryTimer) m_snapshotRetryTimer->stop();
}

void Backend::retrySnapshot(int milliseconds)
{
    if (m_shuttingDown) return;
    if (!m_snapshotRetryTimer) {
        m_snapshotRetryTimer = new QTimer(this);
        m_snapshotRetryTimer->setSingleShot(true);
        connect(m_snapshotRetryTimer, &QTimer::timeout, this, [this] {
            if (m_autosavePending) autosave(/*synchronous=*/false);
            pumpSave();
        });
    }
    m_snapshotRetryTimer->start(milliseconds);
}

Backend::SnapshotResult Backend::captureProjectSnapshot(session::Project& snapshot, QString& error)
{
    if (deliveryBoundaryActive()) { retrySnapshot(); return SnapshotResult::Pending; }
    pollPluginNotifications();
    // Polling can start native-history capture and acquire (or inherit) the
    // handoff. Do not retain a second, invalid ownership request for Save.
    if (deliveryBoundaryActive()) { retrySnapshot(); return SnapshotResult::Pending; }
    bool hasState = false;
    for (const auto& [key, chain] : m_chains) {
        (void)key;
        if (chain) for (std::size_t slot = 0; slot < chain->size(); ++slot)
            hasState = hasState || chain->hasOpaqueState(slot);
    }
    // Input capture has its own clock. Never interrupt its output accompaniment
    // to serialize an effect; keep the request queued until recording finishes.
    const bool needsOwnership = hasState || processOwnedAutomation();
    if (needsOwnership && recording()) {
        cancelSnapshot();
        retrySnapshot(50);
        return SnapshotResult::Pending;
    }
    if (needsOwnership) {
        if (!m_snapshotAccess) m_snapshotAccess.emplace(m_transport.requestControl());
        if (!m_snapshotAccess->ready()) {
            retrySnapshot();
            return SnapshotResult::Pending;
        }
    }
    const auto release = qScopeGuard([this] {
        // Capture can flush idle native changes too. Associate those notifications
        // with the captured revision before releasing the processor handoff.
        consumePluginNotifications();
        cancelSnapshot();
    });
    try {
        if (needsOwnership) {
            // Drain the entire FIFO, including GUI overflow, before inspecting
            // live instances. No callback or parameter producer can run here.
            do {
                (void)m_snapshotAccess->drainCommands();
                flushPendingCommands();
            } while (!m_commands.empty());
        }
        // Complete the ownership boundary before copying curves. Native input
        // accepted during the last callback may have been flushed without ever
        // rendering samples, so save that intent only at its next legal frame.
        if (needsOwnership) {
            for (const auto& [key, chain] : m_chains) {
                (void)key;
                if (!chain) continue;
                std::string reason;
                if (!chain->flushParameterChanges(reason)) {
                    error = tr("Could not flush effect settings for Save: %1").arg(QString::fromStdString(reason));
                    return SnapshotResult::Failed;
                }
            }
            consumePluginNotifications();
            if (!capturePendingAutomation(error, m_automationPass ? m_automationPass->manualPending : std::nullopt)) return SnapshotResult::Failed;
        }
        session::Project candidate = m_project;
        const auto capture = [&](session::EffectOwner key, auto& inserts) {
            const auto found = m_chains.find(key);
            if (found == m_chains.end() || !found->second) return true;
            auto& chain = *found->second;
            for (std::size_t slot = 0; slot < inserts.size(); ++slot) {
                if (!chain.hasOpaqueState(slot)) continue;
                std::string reason;
                if (!chain.captureInsertState(slot, inserts[slot], reason)) {
                    error = QStringLiteral("Could not capture plug-in state for %1 (insert %2): %3")
                        .arg(QString::fromStdString(inserts[slot].uri)).arg(slot + 1)
                        .arg(QString::fromStdString(reason));
                    return false;
                }
                consumePluginNotifications();
                verifyReadoutState(key, found->second, slot, inserts[slot]);
            }
            return true;
        };
        bool captured = true;
        session::forEachEffectRack(candidate, [&](session::EffectOwner owner, const auto&) {
            if (!captured) return;
            auto edit = session::prepareEffectRackEdit(candidate, owner);
            if (!edit) {
                error = tr("The effect owner changed while preparing Save.");
                captured = false;
                return;
            }
            captured = capture(owner, edit->inserts);
            if (captured) {
                std::string reason;
                captured = session::replaceEffectRack(candidate, *edit, reason);
                if (!captured) error = tr("Could not capture effect settings for Save: %1")
                    .arg(QString::fromStdString(reason));
            }
        });
        if (!captured) return SnapshotResult::Failed;
        // Saving is not an edit. Do not replace model/history snapshots or the
        // chain's instantiation record with serialization side effects. The copy
        // records which controls belong to captured state, retaining automation bases.
        snapshot = std::move(candidate);
        return SnapshotResult::Ready;
    } catch (const std::exception& exception) {
        error = QStringLiteral("Could not capture project state: %1").arg(QString::fromUtf8(exception.what()));
        return SnapshotResult::Failed;
    }
}

void Backend::pumpSave()
{
    if (m_saveRequests.empty() || m_saveActive || m_busy.load(std::memory_order_acquire) || m_autosaving) {
        if (!m_autosavePending) cancelSnapshot();
        return;
    }
    joinWorker();
    session::Project snapshot(m_project.sampleRate());
    QString captureError;
    const auto captured = captureProjectSnapshot(snapshot, captureError);
    if (captured == SnapshotResult::Pending) return;
    const std::string target = m_saveRequests.front().toStdString();
    m_saveRequests.pop_front();
    if (captured == SnapshotResult::Failed) {
        m_saving = !m_saveRequests.empty();
        setError(captureError);
        emit timelineChanged();
        if (m_saving) retrySnapshot();
        return;
    }
    m_saveActive = true;
    m_saving = true;
    // A retry replaces the previous save's error. Clear at the start so an
    // unrelated error raised during this write is not erased by its completion.
    setError({});
    m_busy.store(true, std::memory_order_release);

    // Tracks and settings are a snapshot. Imported sources share immutable audio,
    // so this copy costs the session metadata rather than the length of the take.
    const io::ProjectView view = currentView();
    const std::uint64_t generation = m_sessionGeneration;
    const std::uint64_t revision = m_editRevision;
    emit timelineChanged();

    m_worker = std::thread([this, snapshot, view, target, generation, revision] {
        SaveJob out;
        out.sessionGeneration = generation;
        out.revision = revision;
        out.path = target;
        const io::SaveResult result = io::saveProject(snapshot, view, target);
        out.ok = result.ok;
        out.error = result.error;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_pendingSave.push_back(std::move(out));
            m_finishedJobs.push_back(Job::Save);
        }
        m_busy.store(false, std::memory_order_release);
        emit workerFinished();
    });
}

void Backend::loadProject(const QString &path)
{
    if (recording()) { setError(QStringLiteral("stop recording before opening a project")); return; }
    if (m_loading) {
        setError(QStringLiteral("a file is already opening"));
        return;
    }
    const QUrl url(path);
    ProjectLoadRequest request;
    setError({});
    request.path = url.isLocalFile() ? url.toLocalFile() : path;
    request.sessionGeneration = m_sessionGeneration;
    request.revision = m_editRevision;
    m_projectLoadRequest = std::move(request);
    m_cancelLoad.store(false, std::memory_order_relaxed);
    m_loading = true;
    emit loadingChanged();
    pumpProjectLoad();
}

void Backend::importSession(const QString& path, const QString& mediaFolder)
{
    if (recording() || m_loading || m_saving) {
        setError(QStringLiteral("Finish the current recording, import or save before importing a session file."));
        return;
    }
    if (deferAutomationEdit([this, path, mediaFolder] { importSession(path, mediaFolder); return true; })) return;
    const QUrl url(path), media(mediaFolder);
    ProjectLoadRequest request;
    request.path = url.isLocalFile() ? url.toLocalFile() : path;
    request.mediaFolder = media.isLocalFile() ? media.toLocalFile() : mediaFolder;
    request.sessionImport = true;
    request.sessionGeneration = m_sessionGeneration;
    request.revision = m_editRevision;
    request.hostRevision = m_hostEditRevision;
    m_projectLoadRequest = std::move(request);
    setError({});
    m_sessionImportMessage = QStringLiteral("Reading the %1 timeline and copying its audio…").arg(omaaudio::aaf::formatOf(m_projectLoadRequest->path));
    emit sessionImportChanged();
    m_cancelLoad.store(false, std::memory_order_relaxed);
    m_loading = true;
    emit loadingChanged();
    pumpProjectLoad();
}

void Backend::pumpProjectLoad()
{
    if (m_shuttingDown || !m_projectLoadRequest || m_projectLoadActive || m_saving
        || m_busy.load(std::memory_order_acquire))
        return;
    joinWorker();
    ProjectLoadRequest request = std::move(*m_projectLoadRequest);
    m_projectLoadRequest.reset();
    m_projectLoadActive = true;
    m_busy.store(true, std::memory_order_release);
    auto candidate = request.sessionImport ? std::optional<session::Project>(m_project) : std::nullopt;
    const bool needsCatalogue = !m_lv2->scanned() || (m_vst3ScanOptions && !m_pluginCatalogueComplete);
    const auto scanOptions = m_vst3ScanOptions;
    m_worker = std::thread([this, request = std::move(request), candidate = std::move(candidate), needsCatalogue, scanOptions]() mutable {
        ProjectLoadResult out;
        out.request = request;
        if (request.sessionImport) {
            auto imported = omaaudio::aaf::read(request.path, request.mediaFolder, std::move(*candidate), &m_cancelLoad);
            out.result.ok = imported.project.has_value();
            out.result.error = imported.error.toStdString();
            out.result.project = std::move(imported.project);
            out.importName = imported.name;
            out.importFormat = imported.format;
            out.importMessage = !out.result.ok ? imported.error
                : imported.video.isEmpty() ? QStringLiteral("Imported %1 tracks and %2 clips. Undo removes this import.")
                    .arg(imported.tracks).arg(imported.clips)
                : QStringLiteral("Imported %1 tracks and %2 clips, and the timeline's video, %3, which plays in the Video tab. Undo removes this import.")
                    .arg(imported.tracks).arg(imported.clips).arg(imported.video);
            if (!imported.warnings.isEmpty()) out.importMessage += QStringLiteral("\n\n") + imported.warnings.join(QStringLiteral("\n"));
        } else out.result = io::loadProject(request.path.toStdString(),
            request.recovery ? io::Layer::Recovery : io::Layer::Project);
        if (needsCatalogue && out.result.ok && out.result.project
            && !m_cancelLoad.load(std::memory_order_relaxed)) {
            const auto& project = *out.result.project;
            const bool hasEffects = !project.masterInserts().empty()
                || std::any_of(project.tracks().begin(), project.tracks().end(),
                    [](const auto& track) { return !track.inserts.empty(); });
            if (hasEffects) {
                // Discovery can take seconds. Never mutate the live catalogue
                // while QML reads it; hand over a complete worker-owned world.
                out.catalogue = scanPluginCatalogue(scanOptions, m_cancelLoad);
            }
        }
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_pendingProjectLoad.push_back(std::move(out));
            m_finishedJobs.push_back(Job::LoadProject);
        }
        m_busy.store(false, std::memory_order_release);
        emit workerFinished();
    });
}

void Backend::finishProjectLoad(ProjectLoadResult loaded, bool verified, QString verificationError)
{
    if (loaded.request.sessionImport && loaded.result.ok && loaded.result.project
        && !m_shuttingDown && !m_cancelLoad.load(std::memory_order_relaxed)
        && !m_applyingAutomationEdit && !m_applyingPluginEdit) {
        auto pending = std::make_shared<ProjectLoadResult>(std::move(loaded));
        if (deferAutomationEdit([this, pending, verified, verificationError] {
            finishProjectLoad(std::move(*pending), verified, verificationError); return true;
        }, [this, generation = pending->request.sessionGeneration, format = pending->importFormat] {
            m_projectLoadActive = false;
            m_loading = m_projectLoadRequest.has_value();
            // The worker completed, but its result was discarded when the
            // automation boundary failed or was cancelled. Retire the panel's
            // progress text too, without replacing a newer session's status.
            if (generation == m_sessionGeneration && !m_shuttingDown && !m_projectLoadRequest) {
                m_sessionImportMessage = tr("%1 import was not applied. Your current session was kept.").arg(format);
                emit sessionImportChanged();
            }
            emit loadingChanged();
        })) return;
        loaded = std::move(*pending);
    }
    if (!verified && loaded.request.sessionImport && loaded.result.ok && loaded.result.project
        && loaded.request.sessionGeneration == m_sessionGeneration && !m_shuttingDown
        && !m_cancelLoad.load(std::memory_order_relaxed)) {
        joinWorker();
        finishAutomationRecording();
        if (loaded.request.sessionGeneration == m_sessionGeneration
            && loaded.request.hostRevision == m_hostEditRevision
            && loaded.result.project->tracks().size() <= engine::Transport::kMaxTracks) {
            prepareSessionImport(std::move(loaded));
            return;
        }
    }
    if (!verified && !loaded.request.sessionImport && loaded.result.ok && loaded.result.project
        && loaded.request.sessionGeneration == m_sessionGeneration && !m_shuttingDown
        && !m_cancelLoad.load(std::memory_order_relaxed)) {
        // Retain the worker-owned project/catalogue while audio grants capture
        // ownership. Cancellation does not require waiting for that callback.
        auto pending = std::make_shared<ProjectLoadResult>(std::move(loaded));
        verifyLoadRevision(pending->request.sessionGeneration, pending->request.revision,
            [this, pending](QString error) {
                finishProjectLoad(std::move(*pending), true, std::move(error));
                pumpWorkerJobs();
            });
        return;
    }
    if (loaded.catalogue.world && !m_shuttingDown && !m_cancelLoad.load(std::memory_order_relaxed)) {
        acceptPluginCatalogue(std::move(loaded.catalogue));
        // An already open browser must see the same catalogue used to reopen.
        if (m_pluginsListed) publishPluginCatalogue();
    }
    m_projectLoadActive = false;
    const ProjectLoadRequest& request = loaded.request;
    if (m_shuttingDown || request.sessionGeneration != m_sessionGeneration) {
        m_loading = m_projectLoadRequest.has_value();
        emit loadingChanged();
        return;
    }
    if (request.sessionImport) {
        joinWorker();
        // Finalising a live automation pass can add its last point and advance
        // the revision. Do that before deciding whether the candidate is stale.
        if (loaded.result.ok && !m_cancelLoad.load(std::memory_order_relaxed))
            finishAutomationRecording();
        m_sessionImportMessage = loaded.importMessage;
        const auto stale = [this, format = loaded.importFormat] {
            m_sessionImportMessage = QStringLiteral("The session changed during import. Your edits were kept; import the %1 again.").arg(format);
            setError(m_sessionImportMessage);
        };
        if (m_cancelLoad.load(std::memory_order_relaxed))
            m_sessionImportMessage = QStringLiteral("%1 import cancelled; the session is unchanged.").arg(loaded.importFormat);
        else if (!loaded.result.ok) setError(QString::fromStdString(loaded.result.error));
        else if (!verificationError.isEmpty()) {
            m_sessionImportMessage = verificationError;
            setError(verificationError);
        } else if (request.hostRevision != m_hostEditRevision) stale();
        else if (loaded.result.project && loaded.result.project->tracks().size() > engine::Transport::kMaxTracks) {
            // The engine renders a fixed number of tracks; any beyond it would
            // appear in the session and play as silence.
            m_sessionImportMessage = QStringLiteral("The import would bring the session to %1 tracks and OmaAudio plays %2. Nothing was imported; remove tracks here or export fewer from the other application.")
                .arg(loaded.result.project->tracks().size()).arg(engine::Transport::kMaxTracks);
            setError(m_sessionImportMessage);
        } else if (loaded.result.project) {
            const auto before = captureEditState();
            const auto first = m_project.tracks().size();
            const bool empty = m_project.tracks().empty();
            auto& candidate = *loaded.result.project;
            const bool matchingTracks = std::all_of(m_project.tracks().begin(), m_project.tracks().end(),
                [&](const auto& track) { return candidate.track(track.id) != nullptr; });
            if (request.hostRevision != m_hostEditRevision || !matchingTracks) stale();
            else {
                // Only the imported tracks/media come from the worker. Existing
                // tracks and effects now contain coherent live settings and their
                // native History entries, which precede this independent import.
                for (const auto& track : m_project.tracks()) *candidate.track(track.id) = track;
                candidate.masterInserts() = m_project.masterInserts();
                // Both projects share the old immutable sources, including those
                // borrowed by playback/Undo. Newly imported media stays private.
                m_project = std::move(candidate);
                m_focused = m_project.tracks()[first].id;
                m_loaded = true;
                if (empty) { m_takeName = loaded.importName; m_takePath = request.path; }
                commit(("import " + loaded.importFormat).toStdString(), session::kInvalidTrack, before);
                emit takeChanged();
            }
        }
        m_loading = false;
        emit loadingChanged();
        emit sessionImportChanged();
        return;
    }
    if (m_cancelLoad.load(std::memory_order_relaxed))
        setError(QStringLiteral("Project opening cancelled; the session is unchanged."));
    else if (!loaded.result.ok)
        setError(QString::fromStdString(loaded.result.error));
    else if (!verificationError.isEmpty())
        setError(verificationError);
    else if (!loaded.result.project)
        setError(QStringLiteral("this project has no session"));
    else {
        const auto recovery = request.recovery;
        installLoaded(std::move(loaded.result), recovery ? recovery->project : request.path,
                      recovery.has_value());
        if (!recovery) rememberDocument(request.path, true);
        if (recovery) {
            // Adopt its bundle and index only after the whole session loaded.
            if (recovery->bundle != recovery->project)
                m_scratchBundle = recovery->bundle;
            m_recoveryWrittenTo = recovery->bundle;
            RecoveryEntry mine = *recovery;
            mine.written = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
            mine.pid = static_cast<qint64>(::getpid());
            if (const std::string error = writeRecoveryIndex(recoveryIndexPath().toStdString(), mine);
                !error.empty())
                setError(QString::fromStdString(error));
            else if (recovery->file != recoveryIndexPath())
                QFile::remove(recovery->file);
            m_recoveryOffer.reset();
            emit recoveryChanged();
            m_takeName = recovery->project.isEmpty() ? recovery->name : projectName();
            emit takeChanged();
        }
    }
    m_loading = false;
    emit loadingChanged();
}

void Backend::resetSessionJobs()
{
    // A search or an auto-heal still waiting for the worker belongs to the
    // session that asked for it.
    m_autoHealPending = false;
    if (m_queuedDetect) { m_queuedDetect.reset(); m_detecting = false; emit clicksChanged(); }
    cancelTrackLoudness(); cancelDucking();
    m_ducking = {}; m_duckingReady = false; m_duckingStale = true;
    m_duckingMessage.clear(); emit duckingChanged();
    m_trackLoudness = {};
    m_trackLoudnessStale = true;
    m_trackLoudnessMessage.clear();
    emit trackLoudnessChanged();
    m_resetStreaming = true;
    cancelStorage();
    clearNoisePrint();
    m_cleanupPreview.clear(); m_unusedAudioFiles.clear(); m_storagePreviewStamp.clear(); m_storageMessage.clear();
    emit storageChanged();
    finishAutomationRecording();
    m_automationMode = "read"; m_automationDescriptor.clear(); emit automationLiveChanged();
    stopRecording();
    m_recordArmedTrack = -1;
    emit recordingChanged();
    cancelClipMove();
    setLoopEnabled(false);
    m_loopStart = m_loopEnd = 0;
    emit loopChanged();
    cancelSampleEdit();
    cancelPitchTracking();
    ++m_sessionGeneration;
    m_duckingInputs.clear(); // a new session's recipes describe its own triggers
    pruneEditors(true);
    cancelClipEffectRenders(true);
    m_clipEffectFailures.clear();
    m_automationPass.reset();
    discardDeferredAutomationEdits();
    m_automationFinishPending = m_automationFinishFailed = m_automationFinishCancel = false;
    m_nativeHistory.clear();
    cancelPluginEdits();
    m_editRevision = m_confirmedEditRevision = m_savedRevision = m_hostEditRevision = 0;
    emit automationStampChanged();
    m_saveRequests.clear();
    cancelSnapshot();
    pumpDeliverySnapshots();
    if (m_autosavePending) { m_autosavePending = false; m_autosaving = false; emit autosaveChanged(); }
    m_saving = false;
    m_projectLoadRequest.reset();
    m_loading = false;
    emit loadingChanged();
}

void Backend::installLoaded(io::LoadResult result, const QString &projectPath, bool dirty)
{
    joinWorker();

    if (!result.project.has_value()) {
        setError(QStringLiteral("that project has no session in it"));
        return;
    }
    resetSessionJobs();
    const auto oldRate = m_project.sampleRate();
    const auto oldBits = m_project.defaultExportBits();
    const bool restart = prepareProjectRate(result.project->sampleRate());

    // Whatever the old session was autosaving is over: the user chose this one.
    dropRecovery();

    m_retiredProjects.push_back(std::move(m_project));
    m_project = std::move(*result.project);
    rescaleProjectView(oldRate, false);

    for (auto &entry : m_peaks)
        removeTextures(entry.second);
    m_peaks.clear();
    m_dirtyPeaks.clear();
    removeTextures(m_analysis.peaks);
    m_analysis = Analysis{};
    m_masterLevels.clear();
    m_masterReport.clear();
    m_shortTermHistory.clear();
    m_momentaryHistory.clear();
    m_analysisStale = true;
    emit analysisChanged();
    clearSpectral();

    m_undo.clear();
    m_clipboard.clear(); m_clipboardFrames = 0; m_clipboardChannel = -1;
    m_multitrackClipboard.reset(); m_selectedTracks.clear(); m_selectedClips.clear(); m_trackSelectionAnchor = session::kInvalidTrack; m_clipGroupsLinked = true;
    ++m_trackSelectionRevision;
    m_projectPath = projectPath;
    m_dirty = dirty;
    m_focused = m_project.track(result.view.focus) != nullptr ? result.view.focus
              : m_project.tracks().empty() ? session::kInvalidTrack
                                           : m_project.tracks().front().id;
    m_loaded = !m_project.tracks().empty();
    m_takeName = projectName();
    m_takePath = m_projectPath;

    rebuildAllChains();
    publishPlan();
    publishTrackParams();
    enqueueAudioCommand(engine::Command::setMasterTrim(m_project.masterTrim()));
    enqueueAudioCommand(engine::Command::resetLoudness());
    finishProjectRate(restart, oldRate, oldBits);
    m_selection = result.view.selection;
    clampSelection();
    for (const session::Track &t : m_project.tracks())
        markPeaksDirty(t.id);
    pumpPeaks();
    rebuildTracks();
    rebuildMarkers();

    // The Spectral page's state, as it was (A22).
    const double nyquist = static_cast<double>(m_project.sampleRate()) / 2.0;
    m_bandLowHz = std::clamp(static_cast<double>(result.view.band.lowHz), 0.0, nyquist);
    m_bandHighHz = std::clamp(static_cast<double>(result.view.band.highHz), 0.0, nyquist);
    if (m_bandHighHz <= m_bandLowHz)
        m_bandLowHz = m_bandHighHz = 0.0;
    emit selectionChanged();
    m_mask = std::move(result.view.mask);
    m_maskFeather = static_cast<int>(std::min<std::uint32_t>(m_mask.featherBins, 64));
    rebuildMaskShapes();
    m_noisePrint = std::move(result.view.noisePrint);
    m_noisePrintSummary = QString::fromStdString(result.view.noisePrintSummary);
    emit noisePrintChanged();
    m_soundChoices = std::move(result.view.sound);

    if (m_dirty)
        armAutosave();
    emit takeChanged();
    emit timelineChanged();
    emit projectSampleSettingsChanged();
}

void Backend::newProject()
{
    // Accepted timeline jobs may still need the outgoing pass's last callback.
    // Otherwise New is an explicit discard, including escape from a plug-in
    // which refuses capture. Do not make that discard depend on saving it.
    if (hasPendingTimelineSnapshot() && deferAutomationEdit([this] { newProject(); return true; })) return;
    m_cancelLoad.store(true, std::memory_order_relaxed);
    // Stop obsolete calculations before joining their worker, not afterwards in
    // resetSessionJobs(). An active export or save still finishes its write.
    if (m_activeRender < 0) m_cancelJob.store(true, std::memory_order_relaxed);
    cancelSampleEdit();
    cancelStorage();
    cancelRepair();
    cancelPitchTracking();
    ++m_noisePrintSequence;
    m_noisePrintRequest.reset();
    m_cancelNoisePrint.store(true, std::memory_order_relaxed);
    cancelTrackLoudness(); cancelDucking();
    m_cancelPeaks.store(true, std::memory_order_relaxed);
    joinWorker();
    m_cancelPeaks.store(false, std::memory_order_relaxed);
    resetSessionJobs();
    setError({});
    dropRecovery();

    const std::uint32_t rate = m_project.sampleRate();
    m_retiredProjects.push_back(std::move(m_project));
    m_project = session::Project(rate);

    for (auto &entry : m_peaks)
        removeTextures(entry.second);
    m_peaks.clear();
    m_dirtyPeaks.clear();
    removeTextures(m_analysis.peaks);
    m_analysis = Analysis{};
    m_masterLevels.clear();
    m_masterReport.clear();
    m_shortTermHistory.clear();
    m_momentaryHistory.clear();
    m_analysisStale = true;
    emit analysisChanged();
    clearSpectral();
    for (auto &entry : m_chains) {
        if (entry.second != nullptr)
            m_retiredChains.push_back(std::move(entry.second));
    }
    m_chains.clear();
    m_chainSpecs.clear();

    m_undo.clear();
    m_clipboard.clear(); m_clipboardFrames = 0; m_clipboardChannel = -1;
    m_multitrackClipboard.reset(); m_selectedTracks.clear(); m_selectedClips.clear(); m_trackSelectionAnchor = session::kInvalidTrack; m_clipGroupsLinked = true;
    ++m_trackSelectionRevision;
    m_projectPath.clear();
    m_dirty = false;
    m_confirmedEditRevision = m_savedRevision = m_editRevision;
    // Session state that was about the old take.
    m_bandLowHz = 0.0;
    m_bandHighHz = 0.0;
    clearNoisePrint();
    clearMask();
    m_soundChoices.clear();
    if (m_hearWithoutEffects) { m_hearWithoutEffects = false; emit previewChanged(); }
    m_loaded = false;
    m_takeName.clear();
    m_takePath.clear();
    m_focused = session::kInvalidTrack;
    m_selection = session::Selection{};

    publishPlan();
    publishTrackParams();
    enqueueAudioCommand(engine::Command::setMasterTrim(1.0f));
    enqueueAudioCommand(engine::Command::resetLoudness());
    rebuildTracks();
    rebuildMarkers();
    emit takeChanged();
    emit timelineChanged();
    emit selectionChanged();
    emit projectSampleSettingsChanged();
}

// --- plugins ---------------------------------------------------------------------

lv2::HostFeatures &Backend::hostFeatures()
{
    if (m_features == nullptr) {
        m_features = std::make_unique<lv2::HostFeatures>(
            m_urids, static_cast<double>(m_project.sampleRate()), lv2::kMaxBlock);
    }
    return *m_features;
}

bool Backend::vst3DiscoveryAvailable() const
{
    return QFileInfo::exists(QString::fromStdString(defaultVst3ScanOptions().helper));
}

void Backend::setVst3Discovery(bool on)
{
    m_vst3Discovery = on;
    const bool wanted = on && vst3DiscoveryAvailable();
    if (wanted == m_vst3ScanOptions.has_value()) { emit pluginsChanged(); return; }
    if (wanted) m_vst3ScanOptions = defaultVst3ScanOptions(); else m_vst3ScanOptions.reset();
    // The next catalogue read follows the choice; effects already in use keep running.
    m_pluginCatalogueComplete = false;
    emit pluginsChanged();
}

vst3::CatalogueOptions Backend::defaultVst3ScanOptions()
{
    const auto folder = QCoreApplication::applicationDirPath();
    auto helper = folder + QStringLiteral("/omaaudio-vst3-scan");
    if (!QFileInfo::exists(helper))
        helper = QDir::cleanPath(folder + QStringLiteral("/../lib/omaaudio/omaaudio-vst3-scan"));
    return {helper.toStdString(), vst3::searchPath(folder.toStdString())};
}

Backend::PluginCatalogueResult Backend::scanPluginCatalogue(
    const std::optional<vst3::CatalogueOptions>& options, const std::atomic<bool>& cancel)
{
    PluginCatalogueResult result;
    if (cancel.load(std::memory_order_relaxed)) { result.cancelled = true; return result; }
    try {
        result.world = std::make_unique<lv2::World>();
        result.world->scan();
        std::vector<std::string> warnings;
        result.complete = !options || result.world->scanVst3(*options, warnings, &cancel);
        for (const auto& warning : warnings) result.warnings.append(QString::fromStdString(warning));
    } catch (const std::exception& error) {
        result.world.reset();
        result.warnings.append(tr("Plug-in discovery failed: %1").arg(QString::fromUtf8(error.what())));
    } catch (...) {
        result.world.reset();
        result.warnings.append(tr("Plug-in discovery failed."));
    }
    result.cancelled = cancel.load(std::memory_order_relaxed);
    if (result.cancelled) result.world.reset();
    return result;
}

void Backend::acceptPluginCatalogue(PluginCatalogueResult result)
{
    if (result.cancelled || m_pluginCatalogueComplete) return;
    m_pluginScanWarnings = std::move(result.warnings);
    if (result.world && (!m_lv2->scanned() || result.complete)) {
        if (m_lv2->scanned()) m_earlyCatalogues.push_back(std::move(m_lv2));
        m_lv2 = std::move(result.world);
        m_pluginCatalogueComplete = result.complete;
    }
    if (result.complete && m_lv2->scanned()) m_pluginCatalogueComplete = true;
    emit pluginsChanged();
}

void Backend::publishPluginCatalogue()
{
    QVariantList list;
    for (const auto& p : m_lv2->plugins()) {
        QVariantMap entry;
        entry["uri"] = QString::fromStdString(p.uri);
        entry["name"] = QString::fromStdString(p.name);
        entry["className"] = QString::fromStdString(p.className);
        entry["category"] = pluginCategory(entry["className"].toString());
        entry["author"] = QString::fromStdString(p.author);
        entry["format"] = QString::fromLatin1(host::formatName(p.format));
        entry["layout"] = p.layout == lv2::Layout::Mono ? QStringLiteral("mono")
                        : p.layout == lv2::Layout::Stereo ? QStringLiteral("stereo")
                        : p.layout == lv2::Layout::MonoOrStereo ? QStringLiteral("either") : QStringLiteral("none");
        auto keyChannels = p.sidechainIn;
        if (!p.audioConfigurations.empty()) {
            const bool stereo = p.fits(kOutputChannels);
            const auto* configuration = p.configuration(stereo ? kOutputChannels : 1);
            keyChannels = configuration ? (stereo ? configuration->sidechainChannels : configuration->sidechainChannels ? kOutputChannels : 0) : 0;
        }
        entry["sidechain"] = static_cast<int>(keyChannels);
        entry["controls"] = static_cast<int>(p.controlIn);
        entry["usable"] = p.usable;
        entry["reason"] = QString::fromStdString(p.unusableReason);
        list.append(entry);
    }
    m_plugins = std::move(list);
    m_pluginsListed = true;
    emit pluginsChanged();
}

void Backend::listPlugins()
{
    if (m_pluginsListed || m_scanning) return;
    if (m_shuttingDown || m_loading || m_saving || m_busy.load(std::memory_order_acquire)) {
        setError(QStringLiteral("still working on the previous job; wait for it to finish or cancel it"));
        return;
    }
    // A loaded project may already have prepared the complete catalogue.
    if (m_pluginCatalogueComplete) { publishPluginCatalogue(); return; }
    joinWorker();
    m_busy.store(true, std::memory_order_release);
    m_cancelPluginScan.store(false, std::memory_order_relaxed);
    m_scanning = true;
    emit pluginsChanged();
    const auto options = m_vst3ScanOptions;
    const bool needsCatalogue = !m_lv2->scanned() || options.has_value();
    m_worker = std::thread([this, options, needsCatalogue] {
        PluginCatalogueResult catalogue;
        if (needsCatalogue) catalogue = scanPluginCatalogue(options, m_cancelPluginScan);
        else catalogue.complete = true;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_pendingCatalogues.push_back(std::move(catalogue));
            m_finishedJobs.push_back(Job::Scan);
        }
        m_busy.store(false, std::memory_order_release);
        emit workerFinished();
    });
}

void Backend::rebuildChain(session::TrackId id)
{
    session::Track *track = m_project.track(id);
    rebuildChainFor(session::EffectOwner::track(id), track != nullptr ? &track->inserts : nullptr);
}

bool Backend::rebuildChainFor(session::EffectOwner key, std::vector<session::Insert> *inserts, bool restarting)
{
    if ((!key.isTrack() && !key.isMaster()) || (inserts && !session::findEffectRack(m_project, key))) {
        setError(tr("This effect owner is unavailable."));
        return false;
    }
    // A restart is transactional: preparation, state capture and metadata must
    // succeed before any live processor, model setting or Undo baseline changes.
    auto* published = inserts;
    std::vector<session::Insert> replacement;
    if (restarting) {
        const auto running = m_chains.find(key);
        if (!inserts || inserts->empty() || running == m_chains.end() || !running->second) return false;
        replacement = *inserts;
        inserts = &replacement;
    }
    if (inserts) for (auto& insert : *inserts) {
        if (!insert.legacyClapControls) continue;
        if (!m_lv2->scanned()) m_lv2->scan();
        std::string error;
        auto probe = host::createInstance(*m_lv2, insert.uri, hostFeatures(), error);
        if (!probe) continue; // A missing plugin keeps its original values intact.
        const auto& ports = probe->controls();
        if (std::any_of(insert.controls.begin(), insert.controls.end(),
                [&](const auto& control) { return control.port >= ports.size(); })) continue;
        for (auto& control : insert.controls) control.port = ports[control.port].index;
        insert.legacyClapControls = false;
    }
    // Nothing to do when the model still describes the chain that is running. Undo
    // restores whole snapshots, so without this an undone cut on one track would
    // re-instantiate every plugin on every other one.
    const auto sameControls = [](const auto& a, const auto& b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (a[i].identity != b[i].identity || a[i].uri != b[i].uri || a[i].bypass != b[i].bypass
                || a[i].legacyClapControls != b[i].legacyClapControls
                || a[i].instanceState != b[i].instanceState
                || a[i].controls.size() != b[i].controls.size()) return false;
            for (std::size_t c = 0; c < a[i].controls.size(); ++c)
                if (a[i].controls[c].port != b[i].controls[c].port
                    || a[i].controls[c].value != b[i].controls[c].value
                    || a[i].controls[c].fromState != b[i].controls[c].fromState) return false;
        }
        return true;
    };
    const auto live = m_chains.find(key);
    if (const auto built = m_chainSpecs.find(key);
        !restarting && built != m_chainSpecs.end() && live != m_chains.end() && live->second
        && inserts != nullptr && sameControls(built->second, *inserts)) {
        return false; // Automation belongs to render plans, never the instance cache.
    }
    const auto retire = [&] {
        // Existing plans can still own the outgoing processor until the callback
        // acknowledges a new plan. Destruction stays on the control thread.
        if (const auto it = m_chains.find(key); it != m_chains.end()) {
            if (it->second) m_retiredChains.push_back(std::move(it->second));
            m_chains.erase(it);
        }
        m_chainSpecs.erase(key);
        m_nativeHistory.erase(key);
    };
    if (!restarting) retire();
    if (inserts == nullptr || inserts->empty()) return false;

    // A project reopened in a fresh session names plugins before anything has
    // asked for the browser. Without the catalogue every append fails "not
    // installed" and the chain comes back silent -- found by the app suite's
    // insert round trip the day inserts were first saved.
    if (!m_lv2->scanned())
        m_lv2->scan();

    ++m_chainRebuilds;
    auto chain = std::make_shared<lv2::Chain>(kOutputChannels, lv2::kMaxBlock);
    for (std::size_t slot = 0; slot < inserts->size(); ++slot) {
        const auto& insert = (*inserts)[slot];
        std::string error;
        if (!chain->append(*m_lv2, hostFeatures(), insert, error)) {
            if (restarting && live->second->hasProcessor(slot)) {
                setError(tr("Effect restart failed; the current effect chain is still running: %1")
                    .arg(QString::fromStdString(error)));
                return false;
            }
            chain->appendMissing(insert.identity);
            // The slot stays in the model: a project that names a plugin this box does
            // not have must keep it, values and all.
            setError(QString::fromStdString(error));
        }
    }
    if (chain->empty()) return false;

    // Before anything can hear it: 28 of the plugins installed here allocate during
    // their first blocks, and this is the thread where they are allowed to.
    if (!chain->warmUp(nullptr)) {
        setError(restarting ? tr("Effect restart warm-up failed; the current effect chain is still running.")
                            : tr("Effect warm-up failed. The effect chain could not be activated."));
        return false;
    }
    auto runtime = preparePluginHistory(chain, *inserts, restarting);
    if (restarting) {
        if (!runtime) return false;
        if (chain->restartRequested()) {
            setError(tr("Effect preparation requested another restart; the current effect chain is still running."));
            return false;
        }
        retire();
        *published = std::move(replacement);
        inserts = published;
    }
    chain->setParameterClock([](void* opaque) noexcept {
        const auto& backend = *static_cast<const Backend*>(opaque);
        const auto time = backend.m_meters.timelineState();
        return host::ParameterTime{time.position, backend.m_sessionGeneration, time.epoch, time.playing, time.valid};
    }, this);
    m_chains[key] = std::move(chain);
    m_nativeHistory[key] = std::move(runtime);
    // Cache only instance settings. Copying dense automation just to discard it
    // on every Undo/rebuild needlessly allocated on the interface thread.
    auto& specs = m_chainSpecs[key];
    specs.reserve(inserts->size());
    for (const auto& insert : *inserts) {
        session::Insert spec{.uri = insert.uri, .bypass = insert.bypass, .controls = {},
                             .legacyClapControls = insert.legacyClapControls, .instanceState = insert.instanceState,
                             .identity = insert.identity};
        spec.controls.reserve(insert.controls.size());
        for (const auto& control : insert.controls)
            spec.controls.push_back({.port = control.port, .value = control.value, .automation = {}, .fromState = control.fromState});
        specs.push_back(std::move(spec));
    }
    return true;
}

void Backend::rebuildAllChains()
{
    session::forEachLegacyEffectRack(m_project, [this](session::EffectOwner owner, auto& inserts) {
        rebuildChainFor(owner, &inserts);
    });

    for (auto it = m_chains.begin(); it != m_chains.end();) {
        if (session::findEffectRack(m_project, it->first) == nullptr) {
            if (it->second != nullptr)
                m_retiredChains.push_back(std::move(it->second));
            m_chainSpecs.erase(it->first);
            m_nativeHistory.erase(it->first);
            it = m_chains.erase(it);
        } else {
            ++it;
        }
    }
}

// Every plugin call takes a track argument: a track id, -1 for the focused track,
// or kMasterTarget for the bus. This resolves it once, so the six calls below do
// not each carry the branch.
Backend::ChainTarget Backend::chainTarget(int track)
{
    if (track == kMasterTarget) return chainTarget(kMasterChainKey);
    if (track < -1) return {};
    return chainTarget(session::EffectOwner::track(trackArg(track)));
}

Backend::ChainTarget Backend::chainTarget(session::EffectOwner owner)
{
    ChainTarget out;
    if (!owner.isTrack() && !owner.isMaster()) return out;
    auto* inserts = session::findLegacyEffectRack(m_project, owner);
    if (!inserts) return out;
    out.inserts = inserts;
    out.key = owner;
    out.track = owner.trackId();
    out.target = owner.isMaster() ? kMasterTarget : static_cast<int>(owner.trackId());
    return out;
}

bool Backend::setPluginFavorite(const QString &uri, bool favorite)
{
    if (const auto error = m_pluginLibrary.setFavorite(uri, favorite); !error.isEmpty()) {
        setError(error);
        return false;
    }
    emit pluginLibraryChanged();
    return true;
}

bool Backend::setPluginEnabled(const QString &uri, bool enabled)
{
    if (const auto error = m_pluginLibrary.setEnabled(uri, enabled); !error.isEmpty()) {
        setError(error);
        return false;
    }
    emit pluginLibraryChanged();
    return true;
}

bool Backend::clearRecentPlugins()
{
    if (const auto error = m_pluginLibrary.clearRecent(); !error.isEmpty()) {
        setError(error);
        return false;
    }
    emit pluginLibraryChanged();
    return true;
}

bool Backend::addPlugin(const QString &uri, int track)
{
    if (const auto deferred = deferAutomationEdit([this, uri, track] { return addPlugin(uri, track); })) return *deferred;
    if (m_scanning || m_loading || m_shuttingDown) {
        setError(QStringLiteral("wait until the plugin catalogue and session are ready"));
        return false;
    }
    if (m_pluginLibrary.disabled().contains(uri)) {
        setError(QStringLiteral("This plugin is disabled for new inserts. Enable it in the Plug-in Manager."));
        return false;
    }
    setError({});
    const ChainTarget target = chainTarget(track);
    if (target.inserts == nullptr) {
        setError(QStringLiteral("no track to add a plugin to: open or import audio first"));
        return false;
    }
    if (uri.startsWith(QLatin1String("vst3:")) && !m_pluginCatalogueComplete) {
        setError(tr("Read the installed effects first: open Add Effect or run plugin.list, then add this effect."));
        return false;
    }
    if (!m_lv2->scanned())
        m_lv2->scan();       // Legacy direct LV2/CLAP callers retain their synchronous API.

    const lv2::PluginInfo *info = m_lv2->find(uri.toStdString());
    if (info == nullptr) {
        setError(QStringLiteral("plugin not installed: ") + uri);
        return false;
    }
    if (!info->usable) {
        setError(QString::fromStdString(info->name + " cannot be an insert: "
                                        + info->unusableReason));
        return false;
    }
    // Refuse before touching the model, so a refusal is not an undo step.
    if (!info->fits(kOutputChannels) && !info->fits(1)) {
        setError(QString::fromStdString(info->name + " does not fit this track"));
        return false;
    }

    if (auto deferred = deferPluginEdit([this, uri, track = target.target] { return addPlugin(uri, track); }))
        return *deferred;
    const session::SessionState before = captureEditState();
    session::Insert insert;
    insert.uri = uri.toStdString();
    target.inserts->push_back(std::move(insert));

    rebuildChainFor(target.key, target.inserts);
    markDirty();
    publishPlan();
    rebuildTracks();
    (void)m_undo.push("add " + info->name, target.track, before,
                      session::captureState(m_project));
    emit timelineChanged();
    if (const auto error = m_pluginLibrary.remember(uri); !error.isEmpty())
        setError(error);
    else
        emit pluginLibraryChanged();
    return true;
}

void Backend::removePlugin(int slot, int track)
{
    if (deferAutomationEdit([this, slot, track] { removePlugin(slot, track); return true; })) return;
    const ChainTarget target = chainTarget(track);
    if (target.inserts == nullptr || slot < 0
        || static_cast<std::size_t>(slot) >= target.inserts->size())
        return;

    const auto identity = (*target.inserts)[static_cast<std::size_t>(slot)].identity;
    if (deferPluginEdit([this, key = target.key, track = target.target, identity] {
        const int current = pluginSlot(key, identity);
        if (current >= 0) removePlugin(current, track);
        return current >= 0;
    })) return;
    const session::SessionState before = captureEditState();
    target.inserts->erase(target.inserts->begin() + slot);
    followPluginPorts(target.target, slot, -1);

    rebuildChainFor(target.key, target.inserts);
    markDirty();
    publishPlan();
    rebuildTracks();
    (void)m_undo.push("remove plugin", target.track, before, session::captureState(m_project));
    emit timelineChanged();
}

void Backend::movePlugin(int from, int to, int track)
{
    if (deferAutomationEdit([this, from, to, track] { movePlugin(from, to, track); return true; })) return;
    const ChainTarget target = chainTarget(track);
    if (target.inserts == nullptr || from < 0
        || static_cast<std::size_t>(from) >= target.inserts->size())
        return;

    const auto size = static_cast<int>(target.inserts->size());
    const int index = std::clamp(to, 0, size - 1);
    if (index == from)
        return;

    const auto identity = (*target.inserts)[static_cast<std::size_t>(from)].identity;
    if (deferPluginEdit([this, key = target.key, track = target.target, identity, index] {
        const int current = pluginSlot(key, identity);
        if (current >= 0) movePlugin(current, index, track);
        return current >= 0;
    })) return;
    const session::SessionState before = captureEditState();
    session::Insert moved = (*target.inserts)[static_cast<std::size_t>(from)];
    target.inserts->erase(target.inserts->begin() + from);
    target.inserts->insert(target.inserts->begin() + index, std::move(moved));
    followPluginPorts(target.target, from, index);

    rebuildChainFor(target.key, target.inserts);
    markDirty();
    publishPlan();
    rebuildTracks();
    (void)m_undo.push("reorder plugins", target.track, before, session::captureState(m_project));
    emit timelineChanged();
}

void Backend::setPluginBypass(int slot, bool on, int track)
{
    if (deferAutomationEdit([this, slot, on, track] { setPluginBypass(slot, on, track); return true; })) return;
    const ChainTarget target = chainTarget(track);
    if (target.inserts == nullptr || slot < 0
        || static_cast<std::size_t>(slot) >= target.inserts->size())
        return;

    const auto identity = (*target.inserts)[static_cast<std::size_t>(slot)].identity;
    if (deferPluginEdit([this, key = target.key, track = target.target, identity, on] {
        const int current = pluginSlot(key, identity);
        if (current >= 0) setPluginBypass(current, on, track);
        return current >= 0;
    })) return;
    const session::SessionState before = captureEditState();
    (*target.inserts)[static_cast<std::size_t>(slot)].bypass = on;

    // Straight at the audio thread rather than through a rebuild: a bypass toggle must
    // not reload the plugin, both because it would drop a block and because whatever
    // the plugin had settled into would be gone.
    enqueueAudioCommand(
        engine::Command::setPluginBypass(target.key, static_cast<std::size_t>(slot), on));

    // The rebuild guard reads chainSpecs, so the record of what is running has to
    // follow the model or the next unrelated edit would rebuild the rack.
    if (const auto built = m_chainSpecs.find(target.key); built != m_chainSpecs.end()
        && static_cast<std::size_t>(slot) < built->second.size()) {
        built->second[static_cast<std::size_t>(slot)].bypass = on;
    }

    markDirty();
    rebuildTracks();
    (void)m_undo.push(on ? "bypass plugin" : "unbypass plugin", target.track, before,
                      session::captureState(m_project));
    emit timelineChanged();
}

void Backend::setPluginControl(int slot, qint64 port, double value, int track)
{
    if (!std::isfinite(value)) return;
    const ChainTarget target = chainTarget(track);
    if (target.inserts == nullptr || slot < 0
        || static_cast<std::size_t>(slot) >= target.inserts->size())
        return;

    if (port < 0 || port > std::numeric_limits<std::uint32_t>::max()) return;
    const auto index = static_cast<std::uint32_t>(port);
    const auto chain = m_chains.find(target.key);
    if (chain != m_chains.end() && chain->second) {
        if (const auto* ports = chain->second->controls(static_cast<std::size_t>(slot)); ports
            && std::none_of(ports->begin(), ports->end(), [index](const auto& control) {
                return control.index == index && !control.output;
            })) {
            setError(tr("This effect control is no longer editable. Refresh the Track Effects panel."));
            return;
        }
    }
    if (writeMatchingAutomation(track, QStringLiteral("fx:%1:%2").arg(slot).arg(port), value)) return;
    if (deferAutomationEdit([this, slot, port, value, track] { setPluginControl(slot, port, value, track); return true; })) return;
    finishAutomationRecording();

    const auto settingsRevision = ++m_pluginSettingSequence;
    // An explicit host edit remains independent of an open native gesture. Its
    // override belongs on both sides of that gesture's eventual Undo command.
    if (const auto pending = m_nativeHistory.find(target.key); pending != m_nativeHistory.end()
        && static_cast<std::size_t>(slot) < pending->second->inserts.size()) {
        auto& state = pending->second->inserts[static_cast<std::size_t>(slot)];
        if (state.before) state.before->setControl(index, value, settingsRevision);
        ++state.hostRevision;
        const auto control = std::find_if(state.hostControls.begin(), state.hostControls.end(),
            [index](const auto& item) { return item.port == index; });
        if (control == state.hostControls.end()) state.hostControls.push_back({index, value, state.hostRevision, settingsRevision});
        else *control = {index, value, state.hostRevision, settingsRevision};
    }
    (*target.inserts)[static_cast<std::size_t>(slot)].setControl(index, value, settingsRevision);
    const auto parameterVersion = chain != m_chains.end() && chain->second
        ? chain->second->parameterVersion(static_cast<std::size_t>(slot)) : 0;
    enqueueAudioCommand(engine::Command::setPluginControl(
        target.key, static_cast<std::size_t>(slot), index, value, parameterVersion));

    // The drawn descriptors follow -- but SILENTLY. Emitting pluginPortsChanged
    // here would reset the rack's ListView model on every drag tick, destroying
    // the delegate under the pointer and killing the drag it is mid-way through.
    // The delegate draws its own value while pressed and calls flushPluginPorts()
    // on release; this keeps the list correct for whenever it is next rebuilt.
    if (slot == m_pluginPortsSlot && target.target == m_pluginPortsTarget) {
        for (auto &entry : m_pluginPorts) {
            QVariantMap p = entry.toMap();
            if (p.value(QStringLiteral("index")).toUInt() == index) {
                p[QStringLiteral("value")] = value;
                entry = p;
                break;
            }
        }
    }

    // The rebuild guard compares the model with this record of what is running.
    // The live instance has just been given the value, so the record takes it
    // too: otherwise an Undo back to the record's stale value looks like "no
    // change" and the plugin keeps playing a setting the project no longer has.
    if (const auto built = m_chainSpecs.find(target.key); built != m_chainSpecs.end()
        && static_cast<std::size_t>(slot) < built->second.size())
        built->second[static_cast<std::size_t>(slot)].setControl(index, value, settingsRevision);

    ++m_pluginValuesRevision; emit pluginValuesChanged();
    // Dirty but not undoable, exactly as a fader move is.
    markDirty();
    for (const auto& c : (*target.inserts)[static_cast<std::size_t>(slot)].controls)
        if (c.port == index && c.hasAutomationData()) { publishPlan(); break; }
}

// The rack's control list describes the plugin in one slot. When the chain is
// reordered or a neighbour is removed, the list follows its plugin to the new
// slot. When that plugin itself is removed, the slot stays and the list is read
// again for whatever sits there now (or nothing), as a history restore does:
// what must never happen is one plugin's controls drawn over, and written
// into, another.
void Backend::followPluginPorts(int target, int from, int to)
{
    if (m_pluginPortsSlot < 0 || target != m_pluginPortsTarget) return;
    int slot = m_pluginPortsSlot;
    if (to < 0) {                                   // removed
        if (from == slot) { requestPluginPorts(slot, target); return; }
        if (from < slot) --slot;
    } else if (from == slot) slot = to;             // the selected one moved
    else if (from < slot && to >= slot) --slot;     // another moved past it
    else if (from > slot && to <= slot) ++slot;
    if (slot == m_pluginPortsSlot) return;
    m_pluginPortsSlot = slot;
    emit pluginPortsChanged();
}

double Backend::pluginPortValue(int slot, qint64 port, int track, double fallback)
{
    const auto target = chainTarget(track);
    if (!target.inserts || slot < 0 || std::size_t(slot) >= target.inserts->size()
        || port < 0 || port > std::numeric_limits<std::uint32_t>::max()) return fallback;
    const auto* control = (*target.inserts)[std::size_t(slot)].control(static_cast<std::uint32_t>(port));
    return control ? control->value : fallback;
}

void Backend::refreshPluginValues()
{
    bool changed = false;
    for (auto& entry : m_pluginPorts) {
        auto field = entry.toMap();
        const double value = pluginPortValue(m_pluginPortsSlot, field.value("index").toUInt(),
                                             m_pluginPortsTarget, field.value("defaultValue").toDouble());
        if (field.value("value").toDouble() == value) continue;
        field["value"] = value; entry = std::move(field); changed = true;
    }
    if (changed) { ++m_pluginValuesRevision; emit pluginValuesChanged(); }
}

QString Backend::pluginPortText(int slot, qint64 port, int track, double value)
{
    const auto target = chainTarget(track);
    if (!target.inserts || slot < 0 || std::size_t(slot) >= target.inserts->size()
        || port < 0 || port > std::numeric_limits<std::uint32_t>::max()) return {};
    const auto chain = m_chains.find(target.key);
    if (chain == m_chains.end() || !chain->second) return {};
    const auto text = chain->second->parameterText(std::size_t(slot), std::uint32_t(port), value);
    // Each reply is a complete UTF-8 string, not a streaming fragment. Keep the
    // numeric fallback for malformed/empty text; QML renders valid text literally.
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    const QString result = decoder(QByteArrayView(text.data(), qsizetype(text.size())));
    return decoder.hasError() || result.trimmed().isEmpty() ? QString{} : result;
}

double Backend::pluginOutputValue(int slot, qint64 port, int track)
{
    const ChainTarget target = chainTarget(track);
    const auto chain = m_chains.find(target.key);
    if (slot < 0 || port < 0 || port > std::numeric_limits<std::uint32_t>::max() || chain == m_chains.end() || chain->second == nullptr) return 0.0;
    return chain->second->outputValue(static_cast<std::size_t>(slot), static_cast<std::uint32_t>(port));
}

QString Backend::pluginKind(const QString &uri)
{
    if (!m_lv2->scanned()) m_lv2->scan();
    const auto *info = m_lv2->find(uri.toStdString());
    if (!info) return QStringLiteral("other");
    const auto category = pluginCategory(QString::fromStdString(info->className));
    return category == QLatin1String("dynamics") || category == QLatin1String("eq") ? category : QStringLiteral("other");
}

double Backend::livePluginControl(int slot, qint64 port, int track)
{
    const ChainTarget target = chainTarget(track);
    const auto chain = m_chains.find(target.key);
    if (slot < 0 || port < 0 || port > std::numeric_limits<std::uint32_t>::max() || chain == m_chains.end() || chain->second == nullptr) return 0.0;
    return chain->second->controlValue(static_cast<std::size_t>(slot), static_cast<std::uint32_t>(port));
}

void Backend::requestPluginPorts(int slot, int track)
{
    const ChainTarget target = chainTarget(track);
    m_pluginPorts.clear();
    m_pluginOutputs.clear();
    m_pluginPortsKind.clear();
    m_pluginPortsUri.clear();
    m_pluginPortsSlot = slot;
    m_pluginPortsTarget = target.target;
    m_pluginPortsComplete = false;

    if (target.inserts == nullptr || slot < 0
        || static_cast<std::size_t>(slot) >= target.inserts->size()) {
        emit pluginPortsChanged();
        return;
    }

    const session::Insert &insert = (*target.inserts)[static_cast<std::size_t>(slot)];
    if (!m_lv2->scanned())
        m_lv2->scan();

    std::string error;
    const auto* info = m_lv2->find(insert.uri);
    const auto channels = insert.instanceState.size() == 2 || (info && !info->fits(kOutputChannels)) ? 1u : kOutputChannels;
    const auto live = m_chains.find(target.key);
    const auto* identity = live != m_chains.end() && live->second ? live->second->insertIdentity(slot) : nullptr;
    // Rack removal can request the replacement slot's controls before rebuilding
    // the old chain. Never borrow metadata from the processor being removed.
    const auto* ports = identity && *identity == insert.identity ? live->second->controls(slot) : nullptr;
    std::unique_ptr<host::Instance> probe;
    if (!ports) {
        probe = host::createInstance(*m_lv2, insert.uri, hostFeatures(), error, insert.legacyClapControls,
            insert.instanceState.empty() ? nullptr : &insert.instanceState.front(), {channels});
        if (probe) ports = &probe->controls();
    }
    if (!ports) {
        setError(QString::fromStdString(error));
        emit pluginPortsChanged();
        return;
    }

    // Null for a CLAP insert: the LV2 presentation extras (units, choices, log
    // scale) then fall back to plain numeric ranges.
    const auto *plugin = m_lv2->lilvPlugin(insert.uri);

    m_pluginPortsUri = QString::fromStdString(insert.uri);
    m_pluginPortsKind = pluginKind(m_pluginPortsUri);

    // Only the knobs. Output controls are meters and readouts; drawing them as
    // controls would offer to set something the plugin owns. They are listed
    // apart, as meters, except the latency a plugin reports for delay compensation.
    for (const lv2::ControlPort &port : *ports) {
        if (port.output) {
            const QString symbol = QString::fromStdString(port.symbol);
            const QString name = QString::fromStdString(port.name);
            if (symbol.compare(QLatin1String("latency"), Qt::CaseInsensitive) == 0
                || name.contains(QLatin1String("latency"), Qt::CaseInsensitive))
                continue;
            QVariantMap meter = pluginControlPresentation(m_lv2->raw(), plugin,
                (plugin ? lilv_plugin_get_port_by_index(plugin, port.index) : nullptr), port.minimum, port.maximum);
            meter["index"] = port.index;
            meter["symbol"] = symbol;
            meter["name"] = name;
            meter["min"] = static_cast<double>(port.minimum);
            meter["max"] = static_cast<double>(port.maximum);
            meter["toggled"] = port.toggled;
            meter["output"] = true;
            m_pluginOutputs.append(meter);
            continue;
        }
        QVariantMap entry = pluginControlPresentation(m_lv2->raw(), plugin,
            (plugin ? lilv_plugin_get_port_by_index(plugin, port.index) : nullptr), port.minimum, port.maximum);
        entry["index"] = port.index;
        entry["symbol"] = QString::fromStdString(port.symbol);
        entry["name"] = QString::fromStdString(port.name);
        entry["min"] = static_cast<double>(port.minimum);
        entry["max"] = static_cast<double>(port.maximum);
        entry["defaultValue"] = static_cast<double>(port.defaultValue);
        entry["toggled"] = port.toggled;
        entry["integer"] = port.integer;
        // Reserve a stable readout width even when this formatter temporarily
        // refuses a value. A fallback must not move a slider under a pointer.
        entry["nativeText"] = info && (info->format == host::Format::Clap || info->format == host::Format::Vst3);

        // What the model holds, which is what a save would write.
        const session::InsertControl *stored = insert.control(port.index);
        entry["value"] = stored ? stored->value : probe ? probe->parameterValue(port.index)
            : live->second->controlValue(slot, port.index);
        m_pluginPorts.append(entry);
    }

    m_pluginPortsComplete = true;
    emit pluginPortsChanged();
}

// How precisely automation reaches a plugin (W10): "sample" for a format the
// host hands values with frame offsets (CLAP and VST3), "block" for one value per run
// with runs split at points (LV2 control ports), "" for a plugin not installed.
QString Backend::automationTiming(const lv2::PluginInfo *info)
{
    if (info == nullptr) return {};
    return info->format == host::Format::Clap || info->format == host::Format::Vst3
        ? QStringLiteral("sample") : QStringLiteral("block");
}

QVariantMap Backend::manual() const
{
    if (!m_manual.isEmpty()) return m_manual;
    const QString path = omaaudio::manual::findManual();
    QString error;
    const QString text = omaaudio::manual::readManual(path, &error);
    QVariantList sections;
    for (const auto &section : omaaudio::manual::sections(text)) {
        sections.append(QVariantMap{{QStringLiteral("title"), section.title},
                                    {QStringLiteral("level"), section.level},
                                    {QStringLiteral("text"), section.body}});
    }
    m_manual = QVariantMap{{QStringLiteral("available"), !sections.isEmpty()},
                           {QStringLiteral("path"), path},
                           {QStringLiteral("error"), error},
                           {QStringLiteral("sections"), sections}};
    return m_manual;
}

QString Backend::manualHtml(const QString &markdown, const QFont &font, const QColor &linkColor) const
{
    // Keep Qt's markdown parser (including tables and fenced code), then style
    // actual anchors. MarkdownText's linkColor and palette do not recolour them.
    QTextDocument document;
    document.setDefaultFont(font);
    document.setMarkdown(markdown);
    for (auto block = document.begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (!fragment.isValid() || !fragment.charFormat().isAnchor()) continue;
            QTextCursor cursor(&document);
            cursor.setPosition(fragment.position());
            cursor.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
            QTextCharFormat format;
            format.setForeground(linkColor);
            cursor.mergeCharFormat(format);
        }
    }
    return document.toHtml();
}

int Backend::trackCount() const { return static_cast<int>(m_project.tracks().size()); }

QVariantList Backend::trackIds() const
{
    QVariantList out;
    for (const session::Track &t : m_project.tracks())
        out.append(static_cast<int>(t.id));
    return out;
}

QVariantMap Backend::trackById(int id) const
{
    for (const QVariant &entry : m_tracks) {
        const QVariantMap map = entry.toMap();
        if (map.value("id").toInt() == id)
            return map;
    }
    return {};
}

int Backend::trackIndexById(int id) const
{
    for (int i = 0; i < m_tracks.size(); ++i) {
        if (m_tracks.at(i).toMap().value("id").toInt() == id)
            return i;
    }
    return -1;
}

// --- Phase 1: roles, colours, heights ---------------------------------------------

QStringList Backend::trackRoles() const
{
    QStringList out;
    for (const session::TrackRole r : {session::TrackRole::Dialogue, session::TrackRole::Host,
                                       session::TrackRole::Guest, session::TrackRole::Music,
                                       session::TrackRole::Ambience, session::TrackRole::Sfx,
                                       session::TrackRole::Foley, session::TrackRole::Aux,
                                       session::TrackRole::Bus, session::TrackRole::Master}) {
        const std::string_view name = session::roleName(r);
        out << QString::fromUtf8(name.data(), static_cast<int>(name.size()));
    }
    return out;
}

QStringList Backend::trackHeights() const
{
    QStringList out;
    for (const session::TrackHeight h : {session::TrackHeight::Minimized,
                                         session::TrackHeight::Compact,
                                         session::TrackHeight::Normal,
                                         session::TrackHeight::Expanded}) {
        const std::string_view name = session::heightName(h);
        out << QString::fromUtf8(name.data(), static_cast<int>(name.size()));
    }
    return out;
}

QStringList Backend::markerTypes() const
{
    QStringList out;
    for (const session::MarkerType t : {session::MarkerType::Cue, session::MarkerType::Range,
                                        session::MarkerType::Chapter,
                                        session::MarkerType::Warning}) {
        const std::string_view name = session::markerTypeName(t);
        out << QString::fromUtf8(name.data(), static_cast<int>(name.size()));
    }
    return out;
}

void Backend::setTrackRole(int track, const QString &role)
{
    if (deferAutomationEdit([this, track, role] { setTrackRole(track, role); return true; })) return;
    session::Track *t = m_project.track(trackArg(track));
    const std::optional<session::TrackRole> parsed =
        session::roleFromName(role.toStdString());
    if (t == nullptr || !parsed.has_value()) {
        if (!parsed.has_value())
            setError(QStringLiteral("no such track role: ") + role);
        return;
    }
    const session::SessionState before = captureEditState();
    t->role = *parsed;
    commit("set role", t->id, before);
}

void Backend::setTrackColor(int track, const QString &color)
{
    if (deferAutomationEdit([this, track, color] { setTrackColor(track, color); return true; })) return;
    session::Track *t = m_project.track(trackArg(track));
    if (t == nullptr)
        return;
    // "#RRGGBB" or nothing. Anything else is a typo, not a colour.
    const QString trimmed = color.trimmed();
    if (!trimmed.isEmpty()) {
        const bool wellFormed = trimmed.size() == 7 && trimmed.at(0) == QLatin1Char('#')
            && std::all_of(trimmed.begin() + 1, trimmed.end(),
                           [](QChar ch) { return ch.isDigit()
                                              || (ch.toLower() >= QLatin1Char('a')
                                                  && ch.toLower() <= QLatin1Char('f')); });
        if (!wellFormed) {
            setError(QStringLiteral("a track colour is #RRGGBB, not ") + trimmed);
            return;
        }
    }
    const session::SessionState before = captureEditState();
    t->colour = trimmed.toUpper().toStdString();
    commit("set colour", t->id, before);
}

void Backend::setTrackHeight(int track, const QString &height)
{
    session::Track *t = m_project.track(trackArg(track));
    const std::optional<session::TrackHeight> parsed =
        session::heightFromName(height.toStdString());
    if (t == nullptr || !parsed.has_value())
        return;
    if (t->height == *parsed)
        return;
    t->height = *parsed;
    // A view preference, saved but not undoable -- see restoreState.
    markDirty();
    rebuildTracks();
}

void Backend::stepTrackHeight(int track, int delta)
{
    session::Track *t = m_project.track(trackArg(track));
    if (t == nullptr)
        return;
    const int current = static_cast<int>(t->height);
    const int next = std::clamp(current + delta, 0,
                                static_cast<int>(session::TrackHeight::Expanded));
    const std::string_view name =
        session::heightName(static_cast<session::TrackHeight>(next));
    setTrackHeight(static_cast<int>(t->id),
                   QString::fromUtf8(name.data(), static_cast<int>(name.size())));
}

void Backend::duplicateTrack(int track)
{
    if (deferAutomationEdit([this, track] { duplicateTrack(track); return true; })) return;
    const session::TrackId id = trackArg(track);
    if (deferPluginEdit([this, id] { duplicateTrack(static_cast<int>(id)); return true; })) return;
    const session::Track *original = m_project.track(id);
    if (original == nullptr)
        return;
    if (m_project.tracks().size() >= engine::Transport::kMaxTracks) {
        setError(QStringLiteral("this session already has as many tracks as OmaAudio plays (%1); remove one to add another").arg(engine::Transport::kMaxTracks));
        return;
    }
    const session::SessionState before = captureEditState();

    // A fresh id from the counter, then everything else copied across. Placed
    // directly below the original, which is where a duplicate is looked for.
    // `original` is dead after addTrack -- the track vector may have moved -- so
    // everything is taken from the copy from here on.
    session::Track copy = *original;
    original = nullptr;
    std::size_t copiedPoints = copy.volumeAutomation.points.size() + copy.panAutomation.points.size();
    for (const auto& insert : copy.inserts) for (const auto& c : insert.controls) copiedPoints += c.automationPointCount();
    std::set<session::EffectOwner> copiedRacks;
    for (const auto& clip : copy.clips) {
        copiedPoints += clip.volumeEnvelope.points.size() + clip.panEnvelope.points.size();
        if (clip.rack && copiedRacks.insert(clip.rack->owner).second)
            for (const auto& insert : clip.rack->inserts)
                for (const auto& control : insert.controls) copiedPoints += control.automationPointCount();
    }
    if (m_project.automationPointCount() + copiedPoints > session::kMaxProjectAutomationPoints) {
        setError(QStringLiteral("Cannot duplicate track: project automation point limit reached")); return;
    }
    for (auto& insert : copy.inserts) insert.forkIdentity();
    try {
        session::edits::ClipRackForkMap forks;
        session::edits::forkClipCopies(copy.clips, forks);
    } catch (const std::exception& error) {
        setError(tr("Cannot duplicate track: %1").arg(QString::fromUtf8(error.what())));
        return;
    }
    std::map<session::ClipGroupId, session::ClipGroupId> groups;
    for (auto& clip : copy.clips) {
        if (clip.group == session::kInvalidClipGroup) continue;
        if (!groups.contains(clip.group)) {
            const auto* originalGroup = m_project.clipGroup(clip.group);
            const auto fresh = originalGroup ? m_project.addClipGroup(originalGroup->name, originalGroup->colour)
                                             : session::kInvalidClipGroup;
            if (fresh == session::kInvalidClipGroup) {
                session::restoreState(m_project, before);
                setError(tr("Cannot duplicate track: its clip groups could not be copied.")); return;
            }
            groups.emplace(clip.group, fresh);
        }
        clip.group = groups.at(clip.group);
    }
    copy.name += " copy";
    const session::TrackId fresh = m_project.addTrack(copy.name);
    copy.id = fresh;
    m_project.removeTrack(fresh);
    const std::optional<std::size_t> index = m_project.trackIndex(id);
    m_project.insertTrack(index.has_value() ? *index + 1 : m_project.tracks().size(),
                          std::move(copy));

    m_focused = fresh;
    rebuildChain(fresh);
    commit("duplicate track", fresh, before);
}

// --- markers -----------------------------------------------------------------------

void Backend::rebuildMarkers()
{
    QVariantList list;
    for (const session::Marker &m : m_project.markers()) {
        QVariantMap entry;
        entry["id"] = static_cast<int>(m.id);
        entry["name"] = QString::fromStdString(m.name);
        entry["position"] = static_cast<qint64>(m.position);
        entry["length"] = static_cast<qint64>(m.length);
        entry["end"] = static_cast<qint64>(m.end());
        const std::string_view type = session::markerTypeName(m.type);
        entry["type"] = QString::fromUtf8(type.data(), static_cast<int>(type.size()));
        entry["color"] = QString::fromStdString(m.colour);
        entry["notes"] = QString::fromStdString(m.notes);
        list.append(entry);
    }
    m_markers = list;
    emit markersChanged();
}

session::Marker *Backend::markerArg(int id)
{
    if (id < 0)
        return nullptr;
    return m_project.marker(static_cast<session::MarkerId>(static_cast<std::uint32_t>(id)));
}

int Backend::addMarker(qint64 frame, const QString &name, const QString &type)
{
    const std::optional<session::MarkerType> parsed =
        session::markerTypeFromName(type.toStdString());
    if (!parsed.has_value()) {
        setError(QStringLiteral("no such marker type: ") + type);
        return -1;
    }
    session::Marker m;
    m.position = std::max<qint64>(0, frame);
    m.type = *parsed;
    m.name = name.isEmpty()
                 ? ("Marker " + std::to_string(m_project.markers().size() + 1))
                 : name.toStdString();
    const auto id = m.id = m_project.reserveMarkerId();
    return addPreparedMarker(std::move(m), "add marker") ? static_cast<int>(id) : -1;
}

int Backend::addRangeMarker(qint64 start, qint64 end, const QString &name)
{
    if (end < start)
        std::swap(start, end);
    if (end <= start) {
        setError(QStringLiteral("a range marker needs a range"));
        return -1;
    }
    session::Marker m;
    m.position = std::max<qint64>(0, start);
    m.length = end - m.position;
    m.type = session::MarkerType::Range;
    m.name = name.isEmpty()
                 ? ("Range " + std::to_string(m_project.markers().size() + 1))
                 : name.toStdString();
    const auto id = m.id = m_project.reserveMarkerId();
    return addPreparedMarker(std::move(m), "add range") ? static_cast<int>(id) : -1;
}

bool Backend::addPreparedMarker(session::Marker marker, const std::string& label)
{
    if (const auto deferred = deferAutomationEdit([this, marker, label] { return addPreparedMarker(marker, label); })) return *deferred;
    if (m_project.marker(marker.id)) { setError(tr("The marker identity is already in use.")); return false; }
    const auto before = captureEditState();
    m_project.insertMarker(std::move(marker));
    commit(label, session::kInvalidTrack, before); return true;
}

void Backend::removeMarker(int id)
{
    if (deferAutomationEdit([this, id] { removeMarker(id); return true; })) return;
    const session::Marker *m = markerArg(id);
    if (m == nullptr)
        return;
    const session::SessionState before = captureEditState();
    m_project.removeMarker(m->id);
    commit("remove marker", session::kInvalidTrack, before);
}

void Backend::renameMarker(int id, const QString &name)
{
    if (deferAutomationEdit([this, id, name] { renameMarker(id, name); return true; })) return;
    session::Marker *m = markerArg(id);
    if (m == nullptr || name.trimmed().isEmpty())
        return;
    const session::SessionState before = captureEditState();
    m->name = name.trimmed().toStdString();
    commit("rename marker", session::kInvalidTrack, before);
}

void Backend::moveMarker(int id, qint64 position, qint64 length)
{
    if (deferAutomationEdit([this, id, position, length] { moveMarker(id, position, length); return true; })) return;
    session::Marker *m = markerArg(id);
    if (m == nullptr)
        return;
    const session::SessionState before = captureEditState();
    m->position = std::max<qint64>(0, position);
    if (length >= 0)
        m->length = m->type == session::MarkerType::Range ? length : 0;
    m_project.sortMarkers();
    commit("move marker", session::kInvalidTrack, before);
}

void Backend::setMarkerType(int id, const QString &type)
{
    if (deferAutomationEdit([this, id, type] { setMarkerType(id, type); return true; })) return;
    session::Marker *m = markerArg(id);
    const std::optional<session::MarkerType> parsed =
        session::markerTypeFromName(type.toStdString());
    if (m == nullptr || !parsed.has_value())
        return;
    const session::SessionState before = captureEditState();
    m->type = *parsed;
    if (m->type != session::MarkerType::Range)
        m->length = 0;
    commit("set marker type", session::kInvalidTrack, before);
}

void Backend::setMarkerColor(int id, const QString &color)
{
    if (deferAutomationEdit([this, id, color] { setMarkerColor(id, color); return true; })) return;
    session::Marker *m = markerArg(id);
    if (m == nullptr)
        return;
    const session::SessionState before = captureEditState();
    m->colour = color.trimmed().toUpper().toStdString();
    commit("set marker colour", session::kInvalidTrack, before);
}

void Backend::setMarkerNotes(int id, const QString &notes)
{
    if (deferAutomationEdit([this, id, notes] { setMarkerNotes(id, notes); return true; })) return;
    session::Marker *m = markerArg(id);
    if (m == nullptr)
        return;
    const session::SessionState before = captureEditState();
    m->notes = notes.toStdString();
    commit("edit marker notes", session::kInvalidTrack, before);
}

QVariantMap Backend::markerById(int id) const
{
    for (const QVariant &entry : m_markers) {
        const QVariantMap map = entry.toMap();
        if (map.value("id").toInt() == id)
            return map;
    }
    return {};
}

qint64 Backend::nextMarkerFrame(qint64 from) const
{
    qint64 best = -1;
    for (const session::Marker &m : m_project.markers()) {
        if (m.position > from && (best < 0 || m.position < best))
            best = m.position;
    }
    return best;
}

qint64 Backend::prevMarkerFrame(qint64 from) const
{
    qint64 best = -1;
    for (const session::Marker &m : m_project.markers()) {
        if (m.position < from && m.position > best)
            best = m.position;
    }
    return best;
}

void Backend::jumpToMarker(int direction)
{
    // A small slack backwards, so pressing "previous" while sat on a marker goes to
    // the one before it rather than staying put.
    const qint64 target = direction < 0
                              ? prevMarkerFrame(m_playhead - 1)
                              : nextMarkerFrame(m_playhead);
    if (target >= 0)
        seekFrames(target);
}

// --- snapping, ripple, history -----------------------------------------------------

void Backend::setSnapping(bool on)
{
    if (m_snapping == on)
        return;
    m_snapping = on;
    emit snappingChanged();
}

void Backend::setSnapToleranceFrames(qint64 frames)
{
    const qint64 clamped = std::max<qint64>(0, frames);
    if (m_snapTolerance == clamped)
        return;
    m_snapTolerance = clamped;
    emit snappingChanged();
}

QStringList Backend::snapGrids() const
{
    return {"none", "bar", "beat", "half", "quarter", "eighth", "triplet"};
}

QString Backend::snapGridLabel(const QString& key) const
{
    if (key == "bar") return tr("Bar");
    if (key == "beat") return tr("Beat");
    if (key == "half") return tr("½ beat");
    if (key == "quarter") return tr("¼ beat");
    if (key == "eighth") return tr("⅛ beat");
    if (key == "triplet") return tr("⅓ beat (triplet)");
    return tr("No musical grid");
}

void Backend::setSnapGrid(const QString& key)
{
    if (!snapGrids().contains(key)) { setError(tr("Choose an available snap grid.")); return; }
    if (m_snapGrid == key) return;
    m_snapGrid = key;
    emit snappingChanged();
}

qint64 Backend::snapGridTicks() const
{
    const auto& music = m_project.musicalTime();
    if (!music.enabled || !music.valid()) return 0;
    const auto beat = music.ticksPerBeat();
    if (m_snapGrid == "bar") return qint64(beat) * music.beats;
    if (m_snapGrid == "beat") return beat;
    if (m_snapGrid == "half") return beat / 2;
    if (m_snapGrid == "quarter") return beat / 4;
    if (m_snapGrid == "eighth") return beat / 8;
    if (m_snapGrid == "triplet") return beat / 3;
    return 0;
}

QString Backend::snapHint() const
{
    if (!m_snapping) return tr("Snapping is off. Editing gestures use their exact positions.");
    if (m_snapGrid == "none") return tr("Align selection edges and moved clips with nearby clip edges, markers or the playhead. Selections can also use nearby zero crossings.");
    if (!snapGridTicks()) return tr("Enable musical timing in Project properties to use the chosen snap grid.");
    return tr("Grid: %1. Nearby clip edges, markers and the playhead take priority; otherwise selection edges and moved clips use the nearest grid point.")
        .arg(snapGridLabel(m_snapGrid));
}

std::optional<qint64> Backend::nearestMusicalGridFrame(qint64 frame) const
{
    const auto step = snapGridTicks();
    if (!m_snapping || !step || frame < 0) return {};
    const auto& music = m_project.musicalTime();
    const auto tick = music.tickAt(frame, m_project.sampleRate());
    if (tick < 0) return {};
    const auto beforeTick = tick - tick % step;
    const auto before = music.sampleAt(beforeTick, m_project.sampleRate());
    if (before < 0 || before > frame) return {};
    if (beforeTick <= std::numeric_limits<qint64>::max() - step) {
        const auto after = music.sampleAt(beforeTick + step, m_project.sampleRate());
        if (after >= frame && after - frame < frame - before) return after;
    }
    return before; // deterministic earlier point for an exact half-grid tie
}

qint64 Backend::snapFrame(qint64 frame) const
{
    if (!m_snapping)
        return frame;
    frame = std::max<qint64>(0, frame);

    // The hard targets first: a clip edge on the focused track, a marker, the
    // playhead. Nearest wins, within the pixel-derived tolerance.
    qint64 best = -1;
    qint64 bestDistance = std::numeric_limits<qint64>::max();
    const auto consider = [&](qint64 candidate) {
        const qint64 distance = std::llabs(candidate - frame);
        if (distance <= m_snapTolerance && (best < 0 || distance < bestDistance)) {
            best = candidate;
            bestDistance = distance;
        }
    };
    if (m_snapTolerance > 0) {
        if (const session::Track *track = m_project.track(m_focused)) {
            for (const session::Clip &c : track->clips) {
                consider(c.timelineStart);
                consider(c.timelineEnd());
            }
        }
        for (const session::Marker &m : m_project.markers()) {
            consider(m.position);
            if (m.length > 0)
                consider(m.end());
        }
        consider(m_playhead);
    }
    if (best >= 0)
        return best;
    if (const auto grid = nearestMusicalGridFrame(frame)) return *grid;

    // Otherwise the nearest zero crossing, as it always did.
    return snapToZeroCrossing(frame);
}

void Backend::setRipple(bool on)
{
    if (m_ripple == on)
        return;
    m_ripple = on;
    emit rippleChanged();
}

QVariantList Backend::history() const
{
    QVariantList out;
    const auto &commands = m_undo.commands();
    for (std::size_t i = 0; i < commands.size(); ++i) {
        QVariantMap entry;
        entry["index"] = static_cast<int>(i + 1);       // "after this many applied"
        entry["label"] = QString::fromStdString(commands[i].label);
        entry["applied"] = i < m_undo.cursor();
        out.append(entry);
    }
    return out;
}

void Backend::jumpHistory(int cursor)
{
    const int target = std::clamp(cursor, 0, static_cast<int>(m_undo.depth()));
    const auto serial = target ? m_undo.commands()[std::size_t(target - 1)].serial : 0;
    const auto oldest = m_undo.commands().empty() ? 0 : m_undo.commands().front().serial;
    const auto apply = [this, serial, oldest] {
        if (!serial) {
            if (!m_undo.commands().empty() && m_undo.commands().front().serial != oldest) {
                setError(tr("That history position is no longer available.")); return false;
            }
            jumpHistory(0); return true;
        }
        const auto& commands = m_undo.commands();
        const auto found = std::find_if(commands.begin(), commands.end(),
            [serial](const auto& command) { return command.serial == serial; });
        if (found == commands.end()) { setError(tr("That history position is no longer available.")); return false; }
        jumpHistory(static_cast<int>(found - commands.begin()) + 1); return true;
    };
    // Resolve the accepted history identity after closing the active pass. Its
    // new entry must exist before the jump starts restoring older snapshots.
    if (deferAutomationEdit(apply)) return;
    if (deferPluginEdit(apply, {}, true)) return;
    // A jump can start without effects and restore one on an intermediate step.
    // The initial boundary already owns the whole history action: do not queue
    // that step's successor and spin waiting for a GUI timer inside this loop.
    const bool applying = m_applyingPluginEdit;
    m_applyingPluginEdit = true;
    const auto finish = qScopeGuard([this, applying] { m_applyingPluginEdit = applying; });
    // Each step is a whole snapshot restore; a long jump is a few of them, which is
    // still nothing next to the audio it leaves untouched.
    while (static_cast<int>(m_undo.cursor()) > target && m_undo.canUndo())
        undo();
    while (static_cast<int>(m_undo.cursor()) < target && m_undo.canRedo())
        redo();
}

// --- the keymap ------------------------------------------------------------------

QString Backend::defaultKeymap() const
{
    QFile file(QStringLiteral(":/keymap/default.toml"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

QString Backend::userKeymap() const
{
    QFile file(userKeymapPath());
    // Having no user keymap is the normal case, not an error worth reporting.
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

QString Backend::userKeymapPath()
{
    return omaaudio::storage::locations().keymapFile;
}

QString Backend::settingsFilePath() const { return omaaudio::storage::locations().settingsFile; }
QString Backend::stateDirectoryPath() const { return omaaudio::storage::locations().stateDir; }
QString Backend::cacheDirectoryPath() const { return omaaudio::storage::locations().cacheDir; }
QString Backend::recordingsDirectoryPath() const { return recordingsDirectory(); }
QString Backend::storageMigrationMessage() const { return omaaudio::storage::launchMigration().summary(); }

// --- the master bus (Phase 3) ----------------------------------------------------

double Backend::masterTrimDb() const
{
    return dbFromLinear(m_project.masterTrim());
}

void Backend::setMasterTrimDb(double db)
{
    if (writeMatchingAutomation(-2, "volume", linearFromDb(db) / std::max(1e-12f, m_project.masterTrim()))) return;
    if (deferAutomationEdit([this, db] { setMasterTrimDb(db); return true; })) return;
    finishAutomationRecording();
    const double clamped = std::clamp(db, -24.0, 24.0);
    m_project.setMasterTrim(linearFromDb(clamped));
    enqueueAudioCommand(engine::Command::setMasterTrim(m_project.masterTrim()));
    // Dirty but not undoable, like a fader.
    markDirty();
    emit masterChanged();
}

void Backend::rebuildMaster()
{
    QVariantList inserts;
    const std::vector<session::Insert> &chain = m_project.masterInserts();
    for (std::size_t j = 0; j < chain.size(); ++j) {
        const session::Insert &insert = chain[j];
        const lv2::PluginInfo *info = m_lv2->find(insert.uri);
        QVariantMap slot;
        slot["slot"] = static_cast<int>(j);
        slot["uri"] = QString::fromStdString(insert.uri);
        slot["name"] = QString::fromStdString(info != nullptr ? info->name : insert.uri);
        slot["bypass"] = insert.bypass;
        slot["available"] = info != nullptr && info->usable;
        slot["timing"] = automationTiming(info);
        inserts.append(slot);
    }
    const auto it = m_chains.find(kMasterChainKey);
    const qint64 latency = it != m_chains.end() && it->second != nullptr
                               ? static_cast<qint64>(it->second->latencyFrames()) : 0;
    if (inserts == m_masterInserts && latency == m_masterLatency)
        return;
    m_masterInserts = inserts;
    m_masterLatency = latency;
    emit masterChanged();
}

// --- the live meter --------------------------------------------------------------

void Backend::resetLoudness()
{
    enqueueAudioCommand(engine::Command::resetLoudness());
}

QVariantMap Backend::reportToMap(const engine::LoudnessReport &report)
{
    QVariantMap m;
    m["integrated"] = static_cast<double>(report.integrated);
    m["range"] = static_cast<double>(report.range);
    m["truePeak"] = static_cast<double>(report.truePeak);
    m["momentaryMax"] = static_cast<double>(report.momentaryMax);
    m["shortTermMax"] = static_cast<double>(report.shortTermMax);
    m["frames"] = static_cast<qint64>(report.frames);
    m["silence"] = static_cast<double>(engine::kLoudnessSilence);
    return m;
}

void Backend::pollLoudness()
{
    const engine::LoudnessMeter &meter = m_transport.loudness();
    QVariantMap m = reportToMap(meter.report());
    m["momentary"] = static_cast<double>(meter.momentary());
    m["shortTerm"] = static_cast<double>(meter.shortTerm());
    m["hops"] = static_cast<qint64>(meter.hops());
    if (m == m_loudness)
        return;
    m_loudness = std::move(m);
    emit loudnessChanged();
}

// --- the analysis ----------------------------------------------------------------

void Backend::setPreviewBefore(bool on)
{
    consumePluginNotifications();
    refreshAnalysisValidity();
    if (m_previewBefore == on)
        return;
    m_previewBefore = on;
    pushPreview();
    emit previewChanged();
}

void Backend::setHearWithoutEffects(bool on)
{
    if (m_hearWithoutEffects == on) return;
    m_hearWithoutEffects = on;
    publishPlan(false, false);
    emit previewChanged();
}

void Backend::pushPreview()
{
    const float makeup = linearFromDb(m_previewBefore && m_previewMatched ? m_previewMatchDb : 0.0);
    enqueueAudioCommand(engine::Command::setMasterBypass(m_previewBefore, makeup));
}

void Backend::invalidateAnalysis(bool readout)
{
    if (!readout) ++m_analysisEpoch;
    if (m_duckingReady) {
        m_duckingStale = true;
        emit duckingChanged();
    }
    if (!m_trackLoudnessStale) {
        m_trackLoudnessStale = true;
        emit trackLoudnessChanged();
    }
    if (m_previewMatched) {
        m_previewMatched = false;
        m_previewMatchDb = 0;
        if (!m_masterReport.isEmpty()) m_masterReport["matchDb"] = 0.0;
        if (m_previewBefore)
            pushPreview();
        emit previewChanged();
    }
    if (m_analysisStale)
        return;
    m_analysisStale = true;
    emit analysisChanged();
}

bool Backend::analysisCurrent(std::uint64_t generation, std::uint64_t revision, std::uint64_t epoch) const
{
    return !m_shuttingDown && generation == m_sessionGeneration && epoch == m_analysisEpoch
        && m_confirmedEditRevision <= revision && !hasUnverifiedPluginState()
        && std::none_of(m_chains.begin(), m_chains.end(), [](const auto& entry) {
            return entry.second && entry.second->parameterFlushRequested();
        });
}

void Backend::refreshAnalysisValidity()
{
    const bool stale = !m_analysis.ok
        || !analysisCurrent(m_analysis.generation, m_analysis.revision, m_analysis.analysisEpoch);
    const bool analysisChangedNow = std::exchange(m_analysisStale, stale) != stale;
    const double mastered = m_analysis.report.integrated, raw = m_analysis.rawReport.integrated;
    const bool matched = !stale && mastered > -100.0 && raw > -100.0;
    const double matchDb = matched ? mastered - raw : 0.0;
    if (matched != m_previewMatched || matchDb != m_previewMatchDb) {
        m_previewMatched = matched; m_previewMatchDb = matchDb;
        if (!m_masterReport.isEmpty()) m_masterReport["matchDb"] = matchDb;
        if (m_previewBefore) pushPreview();
        emit previewChanged();
    }
    if (analysisChangedNow) emit analysisChanged();
    const bool duckStale = !m_duckingReady
        || !analysisCurrent(m_ducking.generation, m_ducking.revision, m_ducking.analysisEpoch);
    if (std::exchange(m_duckingStale, duckStale) != duckStale) emit duckingChanged();
    const bool trackStale = !m_trackLoudness.measurement.ok
        || !analysisCurrent(m_trackLoudness.generation, m_trackLoudness.revision, m_trackLoudness.analysisEpoch);
    if (std::exchange(m_trackLoudnessStale, trackStale) != trackStale) {
        if (!m_measuringTrackLoudness && m_trackLoudness.measurement.ok)
            m_trackLoudnessMessage = trackStale ? tr("The mix changed. Measure again.") : tr("Track output measured.");
        emit trackLoudnessChanged();
    }
}

void Backend::queueAnalysisValidityRefresh()
{
    if (m_analysisValidityQueued) return;
    m_analysisValidityQueued = true;
    // Captures also run under exclusive audio ownership. Restore preview only
    // after that scope has released, and account for any later callback output.
    QTimer::singleShot(0, this, [this] {
        m_analysisValidityQueued = false;
        consumePluginNotifications();
        refreshAnalysisValidity();
    });
}

// Instantiate offline chains here: catalogue access and plugin creation/activation
// stay on the GUI thread. Each private chain warms on its rendering worker before
// processing any real audio; neither operation reads the catalogue or live chains.
std::unique_ptr<lv2::Chain> Backend::buildOfflineChain(const std::vector<session::Insert> &inserts,
                                                    lv2::HostFeatures& features, QString& error)
{
    error.clear();
    if (inserts.empty())
        return nullptr;
    auto chain = std::make_unique<lv2::Chain>(kOutputChannels, lv2::kMaxBlock, host::ProcessingMode::Offline);
    for (std::size_t slot = 0; slot < inserts.size(); ++slot) {
        const auto& insert = inserts[slot];
        // Retain automation/sidechain indices without loading an effect the user
        // deliberately bypassed. An unavailable active effect is never bypassed.
        if (insert.bypass) { chain->appendMissing(insert.identity); continue; }
        if (!m_lv2->scanned()) m_lv2->scan();
        std::string reason;
        if (!chain->append(*m_lv2, features, insert, reason)) {
            error = tr("Insert %1 (%2): %3").arg(slot + 1)
                .arg(QString::fromStdString(insert.uri), QString::fromStdString(reason));
            return nullptr;
        }
    }
    return chain;
}

void Backend::startMasterJob(bool exportIt, int jobId)
{
    joinWorker();
    m_busy.store(true, std::memory_order_release);
    m_cancelJob.store(false, std::memory_order_relaxed);
    m_jobProgress.store(0.0f, std::memory_order_relaxed);

    const auto generation = m_sessionGeneration;
    requestTimelineSnapshot(
        [this, exportIt, generation] {
            return m_cancelJob.load(std::memory_order_relaxed)
                || (!exportIt && (m_shuttingDown || generation != m_sessionGeneration));
        },
        [this, exportIt, jobId](DeliverySnapshot snapshot, QString error, bool cancelled) {
            startCapturedMasterJob(std::move(snapshot), exportIt, jobId, std::move(error), cancelled);
        });
}

void Backend::startCapturedMasterJob(DeliverySnapshot captured, bool exportIt, int jobId, QString error, bool cancelled)
{
    const auto failed = [&](const QString& message) {
        // Use the normal completion queue even before a worker exists.
        // drainWorker also resumes queued saves/renders/display work.
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (exportIt) {
                RenderOutcome out; out.id = jobId;
                out.result.cancelled = cancelled; out.result.error = message.toStdString();
                m_pendingRender.push_back(std::move(out));
                m_finishedJobs.push_back(Job::Render);
            } else {
                Analysis out; out.generation = captured.generation; out.revision = captured.revision;
                out.analysisEpoch = captured.analysisEpoch; out.error = message.toStdString();
                m_pendingAnalysis.push_back(std::move(out));
                m_finishedJobs.push_back(Job::Analyse);
            }
        }
        m_busy.store(false, std::memory_order_release);
        emit workerFinished();
    };
    if (cancelled || !error.isEmpty()) { failed(error); return; }

    // Copied for the worker, as a save is: an edit while it runs must not change what
    // is rendered. Shared pointers would be lighter and would also be a data race.
    session::Project mixed = std::move(captured.project);
    if (exportIt) {
        const auto job = std::find_if(m_renders.begin(), m_renders.end(), [&](const RenderJob& j) { return j.id == jobId; });
        if (job != m_renders.end() && job->stemTrack != session::kInvalidTrack) {
            std::string reason;
            auto isolated = render::isolateTrackOutput(mixed, job->stemTrack, reason);
            if (!isolated) { failed(tr("Cannot render this stem: %1").arg(QString::fromStdString(reason))); return; }
            if (job->stemChosen)
                for (auto& t : isolated->tracks()) if (t.id == job->stemTrack) t.muted = false;
            // A stem is the track's output as it reaches the master, before the master bus.
            isolated->masterInserts().clear();
            isolated->setMasterTrim(1.0f);
            isolated->masterVolumeAutomation() = {};
            mixed = std::move(*isolated);
        }
    }
    const session::Project snapshot = std::move(mixed);
    auto features = std::make_shared<lv2::HostFeatures>(m_urids, snapshot.sampleRate(), lv2::kMaxBlock);
    std::shared_ptr<lv2::Chain> chain = buildOfflineChain(snapshot.masterInserts(), *features, error);
    if (!error.isEmpty()) { failed(tr("Cannot prepare an active effect on the master: %1").arg(error)); return; }
    // Every track's inserts too: the transport runs them live, so the render
    // must, or an export would not be what was heard.
    std::vector<std::shared_ptr<lv2::Chain>> trackChains;
    std::vector<render::TrackChain> chainList;
    for (const session::Track &t : snapshot.tracks()) {
        if (t.inserts.empty())
            continue;
        std::shared_ptr<lv2::Chain> built = buildOfflineChain(t.inserts, *features, error);
        if (!error.isEmpty()) {
            failed(tr("Cannot prepare an active effect on track %1: %2").arg(QString::fromStdString(t.name), error));
            return;
        }
        if (built == nullptr)
            continue;
        chainList.push_back(render::TrackChain{.track = t.id, .chain = built.get()});
        trackChains.push_back(std::move(built));
    }
    // Publish ownership only after every active insert was prepared. Refused
    // candidates unwind here, on the creation thread, before any file is opened.
    m_offlineFeatures = std::move(features);
    m_offlineChains = trackChains;
    if (chain) m_offlineChains.push_back(chain);
    const std::uint32_t rate = snapshot.sampleRate();
    const std::uint32_t sequence = ++m_masterPeakSequence;
    const std::uint64_t generation = captured.generation, revision = captured.revision, epoch = captured.analysisEpoch;
    render::ExportSpec spec;
    session::FramePos rangeStart = -1, rangeEnd = -1;
    if (exportIt) {
        if (const RenderJob *job = renderJob(jobId)) {
            spec = job->spec;
            rangeStart = job->rangeStart;
            rangeEnd = job->rangeEnd;
        }
    }

    m_worker = std::thread([this, snapshot, chain, trackChains, chainList, rate, sequence, exportIt,
                            jobId, spec, generation, revision, epoch, rangeStart, rangeEnd] {
        // These instances belong only to this offline job. Warm all 100 blocks
        // away from the interface and honour Cancel before real rendering starts.
        bool warmed = !chain || chain->warmUp(&m_cancelJob);
        for (const auto& trackChain : trackChains) {
            if (!warmed) break;
            warmed = trackChain->warmUp(&m_cancelJob);
        }
        const auto progress = [this, exportIt](float f) {
            m_jobProgress.store(exportIt ? f * 0.5f : f, std::memory_order_relaxed);
        };
        // The bus input once -- every track through its inserts, gain and pan --
        // then the bus over it; the plain mix is what "before" is measured on.
        render::MixRender mixed;
        if (warmed) {
            mixed = render::mixTracks(snapshot, kOutputChannels, chainList,
                                      &m_cancelJob, [&](float f) { progress(0.2f * f); });
        } else {
            mixed.cancelled = m_cancelJob.load(std::memory_order_relaxed);
            mixed.error = mixed.cancelled ? "Effect warm-up cancelled." : "Effect warm-up failed.";
        }
        render::MasterRender mastered;
        if (!mixed.ok) {
            mastered.cancelled = mixed.cancelled;
            mastered.error = mixed.error;
        } else {
            mastered = render::renderBus(mixed.audio, snapshot.masterTrim(), chain.get(),
                                         &m_cancelJob, progress, snapshot.masterVolumeAutomation(), snapshot.masterInserts(), snapshot.musicalTime());
        }

        if (exportIt) mixed.audio = {};
        if (!exportIt) {
            Analysis out;
            out.generation = generation;
            out.revision = revision;
            out.analysisEpoch = epoch;
            if (!mastered.ok) {
                out.error = mastered.error;
            } else if (!m_displayCache.isValid()) {
                out.error = "Could not create a private graphics cache: " + m_displayCache.errorString().toStdString();
            } else {
                out.frames = static_cast<qint64>(mastered.audio.frames());
                out.report = engine::analyseLoudness(mastered.audio, rate, &out.history);
                // The plain mix too, so "before" can be heard at the same loudness.
                std::vector<engine::LoudnessPoint> unused;
                out.rawReport = engine::analyseLoudness(mixed.audio, rate, &unused);
                out.hop = std::max<std::uint32_t>(1, rate / 10);

                // The mixdown's waveform, as textures, the way a track's are.
                io::PeakLevel base = io::buildPeaks(mastered.audio, kSamplesPerPeak);
                if (!base.empty()) {
                    out.peaks.pyramid = io::pyramidFromLevel(std::move(base));
                    out.peaks.ok = writePeakTextures(out.peaks, QStringLiteral("master-%1").arg(sequence));
                    if (!out.peaks.ok) out.error = out.peaks.error.toStdString();
                }
                out.ok = out.error.empty();
            }
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                m_pendingAnalysis.push_back(std::move(out));
                m_finishedJobs.push_back(Job::Analyse);
            }
        } else {
            RenderOutcome out;
            out.id = jobId;
            if (!mastered.ok) {
                out.result.ok = false;
                out.result.cancelled = mastered.cancelled;
                out.result.error = mastered.error;
            } else {
                if (rangeStart >= 0 && rangeEnd > rangeStart) {
                    // Exactly the range's frames; past the project's end is silence.
                    const auto frames = static_cast<std::size_t>(rangeEnd - rangeStart);
                    session::AudioBuffer cut(mastered.audio.channels(), frames);
                    const auto from = static_cast<std::size_t>(rangeStart);
                    for (std::uint32_t c = 0; c < cut.channels(); ++c) {
                        const auto src = std::as_const(mastered.audio).plane(c);
                        const auto dst = cut.plane(c);
                        for (std::size_t i = 0; i < frames && from + i < src.size(); ++i) dst[i] = src[from + i];
                    }
                    mastered.audio = std::move(cut);
                }
                out.result = render::exportMix(
                    mastered.audio, rate, spec, &m_cancelJob, [this](float f) {
                        m_jobProgress.store(0.5f + f * 0.5f, std::memory_order_relaxed);
                    });
            }
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                m_pendingRender.push_back(std::move(out));
                m_finishedJobs.push_back(Job::Render);
            }
        }
        m_busy.store(false, std::memory_order_release);
        emit workerFinished();
    });
}

void Backend::analyseMaster()
{
    if (m_analysing || m_activeRender >= 0)
        return;
    if (m_recording) {
        setError(QStringLiteral("stop recording before analysing the mix"));
        return;
    }
    if (m_project.tracks().empty()) {
        setError(QStringLiteral("nothing to analyse: the session is empty; open or import audio first"));
        return;
    }
    if (m_shuttingDown || m_loading || m_saving || m_busy.load(std::memory_order_acquire)) {
        setError(QStringLiteral("still working on the previous job; wait for it to finish or cancel it"));
        return;
    }
    m_analysing = true;
    emit analysisChanged();
    startMasterJob(/*exportIt=*/false, -1);
}

void Backend::installAnalysis(Analysis analysis)
{
    m_analysing = false;
    if (analysis.generation != m_sessionGeneration || m_shuttingDown) {
        removeTextures(analysis.peaks);
        emit analysisChanged();
        return;
    }
    if (!analysis.ok) {
        setError(QString::fromStdString(analysis.error));
        emit analysisChanged();
        return;
    }

    removeTextures(m_analysis.peaks);
    m_analysis = std::move(analysis);
    // An edit can complete while this immutable snapshot is on the worker. Keep
    // the old report viewable, but never advertise it as the current mix.
    consumePluginNotifications();
    m_analysisStale = !analysisCurrent(m_analysis.generation, m_analysis.revision, m_analysis.analysisEpoch);

    QVariantList levels;
    if (m_analysis.peaks.ok) {
        for (std::size_t level = 0; level < m_analysis.peaks.textures.size(); ++level) {
            const io::PeakTexture &tex = m_analysis.peaks.textures[level];
            if (!tex.ok)
                continue;
            QVariantMap l;
            l["texture"] = QString::fromStdString(tex.path);
            l["asset"] = m_analysis.peaks.cache ? m_analysis.peaks.cache->token() : QString{};
            l["textureUrl"] = m_analysis.peaks.cache ? m_analysis.peaks.cache->textureUrl(QString::fromStdString(tex.path)) : QUrl{};
            l["peakCount"] = static_cast<int>(tex.peakCount);
            l["samplesPerPeak"] =
                static_cast<int>(m_analysis.peaks.pyramid.levels[level].samplesPerPeak);
            l["textureWidth"] = static_cast<int>(tex.width);
            l["textureHeight"] = static_cast<int>(tex.height);
            l["channels"] = static_cast<int>(tex.channels);
            levels.append(l);
        }
    }
    m_masterLevels = levels;
    m_masterReport = reportToMap(m_analysis.report);
    // The level match: how much louder the mastered mix is than the plain one.
    {
        const double mastered = static_cast<double>(m_analysis.report.integrated);
        const double raw = static_cast<double>(m_analysis.rawReport.integrated);
        m_previewMatched = !m_analysisStale && mastered > -100.0 && raw > -100.0;
        m_previewMatchDb = m_previewMatched ? mastered - raw : 0.0;
        m_masterReport["rawIntegrated"] = raw;
        m_masterReport["matchDb"] = m_previewMatchDb;
        if (m_previewBefore)
            pushPreview();
        emit previewChanged();
    }

    QVariantList shortTerm;
    QVariantList momentary;
    shortTerm.reserve(static_cast<int>(m_analysis.history.size()));
    momentary.reserve(static_cast<int>(m_analysis.history.size()));
    for (const engine::LoudnessPoint &p : m_analysis.history) {
        shortTerm.append(static_cast<double>(p.shortTerm));
        momentary.append(static_cast<double>(p.momentary));
    }
    m_shortTermHistory = shortTerm;
    m_momentaryHistory = momentary;
    emit analysisChanged();
}

// --- delivery ----------------------------------------------------------------------

QVariantList Backend::exportFormats() const
{
    QVariantList out;
    for (const render::FormatInfo &f : render::availableFormats()) {
        QVariantMap m;
        m["id"] = QString::fromStdString(f.id);
        m["name"] = QString::fromStdString(f.name);
        m["extension"] = QString::fromStdString(f.extension);
        m["lossy"] = f.lossy;
        m["available"] = f.available;
        m["reason"] = QString::fromStdString(f.reason);
        QVariantList depths;
        for (const render::SampleFormat s : f.sampleFormats)
            depths.append(render::bitsOf(s));
        m["bitDepths"] = depths;
        QVariantList rates;
        for (const std::uint32_t r : f.sampleRates) rates.append(static_cast<int>(r));
        m["sampleRates"] = rates;               // empty: any rate from min to max
        m["minSampleRate"] = static_cast<int>(f.minSampleRate);
        m["maxSampleRate"] = static_cast<int>(f.maxSampleRate);
        m["maxChannels"] = static_cast<int>(f.maxChannels);
        out.append(m);
    }
    return out;
}

QVariantMap Backend::checkDelivery(const QVariantMap &spec) const
{
    render::ExportSpec s;
    QVariantMap out;
    const auto container = render::containerFromId(spec.value(QStringLiteral("format"), QStringLiteral("wav")).toString().toStdString());
    if (!container) {
        out["ok"] = false;
        out["reason"] = QStringLiteral("unknown format: ") + spec.value(QStringLiteral("format")).toString();
        return out;
    }
    s.container = *container;
    const auto sampleFormat = render::sampleFormatFromBits(spec.value(QStringLiteral("bitDepth"), 24).toInt());
    s.sampleFormat = sampleFormat.value_or(render::SampleFormat::Pcm24);
    s.sampleRate = static_cast<std::uint32_t>(std::max(0, spec.value(QStringLiteral("sampleRate"), 0).toInt()));
    s.channels = static_cast<std::uint32_t>(std::max(0, spec.value(QStringLiteral("channels"), 0).toInt()));
    s.bitrateKbps = static_cast<unsigned>(std::max(0, spec.value(QStringLiteral("bitrateKbps"), 0).toInt()));
    const auto check = render::checkSpec(s, static_cast<std::uint32_t>(m_project.sampleRate()), kOutputChannels);
    out["ok"] = check.ok && sampleFormat.has_value();
    out["reason"] = !sampleFormat.has_value() ? QStringLiteral("bit depth must be 16, 24 or 32 (float)")
                                              : QString::fromStdString(check.reason);
    out["suggestedSampleRate"] = static_cast<int>(check.suggestedSampleRate.value_or(0));
    out["suggestedBitDepth"] = check.suggestedSampleFormat ? render::bitsOf(*check.suggestedSampleFormat) : 0;
    out["suggestedChannels"] = static_cast<int>(check.suggestedChannels.value_or(0));
    out["suggestedBitrateKbps"] = check.suggestedBitrateKbps ? static_cast<int>(*check.suggestedBitrateKbps) : -1;
    return out;
}

QUrl Backend::suggestedExportUrl(const QString &format) const
{
    const auto container = render::containerFromId(format.toStdString()).value_or(render::Container::Wav);
    const auto extension = QString::fromLatin1(render::containerExtension(container).data());
    QString base = projectName();
    if (base.isEmpty() && m_loaded)
        base = QFileInfo(m_takeName).completeBaseName();
    if (base.isEmpty()) base = QStringLiteral("mix");
    QString directory = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    if (directory.isEmpty()) directory = QDir::homePath();
    return QUrl::fromLocalFile(QDir(directory).filePath(base + QStringLiteral(" master.") + extension));
}

QVariantList Backend::deliveryPresets() const
{
    // The delivery targets the brief names (section 10), with the numbers the
    // platforms publish. A preset is a starting point: every field stays editable.
    const auto preset = [](const char *id, const char *name, const char *description,
                           const char *normalise, double targetLufs, double ceilingDbTp,
                           double peakTargetDb, const char *format, int bitDepth,
                           int sampleRate, int channels, bool dither) {
        QVariantMap m;
        m["id"] = QString::fromLatin1(id);
        m["name"] = QString::fromUtf8(name);
        m["description"] = QString::fromUtf8(description);
        m["normalise"] = QString::fromLatin1(normalise);
        m["targetLufs"] = targetLufs;
        m["ceilingDbTp"] = ceilingDbTp;
        m["peakTargetDb"] = peakTargetDb;
        m["format"] = QString::fromLatin1(format);
        m["bitDepth"] = bitDepth;
        m["sampleRate"] = sampleRate;
        m["channels"] = channels;
        m["dither"] = dither;
        return m;
    };
    QVariantList out;
    out.append(preset("podcast", "Podcast", "Spoken word for podcast platforms: −16 LUFS, −1 dBTP, MP3",
                      "loudness", -16.0, -1.0, -0.3, "mp3", 16, 44100, 0, true));
    out.append(preset("podcast-wav", "Podcast (WAV)", "Spoken word, uncompressed: −16 LUFS, −1 dBTP, 16-bit 44.1 kHz",
                      "loudness", -16.0, -1.0, -0.3, "wav", 16, 44100, 0, true));
    out.append(preset("streaming", "Streaming (−14 LUFS)", "Spotify, YouTube, Amazon and Tidal: −14 LUFS, −1 dBTP",
                      "loudness", -14.0, -1.0, -0.3, "wav", 24, 0, 0, true));
    out.append(preset("apple", "Apple Music", "Apple's target: −16 LUFS, −1 dBTP",
                      "loudness", -16.0, -1.0, -0.3, "wav", 24, 0, 0, true));
    out.append(preset("ebu", "Broadcast EBU R128", "European broadcast: −23 LUFS, −1 dBTP, 48 kHz",
                      "loudness", -23.0, -1.0, -0.3, "wav", 24, 48000, 0, true));
    out.append(preset("atsc", "Broadcast ATSC A/85", "US broadcast: −24 LKFS, −2 dBTP, 48 kHz",
                      "loudness", -24.0, -2.0, -0.3, "wav", 24, 48000, 0, true));
    out.append(preset("cd", "CD", "Red Book: 16-bit 44.1 kHz, peak at −0.3 dBTP, dithered",
                      "peak", -16.0, -1.0, -0.3, "wav", 16, 44100, 0, true));
    out.append(preset("archive", "Archive", "The mix as it is: 32-bit float at the session rate, no gain",
                      "none", -16.0, -1.0, -0.3, "wav", 32, 0, 0, false));
    return out;
}

QVariantMap Backend::presetById(const QString &id) const
{
    for (const QVariant &p : deliveryPresets()) {
        if (p.toMap().value(QStringLiteral("id")).toString() == id)
            return p.toMap();
    }
    return {};
}

Backend::RenderJob *Backend::renderJob(int id)
{
    for (RenderJob &job : m_renders) {
        if (job.id == id)
            return &job;
    }
    return nullptr;
}

QVariantMap Backend::renderJobToMap(const RenderJob &job) const
{
    QVariantMap m;
    m["id"] = job.id;
    m["name"] = job.name;
    m["path"] = QString::fromStdString(job.spec.path);
    m["format"] = QString::fromUtf8(render::containerId(job.spec.container).data(),
                                    static_cast<int>(render::containerId(job.spec.container).size()));
    m["bitDepth"] = render::bitsOf(job.spec.sampleFormat);
    m["bitrateKbps"] = int(job.spec.bitrateKbps);
    m["sampleRate"] = static_cast<int>(job.spec.sampleRate == 0 ? m_project.sampleRate()
                                                                : job.spec.sampleRate);
    m["channels"] = static_cast<int>(job.spec.channels == 0 ? kOutputChannels : job.spec.channels);
    m["normalise"] = job.spec.normalise == render::Normalise::Loudness ? QStringLiteral("loudness")
                   : job.spec.normalise == render::Normalise::Peak ? QStringLiteral("peak")
                   : QStringLiteral("none");
    m["targetLufs"] = static_cast<double>(job.spec.targetLufs);
    m["ceilingDbTp"] = static_cast<double>(job.spec.ceilingDbTp);
    if (job.rangeStart >= 0) {
        m["start"] = static_cast<qint64>(job.rangeStart);
        m["end"] = static_cast<qint64>(job.rangeEnd);
    }
    m["state"] = job.state;
    m["progress"] = job.progress;
    m["message"] = job.message;
    m["warning"] = job.warning;
    m["report"] = job.report;
    m["container"] = job.report.value(QStringLiteral("container")).toString();
    m["metadataWritten"] = job.report.value(QStringLiteral("metadataWritten")).toBool();
    m["markersWritten"] = job.report.value(QStringLiteral("markersWritten")).toInt();
    return m;
}

QVariantList Backend::renderQueue() const
{
    QVariantList out;
    for (const RenderJob &job : m_renders)
        out.append(renderJobToMap(job));
    return out;
}

QVariantMap Backend::renderById(int id) const
{
    for (const RenderJob &job : m_renders) {
        if (job.id == id)
            return renderJobToMap(job);
    }
    return {};
}

int Backend::queueRender(const QVariantMap &spec)
{
    if (m_project.tracks().empty()) {
        setError(QStringLiteral("nothing to render: the session is empty; open or import audio first"));
        return -1;
    }

    RenderJob job;
    const QUrl url(spec.value(QStringLiteral("path")).toString());
    const QString path = url.isLocalFile() ? url.toLocalFile()
                                           : spec.value(QStringLiteral("path")).toString();
    if (path.isEmpty()) {
        setError(QStringLiteral("a render needs an output path"));
        return -1;
    }
    job.spec.path = path.toStdString();

    const QString formatId = spec.value(QStringLiteral("format"), QStringLiteral("wav")).toString();
    const std::optional<render::Container> container =
        render::containerFromId(formatId.toStdString());
    if (!container.has_value()) {
        setError(QStringLiteral("unknown format: ") + formatId);
        return -1;
    }
    job.spec.container = *container;

    const int bits = spec.value(QStringLiteral("bitDepth"), 24).toInt();
    const std::optional<render::SampleFormat> sampleFormat = render::sampleFormatFromBits(bits);
    if (!sampleFormat.has_value()) {
        setError(QStringLiteral("bit depth must be 16, 24 or 32 (float)"));
        return -1;
    }
    job.spec.sampleFormat = *sampleFormat;
    job.spec.sampleRate = static_cast<std::uint32_t>(std::max(0, spec.value(QStringLiteral("sampleRate"), 0).toInt()));
    job.spec.channels = static_cast<std::uint32_t>(std::max(0, spec.value(QStringLiteral("channels"), 0).toInt()));
    job.spec.dither = spec.value(QStringLiteral("dither"), true).toBool();
    const int bitrate=spec.value(QStringLiteral("bitrateKbps"),0).toInt();
    if(bitrate<0 || (bitrate!=0 && (bitrate<64 || bitrate>320))) {
        setError("AAC bitrate must be 64–320 kb/s, or automatic");return -1;
    }
    job.spec.bitrateKbps=unsigned(bitrate);
    // Refuse what the capability model says cannot be written, with its reason,
    // rather than queueing a job that fails.
    if (const auto check = render::checkSpec(job.spec, static_cast<std::uint32_t>(m_project.sampleRate()), kOutputChannels); !check.ok) {
        setError(QString::fromStdString(check.reason));
        return -1;
    }
    {
        const QString markers = spec.value(QStringLiteral("markers"), QStringLiteral("none")).toString();
        const std::uint32_t exportRate = job.spec.sampleRate == 0 ? m_project.sampleRate() : job.spec.sampleRate;
        job.spec.writeMetadata = spec.value(QStringLiteral("includeMetadata"), false).toBool();
        job.spec.writeMarkers = markers != QLatin1String("none");
        job.spec.metadata = exportMetadata(markers, exportRate);
    }
    const QString normalise = spec.value(QStringLiteral("normalise"), QStringLiteral("none")).toString();
    job.spec.normalise = normalise == QLatin1String("loudness") ? render::Normalise::Loudness
                       : normalise == QLatin1String("peak") ? render::Normalise::Peak
                       : render::Normalise::None;
    job.spec.targetLufs = static_cast<float>(spec.value(QStringLiteral("targetLufs"), -16.0).toDouble());
    job.spec.ceilingDbTp = static_cast<float>(spec.value(QStringLiteral("ceilingDbTp"), -1.0).toDouble());
    job.spec.peakTargetDb = static_cast<float>(spec.value(QStringLiteral("peakTargetDb"), -0.3).toDouble());

    // A time range: only those frames are written, cut from the whole render, so
    // effects sound the same as in the whole export. Markers move with it.
    const bool selectionRange = spec.value(QStringLiteral("range")).toString() == QLatin1String("selection");
    if (selectionRange && m_selection.empty()) {
        setError(tr("Select a time range to export first: drag across a lane or the waveform."));
        return -1;
    }
    if (selectionRange || spec.contains(QStringLiteral("start")) || spec.contains(QStringLiteral("end"))) {
        bool startOk = selectionRange, endOk = selectionRange;
        const qint64 start = selectionRange ? qint64(m_selection.start) : spec.value(QStringLiteral("start")).toLongLong(&startOk);
        const qint64 end = selectionRange ? qint64(m_selection.end) : spec.value(QStringLiteral("end")).toLongLong(&endOk);
        if (!startOk || !endOk || start < 0 || end <= start) {
            setError(tr("An export range needs a start before its end, in samples from the project's start."));
            return -1;
        }
        job.rangeStart = start;
        job.rangeEnd = end;
        const std::uint32_t exportRate = job.spec.sampleRate == 0 ? m_project.sampleRate() : job.spec.sampleRate;
        const auto first = session::rescaleFrames(start, m_project.sampleRate(), exportRate);
        const auto last = session::rescaleFrames(end, m_project.sampleRate(), exportRate);
        auto& markers = job.spec.metadata.markers;
        std::erase_if(markers, [&](const auto& m) { return m.position < first || m.position >= last; });
        for (auto& m : markers) {
            m.position -= first;
            if (m.position + m.length > last - first) m.length = last - first - m.position;
        }
        job.spec.metadata.bext.timeReference += first;
    }

    job.name = spec.value(QStringLiteral("name")).toString();
    if (job.name.isEmpty())
        job.name = QFileInfo(path).fileName();
    job.state = QStringLiteral("queued");
    job.generation = m_sessionGeneration;
    if (spec.value(QStringLiteral("stems"), false).toBool()) {
        // One file per track that reaches the master: "<name> - <track>.<ext>".
        // With "tracks", only those tracks, muted or not, wherever they are routed;
        // without it every audible track (a muted one would be a silent file).
        const bool chooses = spec.contains(QStringLiteral("tracks"));
        QSet<int> chosen;
        for (const auto& value : spec.value(QStringLiteral("tracks")).toList()) {
            const int id = value.toInt();
            if (!m_project.trackIndex(session::TrackId(id))) { setError(tr("Choose existing tracks to export.")); return -1; }
            chosen.insert(id);
        }
        if (chooses && chosen.isEmpty()) { setError(tr("Select the tracks to export first.")); return -1; }
        const QFileInfo base(path);
        int first = -1;
        QSet<QString> used;
        for (const auto& track : m_project.tracks()) {
            if (track.clips.empty() && !std::any_of(m_project.tracks().begin(), m_project.tracks().end(), [&](const auto& other) {
                    return other.routing.output == track.id; })) continue;
            if (chooses) {
                if (!chosen.contains(int(track.id))) continue;
            } else {
                if (track.muted) continue;
                if (track.routing.output != session::kInvalidTrack) continue; // feeds another track, heard in its stem
            }
            QString name = QString::fromStdString(track.name).trimmed();
            // A track named after its file ("Voice.wav") gives "Mix - Voice.wav", not ".wav.wav".
            name.remove(QRegularExpression(QStringLiteral("\\.(wav|wave|flac|aiff?|aifc|mp3|ogg|oga|opus|m4a|aac|w64|rf64|caf)$"),
                                           QRegularExpression::CaseInsensitiveOption));
            name.replace(QRegularExpression(QStringLiteral("[/\\:*?\"<>|\\x00-\\x1f]")), QStringLiteral("_"));
            if (name.isEmpty()) name = tr("Track %1").arg(int(track.id) + 1);
            QString unique = name;
            for (int n = 2; used.contains(unique.toLower()); ++n) unique = QStringLiteral("%1 %2").arg(name).arg(n);
            used.insert(unique.toLower());
            RenderJob stem = job;
            const auto file = base.dir().filePath(QStringLiteral("%1 - %2.%3").arg(base.completeBaseName(), unique, base.suffix()));
            stem.spec.path = file.toStdString();
            stem.name = QFileInfo(file).fileName();
            stem.stemTrack = track.id;
            stem.stemChosen = chooses;
            stem.id = m_nextRenderId++;
            if (first < 0) first = stem.id;
            m_renders.push_back(std::move(stem));
        }
        if (first < 0) { setError(chooses ? tr("The selected tracks have no audio to export.")
                                          : QStringLiteral("no tracks to render as stems")); return -1; }
        emit renderQueueChanged();
        pumpRenders();
        return first;
    }
    job.id = m_nextRenderId++;
    m_renders.push_back(std::move(job));
    emit renderQueueChanged();
    pumpRenders();
    return m_renders.back().id;
}

void Backend::pumpRenders()
{
    if (m_shuttingDown || m_loading || m_saving || recording() || m_activeRender >= 0 || m_analysing
        || m_busy.load(std::memory_order_acquire))
        return;
    for (RenderJob &job : m_renders) {
        if (job.state != QLatin1String("queued"))
            continue;
        if (job.generation != m_sessionGeneration) {
            // Queued for a session that has since been replaced: rendering now
            // would write the new project to the old one's file, under its name.
            job.state = QStringLiteral("cancelled");
            job.message = QStringLiteral("The session changed before this export started. Queue it again from the session you want.");
            emit renderQueueChanged();
            continue;
        }
        job.state = QStringLiteral("rendering");
        job.progress = 0.0;
        job.message.clear();
        m_activeRender = job.id;
        m_renderProgress = 0.0;
        emit renderQueueChanged();
        emit renderProgressChanged();
        startMasterJob(/*exportIt=*/true, job.id);
        return;
    }
}

void Backend::finishRender(RenderOutcome outcome)
{
    m_activeRender = -1;
    RenderJob *job = renderJob(outcome.id);
    if (job == nullptr) {
        emit renderQueueChanged();
        return;
    }
    if (outcome.result.ok) {
        job->state = QStringLiteral("done");
        job->progress = 1.0;
        job->warning = QString::fromStdString(outcome.result.warning);
        QVariantMap report;
        report["measured"] = reportToMap(outcome.result.report.measured);
        report["delivered"] = reportToMap(outcome.result.report.delivered);
        report["appliedGainDb"] = static_cast<double>(outcome.result.report.appliedGainDb);
        report["ceilingLimited"] = outcome.result.report.ceilingLimited;
        report["frames"] = static_cast<qint64>(outcome.result.report.frames);
        report["sampleRate"] = static_cast<int>(outcome.result.report.sampleRate);
        report["channels"] = static_cast<int>(outcome.result.report.channels);
        report["container"] = QString::fromStdString(outcome.result.report.container);
        report["metadataWritten"] = outcome.result.report.metadataWritten;
        report["markersWritten"] = static_cast<int>(outcome.result.report.markersWritten);
        job->report = report;
        job->message = QStringLiteral("Rendered to %1").arg(QString::fromStdString(outcome.result.path));
    } else if (outcome.result.cancelled) {
        job->state = QStringLiteral("cancelled");
        job->message = QStringLiteral("Cancelled");
    } else {
        job->state = QStringLiteral("failed");
        job->message = QString::fromStdString(outcome.result.error);
        setError(job->message);
    }
    emit renderQueueChanged();
    emit renderFinished(job->id, job->state, QString::fromStdString(job->spec.path), job->message);
}

void Backend::cancelRender(int id)
{
    RenderJob *job = renderJob(id);
    if (job == nullptr)
        return;
    if (job->state == QLatin1String("rendering")) {
        m_cancelJob.store(true, std::memory_order_relaxed);
        pumpDeliverySnapshots();
        return;
    }
    if (job->state == QLatin1String("queued")) {
        job->state = QStringLiteral("cancelled");
        job->message = QStringLiteral("Cancelled before it started");
        emit renderQueueChanged();
    }
}

void Backend::retryRender(int id)
{
    RenderJob *job = renderJob(id);
    if (job == nullptr || job->state == QLatin1String("rendering")
        || job->state == QLatin1String("queued"))
        return;
    job->state = QStringLiteral("queued");
    job->generation = m_sessionGeneration;   // asked for again, of the session open now
    job->progress = 0.0;
    job->message.clear();
    job->warning.clear();
    job->report.clear();
    emit renderQueueChanged();
    pumpRenders();
}

void Backend::removeRender(int id)
{
    for (auto it = m_renders.begin(); it != m_renders.end(); ++it) {
        if (it->id != id)
            continue;
        if (it->state == QLatin1String("rendering"))
            return;             // cancel first; the worker holds its spec
        m_renders.erase(it);
        emit renderQueueChanged();
        return;
    }
}

void Backend::clearFinishedRenders()
{
    const auto before = m_renders.size();
    std::erase_if(m_renders, [](const RenderJob &job) {
        return job.state == QLatin1String("done") || job.state == QLatin1String("failed")
            || job.state == QLatin1String("cancelled");
    });
    if (m_renders.size() != before)
        emit renderQueueChanged();
}

void Backend::revealRender(int id)
{
    const RenderJob *job = renderJob(id);
    if (job == nullptr)
        return;
    const QFileInfo info(QString::fromStdString(job->spec.path));
    QDesktopServices::openUrl(QUrl::fromLocalFile(info.absolutePath()));
}

// --- the Spectral page -----------------------------------------------------------

int Backend::trackChannels(const session::Track &track) const
{
    // The take's own channel count -- the widest source on the track. Peaks are
    // built at the output's two channels whatever the source is, so this is what
    // says whether a page draws one lane or two.
    int channels = 0;
    for (const session::Clip &c : track.clips) {
        const session::AudioSource *source = m_project.source(c.source);
        if (source != nullptr)
            channels = std::max(channels, static_cast<int>(source->audio.channels()));
    }
    return channels;
}

spectral::StftConfig Backend::stftConfig() const
{
    spectral::StftConfig config = m_stft;
    config.hop = std::max<std::uint32_t>(1, config.fftSize / static_cast<std::uint32_t>(m_overlap));
    return config;
}

QString Backend::spectralWindow() const
{
    const std::string_view name = spectral::windowName(m_stft.window);
    return QString::fromUtf8(name.data(), static_cast<int>(name.size()));
}

QVariantList Backend::spectralFftSizes() const
{
    return {512, 1024, 2048, 4096, 8192};
}

QStringList Backend::spectralWindows() const
{
    QStringList out;
    for (const spectral::Window w : {spectral::Window::Hann, spectral::Window::Hamming,
                                     spectral::Window::BlackmanHarris}) {
        const std::string_view name = spectral::windowName(w);
        out << QString::fromUtf8(name.data(), static_cast<int>(name.size()));
    }
    return out;
}

QVariantList Backend::spectralOverlaps() const
{
    return {2, 4, 8};
}

void Backend::setSpectralConfig(int fftSize, const QString &window, int overlap)
{
    const auto size = static_cast<std::uint32_t>(std::max(0, fftSize));
    if (!spectral::Fft::isPowerOfTwo(size) || size < 512 || size > 8192) {
        setError(QStringLiteral("the FFT size must be a power of two from 512 to 8192"));
        return;
    }
    const std::optional<spectral::Window> w = spectral::windowFromName(window.toStdString());
    if (!w.has_value()) {
        setError(QStringLiteral("unknown window: %1").arg(window));
        return;
    }
    if (overlap != 2 && overlap != 4 && overlap != 8) {
        setError(QStringLiteral("the overlap must be 2, 4 or 8"));
        return;
    }
    if (m_stft.fftSize == size && m_stft.window == *w && m_overlap == overlap)
        return;
    m_stft.fftSize = size;
    m_stft.window = *w;
    m_overlap = overlap;
    emit spectralConfigChanged();
    markSpectralStale();
}

void Backend::setSpectralWanted(bool wanted)
{
    if (m_spectralWanted == wanted)
        return;
    m_spectralWanted = wanted;
    emit spectralChanged();
    if (wanted && m_spectralStale)
        pumpSpectral();
}

void Backend::markSpectralStale()
{
    clearClicks();
    m_spectralStale = true;
    emit spectralChanged();
    if (m_spectralWanted)
        m_spectralTimer->start();
}

void Backend::rebuildSpectral()
{
    m_spectralStale = true;
    emit spectralChanged();
    pumpSpectral();
}

void Backend::clearSpectral()
{
    m_spectralCache.reset();
    m_spectralSet = spectral::SpectralSet{};
    m_spectralTrack = session::kInvalidTrack;
    if (!m_spectral.isEmpty()) {
        m_spectral.clear();
        emit spectralChanged();
    }
}

void Backend::pumpSpectral()
{
    if (!m_spectralWanted || !m_spectralStale || m_spectralBuilding)
        return;
    if (m_shuttingDown || m_loading || m_saving || m_busy.load(std::memory_order_acquire) || finishedJobsWaiting()) {
        // Busy, or a finished job has yet to be installed: its result may
        // replace the project this build would read on the worker.
        m_spectralTimer->start();       // the slot frees, drainWorker restarts us
        return;
    }
    const session::Track *track = m_project.track(m_focused);
    if (track == nullptr || track->clips.empty()) {
        clearSpectral();
        m_spectralStale = false;
        emit spectralChanged();
        return;
    }

    joinWorker();
    m_busy.store(true, std::memory_order_release);
    m_cancelJob.store(false, std::memory_order_relaxed);
    m_jobProgress.store(0.0f, std::memory_order_relaxed);
    m_spectralBuilding = true;
    m_spectralStale = false;
    m_spectralProgress = 0.0;
    emit spectralChanged();
    emit spectralProgressChanged();

    const std::vector<session::Clip> snapshot = track->clips;
    const auto channels = static_cast<std::uint32_t>(std::max(1, trackChannels(*track)));
    const session::TrackId id = track->id;
    const std::uint32_t sequence = ++m_spectralSequence;
    const spectral::StftConfig config = stftConfig();
    const std::uint32_t rate = m_project.sampleRate();

    const bool midSide = m_spectralView == "mid-side" && channels == 2;
    const auto generation = m_sessionGeneration;
    const SpectralStore store = m_spectralStore;
    const qint64 storeLimit = qint64(m_spectralStoreGiB) << 30;
    m_worker = std::thread([this, snapshot, channels, id, sequence, config, rate, midSide, generation, store, storeLimit] {
        // Reads m_project for its sources only, as the peaks job does: sources are
        // never added or removed while a worker runs.
        SpectralResult out;
        out.generation = generation;
        out.track = id;
        out.sequence = sequence;
        try {
        if (!m_displayCache.isValid()) throw std::runtime_error("Could not create a private graphics cache: " + m_displayCache.errorString().toStdString());
        auto audio = session::renderTimeline(m_project, snapshot, channels, &m_cancelJob);
        out.midSide = midSide;
        if (midSide) {
            for (std::size_t i = 0; i < audio.frames(); ++i) {
                const float l = audio.plane(0)[i], r = audio.plane(1)[i];
                audio.plane(0)[i] = (l + r) * 0.5f;
                audio.plane(1)[i] = (l - r) * 0.5f;
            }
        }
        const std::string stem = "spectral-" + std::to_string(static_cast<std::uint32_t>(id))
                               + "-" + std::to_string(sequence);
        // The same audio analysed the same way has the same tiles: take them from
        // the store when it has them, and keep the ones that were slow to make.
        std::vector<std::span<const float>> planes;
        for (std::uint32_t c = 0; c < audio.channels(); ++c) planes.push_back(std::as_const(audio).plane(c));
        const auto key = SpectralStore::key(planes, rate, config, midSide, &m_cancelJob);
        if (store.restore(key, QString::fromStdString(runtimeDir()), QString::fromStdString(stem), out.set)) {
            m_spectralStoreHits.fetch_add(1, std::memory_order_relaxed);
            m_jobProgress.store(1.0f, std::memory_order_relaxed);
        } else {
            QElapsedTimer built; built.start();
            out.set = spectral::buildSpectralTiles(audio, rate, config, runtimeDir(), stem, &m_cancelJob,
                                                   [this](float f) {
                                                       m_jobProgress.store(f, std::memory_order_relaxed);
                                                   });
            if (out.set.ok && !key.isEmpty()
                && built.elapsed() >= m_spectralStoreMinimumMs.load(std::memory_order_relaxed)
                && store.save(key, out.set))
                store.prune(storeLimit);
        }
        out.ok = out.set.ok;
        out.error = out.set.error;
        if (out.ok) {
            QStringList paths;
            for (const auto &path : out.set.files()) paths.append(QString::fromStdString(path));
            const auto cleanup = qScopeGuard([&] {
                if (!out.cache) for (const auto &path : paths) QFile::remove(path);
            });
            QString error;
            out.cache = m_displayCache.adopt(paths, error);
            out.ok = bool(out.cache);
            out.error = error.toStdString();
        }
        } catch (const std::exception& e) { out.ok = false; out.error = e.what(); }
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_pendingSpectral.push_back(std::move(out));
            m_finishedJobs.push_back(Job::Spectral);
        }
        m_busy.store(false, std::memory_order_release);
        emit workerFinished();
    });
}

void Backend::installSpectral(SpectralResult result)
{
    m_spectralBuilding = false;
    m_spectralProgress = 1.0;
    emit spectralProgressChanged();
    if (result.generation != m_sessionGeneration || m_shuttingDown) {
        emit spectralChanged();
        return; // The discarded result releases its batch, just like old peaks.
    }
    if (!result.ok) {
        if (!result.set.cancelled)
            setError(QString::fromStdString("spectral display: " + result.error));
        emit spectralChanged();
        return;
    }

    // Accepted readers keep the previous batch until decoding finishes.
    m_spectralCache = std::move(result.cache);
    m_spectralSet = std::move(result.set);
    m_spectralTrack = result.track;

    QVariantMap map;
    map["ready"] = true;
    map["track"] = static_cast<int>(result.track);
    map["sequence"] = static_cast<int>(result.sequence);
    map["channels"] = static_cast<int>(m_spectralSet.channels);
    map["sampleRate"] = static_cast<int>(m_spectralSet.sampleRate);
    map["fftSize"] = static_cast<int>(m_spectralSet.fftSize);
    map["midSide"] = result.midSide;
    map["analysisHop"] = static_cast<int>(m_spectralSet.analysisHop);
    map["hop"] = static_cast<int>(m_spectralSet.hop);
    map["bins"] = static_cast<int>(m_spectralSet.bins);
    map["columns"] = static_cast<qint64>(m_spectralSet.columns);
    map["tileColumns"] = static_cast<int>(m_spectralSet.tileColumns);
    map["floorDb"] = static_cast<double>(spectral::kFloorDb);
    QVariantList channels;
    for (const spectral::SpectralChannel &ch : m_spectralSet.perChannel) {
        QVariantList levels;
        for (const spectral::SpectralLevel &level : ch.levels) {
            QVariantMap l;
            l["decimation"] = static_cast<int>(level.decimation);
            l["columns"] = static_cast<qint64>(level.columns);
            QVariantList tiles;
            for (const spectral::SpectralTile &tile : level.tiles) {
                QVariantMap t;
                t["texture"] = QString::fromStdString(tile.path);
                t["asset"] = m_spectralCache ? m_spectralCache->token() : QString{};
                t["textureUrl"] = m_spectralCache ? m_spectralCache->textureUrl(QString::fromStdString(tile.path)) : QUrl{};
                t["firstColumn"] = static_cast<qint64>(tile.firstColumn);
                t["columns"] = static_cast<int>(tile.columns);
                t["width"] = static_cast<int>(tile.width);
                t["height"] = static_cast<int>(tile.height);
                tiles.append(t);
            }
            l["tiles"] = tiles;
            levels.append(l);
        }
        QVariantMap entry;
        entry["levels"] = levels;
        channels.append(entry);
    }
    map["perChannel"] = channels;
    m_spectral = map;
    emit spectralChanged();
}

void Backend::setBand(double lowHz, double highHz)
{
    const double nyquist = static_cast<double>(m_project.sampleRate()) / 2.0;
    double low = std::clamp(lowHz, 0.0, nyquist);
    double high = highHz <= 0.0 ? nyquist : std::clamp(highHz, 0.0, nyquist);
    if (high < low)
        std::swap(low, high);
    if (low <= 0.0 && high >= nyquist) {
        clearBand();
        return;
    }
    if (low == m_bandLowHz && high == m_bandHighHz)
        return;
    m_bandLowHz = low;
    m_bandHighHz = high;
    emit selectionChanged();
}

void Backend::clearBand()
{
    if (bandEmpty())
        return;
    m_bandLowHz = 0.0;
    m_bandHighHz = 0.0;
    emit selectionChanged();
}

void Backend::attenuateSpectral(double gainDb)
{
    RepairRequest request; request.kind = RepairKind::Attenuate; request.gainDb = gainDb;
    startRepair(std::move(request));
}

void Backend::healSpectral()
{
    RepairRequest request; request.kind = RepairKind::Heal; startRepair(std::move(request));
}

void Backend::denoiseSpectral(double amountDb, double strength)
{
    if (m_noisePrint.empty()) {
        setError(QStringLiteral("learn the noise first: select some noise on its own, then Effects › Restoration › Learn Noise from Selection"));
        return;
    }
    RepairRequest request; request.kind = RepairKind::Denoise;
    request.amountDb = amountDb; request.strength = strength; startRepair(std::move(request));
}

// --- clicks and pops (A23) -----------------------------------------------------------

void Backend::detectClicks(double sensitivity)
{
    const session::Track *track = m_project.track(m_focused);
    if (track == nullptr || track->clips.empty()) {
        setError(QStringLiteral("nothing to search: no take is focused; click a track to focus it"));
        return;
    }
    if (m_detecting) {
        setError(QStringLiteral("still looking for clicks"));
        return;
    }
    if (m_shuttingDown || m_loading || m_saving || m_busy.load(std::memory_order_acquire)) {
        // The slot is taken (a peak rebuild after the last edit, usually): wait for
        // it rather than making the user ask twice. drainWorker runs it.
        m_queuedDetect = sensitivity;
        m_detecting = true;
        emit clicksChanged();
        return;
    }
    runDetect(sensitivity);
}

void Backend::pumpDetect()
{
    if (m_shuttingDown || m_loading || m_saving || !m_queuedDetect.has_value()
        || m_busy.load(std::memory_order_acquire))
        return;
    const double sensitivity = *m_queuedDetect;
    m_queuedDetect.reset();
    const session::Track *track = m_project.track(m_focused);
    if (track == nullptr || track->clips.empty()) {
        m_detecting = false;
        m_autoHealPending = false;
        emit clicksChanged();
        setError(QStringLiteral("the search was dropped: the take went away while it waited"));
        return;
    }
    runDetect(sensitivity);
}

void Backend::runDetect(double sensitivity)
{
    const session::Track *track = m_project.track(m_focused);
    const session::FramePos length = track->length();
    const bool wholeTake = m_selection.empty() || m_selection.track != m_focused;
    const session::FramePos start = wholeTake ? 0 : std::clamp<session::FramePos>(m_selection.start, 0, length);
    const session::FramePos end = wholeTake ? length : std::clamp<session::FramePos>(m_selection.end, 0, length);
    if (end <= start) {
        m_detecting = false;
        m_autoHealPending = false;
        emit clicksChanged();
        setError(QStringLiteral("the selection is past the end of the take"));
        return;
    }
    // A little context either side, so a click at the edge has surroundings.
    const auto pad = static_cast<session::FramePos>(m_project.sampleRate() / 50);
    const session::FramePos from = std::max<session::FramePos>(0, start - pad);
    const session::FramePos to = std::min<session::FramePos>(length, end + pad);
    auto snapshot = m_project;
    const auto channels = std::uint32_t(std::max(1, trackChannels(*track)));

    DetectResult job;
    job.track = track->id;
    job.from = from;
    job.expected = session::captureState(m_project);
    job.wholeTake = wholeTake;
    job.thenHeal = m_autoHealPending;
    m_autoHealPending = false;
    const spectral::Region where{.start = static_cast<std::size_t>(start - from),
                                 .end = static_cast<std::size_t>(end - from)};
    const std::uint32_t rate = m_project.sampleRate();
    const auto sens = static_cast<float>(std::clamp(sensitivity, 0.0, 1.0));

    joinWorker();
    m_busy.store(true, std::memory_order_release);
    m_cancelDetect.store(false, std::memory_order_relaxed);
    m_detecting = true;
    emit clicksChanged();
    m_worker = std::thread([this, job = std::move(job), snapshot = std::move(snapshot), from, to, channels, where, rate, sens]() mutable {
        try {
            const auto region = renderRange(snapshot, job.track, from, to, channels, &m_cancelDetect);
            if (!m_cancelDetect.load(std::memory_order_relaxed))
                job.clicks = spectral::detectClicks(region, rate, where, sens, &m_cancelDetect);
            job.cancelled = m_cancelDetect.load(std::memory_order_relaxed);
            for (spectral::Click &c : job.clicks) c.frame += static_cast<std::size_t>(job.from);
        } catch (const std::exception& e) { job.error = QString::fromUtf8(e.what()); }
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_pendingDetect.push_back(std::move(job));
            m_finishedJobs.push_back(Job::Detect);
        }
        m_busy.store(false, std::memory_order_release);
        emit workerFinished();
    });
}

void Backend::finishDetect(DetectResult result)
{
    m_detecting = false;
    if (result.cancelled) { emit clicksChanged(); return; }
    if (!result.error.isEmpty()) { setError(result.error); emit clicksChanged(); return; }
    if (session::captureState(m_project) != result.expected || result.track != m_focused) {
        m_clicks.clear();
        m_clickList.clear();
        m_clickSummary.clear();
        emit clicksChanged();
        setError(QStringLiteral("the take changed while the clicks were being found; look again"));
        return;
    }
    const QString where = result.wholeTake ? QStringLiteral("the take") : QStringLiteral("the selection");
    const std::size_t n = result.clicks.size();
    setClicks(std::move(result.clicks),
              n == 0 ? QStringLiteral("No clicks found in %1.").arg(where)
              : n == 1 ? QStringLiteral("1 click found in %1.").arg(where)
                       : QStringLiteral("%1 clicks found in %2.").arg(n).arg(where));
    if (result.thenHeal && !m_clicks.empty())
        healClicks();
}

void Backend::setClicks(std::vector<spectral::Click> clicks, const QString &summary)
{
    m_clicks = std::move(clicks);
    m_clickList.clear();
    for (const spectral::Click &c : m_clicks) {
        m_clickList.append(QVariantMap{{QStringLiteral("frame"), static_cast<qint64>(c.frame)},
                                       {QStringLiteral("length"), static_cast<qint64>(c.length)},
                                       {QStringLiteral("strength"), static_cast<double>(c.strength)}});
    }
    m_clickSummary = summary;
    emit clicksChanged();
}

void Backend::clearClicks()
{
    if (m_clicks.empty() && m_clickSummary.isEmpty())
        return;
    m_clicks.clear();
    m_clickList.clear();
    m_clickSummary.clear();
    emit clicksChanged();
}

void Backend::healClicks()
{
    const session::Track *track = m_project.track(m_focused);
    if (track == nullptr || track->clips.empty()) {
        setError(QStringLiteral("nothing to heal: no take is focused; click a track to focus it"));
        return;
    }
    if (m_clicks.empty()) {
        setError(QStringLiteral("find the clicks first: Effects › Find Clicks and Pops"));
        return;
    }
    // From the first to the last, with room for the cubic's anchors either side.
    const session::FramePos length = track->length();
    const auto pad = static_cast<session::FramePos>(m_project.sampleRate() / 100);
    const auto first = static_cast<session::FramePos>(m_clicks.front().frame);
    const auto last = static_cast<session::FramePos>(m_clicks.back().frame + m_clicks.back().length);
    RepairRequest request;
    request.kind = RepairKind::Clicks;
    request.clicks = m_clicks;
    request.region = std::make_pair(std::clamp<session::FramePos>(first - pad, 0, length),
                                    std::clamp<session::FramePos>(last + pad, 0, length));
    startRepair(std::move(request));
}

void Backend::healClickAt(int index)
{
    const session::Track *track = m_project.track(m_focused);
    if (track == nullptr || track->clips.empty()) {
        setError(QStringLiteral("nothing to heal: no take is focused; click a track to focus it"));
        return;
    }
    if (index < 0 || static_cast<std::size_t>(index) >= m_clicks.size()) {
        setError(QStringLiteral("no such click in the list"));
        return;
    }
    const spectral::Click one = m_clicks[static_cast<std::size_t>(index)];
    const session::FramePos length = track->length();
    const auto pad = static_cast<session::FramePos>(m_project.sampleRate() / 100);
    RepairRequest request;
    request.kind = RepairKind::Clicks;
    request.clicks = {one};
    request.keepClicks = m_clicks;
    request.keepClicks.erase(request.keepClicks.begin() + index);
    request.region = std::make_pair(
        std::clamp<session::FramePos>(static_cast<session::FramePos>(one.frame) - pad, 0, length),
        std::clamp<session::FramePos>(static_cast<session::FramePos>(one.frame + one.length) + pad, 0, length));
    startRepair(std::move(request));
}

void Backend::autoHealClicks(double sensitivity)
{
    if (m_detecting) {
        // A search already running was not asked to heal, and must not be
        // made to by a request it never saw.
        setError(QStringLiteral("still looking for clicks"));
        return;
    }
    m_autoHealPending = true;
    detectClicks(sensitivity);
    if (!m_detecting)
        m_autoHealPending = false;      // refused; the reason is in lastError
    else recordStep({{"kind", "clicks"}, {"on", true}, {"sensitivity", std::round(sensitivity * 100)}});
}

session::AudioBuffer Backend::renderFocusedRange(session::FramePos from, session::FramePos to) const
{
    const session::Track *track = m_project.track(m_focused);
    if (track == nullptr || to <= from)
        return {};
    const auto channels = static_cast<std::uint32_t>(std::max(1, trackChannels(*track)));
    return renderRange(m_project, m_focused, from, to, channels);
}

void Backend::captureNoisePrint()
{
    const session::Track *track = m_project.track(m_focused);
    if (track == nullptr || track->clips.empty()) {
        setError(QStringLiteral("nothing to measure: no take is focused; click a track to focus it"));
        return;
    }
    if (m_selection.empty()) {
        setError(QStringLiteral("a noise sample needs a selection of nothing but the noise"));
        return;
    }
    const session::FramePos length = track->length();
    const session::FramePos start = std::clamp<session::FramePos>(m_selection.start, 0, length);
    const session::FramePos end = std::clamp<session::FramePos>(m_selection.end, 0, length);
    if (end <= start) {
        setError(QStringLiteral("the selection is past the end of the take"));
        return;
    }
    if (noisePrintBuilding()) { setError("the noise is already being learned"); return; }
    m_noisePrintRequest = NoisePrintRequest{m_sessionGeneration, m_editRevision, ++m_noisePrintSequence,
        m_focused, start, end, stftConfig(), m_repairChannel, QString::fromStdString(track->name)};
    setError({}); emit noisePrintChanged(); pumpNoisePrint();
}

void Backend::pumpNoisePrint()
{
    if (m_shuttingDown || m_noisePrintActive || !m_noisePrintRequest || m_loading || m_saving
        || m_busy.load(std::memory_order_acquire)) return;
    const auto request = *m_noisePrintRequest; m_noisePrintRequest.reset();
    if (request.generation != m_sessionGeneration || request.revision != m_editRevision) {
        setError("noise sample discarded because the session changed"); emit noisePrintChanged(); return;
    }
    const auto* track = m_project.track(request.track);
    if (!track) { emit noisePrintChanged(); return; }
    const auto channels = std::uint32_t(std::max(1, trackChannels(*track)));
    auto snapshot = m_project;
    joinWorker(); m_busy.store(true, std::memory_order_release);
    m_cancelNoisePrint.store(false, std::memory_order_relaxed); m_noisePrintActive = true;
    m_worker = std::thread([this, request, snapshot = std::move(snapshot), channels]() mutable {
        NoisePrintJob job; job.request = request;
        try {
            const auto scope = request.scope == "left" ? spectral::ChannelScope::Left
                : request.scope == "right" ? spectral::ChannelScope::Right
                : request.scope == "mid" ? spectral::ChannelScope::Mid
                : request.scope == "side" ? spectral::ChannelScope::Side : spectral::ChannelScope::All;
            if (!m_cancelNoisePrint.load(std::memory_order_relaxed)) {
                auto region = renderRange(snapshot, request.track, request.from, request.to, channels, &m_cancelNoisePrint);
                if (scope != spectral::ChannelScope::All)
                    region = spectral::repairChannels(region, scope, &m_cancelNoisePrint);
                job.print = spectral::captureNoisePrint(region, {0,region.frames()}, request.config, &m_cancelNoisePrint);
            }
            job.cancelled = m_cancelNoisePrint.load(std::memory_order_relaxed);
            if (!job.cancelled && (job.print.empty() || job.print.columns == 0))
                job.error = "the selection is too short for a noise sample";
            job.print.channelScope = request.scope.toStdString();
            job.summary = QStringLiteral("%1 s of %2 · %3-point · %4 frames averaged · %5")
                .arg(double(request.to-request.from)/snapshot.sampleRate(),0,'f',2).arg(request.name)
                .arg(job.print.fftSize).arg(job.print.columns).arg(request.scope);
        } catch (const std::exception& e) { job.error = QString::fromUtf8(e.what()); }
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_pendingNoisePrint.push_back(std::move(job)); m_finishedJobs.push_back(Job::NoisePrint);
        }
        m_busy.store(false, std::memory_order_release); emit workerFinished();
    });
}

void Backend::finishNoisePrint(NoisePrintJob job)
{
    m_noisePrintActive = false;
    const auto& request = job.request;
    if (request.generation == m_sessionGeneration && request.sequence == m_noisePrintSequence && !job.cancelled) {
        if (request.revision != m_editRevision) setError("noise sample discarded because the session changed");
        else if (!job.error.isEmpty()) setError(job.error);
        else { m_noisePrint = std::move(job.print); m_noisePrintSummary = job.summary; }
    }
    emit noisePrintChanged();
}

void Backend::clearNoisePrint()
{
    ++m_noisePrintSequence; m_noisePrintRequest.reset(); m_cancelNoisePrint.store(true, std::memory_order_relaxed);
    m_noisePrint = spectral::NoisePrint{}; m_noisePrintSummary.clear(); emit noisePrintChanged();
}

// --- autosave and recovery (A22) -----------------------------------------------------

namespace {

[[nodiscard]] std::string writeTextAtomically(const std::filesystem::path &target,
                                              const std::string &body)
{
    const std::filesystem::path temp = std::filesystem::path(target).concat(".tmp");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out)
            return "could not write " + temp.string();
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
        out.flush();
        if (!out)
            return "short write to " + temp.string();
    }
    std::error_code ec;
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return "could not replace " + target.string();
    }
    return {};
}

[[nodiscard]] bool processAlive(qint64 pid)
{
    if (pid <= 0)
        return false;
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
}

} // namespace

QString Backend::stateDir()
{
    return omaaudio::storage::locations().stateDir;
}

QString Backend::recoveryIndexPath() const
{
    return stateDir() + QStringLiteral("/recovery/") + m_instanceId + QStringLiteral(".toml");
}

// Every run owns its recovery bundle, including when two windows edit the same
// saved project. Reuse it so unchanged audio is written only once.
QString Backend::recoveryBundle()
{
    if (m_scratchBundle.isEmpty())
        m_scratchBundle = stateDir() + QStringLiteral("/recovery/") + m_instanceId
                        + QStringLiteral(".omaaudio");
    return m_scratchBundle;
}

io::ProjectView Backend::currentView() const
{
    io::ProjectView view;
    view.focus = m_focused;
    view.selection = m_selection;
    view.band = spectral::Band{.lowHz = static_cast<float>(m_bandLowHz),
                               .highHz = static_cast<float>(m_bandHighHz)};
    view.mask = m_mask;
    view.mask.featherBins = static_cast<std::uint32_t>(m_maskFeather);
    view.mask.featherColumns = static_cast<std::uint32_t>(m_maskFeather);
    view.noisePrint = m_noisePrint;
    view.noisePrintSummary = m_noisePrintSummary.toStdString();
    view.sound = m_soundChoices;
    return view;
}

void Backend::setAutosaveSeconds(int seconds)
{
    seconds = std::clamp(seconds, 0, 3600);
    if (seconds == m_autosaveSeconds)
        return;
    m_autosaveSeconds = seconds;
    if (seconds <= 0)
        m_autosaveTimer->stop();
    else if ((m_dirty || hasUnverifiedPluginState()) && m_loaded)
        m_autosaveTimer->start(seconds * 1000);
    emit autosaveChanged();
}

void Backend::armAutosave()
{
    if (m_autosaveSeconds <= 0 || m_autosaveTimer->isActive())
        return;
    m_autosaveTimer->start(m_autosaveSeconds * 1000);
}

Backend::SaveJob Backend::writeRecovery(const session::Project &project,
                                        const io::ProjectView &view, const std::string &bundle,
                                        const std::string &indexPath,
                                        const RecoveryEntry &entry)
{
    SaveJob out;
    out.path = bundle;
    const io::SaveResult result = io::saveProject(project, view, bundle, io::Layer::Recovery);
    out.ok = result.ok;
    out.error = result.error;
    if (!out.ok)
        return out;
    if (const std::string error = writeRecoveryIndex(indexPath, entry); !error.empty()) {
        out.ok = false;
        out.error = error;
    }
    return out;
}

std::string Backend::writeRecoveryIndex(const std::string &indexPath, const RecoveryEntry &entry)
{
    toml::table index;
    index.insert("bundle", entry.bundle.toStdString());
    index.insert("project", entry.project.toStdString());
    index.insert("name", entry.name.toStdString());
    index.insert("written", entry.written.toStdString());
    index.insert("pid", static_cast<std::int64_t>(entry.pid));
    // One escaped line per string: a project name is a file name, and may hold anything.
    std::ostringstream text;
    text << toml::toml_formatter{index, toml::toml_formatter::default_flags
        & ~toml::format_flags::allow_literal_strings & ~toml::format_flags::allow_multi_line_strings} << '\n';
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(indexPath).parent_path(), ec);
    return writeTextAtomically(indexPath, text.str());
}

void Backend::autosave(bool synchronous)
{
    if ((!m_dirty && !hasUnverifiedPluginState()) || !m_loaded || (m_autosaveSeconds <= 0 && !m_autosavePending)) {
        if (m_autosavePending) {
            cancelSnapshot(); m_autosavePending = false; m_autosaving = false;
            emit autosaveChanged();
        }
        return;
    }
    if (!synchronous
        && (m_busy.load(std::memory_order_acquire) || (m_saving && !m_autosavePending) || m_loading || m_repairing)) {
        if (m_autosavePending) { cancelSnapshot(); retrySnapshot(50); return; }
        m_autosaveTimer->start(m_autosaveSeconds * 1000);     // its turn comes later
        return;
    }
    joinWorker();
    session::Project snapshot(m_project.sampleRate());
    QString captureError;
    const auto captured = captureProjectSnapshot(snapshot, captureError);
    if (captured == SnapshotResult::Pending) {
        m_autosavePending = true; m_autosaving = true;
        emit autosaveChanged();
        return;
    }
    m_autosavePending = false;
    if (captured == SnapshotResult::Failed) {
        m_autosaving = false;
        setError(QStringLiteral("autosave failed: %1").arg(captureError));
        emit autosaveChanged();
        return;
    }

    // The timer also verifies possible persistent readouts. Equal state must
    // not create a recovery bundle or a phantom unsaved session.
    if (!m_dirty) {
        m_autosaving = false;
        emit autosaveChanged();
        return;
    }

    RecoveryEntry entry;
    entry.bundle = recoveryBundle();
    entry.project = m_projectPath;
    entry.name = m_projectPath.isEmpty() ? m_takeName : projectName();
    entry.written = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    entry.pid = static_cast<qint64>(::getpid());
    const std::string bundle = entry.bundle.toStdString();
    const std::string index = recoveryIndexPath().toStdString();
    const io::ProjectView view = currentView();
    const std::uint64_t generation = m_sessionGeneration;

    if (synchronous) {
        const auto result = writeRecovery(snapshot, view, bundle, index, entry);
        if (result.ok)
            m_recoveryWrittenTo = entry.bundle;
        else setError(QStringLiteral("autosave failed: %1").arg(QString::fromStdString(result.error)));
        m_autosaving = false;
        return;
    }

    m_busy.store(true, std::memory_order_release);
    m_autosaving = true;
    emit autosaveChanged();
    m_worker = std::thread([this, snapshot, view, bundle, index, entry, generation] {
        SaveJob out = writeRecovery(snapshot, view, bundle, index, entry);
        out.sessionGeneration = generation;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_pendingAutosave.push_back(std::move(out));
            m_finishedJobs.push_back(Job::Autosave);
        }
        m_busy.store(false, std::memory_order_release);
        emit workerFinished();
    });
}

void Backend::finishAutosave(SaveJob job)
{
    m_autosaving = false;
    if (job.sessionGeneration != m_sessionGeneration) {
        emit autosaveChanged();
        return;
    }
    if (job.ok) {
        m_lastAutosave = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
        m_recoveryWrittenTo = QString::fromStdString(job.path);
    } else
        setError(QStringLiteral("autosave failed: %1").arg(QString::fromStdString(job.error)));
    emit autosaveChanged();
}

void Backend::autosaveNow()
{
    pollPluginNotifications();
    if ((!m_dirty && !hasUnverifiedPluginState()) || !m_loaded) {
        setError(QStringLiteral("nothing to autosave: the session is saved"));
        return;
    }
    if (m_busy.load(std::memory_order_acquire) || m_saving || m_loading || m_repairing) {
        setError(QStringLiteral("still working on the previous job; wait for it to finish or cancel it"));
        return;
    }
    m_autosaveTimer->stop();
    const int seconds = m_autosaveSeconds;
    m_autosaveSeconds = std::max(1, seconds);     // a manual write ignores "off"
    autosave(/*synchronous=*/false);
    m_autosaveSeconds = seconds;
}

// This run's recovery, taken away: after a save, before another session replaces
// this one, and on a clean exit. Never anyone else's -- only what this run wrote,
// which is why the path is remembered at the write and not taken from the project.
void Backend::dropRecovery()
{
    m_autosaveTimer->stop();
    std::error_code ec;
    if (!m_recoveryWrittenTo.isEmpty()) {
        (void)io::discardRecovery(m_recoveryWrittenTo.toStdString());
        m_recoveryWrittenTo.clear();
    }
    if (!m_scratchBundle.isEmpty()) {
        std::filesystem::remove_all(m_scratchBundle.toStdString(), ec);
        m_scratchBundle.clear();
    }
    std::filesystem::remove(recoveryIndexPath().toStdString(), ec);
    if (!m_lastAutosave.isEmpty()) {
        m_lastAutosave.clear();
        emit autosaveChanged();
    }
}

// The index, read once at construction. An entry is offered when its run is over and
// its bundle still has a recovery layer; the newest wins if there are several. An
// entry with nothing behind it is tidied away.
void Backend::scanRecovery(const QString &preferredIndex)
{
    const QString dir = stateDir() + QStringLiteral("/recovery");
    QDirIterator it(dir, {QStringLiteral("*.toml")}, QDir::Files);
    std::optional<RecoveryEntry> best;
    while (it.hasNext()) {
        const QString file = it.next();
        if (!preferredIndex.isEmpty() && file != preferredIndex)
            continue;
        RecoveryEntry entry;
        entry.file = file;
        try {
            const toml::table table = toml::parse_file(file.toStdString());
            entry.bundle = QString::fromStdString(table["bundle"].value_or(std::string{}));
            entry.project = QString::fromStdString(table["project"].value_or(std::string{}));
            entry.name = QString::fromStdString(table["name"].value_or(std::string{}));
            entry.written = QString::fromStdString(table["written"].value_or(std::string{}));
            entry.pid = table["pid"].value_or<std::int64_t>(0);
        } catch (const std::exception &) {
            QFile::remove(file);
            continue;
        }
        // Another run's, still going: not ours to offer. Our own pid means an earlier
        // Backend in this process, which is over.
        if (entry.pid != static_cast<qint64>(::getpid()) && processAlive(entry.pid))
            continue;
        if (entry.bundle.isEmpty() || !io::hasRecovery(entry.bundle.toStdString())) {
            QFile::remove(file);
            if (!entry.bundle.isEmpty() && entry.bundle.startsWith(dir)) {
                std::error_code ec;
                std::filesystem::remove_all(entry.bundle.toStdString(), ec);
            }
            continue;
        }
        if (!best.has_value() || entry.written > best->written)
            best = entry;
    }
    if (best.has_value()) {
        m_recoveryOffer = std::move(best);
        emit recoveryChanged();
    }
}

bool Backend::prepareGraphicsRestart(QString &index)
{
    return prepareSessionCheckpoint(index);
}

bool Backend::prepareSessionCheckpoint(QString &index)
{
    index.clear();
    quiesceForShutdown();
    if (!m_loaded && !m_dirty && m_projectPath.isEmpty())
        return true;

    RecoveryEntry entry;
    entry.bundle = recoveryBundle();
    entry.project = m_projectPath;
    entry.name = m_projectPath.isEmpty() ? m_takeName : projectName();
    entry.written = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    entry.pid = static_cast<qint64>(::getpid());
    session::Project snapshot(m_project.sampleRate());
    QString captureError;
    if (captureProjectSnapshot(snapshot, captureError) != SnapshotResult::Ready) {
        cancelSnapshot();
        setError(QStringLiteral("Could not preserve the session checkpoint: %1").arg(captureError));
        return false;
    }
    const auto result = writeRecovery(snapshot, currentView(), entry.bundle.toStdString(),
                                      recoveryIndexPath().toStdString(), entry);
    if (!result.ok) {
        setError(QStringLiteral("Could not preserve the session checkpoint: %1")
                     .arg(QString::fromStdString(result.error)));
        return false;
    }
    m_recoveryWrittenTo = entry.bundle;
    m_graphicsCheckpoint = true;
    index = recoveryIndexPath();
    return true;
}

bool Backend::offerGraphicsRecovery(const QString &index)
{
    m_recoveryOffer.reset();
    emit recoveryChanged();
    const auto path = QFileInfo(index).absoluteFilePath();
    const auto directory = QDir(stateDir() + QStringLiteral("/recovery")).absolutePath();
    if (!index.isEmpty() && QFileInfo(path).absolutePath() == directory) {
        scanRecovery(path);
        if (m_recoveryOffer && m_recoveryOffer->file == path)
            return true;
    }
    setError(QStringLiteral("The session checkpoint for the graphics restart is unavailable."));
    return false;
}

QString Backend::recoveryName() const
{
    return m_recoveryOffer.has_value() ? m_recoveryOffer->name : QString();
}

QString Backend::recoveryWritten() const
{
    if (!m_recoveryOffer.has_value())
        return {};
    const QDateTime when = QDateTime::fromString(m_recoveryOffer->written, Qt::ISODate);
    if (!when.isValid())
        return m_recoveryOffer->written;
    return when.toLocalTime().toString(QStringLiteral("HH:mm 'on' d MMM"));
}

void Backend::recoverSession()
{
    if (!m_recoveryOffer.has_value())
        return;
    if (m_loading) {
        setError(QStringLiteral("wait until the file has finished opening before recovering"));
        return;
    }
    ProjectLoadRequest request;
    request.path = m_recoveryOffer->bundle;
    request.sessionGeneration = m_sessionGeneration;
    request.revision = m_editRevision;
    request.recovery = m_recoveryOffer;
    m_projectLoadRequest = std::move(request);
    m_cancelLoad.store(false, std::memory_order_relaxed);
    m_loading = true;
    emit loadingChanged();
    pumpProjectLoad();
}

void Backend::discardRecovery()
{
    if (!m_recoveryOffer.has_value())
        return;
    const RecoveryEntry offer = *m_recoveryOffer;
    m_recoveryOffer.reset();
    QFile::remove(offer.file);
    (void)io::discardRecovery(offer.bundle.toStdString());
    if (offer.bundle != offer.project) {
        std::error_code ec;
        std::filesystem::remove_all(offer.bundle.toStdString(), ec);
    }
    emit recoveryChanged();
}

// --- the mask ------------------------------------------------------------------------

spectral::MaskShapes Backend::maskFromVariants(const QVariantList &points,
                                               const QVariantList &stamps, spectral::MaskOp op)
{
    spectral::MaskShapes shapes;
    if (!points.isEmpty()) {
        std::vector<spectral::MaskPoint> poly;
        poly.reserve(static_cast<std::size_t>(points.size()));
        for (const QVariant &v : points) {
            const QVariantMap m = v.toMap();
            poly.push_back(spectral::MaskPoint{
                .seconds = static_cast<float>(m.value(QStringLiteral("seconds")).toDouble()),
                .hz = static_cast<float>(m.value(QStringLiteral("hz")).toDouble())});
        }
        shapes.shapes.push_back(spectral::MaskShape::lasso(std::move(poly), op));
    }
    if (!stamps.isEmpty()) {
        std::vector<spectral::MaskStamp> dabs;
        dabs.reserve(static_cast<std::size_t>(stamps.size()));
        for (const QVariant &v : stamps) {
            const QVariantMap m = v.toMap();
            spectral::MaskStamp st;
            st.seconds = static_cast<float>(m.value(QStringLiteral("seconds")).toDouble());
            st.hz = static_cast<float>(m.value(QStringLiteral("hz")).toDouble());
            st.rSeconds = static_cast<float>(std::max(0.0, m.value(QStringLiteral("rSeconds")).toDouble()));
            st.hzLow = static_cast<float>(m.value(QStringLiteral("hzLow")).toDouble());
            st.hzHigh = static_cast<float>(m.value(QStringLiteral("hzHigh")).toDouble());
            if (st.hzHigh < st.hzLow)
                std::swap(st.hzLow, st.hzHigh);
            st.hz = std::clamp(st.hz, st.hzLow, st.hzHigh);
            dabs.push_back(st);
        }
        shapes.shapes.push_back(spectral::MaskShape::stroke(std::move(dabs), op));
    }
    return shapes;
}

void Backend::rebuildMaskShapes()
{
    ++m_maskRevision;
    m_maskShapes.clear();
    for (const spectral::MaskShape &shape : m_mask.shapes) {
        QVariantMap m;
        m.insert(QStringLiteral("subtract"), shape.op == spectral::MaskOp::Subtract);
        m.insert(QStringLiteral("op"), shape.op == spectral::MaskOp::Subtract ? QStringLiteral("cut")
                                     : shape.op == spectral::MaskOp::Intersect ? QStringLiteral("keep")
                                                                               : QStringLiteral("add"));
        if (!shape.polygon.empty()) {
            m.insert(QStringLiteral("kind"), QStringLiteral("lasso"));
            QVariantList pts;
            for (const spectral::MaskPoint &p : shape.polygon) {
                pts.append(QVariantMap{{QStringLiteral("seconds"), static_cast<double>(p.seconds)},
                                       {QStringLiteral("hz"), static_cast<double>(p.hz)}});
            }
            m.insert(QStringLiteral("points"), pts);
        } else if (shape.comb.has_value()) {
            const spectral::HarmonicComb &comb = *shape.comb;
            m.insert(QStringLiteral("kind"), QStringLiteral("comb"));
            m.insert(QStringLiteral("from"), static_cast<double>(comb.fromSeconds));
            m.insert(QStringLiteral("to"), static_cast<double>(comb.toSeconds));
            m.insert(QStringLiteral("fundamentalHz"), static_cast<double>(comb.fundamentalHz));
            m.insert(QStringLiteral("partials"), static_cast<int>(comb.partials));
            m.insert(QStringLiteral("width"), static_cast<double>(comb.width));
            m.insert(QStringLiteral("widthInHz"), comb.widthInHz);
            m.insert(QStringLiteral("includeFundamental"), comb.includeFundamental);
            QVariantList pitch;
            for (const spectral::MaskPoint& p : comb.pitch)
                pitch.append(QVariantMap{{QStringLiteral("seconds"), p.seconds},
                                         {QStringLiteral("hz"), p.hz}});
            m.insert(QStringLiteral("pitch"), pitch);
            // The bands themselves, so the page draws what the engine will cover
            // without redoing the arithmetic.
            QVariantList bands;
            const float nyquist = static_cast<float>(m_project.sampleRate()) / 2.0f;
            for (const auto &[lo, hi] : comb.bands(nyquist)) {
                bands.append(QVariantMap{{QStringLiteral("hzLow"), static_cast<double>(lo)},
                                         {QStringLiteral("hzHigh"), static_cast<double>(std::min(hi, nyquist))}});
            }
            m.insert(QStringLiteral("bands"), bands);
        } else {
            m.insert(QStringLiteral("kind"), QStringLiteral("stroke"));
            QVariantList dabs;
            for (const spectral::MaskStamp &st : shape.stamps) {
                dabs.append(QVariantMap{{QStringLiteral("seconds"), static_cast<double>(st.seconds)},
                                        {QStringLiteral("hz"), static_cast<double>(st.hz)},
                                        {QStringLiteral("rSeconds"), static_cast<double>(st.rSeconds)},
                                        {QStringLiteral("hzLow"), static_cast<double>(st.hzLow)},
                                        {QStringLiteral("hzHigh"), static_cast<double>(st.hzHigh)}});
            }
            m.insert(QStringLiteral("stamps"), dabs);
        }
        m_maskShapes.append(m);
    }
    emit maskChanged();
}

// The time selection grows to hold everything the mask adds, and the band to its
// frequencies, so the Repair panel's numbers and the "needs a selection" guards
// read true for what the repair will do.
void Backend::selectMaskBounds()
{
    const session::Track *track = m_project.track(m_focused);
    const auto tb = m_mask.timeBounds();
    if (track == nullptr || !tb.has_value())
        return;
    const double rate = m_project.sampleRate();
    const session::FramePos length = track->length();
    session::FramePos from = static_cast<session::FramePos>(std::floor(tb->first * rate));
    session::FramePos to = static_cast<session::FramePos>(std::ceil(tb->second * rate));
    // Room for the feather's columns either side.
    const auto pad = static_cast<session::FramePos>(m_maskFeather) * m_stft.hop;
    from = std::clamp<session::FramePos>(from - pad, 0, length);
    to = std::clamp<session::FramePos>(to + pad, 0, length);
    if (!m_selection.empty() && m_selection.track == m_focused) {
        from = std::min(from, m_selection.start);
        to = std::max(to, m_selection.end);
    }
    if (to > from)
        select(from, to, false, static_cast<int>(m_focused));
    if (const auto hb = m_mask.hzBounds(); hb.has_value()) {
        double low = hb->first;
        double high = hb->second;
        if (!bandEmpty()) {
            low = std::min(low, m_bandLowHz);
            high = std::max(high, m_bandHighHz);
        }
        setBand(low, high);
    }
}

void Backend::setMaskFeather(int cells)
{
    cells = std::clamp(cells, 0, 64);
    if (cells == m_maskFeather)
        return;
    m_maskFeather = cells;
    emit maskChanged();
}

void Backend::addMaskLasso(const QVariantList &points, bool subtract, bool intersect)
{
    if (m_project.track(m_focused) == nullptr) {
        setError(QStringLiteral("nothing to mask: no take is focused; click a track to focus it"));
        return;
    }
    if (points.size() < 3) {
        setError(QStringLiteral("a lasso needs three points; keep dragging before you release"));
        return;
    }
    spectral::MaskShapes shapes = maskFromVariants(points, {}, maskOp(subtract, intersect));
    m_mask.shapes.push_back(std::move(shapes.shapes.front()));
    rebuildMaskShapes();
    selectMaskBounds();
}

void Backend::addMaskStroke(const QVariantList &stamps, bool subtract, bool intersect)
{
    if (m_project.track(m_focused) == nullptr) {
        setError(QStringLiteral("nothing to mask: no take is focused; click a track to focus it"));
        return;
    }
    if (stamps.isEmpty()) {
        setError(QStringLiteral("a stroke needs a dab"));
        return;
    }
    spectral::MaskShapes shapes = maskFromVariants({}, stamps, maskOp(subtract, intersect));
    m_mask.shapes.push_back(std::move(shapes.shapes.front()));
    rebuildMaskShapes();
    selectMaskBounds();
}

double Backend::partialNear(double seconds, double hz)
{
    const session::Track *track = m_project.track(m_focused);
    if (track == nullptr || track->clips.empty() || !(hz > 0.0))
        return hz;
    const double rate = m_project.sampleRate();
    const session::FramePos length = track->length();
    const auto centre = std::clamp<session::FramePos>(
        static_cast<session::FramePos>(std::llround(seconds * rate)), 0, length);
    // One frame's worth either side of the pick is all the refinement reads.
    const auto half = static_cast<session::FramePos>(m_stft.fftSize);
    const session::FramePos from = std::max<session::FramePos>(0, centre - half);
    const session::FramePos to = std::min<session::FramePos>(length, centre + half);
    if (to <= from)
        return hz;
    const session::AudioBuffer region = renderFocusedRange(from, to);
    return static_cast<double>(spectral::refinePartial(region, m_project.sampleRate(), m_stft,
                                                       static_cast<std::size_t>(centre - from),
                                                       static_cast<float>(hz)));
}

void Backend::addMaskHarmonics(double fromSeconds, double toSeconds, double hz, int partials,
                               double width, bool widthInHz, bool includeFundamental, bool subtract,
                               bool snap, bool intersect, bool followPitch, double seedSeconds)
{
    const session::Track *track = m_project.track(m_focused);
    if (track == nullptr) {
        setError(QStringLiteral("nothing to mask: no take is focused; click a track to focus it"));
        return;
    }
    const double rate = m_project.sampleRate();
    const double nyquist = rate / 2.0;
    if (!std::isfinite(hz) || !(hz > 0.0) || hz >= nyquist
        || !std::isfinite(fromSeconds) || !std::isfinite(toSeconds) || !std::isfinite(seedSeconds)) {
        setError(QStringLiteral("a harmonic series needs a fundamental below Nyquist"));
        return;
    }
    if (partials < 1 || !std::isfinite(width) || !(width > 0.0)) {
        setError(QStringLiteral("a harmonic series needs at least one partial, and a width"));
        return;
    }
    if (!includeFundamental && partials < 2) {
        setError(QStringLiteral("the overtones alone start at the second partial"));
        return;
    }
    // No span given: the time selection on this take, or the whole of it.
    if (toSeconds <= fromSeconds) {
        if (!m_selection.empty() && m_selection.track == m_focused) {
            fromSeconds = static_cast<double>(m_selection.start) / rate;
            toSeconds = static_cast<double>(m_selection.end) / rate;
        } else {
            fromSeconds = 0.0;
            toSeconds = static_cast<double>(track->length()) / rate;
        }
    }
    const double takeSeconds = static_cast<double>(track->length()) / rate;
    fromSeconds = std::clamp(fromSeconds, 0.0, takeSeconds);
    toSeconds = std::clamp(toSeconds, 0.0, takeSeconds);
    if (toSeconds <= fromSeconds) {
        setError(QStringLiteral("the harmonic series is past the end of the take"));
        return;
    }
    spectral::HarmonicComb comb;
    comb.fromSeconds = static_cast<float>(fromSeconds);
    comb.toSeconds = static_cast<float>(toSeconds);
    const double seed = seedSeconds < 0 ? (fromSeconds + toSeconds) / 2 :
                                         std::clamp(seedSeconds, fromSeconds, toSeconds);
    comb.fundamentalHz = static_cast<float>(snap && !followPitch ? partialNear(seed, hz) : hz);
    comb.partials = static_cast<std::uint32_t>(std::clamp(partials, 1, 256));
    comb.width = static_cast<float>(width);
    comb.widthInHz = widthInHz;
    comb.includeFundamental = includeFundamental;
    if (followPitch) {
        if (m_pitchTracking) {
            setError(QStringLiteral("pitch tracking is already running; cancel it before picking again"));
            return;
        }
        PitchRequest request;
        request.comb = std::move(comb);
        request.op = maskOp(subtract, intersect);
        request.config = stftConfig();
        request.track = m_focused;
        request.selection = m_selection;
        request.generation = m_sessionGeneration;
        request.revision = m_editRevision;
        request.maskRevision = m_maskRevision;
        request.seed = static_cast<session::FramePos>(std::llround(seed * rate));
        request.bandLow = m_bandLowHz;
        request.bandHigh = m_bandHighHz;
        m_pitchRequest = std::move(request);
        m_pitchTracking = true;
        emit maskChanged();
        pumpPitch();
        return;
    }
    m_mask.shapes.push_back(spectral::MaskShape::harmonics(comb, maskOp(subtract, intersect)));
    rebuildMaskShapes();
    selectMaskBounds();
}

bool Backend::pitchContextCurrent(const PitchRequest& r) const
{
    const auto config = stftConfig();
    return r.generation == m_sessionGeneration && r.revision == m_editRevision
        && r.maskRevision == m_maskRevision && r.track == m_focused
        && r.selection.start == m_selection.start && r.selection.end == m_selection.end
        && r.selection.track == m_selection.track && r.bandLow == m_bandLowHz
        && r.bandHigh == m_bandHighHz && r.config.fftSize == config.fftSize
        && r.config.hop == config.hop && r.config.window == config.window;
}

void Backend::cancelPitchTracking()
{
    m_cancelPitch.store(true, std::memory_order_relaxed);
    m_pitchRequest.reset();
    if (!m_pitchActive)
        m_pitchTracking = false;
    emit maskChanged();
}

void Backend::pumpPitch()
{
    if (m_shuttingDown || m_pitchActive || !m_pitchRequest || m_loading || m_saving
        || m_busy.load(std::memory_order_acquire))
        return;
    PitchRequest request = std::move(*m_pitchRequest);
    m_pitchRequest.reset();
    const session::Track* track = m_project.track(request.track);
    if (!track || !pitchContextCurrent(request)) {
        m_pitchTracking = false;
        emit maskChanged();
        return;
    }
    const auto rate = m_project.sampleRate();
    const auto from = static_cast<session::FramePos>(std::llround(request.comb.fromSeconds * rate));
    const auto to = std::min(track->length(),
        static_cast<session::FramePos>(std::llround(request.comb.toSeconds * rate)));
    const auto pad = static_cast<session::FramePos>(request.config.fftSize);
    const auto begin = std::max<session::FramePos>(0, from - pad);
    const auto end = std::min(track->length(), to + pad);
    const auto clips = session::edits::copyRange(track->clips, begin, end);
    const auto channels = static_cast<std::uint32_t>(std::max(1, trackChannels(*track)));
    const session::Project snapshot = m_project;
    joinWorker();
    m_busy.store(true, std::memory_order_release);
    m_cancelPitch.store(false, std::memory_order_relaxed);
    m_pitchActive = true;
    m_worker = std::thread([this, request = std::move(request), snapshot, clips,
                            channels, rate, from, to, begin]() mutable {
        try {
        if (!m_cancelPitch.load(std::memory_order_relaxed)) {
            const auto audio = session::renderTimeline(snapshot, clips, channels, &m_cancelPitch);
            request.comb.pitch = spectral::followPartial(audio, rate, request.config,
                static_cast<std::size_t>(from - begin), static_cast<std::size_t>(to - begin),
                static_cast<std::size_t>(std::max<session::FramePos>(0, request.seed - begin)),
                request.comb.fundamentalHz, &m_cancelPitch);
            for (spectral::MaskPoint& p : request.comb.pitch)
                p.seconds = std::clamp(static_cast<float>(p.seconds + static_cast<double>(begin) / rate),
                                       request.comb.fromSeconds, request.comb.toSeconds);
            if (!request.comb.pitch.empty())
                request.comb.fundamentalHz = request.comb.fundamentalAt(
                    static_cast<float>(static_cast<double>(request.seed) / rate));
        }
        } catch (const std::exception&) { request.comb.pitch.clear(); }
        PitchResult result{std::move(request), m_cancelPitch.load(std::memory_order_relaxed)};
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_pendingPitch.push_back(std::move(result));
            m_finishedJobs.push_back(Job::Pitch);
        }
        m_busy.store(false, std::memory_order_release);
        emit workerFinished();
    });
}

void Backend::finishPitch(PitchResult result)
{
    m_pitchActive = false;
    m_pitchTracking = false;
    if (!m_shuttingDown && !result.cancelled && !m_cancelPitch.load(std::memory_order_relaxed)
        && pitchContextCurrent(result.request)) {
        if (result.request.comb.pitch.empty() || !result.request.comb.validPitch()) {
            setError(QStringLiteral("no stable peak at the pick; choose a clearer partial or a larger FFT"));
        } else {
            m_mask.shapes.push_back(spectral::MaskShape::harmonics(std::move(result.request.comb), result.request.op));
            rebuildMaskShapes();
            selectMaskBounds();
        }
    }
    emit maskChanged();
}

void Backend::undoMaskShape()
{
    if (m_pitchTracking)
        cancelPitchTracking();
    if (m_mask.shapes.empty())
        return;
    m_mask.shapes.pop_back();
    rebuildMaskShapes();
}

void Backend::clearMask()
{
    if (m_pitchTracking)
        cancelPitchTracking();
    if (m_mask.shapes.empty())
        return;
    m_mask.shapes.clear();
    rebuildMaskShapes();
}

void Backend::spotHeal(const QVariantList &stamps)
{
    const session::Track *track = m_project.track(m_focused);
    if (track == nullptr || track->clips.empty()) {
        setError(QStringLiteral("nothing to heal: no take is focused; click a track to focus it"));
        return;
    }
    if (stamps.isEmpty()) {
        setError(QStringLiteral("a spot heal needs a dab"));
        return;
    }
    RepairRequest request;
    request.kind = RepairKind::Heal;
    request.mask = maskFromVariants({}, stamps, spectral::MaskOp::Add);
    request.mask->featherBins = static_cast<std::uint32_t>(m_maskFeather);
    request.mask->featherColumns = static_cast<std::uint32_t>(m_maskFeather);
    const auto tb = request.mask->timeBounds();
    if (!tb.has_value())
        return;
    const double rate = m_project.sampleRate();
    const session::FramePos length = track->length();
    const auto pad = static_cast<session::FramePos>(m_maskFeather + 1) * m_stft.hop;
    const auto from = std::clamp<session::FramePos>(
        static_cast<session::FramePos>(std::floor(tb->first * rate)) - pad, 0, length);
    const auto to = std::clamp<session::FramePos>(
        static_cast<session::FramePos>(std::ceil(tb->second * rate)) + pad, 0, length);
    if (to <= from) {
        setError(QStringLiteral("the spot is past the end of the take"));
        return;
    }
    request.region = std::make_pair(from, to);
    startRepair(std::move(request));
}

void Backend::setRepairChannel(const QString& channel)
{
    if (channel == m_repairChannel) return;
    if (channel != "all" && channel != "left" && channel != "right" && channel != "mid" && channel != "side") {
        setError(QStringLiteral("unknown repair channel")); return;
    }
    m_repairChannel = channel;
    emit repairChanged();
}

void Backend::setSpectralView(const QString& view)
{
    if (view == m_spectralView) return;
    if (view != "separate" && view != "overlay" && view != "mid-side") {
        setError(QStringLiteral("unknown spectral view")); return;
    }
    const bool rebuild = (view == "mid-side") != (m_spectralView == "mid-side");
    m_spectralView = view;
    emit spectralConfigChanged();
    if (rebuild) {
        m_spectralStale = true; emit spectralChanged();
        if (m_spectralWanted) m_spectralTimer->start();
    }
}

void Backend::declipSpectral(double thresholdDb)
{
    if (!std::isfinite(thresholdDb) || thresholdDb < -40 || thresholdDb > 12) {
        setError(QStringLiteral("clipping threshold must be between -40 and +12 dBFS")); return;
    }
    RepairRequest request;
    request.kind = RepairKind::Declip;
    request.thresholdDb = thresholdDb;
    startRepair(std::move(request));
}

void Backend::dereverbSpectral(double amountDb, double decaySeconds, double strength)
{
    if (!std::isfinite(amountDb) || amountDb < 0 || amountDb > 40
        || !std::isfinite(decaySeconds) || decaySeconds < .05 || decaySeconds > 3
        || !std::isfinite(strength) || strength < 0 || strength > 2) {
        setError(QStringLiteral("invalid late-reverb reduction settings")); return;
    }
    RepairRequest request;
    request.kind = RepairKind::Dereverb;
    request.amountDb = amountDb;
    request.decaySeconds = decaySeconds;
    request.strength = strength;
    startRepair(std::move(request));
}

bool Backend::reduceRoomEcho(int track, double amount)
{
    if (!std::isfinite(amount) || amount < 0 || amount > 1) { setError(tr("Set Room echo from 0 to 1.")); return false; }
    const auto id = trackArg(track);
    const auto* t = m_project.track(id);
    if (!t || t->clips.empty()) { setError(tr("This track has no audio to change.")); return false; }
    if (id != m_focused) { setError(tr("Click the track before reducing its room echo.")); return false; }
    RepairRequest request;
    request.kind = RepairKind::Dereverb;
    request.plain = true;
    request.amountDb = 4 + 16 * amount;
    request.decaySeconds = 0.6;
    request.strength = 1;
    if (m_selection.empty() || m_selection.track != id) request.region = std::pair{session::FramePos(0), t->length()};
    setError({});
    startRepair(std::move(request));
    if (m_lastError.isEmpty()) recordStep({{"kind", "roomEcho"}, {"on", true}, {"amount", std::round(amount * 100)}});
    return m_lastError.isEmpty();
}

void Backend::cancelRepair()
{
    if (!m_repairing) return;
    m_cancelRepair.store(true, std::memory_order_relaxed);
    if (m_queuedRepair) {
        m_queuedRepair.reset();
        m_repairing = false;
        m_repairSummary = QStringLiteral("Repair cancelled; no audio changed.");
        emit repairChanged();
    }
}

void Backend::startRepair(RepairRequest request)
{
    const session::Track *track = m_project.track(m_focused);
    const QString what = request.kind == RepairKind::Heal ? QStringLiteral("repair")
                       : request.kind == RepairKind::Denoise ? QStringLiteral("denoise")
                       : request.kind == RepairKind::Declip ? QStringLiteral("restore clipped peaks")
                       : request.kind == RepairKind::Dereverb ? QStringLiteral("reduce late reverb")
                       : request.kind == RepairKind::Clicks ? QStringLiteral("repair clicks")
                                                             : QStringLiteral("attenuate");
    if (track == nullptr || track->clips.empty()) {
        setError(QStringLiteral("nothing to repair: no take is focused; click a track to focus it"));
        return;
    }
    if (m_selection.empty() && !request.region.has_value()) {
        setError(QStringLiteral("%1 needs a selection: drag across a lane to select a time range first").arg(what));
        return;
    }
    if (!request.plain && !request.mask.has_value() && !m_mask.empty()) {
        request.mask = m_mask;
        request.mask->featherBins = static_cast<std::uint32_t>(m_maskFeather);
        request.mask->featherColumns = static_cast<std::uint32_t>(m_maskFeather);
    }
    if (m_repairing) {
        setError(QStringLiteral("a repair is still running; wait for it to finish"));
        return;
    }
    if (recording()) { setError(QStringLiteral("stop recording before repairing audio")); return; }
    request.scope = request.plain ? spectral::ChannelScope::All
                  : m_repairChannel == "left" ? spectral::ChannelScope::Left
                  : m_repairChannel == "right" ? spectral::ChannelScope::Right
                  : m_repairChannel == "mid" ? spectral::ChannelScope::Mid
                  : m_repairChannel == "side" ? spectral::ChannelScope::Side
                                             : spectral::ChannelScope::All;
    if (request.scope != spectral::ChannelScope::All && trackChannels(*track) != 2) {
        setError(QStringLiteral("choose All channels for a take that is not stereo")); return;
    }
    request.generation = m_sessionGeneration; request.revision = m_editRevision;
    request.requestedTrack = m_focused;
    request.requestedSelection = m_selection;
    request.requestedState = session::captureState(m_project);
    request.band = request.plain ? spectral::Band{} : spectral::Band{.lowHz = float(m_bandLowHz), .highHz = float(m_bandHighHz)};
    m_repairSummary.clear();
    m_cancelRepair.store(false, std::memory_order_relaxed);
    if (m_shuttingDown || m_loading || m_saving || m_busy.load(std::memory_order_acquire)) {
        // The slot is taken (a peak rebuild, a save, the spectral display): wait
        // for it rather than making the user ask twice. drainWorker runs it.
        m_queuedRepair = request;
        m_repairing = true;
        emit repairChanged();
        return;
    }
    runRepair(request);
}

void Backend::pumpRepair()
{
    if (m_shuttingDown || m_loading || m_saving || !m_queuedRepair.has_value()
        || m_busy.load(std::memory_order_acquire))
        return;
    const RepairRequest request = *m_queuedRepair;
    m_queuedRepair.reset();
    const session::Track *track = m_project.track(m_focused);
    if (request.generation != m_sessionGeneration || request.revision != m_editRevision
        || track == nullptr || track->clips.empty() || m_focused != request.requestedTrack
        || session::captureState(m_project) != request.requestedState
        || (!request.region && (m_selection.start != request.requestedSelection.start
                              || m_selection.end != request.requestedSelection.end))
        || (m_selection.empty() && !request.region.has_value())) {
        m_repairing = false;
        emit repairChanged();
        setError(QStringLiteral("the repair was dropped: the take or selection changed while it waited"));
        return;
    }
    runRepair(request);
}

void Backend::runRepair(const RepairRequest &request)
{
    const session::Track *track = m_project.track(m_focused);
    const session::FramePos length = track->length();
    const session::FramePos wantStart = request.region ? request.region->first : m_selection.start;
    const session::FramePos wantEnd = request.region ? request.region->second : m_selection.end;
    const session::FramePos start = std::clamp<session::FramePos>(wantStart, 0, length);
    const session::FramePos end = std::clamp<session::FramePos>(wantEnd, 0, length);
    if (end <= start) {
        m_repairing = false;
        emit repairChanged();
        setError(QStringLiteral("the selection is past the end of the take"));
        return;
    }

    // Copy the immutable model here; render and process the region on the worker.
    const spectral::StftConfig config = stftConfig();
    const auto pad = std::max<session::FramePos>(config.fftSize,
        request.kind == RepairKind::Dereverb ? session::FramePos(m_project.sampleRate() * request.decaySeconds)
        : request.kind == RepairKind::Declip ? 4096 : 0);
    const session::FramePos from = std::max<session::FramePos>(0, start - pad);
    const session::FramePos to = std::min<session::FramePos>(length, end + pad);
    auto snapshot = m_project;
    const auto channels = std::uint32_t(std::max(1, trackChannels(*track)));

    RepairResult job;
    job.generation = m_sessionGeneration; job.revision = m_editRevision;
    job.track = track->id;
    job.start = start;
    job.label = request.kind == RepairKind::Heal ? (request.region ? "repair brush" : "repair")
              : request.kind == RepairKind::Denoise ? "denoise"
              : request.kind == RepairKind::Declip ? "restore clipped peaks"
              : request.kind == RepairKind::Dereverb ? (request.plain ? "reduce room echo" : "reduce late reverb")
              : request.kind == RepairKind::Clicks ? (request.clicks.size() == 1 && !request.keepClicks.empty()
                                                      ? "repair click" : "repair clicks")
                                                   : "attenuate";
    job.keepClicks = request.keepClicks;
    job.expected = session::captureState(m_project);
    const spectral::Region where{.start = static_cast<std::size_t>(start - from),
                                 .end = static_cast<std::size_t>(end - from)};
    const spectral::Band band = request.band;
    const std::uint32_t rate = m_project.sampleRate();
    const spectral::NoisePrint print = request.kind == RepairKind::Denoise ? m_noisePrint
                                                                           : spectral::NoisePrint{};
    // The shapes are in the take's seconds; the buffer starts at `from`.
    const spectral::MaskSource mask =
        request.mask ? spectral::shapeMask(request.mask->shifted(-static_cast<double>(from) / rate))
                     : spectral::bandMask(band);

    joinWorker();
    m_busy.store(true, std::memory_order_release);
    m_repairing = true;
    emit repairChanged();

    m_worker = std::thread([this, job = std::move(job), snapshot = std::move(snapshot), to, channels, where, mask,
                            rate, request, print, config, from]() mutable {
        try {
        const auto checkCancelled = [&] {
            if (m_cancelRepair.load(std::memory_order_relaxed))
                throw std::runtime_error("repair cancelled");
        };
        auto region = renderRange(snapshot, job.track, from, to, channels, &m_cancelRepair);
        checkCancelled();
        // Only a channel-scoped repair needs the original for delta blending.
        // All-channel repairs can work directly on this private rendered region.
        session::AudioBuffer scoped;
        if (request.scope != spectral::ChannelScope::All)
            scoped = spectral::repairChannels(region, request.scope, &m_cancelRepair);
        auto& selected = request.scope == spectral::ChannelScope::All ? region : scoped;
        checkCancelled();
        auto selectedPrint = print;
        if (request.kind == RepairKind::Denoise) {
            const std::string scope = request.scope == spectral::ChannelScope::Left ? "left"
                : request.scope == spectral::ChannelScope::Right ? "right"
                : request.scope == spectral::ChannelScope::Mid ? "mid"
                : request.scope == spectral::ChannelScope::Side ? "side" : "all";
            if (selectedPrint.empty())
                throw std::runtime_error("the noise sample was cleared before this repair ran; learn the noise again");
            if (selectedPrint.channelScope != scope) {
                if (selectedPrint.channelScope == "all" && (scope == "left" || scope == "right")) {
                    const std::size_t ch = std::min<std::size_t>(scope == "right" ? 1 : 0, selectedPrint.magnitude.size() - 1);
                    selectedPrint.magnitude = {selectedPrint.magnitude[ch]};
                } else {
                    throw std::runtime_error("learn the noise in the selected channel scope first");
                }
            }
        }
        switch (request.kind) {
        case RepairKind::Heal:
            spectral::heal(selected, rate, where, mask, config, 4, &m_cancelRepair);
            break;
        case RepairKind::Clicks: {
            // The clicks are in the take's frames; the buffer starts at `from`.
            std::vector<spectral::Click> local = request.clicks;
            for (spectral::Click &c : local)
                c.frame -= static_cast<std::size_t>(from);
            spectral::repairClicks(selected, local, &m_cancelRepair);
            break;
        }
        case RepairKind::Denoise:
            spectral::reduceNoise(selected, rate, where, mask, selectedPrint,
                                  static_cast<float>(request.amountDb),
                                  static_cast<float>(request.strength), config, &m_cancelRepair);
            break;
        case RepairKind::Declip: {
            const auto r = spectral::restoreClippedPeaks(selected, rate, where, mask,
                float(std::pow(10.0, request.thresholdDb / 20)), config, &m_cancelRepair);
            job.cancelled = r.cancelled; job.noChange = !r.changed;
            job.summary = QStringLiteral("%1 clipped regions restored; %2 skipped.%3")
                .arg(r.restored).arg(r.skipped).arg(r.changed ? QString() : QStringLiteral(" No audio changed."));
            break;
        }
        case RepairKind::Dereverb: {
            const auto r = spectral::reduceReverb(selected, rate, where, mask, float(request.amountDb),
                float(request.decaySeconds), float(request.strength), config, &m_cancelRepair);
            job.cancelled = r.cancelled; job.noChange = !r.changed;
            job.summary = r.changed ? QStringLiteral("Late-reverb reduction applied.") : QStringLiteral("No late-tail reduction applied; no audio changed.");
            break;
        }
        case RepairKind::Attenuate:
            spectral::attenuate(selected, rate, where, mask, static_cast<float>(request.gainDb),
                                config, &m_cancelRepair);
            break;
        }
        checkCancelled();
        if (request.scope != spectral::ChannelScope::All)
            spectral::mergeRepairChannels(region, selected, request.scope, &m_cancelRepair);
        scoped = {};
        checkCancelled();
        // Only the selection itself goes back; the context was for the windows.
        const std::size_t frames = where.end - where.start;
        if (where.start == 0 && frames == region.frames()) {
            job.audio = std::move(region);
        } else {
            render::AudioStore trimmed(region.channels(), frames);
            for (std::uint32_t c = 0; c < region.channels(); ++c) {
                const auto src = std::as_const(region).plane(c);
                for (std::size_t at = 0; at < frames;) {
                    checkCancelled();
                    const auto count = std::min(render::AudioStore::blockFrames, frames - at);
                    trimmed.write(c, at, src.subspan(where.start + at, count));
                    at += count;
                    if (region.diskStorage()) region.diskStorage()->releaseMappedPages();
                }
            }
            job.audio = trimmed.finish();
        }
        region = {};
        job.audio.storeOnDisk(session::AudioBuffer::kDiskThreshold, &m_cancelRepair);
        job.ok = true;
        } catch (const std::exception& e) { job.error = e.what(); }
        catch (...) { job.error = "audio repair failed"; }
        job.cancelled = job.cancelled || m_cancelRepair.load(std::memory_order_relaxed);
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_pendingRepair.push_back(std::move(job));
            m_finishedJobs.push_back(Job::Repair);
        }
        m_busy.store(false, std::memory_order_release);
        emit workerFinished();
    });
}

void Backend::finishRepair(RepairResult result)
{
    if (result.ok && !result.cancelled && !m_shuttingDown && !m_cancelRepair.load(std::memory_order_relaxed)
        && !m_applyingAutomationEdit && !m_applyingPluginEdit) {
        auto pending = std::make_shared<RepairResult>(std::move(result));
        if (deferAutomationEdit([this, pending] { finishRepair(std::move(*pending)); return true; },
            [this] { m_repairing = false; emit repairChanged(); })) return;
        result = std::move(*pending);
    }
    m_repairing = false;
    result.cancelled = result.cancelled || m_cancelRepair.load(std::memory_order_relaxed);
    m_repairSummary = result.cancelled ? QStringLiteral("Repair cancelled; no audio changed.") : result.summary;
    emit repairChanged();
    if (result.cancelled) return;
    if (!result.ok) {
        setError(QString::fromStdString(result.error));
        return;
    }
    if (result.generation != m_sessionGeneration || result.revision != m_editRevision
        || session::captureState(m_project) != result.expected) {
        setError(QStringLiteral("the take changed while the repair ran; nothing applied"));
        return;
    }
    if (result.noChange) return;
    session::Track *track = m_project.track(result.track);
    if (track == nullptr)
        return;

    // A new source, laid over the range as an ordinary clip. The audio thread reads
    // sources through the plan, so this joins first like an import does.
    joinWorker();
    const auto frames = static_cast<session::FramePos>(result.audio.frames());
    const session::SessionState before = captureEditState();
    const session::SourceId id = m_project.addSource(
        track->name + " (" + result.label + ")", std::string(), std::move(result.audio),
        std::nullopt, session::Uuid::generate());
    // Retain membership at each timeline interval, including independent gaps.
    const session::Clip clip{.source = id, .timelineStart = result.start, .length = frames};
    session::edits::overwriteRendered(track->clips, clip, track->clips);
    commit(std::move(result.label), result.track, before);
    // The commit marked the display stale and cleared the click list; a single
    // click's heal moved none of the others, so they come back.
    if (!result.keepClicks.empty()) {
        const std::size_t n = result.keepClicks.size();
        setClicks(std::move(result.keepClicks),
                  n == 1 ? QStringLiteral("1 click left.") : QStringLiteral("%1 clicks left.").arg(n));
    }
}

// --- background waveform sample edits -------------------------------------------

void Backend::reverseSelection()
{
    if (m_selection.channel >= 0) startChannelEdit(session::ChannelEdit::Reverse);
    else startSampleEdit(SampleEditKind::Reverse);
}
void Backend::crossfadeSelection(bool equalPower)
{
    if (m_selection.channel >= 0) { setError(QStringLiteral("choose All channels for this whole-track edit")); return; }
    startSampleEdit(equalPower ? SampleEditKind::CrossfadePower : SampleEditKind::CrossfadeLinear);
}
QVariantMap Backend::crossfadeTarget(int trackId, int index) const
{
    const auto* track = m_project.track(trackArg(trackId));
    if (!track) return {};
    if (index < 0) {
        const auto frame = m_selection.empty() ? m_playhead : m_selection.start;
        for (std::size_t i = 0; i < track->clips.size(); ++i)
            if (track->clips[i].covers(frame)) { index = static_cast<int>(i); break; }
    }
    if (index < 0) return {};
    const auto target = session::editableCrossfade(m_project, track->id, static_cast<std::size_t>(index));
    if (!target) return {};
    const auto& r = target->recipe;
    return {{"valid", true}, {"track", static_cast<int>(track->id)}, {"clip", index},
        {"start", qint64(r.start)}, {"end", qint64(r.end)}, {"join", qint64(r.right.timelineStart)},
        {"minimum", qint64(target->minimum)}, {"maximum", qint64(target->maximum)},
        {"equalPower", r.equalPower}, {"stamp", automationStamp()}};
}

void Backend::adjustCrossfade(qint64 start, qint64 end, bool power, const QString& stamp, int track, int clip)
{
    setError({});
    const auto target = crossfadeTarget(track, clip);
    if (!automationStampCurrent(stamp) || track != static_cast<int>(m_focused) || target.isEmpty()) {
        setError(tr("The crossfade or neighbouring clips changed. Reopen the editor or undo those edits.")); return;
    }
    if (start < target.value("minimum").toLongLong() || start >= target.value("join").toLongLong()
        || end <= target.value("join").toLongLong() || end > target.value("maximum").toLongLong()) {
        setError(tr("Keep both handles around the join and inside the available source audio.")); return;
    }
    startSampleEdit(power ? SampleEditKind::AdjustCrossfadePower : SampleEditKind::AdjustCrossfadeLinear,
        0, static_cast<std::size_t>(clip), start, end);
}

QVariantMap Backend::stretchTarget() const
{
    const auto* track=m_project.track(m_focused);
    if(!track)return {};
    const auto frame=m_selection.empty()?m_playhead:m_selection.start;
    // The clip under the playhead, or a track's only clip wherever the playhead is
    // (at its end after playing it through): the same clip Length shows.
    const bool only=track->clips.size()==1;
    for(std::size_t index=0;index<track->clips.size();++index) {
        const auto& clip=track->clips[index];if(!only && !clip.covers(frame))continue;
        const auto* source=m_project.source(clip.source);
        qint64 maximum=clip.length>std::numeric_limits<qint64>::max()/4?std::numeric_limits<qint64>::max():clip.length*4;
        maximum=std::min<qint64>(maximum,std::numeric_limits<qint64>::max()-clip.timelineStart);
        if(index+1<track->clips.size())maximum=std::min<qint64>(maximum,track->clips[index+1].timelineStart-clip.timelineStart);
        session::ElasticSpec input;
        try { input = session::elasticInput(clip); } catch (const std::exception&) { return {}; }
        const auto liveMaximum = std::min<qint64>(maximum,input.inputFrames > std::numeric_limits<qint64>::max()/2 ? std::numeric_limits<qint64>::max() : input.inputFrames*2);
        return {{"track",static_cast<int>(track->id)},{"clip",static_cast<int>(index)},
            {"elastic",clip.elastic.has_value()},{"semitones",clip.elastic ? clip.elastic->semitones : 0.0f},
            {"originalFrames",qint64(input.inputFrames)},
            {"liveMinimum",std::max<qint64>(m_project.sampleRate()/20,(input.inputFrames+1)/2)},{"liveMaximum",liveMaximum},
            {"name",source?QString::fromStdString(source->name):QString{}},{"start",static_cast<qint64>(clip.timelineStart)},
            {"length",static_cast<qint64>(clip.length)},{"minimum",std::max<qint64>(m_project.sampleRate()/20,clip.length/4+(clip.length%4!=0))},
            {"maximum",maximum},{"stamp",automationStamp()},
            {"valid",source && source->audio.channels()<=2 && clip.length>=m_project.sampleRate()/20}};
    }
    return {};
}

void Backend::setFocusedClipElastic(qint64 frames, double semitones, const QString& stamp, int track, int index)
{
    if (deferAutomationEdit([this, frames, semitones, stamp, track, index] { setFocusedClipElastic(frames, semitones, stamp, track, index); return true; })) return;
    if (recording() || loading() || sampleEditing()) { setError(tr("Finish recording or the current audio edit first.")); return; }
    const auto target = stretchTarget();
    if (!automationStampCurrent(stamp) || target.isEmpty() || track != target.value("track").toInt()
        || index != target.value("clip").toInt()) {
        setError(tr("The selected clip changed. Reopen Pitch and duration and try again.")); return;
    }
    if (!std::isfinite(semitones) || semitones < -12 || semitones > 12) { setError(tr("Choose pitch from -12 to +12 semitones.")); return; }
    const auto before = captureEditState();
    const auto error = session::setElasticClip(m_project,m_focused,std::size_t(index),frames,float(semitones));
    if (!error.empty()) { setError(QString::fromStdString(error)); return; }
    setError({});
    commit("clip pitch and duration",m_focused,before);
}

void Backend::stretchFocusedClip(qint64 frames, const QString& stamp, int track, int index)
{
    const auto target=stretchTarget();
    if(!automationStampCurrent(stamp) || target.isEmpty() || track!=target.value("track").toInt()
        || index!=target.value("clip").toInt()) {
        setError(tr("The selected clip changed. Reopen Stretch and try again."));return;
    }
    if(!target.value("valid").toBool() || frames<target.value("minimum").toLongLong()
        || frames>target.value("maximum").toLongLong()) {
        setError(tr("Choose 25–400% duration, at least 50 ms, without overlapping the next clip."));return;
    }
    startSampleEdit(SampleEditKind::Stretch,frames,static_cast<std::size_t>(index));
}

QString Backend::sampleEditName(SampleEditKind kind)
{
    switch (kind) {
    case SampleEditKind::Normalize: return QStringLiteral("normalize");
    case SampleEditKind::VoiceCleanup: return QStringLiteral("voice cleanup");
    case SampleEditKind::ApplyEffect: return QStringLiteral("apply effect");
    case SampleEditKind::Stretch: return QStringLiteral("stretch clip");
    case SampleEditKind::GroupStretch: return QStringLiteral("stretch selected clips");
    case SampleEditKind::AdjustCrossfadeLinear: case SampleEditKind::AdjustCrossfadePower:
        return QStringLiteral("adjust crossfade");
    case SampleEditKind::ProjectRate: return QStringLiteral("change project sample settings");
    case SampleEditKind::Channels: return QStringLiteral("edit selected channels");
    case SampleEditKind::MapChannels: return QStringLiteral("map track channels");
    case SampleEditKind::InsertSilence: return QStringLiteral("insert silence");
    case SampleEditKind::PasteNew: return QStringLiteral("paste to new track");
    case SampleEditKind::MultitrackPaste: return QStringLiteral("paste tracks");
    case SampleEditKind::Reverse: return QStringLiteral("reverse selection");
    case SampleEditKind::Mono: return QStringLiteral("convert track to mono");
    case SampleEditKind::Stereo: return QStringLiteral("convert track to stereo");
    case SampleEditKind::CrossfadeLinear: return QStringLiteral("linear crossfade");
    case SampleEditKind::CrossfadePower: return QStringLiteral("equal-power crossfade");
    }
    return {};
}

QVariantMap Backend::voiceCleanupTarget() const
{
    QVariantMap out{{"valid", false}, {"stamp", automationStamp()
        + ':' + QString::number(m_selection.start) + ':' + QString::number(m_selection.end)
        + ':' + QString::number(m_selection.channel) + ':' + QString::number(int(m_focused))}};
    const auto reject = [&](const QString& reason) { out["reason"] = reason; return out; };
    const auto* track = m_project.track(m_focused);
    if (!track || track->clips.empty()) return reject(tr("Focus a voice track and select the audio to clean."));
    if (m_loading || recording() || sampleEditing()) return reject(tr("Finish loading, recording or the current audio edit first."));
    if (m_selection.empty() || m_selection.track != m_focused || m_selection.channel != -1)
        return reject(tr("Select a time range across all channels of this voice track."));
    unsigned channels = 0;
    for (const auto& clip : track->clips) {
        const auto* source = m_project.source(clip.source);
        if (!source || source->audio.channels() < 1 || source->audio.channels() > 2)
            return reject(tr("Voice Cleanup supports mono and stereo tracks."));
        if (channels && channels != source->audio.channels())
            return reject(tr("Convert the mixed-layout track to mono or stereo first."));
        channels = source->audio.channels();
    }
    out["valid"] = true;
    out["trackName"] = QString::fromStdString(track->name);
    out["start"] = qint64(m_selection.start); out["end"] = qint64(m_selection.end);
    out["seconds"] = double(m_selection.length()) / m_project.sampleRate();
    out["channels"] = int(channels);
    return out;
}

bool Backend::applyVoiceCleanup(double amount, const QString& stamp)
{
    const auto target = voiceCleanupTarget();
    if (!target.value("valid").toBool()) { setError(target.value("reason").toString()); return false; }
    if (stamp != target.value("stamp").toString()) {
        setError(tr("The voice selection changed. Reload before applying cleanup.")); return false;
    }
    if (!std::isfinite(amount) || amount <= 0 || amount > 100) {
        setError(tr("Choose a cleanup amount above 0 and up to 100%.")); return false;
    }
    startSampleEdit(SampleEditKind::VoiceCleanup, 0, 0, 0, 0, std::nullopt, {}, false,
                    float(amount / 100));
    if (sampleEditing()) recordStep({{"kind", "voiceCleanup"}, {"on", true}, {"amount", amount}});
    return sampleEditing();
}

void Backend::convertTrackChannels(int channels)
{
    if (channels != 1 && channels != 2) {
        setError(QStringLiteral("choose mono or stereo"));
        return;
    }
    startSampleEdit(channels == 1 ? SampleEditKind::Mono : SampleEditKind::Stereo);
}

void Backend::startSampleEdit(SampleEditKind kind, qint64 targetFrames, std::size_t clipIndex,
                              qint64 rangeStart, qint64 rangeEnd,
                             std::optional<session::ChannelEditSpec> spec,
                              std::vector<std::vector<float>> matrix, bool allTracks, float voiceMix)
{
    if (deferAutomationEdit([this, kind, targetFrames, clipIndex, rangeStart, rangeEnd, spec, matrix, allTracks, voiceMix] { startSampleEdit(kind, targetFrames, clipIndex, rangeStart, rangeEnd, spec, matrix, allTracks, voiceMix); return true; })) return;
    if (recording()) { setError(QStringLiteral("stop recording before editing samples")); return; }
    (void)captureEditState();
    const auto* track = m_project.track(m_focused);
    const bool creates = kind == SampleEditKind::PasteNew || kind == SampleEditKind::InsertSilence || kind == SampleEditKind::ProjectRate;
    const bool paste = spec && (spec->operation == session::ChannelEdit::Paste || spec->operation == session::ChannelEdit::MixPaste);
    if ((!track && !creates) || (track && track->clips.empty() && !creates && !paste)) {
        setError(QStringLiteral("choose a take before editing samples: click a track to focus it")); return;
    }
    if (!paste && kind != SampleEditKind::MapChannels && kind != SampleEditKind::InsertSilence && kind != SampleEditKind::ProjectRate
        && kind != SampleEditKind::Mono && kind != SampleEditKind::Stereo && kind != SampleEditKind::Stretch
        && kind != SampleEditKind::AdjustCrossfadeLinear && kind != SampleEditKind::AdjustCrossfadePower && m_selection.empty()) {
        setError(QStringLiteral("select a time range before editing samples: drag across a lane"));
        return;
    }
    if (sampleEditing()) {
        setError(QStringLiteral("sample editing is already running; cancel it before starting another edit"));
        return;
    }
    setError({});
    m_sampleEditRequest = SampleEditRequest{kind, m_focused, m_selection,
                                           m_sessionGeneration, m_editRevision, targetFrames, clipIndex, rangeStart, rangeEnd,
                                           std::move(spec), std::move(matrix), allTracks, {}, voiceMix, {}, false, 0, {}};
    m_sampleEditLabel = sampleEditName(kind);
    if (kind == SampleEditKind::ApplyEffect && m_stagedEffect)
        m_sampleEditLabel = QStringLiteral("apply ") + QString::fromStdString(m_stagedEffect->name);
    if (m_sampleEditRequest->channelSpec && kind != SampleEditKind::PasteNew && kind != SampleEditKind::Normalize) {
        using E = session::ChannelEdit;
        switch (m_sampleEditRequest->channelSpec->operation) {
        case E::Gain: m_sampleEditLabel = "channel gain"; break;
        case E::Normalize: m_sampleEditLabel = "normalize channel"; break;
        case E::Silence: m_sampleEditLabel = "silence channel"; break;
        case E::FadeIn: m_sampleEditLabel = "fade channel in"; break;
        case E::FadeOut: m_sampleEditLabel = "fade channel out"; break;
        case E::Reverse: m_sampleEditLabel = "reverse channel"; break;
        case E::Paste: m_sampleEditLabel = "paste channels"; break;
        case E::MixPaste: m_sampleEditLabel = "paste and mix"; break;
        }
    }
    m_sampleEditRequest->label = m_sampleEditLabel;
    emit sampleEditChanged();
    pumpSampleEdit();
}

bool Backend::sampleEditContextCurrent(const SampleEditRequest& r) const
{
    if (r.kind == SampleEditKind::GroupStretch) {
        return r.generation == m_sessionGeneration && r.revision == m_editRevision
            && !r.clips.empty() && std::all_of(r.clips.begin(), r.clips.end(), [this](const auto& address) {
                const auto* track = m_project.track(address.track);
                return track && address.index < track->clips.size() && track->clips[address.index].sameDescriptor(address.expected);
            });
    }
    return r.generation == m_sessionGeneration && r.revision == m_editRevision
        && (r.kind != SampleEditKind::MultitrackPaste || r.selectionRevision == m_trackSelectionRevision)
        && r.track == m_focused && (m_project.track(r.track) || r.kind == SampleEditKind::PasteNew || r.kind == SampleEditKind::InsertSilence || r.kind == SampleEditKind::ProjectRate || r.kind == SampleEditKind::MultitrackPaste)
        && ((r.kind == SampleEditKind::Mono || r.kind == SampleEditKind::Stereo || r.kind == SampleEditKind::MapChannels || r.kind == SampleEditKind::InsertSilence || r.kind == SampleEditKind::ProjectRate) || (r.selection.start == m_selection.start
            && r.selection.end == m_selection.end && r.selection.track == m_selection.track
            && r.selection.channel == m_selection.channel));
}

void Backend::cancelSampleEdit()
{
    ++m_groupStretchCancelSerial; m_groupStretchDeferred = false;
    m_cancelSampleEdit.store(true, std::memory_order_relaxed);
    m_sampleEditRequest.reset();
    emit sampleEditChanged();
}

void Backend::pumpSampleEdit()
{
    if (m_shuttingDown || m_sampleEditActive || !m_sampleEditRequest || m_loading || m_saving
        || m_busy.load(std::memory_order_acquire))
        return;
    if (!m_applyingPluginEdit && (m_pluginEditCapturing || !m_pluginEdits.empty())) return;
    if (deferPluginEdit([this] { pumpSampleEdit(); return true; }, [this] { cancelSampleEdit(); })) return;
    const auto request = *m_sampleEditRequest;
    m_sampleEditRequest.reset();
    if (!sampleEditContextCurrent(request)) {
        setError(QStringLiteral("sample edit discarded because its target changed"));
        emit sampleEditChanged();
        return;
    }
    auto snapshot = m_project; // shares immutable audio; no sample copies on the GUI thread
    joinWorker();
    // An applied effect's offline instances are owned here, on the main thread,
    // like a render's: joinWorker releases them once the worker is done.
    lv2::Chain* effectChain = nullptr;
    std::vector<session::Insert> effectInserts;
    std::string effectName;
    if (request.kind == SampleEditKind::ApplyEffect) {
        if (!m_stagedEffect) {
            setError(tr("The effect to apply is no longer prepared. Choose it again."));
            emit sampleEditChanged();
            return;
        }
        m_offlineFeatures = std::move(m_stagedEffect->features);
        m_offlineChains = {m_stagedEffect->chain};
        effectChain = m_stagedEffect->chain.get();
        effectInserts = std::move(m_stagedEffect->inserts);
        effectName = std::move(m_stagedEffect->name);
        m_stagedEffect.reset();
    }
    m_busy.store(true, std::memory_order_release);
    m_cancelSampleEdit.store(false, std::memory_order_relaxed);
    m_sampleEditActive = true;
    emit sampleEditChanged();
    m_worker = std::thread([this, request, snapshot = std::move(snapshot), effectChain,
                            effectInserts = std::move(effectInserts), effectName = std::move(effectName)]() mutable {
        session::SampleEditResult result;
        std::vector<session::ClipAddress> stretchedClips;
        switch (request.kind) {
        case SampleEditKind::ApplyEffect:
            result = render::applyEffect(std::move(snapshot), request.track, request.selection.start,
                request.selection.end, *effectChain, effectInserts, effectName, &m_cancelSampleEdit); break;
        case SampleEditKind::GroupStretch: {
            auto stretched = session::stretchSelectedClips(std::move(snapshot), request.clips,
                request.targetFrames, false, 0, &m_cancelSampleEdit);
            result = std::move(stretched.edit); stretchedClips = std::move(stretched.clips); break;
        }
        case SampleEditKind::MultitrackPaste:
            result = session::pasteTracks(std::move(snapshot), *request.multitrack, request.clipIndex,
                request.rangeStart, request.ripple, engine::Transport::kMaxTracks, &m_cancelSampleEdit); break;
        case SampleEditKind::VoiceCleanup:
            result = session::cleanupVoice(std::move(snapshot), request.track,
                request.selection.start, request.selection.end, request.voiceMix, &m_cancelSampleEdit); break;
        case SampleEditKind::Normalize:
            result = session::normalizeSelection(std::move(snapshot), request.track, request.selection.start,
                request.selection.end, request.channelSpec->gain, &m_cancelSampleEdit); break;
        case SampleEditKind::ProjectRate:
            result = session::convertProjectRate(std::move(snapshot), std::uint32_t(request.targetFrames),
                std::uint32_t(request.rangeStart), &m_cancelSampleEdit); break;
        case SampleEditKind::Channels: case SampleEditKind::PasteNew:
            result = session::editChannels(std::move(snapshot), request.track, *request.channelSpec, &m_cancelSampleEdit); break;
        case SampleEditKind::MapChannels:
            result = session::mapTrackChannels(std::move(snapshot), request.track, request.channelMatrix, &m_cancelSampleEdit); break;
        case SampleEditKind::InsertSilence:
            result = session::insertSilence(std::move(snapshot), request.track, request.rangeStart, request.targetFrames,
                request.allTracks, &m_cancelSampleEdit); break;
        case SampleEditKind::AdjustCrossfadeLinear: case SampleEditKind::AdjustCrossfadePower:
            result = session::adjustCrossfade(std::move(snapshot), request.track, request.clipIndex,
                request.rangeStart, request.rangeEnd, request.kind == SampleEditKind::AdjustCrossfadePower,
                &m_cancelSampleEdit); break;
        case SampleEditKind::Stretch:
            result = session::stretchClip(std::move(snapshot), request.track, request.clipIndex,
                request.targetFrames, &m_cancelSampleEdit); break;
        case SampleEditKind::Reverse:
            result = session::reverseSelection(std::move(snapshot), request.track,
                request.selection.start, request.selection.end, &m_cancelSampleEdit); break;
        case SampleEditKind::Mono: case SampleEditKind::Stereo:
            result = session::convertTrackChannels(std::move(snapshot), request.track,
                request.kind == SampleEditKind::Mono ? 1U : 2U, &m_cancelSampleEdit); break;
        case SampleEditKind::CrossfadeLinear: case SampleEditKind::CrossfadePower:
            result = session::crossfadeSelection(std::move(snapshot), request.track,
                request.selection.start, request.selection.end,
                request.kind == SampleEditKind::CrossfadePower, &m_cancelSampleEdit); break;
        }
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_pendingSampleEdit.push_back({request, std::move(result), std::move(stretchedClips)});
            m_finishedJobs.push_back(Job::SampleEdit);
        }
        m_busy.store(false, std::memory_order_release);
        emit workerFinished();
    });
}

void Backend::finishSampleEdit(SampleEditJob job)
{
    if (!m_applyingAutomationEdit && !m_applyingPluginEdit && !m_shuttingDown && !job.result.cancelled
        && !m_cancelSampleEdit.load(std::memory_order_relaxed) && sampleEditContextCurrent(job.request)) {
        auto pending = std::make_shared<SampleEditJob>(std::move(job));
        if (deferAutomationEdit([this, pending] { finishSampleEdit(std::move(*pending)); return true; },
            [this] { m_sampleEditActive = false; emit sampleEditChanged(); },
            pending->request.kind == SampleEditKind::GroupStretch)) return;
        job = std::move(*pending);
    }
    if (!m_applyingPluginEdit && !m_shuttingDown && !job.result.cancelled
        && !m_cancelSampleEdit.load(std::memory_order_relaxed) && sampleEditContextCurrent(job.request)) {
        auto completion = std::make_shared<SampleEditJob>(std::move(job));
        if (deferPluginEdit([this, completion] { finishSampleEdit(std::move(*completion)); return true; },
                           [this] { m_sampleEditActive = false; emit sampleEditChanged(); })) return;
        job = std::move(*completion);
    }
    m_sampleEditActive = false;
    if (!m_shuttingDown && !job.result.cancelled
        && !m_cancelSampleEdit.load(std::memory_order_relaxed)) {
        if (!sampleEditContextCurrent(job.request)) {
            setError(QStringLiteral("sample edit discarded because its target changed"));
        } else if (!job.result.error.empty()) {
            setError(QString::fromStdString(job.result.error));
        } else if (job.result.project) {
            const auto before = captureEditState();
            const auto oldRate = m_project.sampleRate();
            const auto oldBits = m_project.defaultExportBits();
            const bool restart = prepareProjectRate(job.result.project->sampleRate());
            // Every old source is still shared by the new model, including the
            // sources held by old playback plans and by undo/redo clip references.
            joinWorker();           // nothing on the worker may be reading the old one
            m_project = std::move(*job.result.project);
            rescaleProjectView(oldRate, true);
            if (job.request.kind == SampleEditKind::Stretch) {
                const auto& clip = m_project.track(job.request.track)->clips[job.request.clipIndex];
                m_selection = {clip.timelineStart, clip.timelineEnd(), job.request.track};
            }
            if (job.request.kind == SampleEditKind::AdjustCrossfadeLinear || job.request.kind == SampleEditKind::AdjustCrossfadePower)
                m_selection = {job.request.rangeStart, job.request.rangeEnd, job.request.track};
            if (job.request.kind == SampleEditKind::MapChannels || job.request.kind == SampleEditKind::Mono
                || job.request.kind == SampleEditKind::Stereo || job.request.kind == SampleEditKind::ProjectRate) rebuildAllChains();
            auto changedTrack = job.request.track;
            if (!m_project.tracks().empty() && (job.request.kind == SampleEditKind::PasteNew || !m_project.track(m_focused))) {
                m_focused = m_project.tracks().back().id; changedTrack = m_focused;
                m_selection = {0, m_project.track(m_focused)->length(), m_focused};
                m_loaded = true;
            }
            if (job.request.kind == SampleEditKind::MultitrackPaste) {
                m_selectedClips.clear(); m_selectedTracks.clear();
                for (const auto& lane : job.request.multitrack->tracks)
                    m_selectedTracks.push_back(m_project.tracks()[job.request.clipIndex + lane.offset].id);
                m_focused = m_selectedTracks.front(); m_trackSelectionAnchor = m_focused;
                m_selection = {job.request.rangeStart, job.request.rangeStart + job.request.multitrack->frames, m_focused};
                ++m_trackSelectionRevision; m_loaded = true;
                if (job.request.multitrack->wholeClips) {
                    std::vector<session::ClipAddress> selected;
                    for (const auto& lane : job.request.multitrack->tracks) {
                        const auto& track = m_project.tracks()[job.request.clipIndex + lane.offset];
                        for (auto clip : lane.clips) {
                            clip.timelineStart += job.request.rangeStart;
                            // Paste forks persistent groups. Find the placed audio
                            // descriptor, then keep its actual new membership.
                            const auto found = std::find_if(track.clips.begin(), track.clips.end(), [&](auto candidate) {
                                candidate.group = clip.group; return candidate == clip;
                            });
                            if (found != track.clips.end())
                                selected.push_back({track.id, std::size_t(found - track.clips.begin()), *found});
                        }
                    }
                    if (!selected.empty()) {
                        const auto first = selected.front();
                        applyClipSelection(std::move(selected), first.track, first.index);
                    }
                }
            }
            if (job.request.kind == SampleEditKind::GroupStretch)
                remapStretchedSelection(job.request.clips, job.clips);
            if (job.request.allTracks || job.request.kind == SampleEditKind::ProjectRate || job.request.kind == SampleEditKind::GroupStretch)
                changedTrack = session::kInvalidTrack;
            commit(job.request.label.toStdString(), changedTrack, before);
            finishProjectRate(restart, oldRate, oldBits);
            emit takeChanged();
        } else if (job.result.unchanged) {
            emit notice(job.request.kind == SampleEditKind::ApplyEffect
                ? tr("The effect made no change to the selection, so there is nothing to undo.")
                : tr("That left the audio exactly as it was, so there is nothing to undo."));
        }
    }
    emit sampleEditChanged();
}

// --- clip placement and loop playback --------------------------------------------

void Backend::setLoopEnabled(bool enabled)
{
    if (enabled && recording()) return;
    if (enabled == m_loopEnabled)
        return;
    if (enabled) {
        if (m_selection.empty()) {
            setError(QStringLiteral("select a time range to loop: drag across a lane, then turn Loop on"));
            return;
        }
        m_loopStart = m_selection.start;
        m_loopEnd = m_selection.end;
        setError({});
    }
    m_loopEnabled = enabled;
    refreshLoop();
    emit loopChanged();
}

void Backend::toggleLoop() { setLoopEnabled(!m_loopEnabled); }

void Backend::refreshLoop()
{
    if (m_loopEnabled) {
        const auto end = std::min<qint64>(m_loopEnd, m_project.length());
        const bool valid = end > m_loopStart;
        if (m_loopEnd != end || !valid) {
            m_loopEnd = end;
            m_loopEnabled = valid;
            emit loopChanged();
        }
    }
    if (m_currentPlan && m_loopEnabled) engine::prefetchStreaming(*m_currentPlan, m_loopStart, false, false);
    enqueueAudioCommand(engine::Command::setLoop(m_loopEnabled, m_loopStart, m_loopEnd));
}

bool Backend::clipMoveCurrent() const
{
    if (!m_clipMove || m_clipMove->generation != m_sessionGeneration
        || m_clipMove->revision != m_editRevision)
        return false;
    const auto* track = m_project.track(m_clipMove->from);
    return track && m_clipMove->index < track->clips.size()
        && track->clips[m_clipMove->index].sameDescriptor(m_clipMove->clip)
        && m_clipMove->selectionRevision == m_trackSelectionRevision && selectedClipsCurrent();
}

bool Backend::beginClipMove(int id, qint64 frame)
{
    cancelClipMove();
    if (recording() || loading()) { setError(tr("Finish recording or loading before moving clips.")); return false; }
    const auto from = static_cast<session::TrackId>(id);
    const auto* track = m_project.track(from);
    if (!track)
        return false;
    for (std::size_t index = 0; index < track->clips.size(); ++index) {
        const auto& clip = track->clips[index];
        if (!clip.covers(frame))
            continue;
        setError({});
        const bool member = selectedClipsCurrent() && std::any_of(m_selectedClips.begin(), m_selectedClips.end(), [&](const auto& c) {
            return c.track == from && c.index == index;
        });
        if (!member) { if (!selectClipAt(id, frame)) return false; }
        else applyClipSelection(m_selectedClips, from, index);
        m_clipMove = ClipMove{from, from, index, clip, clip.timelineStart,
                              m_sessionGeneration, m_editRevision, false, 0, m_selectedClips, m_trackSelectionRevision};
        previewClipMove(id, clip.timelineStart, false);
        return true;
    }
    return false;
}

bool Backend::beginClipSlip(int id, qint64 frame)
{
    focusTrack(id); // Slip has one source interval, independently of a move group.
    if (!beginClipMove(id, frame)) return false;
    m_clipMove->slip = true;
    previewClipSlip(0);
    if (!m_clipMovePreview.value("valid").toBool()) {
        cancelClipMove();
        setError(QStringLiteral("that clip has no valid source interval to slip"));
        return false;
    }
    return true;
}

void Backend::previewClipSlip(qint64 dragFrames)
{
    if (!m_clipMove || !m_clipMove->slip) return;
    auto& move = *m_clipMove;
    const auto* source = m_project.source(move.clip.source);
    if (!source || move.clip.sourceFrames(session::FramePos(source->audio.frames())) < move.clip.length) {
        m_clipMovePreview = {{"valid", false}};
        emit clipMoveChanged();
        return;
    }
    const auto maximum = move.clip.sourceFrames(session::FramePos(source->audio.frames())) - move.clip.length;
    // Clamp the delta first so subtracting a hostile/extreme pointer coordinate cannot overflow.
    const auto bounded = std::clamp<qint64>(dragFrames, move.clip.sourceOffset - maximum, move.clip.sourceOffset);
    move.sourceOffset = move.clip.sourceOffset - bounded;
    const bool valid = clipMoveCurrent()
        && session::edits::canSlipClip(m_project, move.from, move.index, move.sourceOffset);
    m_clipMovePreview = {{"track", static_cast<int>(move.from)}, {"start", qint64(move.clip.timelineStart)},
        {"length", qint64(move.clip.length)}, {"valid", valid}, {"slip", true},
        {"sourceOffset", move.sourceOffset}, {"delta", move.sourceOffset - move.clip.sourceOffset},
        {"limited", bounded != dragFrames}};
    emit clipMoveChanged();
}

void Backend::slipFocusedClip(qint64 sourceDelta)
{
    const auto frame = m_selection.empty() ? m_playhead : m_selection.start;
    if (!beginClipSlip(static_cast<int>(m_focused), frame)) return;
    // A source offset can never use INT64_MIN; avoid negating it.
    previewClipSlip(sourceDelta == std::numeric_limits<qint64>::min()
        ? std::numeric_limits<qint64>::max() : -sourceDelta);
    finishClipMove();
}

void Backend::previewClipMove(int id, qint64 position, bool snap)
{
    if (!m_clipMove || m_clipMove->slip) return;
    auto& move = *m_clipMove;
    move.to = session::TrackId(id);
    position = std::max<qint64>(0, position);
    const auto from = m_project.trackIndex(move.from), to = m_project.trackIndex(move.to);
    const int rows = from && to ? int(*to) - int(*from) : 0;
    if (snap && m_snapping && (m_snapTolerance > 0 || musicalSnapActive()) && from && to) {
        qint64 bestDistance = m_snapTolerance, best = position;
        bool contentFound = false;
        std::optional<std::pair<qint64, qint64>> musical; // distance, anchor position
        const auto consider = [&](qint64 edge, qint64 relative, bool grid = false) {
            // candidate = edge - relative, checked before signed arithmetic.
            if (edge < 0 || (relative >= 0 ? edge < relative : edge > std::numeric_limits<qint64>::max() + relative)) return;
            const auto candidate = edge - relative;
            const auto distance = candidate >= position ? candidate - position : position - candidate;
            if (grid) {
                if (!musical || distance < musical->first) musical = {distance, candidate};
            } else if (distance < bestDistance) { bestDistance = distance; best = candidate; contentFound = true; }
        };
        std::set<std::pair<session::TrackId, std::size_t>> excluded;
        for (const auto& address : move.group) excluded.emplace(address.track, address.index);
        std::map<session::TrackId, std::vector<qint64>> targetEdges;
        for (const auto& address : move.group) {
            const auto original = m_project.trackIndex(address.track);
            if (!original) continue;
            const auto target = std::int64_t(*original) + rows;
            if (target < 0 || std::size_t(target) >= m_project.tracks().size()) continue;
            const auto& track = m_project.tracks()[std::size_t(target)];
            if (!targetEdges.contains(track.id)) {
                std::vector<qint64> edges{0, m_playhead};
                for (std::size_t i = 0; i < track.clips.size(); ++i) {
                    if (excluded.contains({track.id, i})) continue;
                    edges.push_back(track.clips[i].timelineStart); edges.push_back(track.clips[i].timelineEnd());
                }
                for (const auto& marker : m_project.markers()) { edges.push_back(marker.position); edges.push_back(marker.end()); }
                std::sort(edges.begin(), edges.end()); edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
                targetEdges.emplace(track.id, std::move(edges));
            }
            const auto& edges = targetEdges.at(track.id);
            for (const qint64 relative : {address.expected.timelineStart - move.clip.timelineStart,
                                          address.expected.timelineEnd() - move.clip.timelineStart}) {
                const auto desired = relative >= 0
                    ? position <= std::numeric_limits<qint64>::max() - relative ? position + relative : std::numeric_limits<qint64>::max()
                    : position >= -relative ? position + relative : qint64(0);
                const auto found = std::lower_bound(edges.begin(), edges.end(), desired);
                if (found != edges.end()) consider(*found, relative);
                if (found != edges.begin()) consider(*std::prev(found), relative);
                if (const auto grid = nearestMusicalGridFrame(desired)) consider(*grid, relative, true);
            }
        }
        position = contentFound ? best : musical ? musical->second : position;
    }
    move.position = position;
    const qint64 delta = position - move.clip.timelineStart;
    QString error;
    if (!clipMoveCurrent()) error = tr("Target changed");
    else if (!from || !to) error = tr("Choose destination tracks");
    else {
        auto candidate = m_project;
        const auto result = session::moveSelectedClips(candidate, move.group, delta, rows);
        error = QString::fromStdString(result.error);
    }
    QVariantList ghosts;
    for (const auto& address : move.group) {
        const auto original = m_project.trackIndex(address.track);
        if (!original) continue;
        const auto target = std::int64_t(*original) + rows;
        if (target < 0 || std::size_t(target) >= m_project.tracks().size()) continue;
        const auto start = address.expected.timelineStart;
        const bool representable = delta >= 0 ? start <= std::numeric_limits<qint64>::max() - delta : delta >= -start;
        ghosts.push_back(QVariantMap{{"track", int(m_project.tracks()[std::size_t(target)].id)},
            {"start", representable ? start + delta : qint64(0)}, {"length", qint64(address.expected.length)}});
    }
    m_clipMovePreview = {{"track", id}, {"start", position}, {"length", qint64(move.clip.length)},
                        {"valid", error.isEmpty()}, {"reason", error}, {"clips", ghosts}};
    emit clipMoveChanged();
}

void Backend::finishClipMove()
{
    if (!m_clipMove)
        return;
    if (!clipMoveCurrent()) {
        cancelClipMove();
        setError(QStringLiteral("clip move discarded because the session changed"));
        return;
    }
    const auto move = *m_clipMove;
    cancelClipMove();
    (void)applyClipMove(move);
}

bool Backend::applyClipMove(const ClipMove& move)
{
    if (const auto deferred = deferAutomationEdit([this, move] { return applyClipMove(move); })) return *deferred;
    const auto* track = m_project.track(move.from);
    if (move.generation != m_sessionGeneration || !track || move.index >= track->clips.size()
        || track->clips[move.index] != move.clip) {
        setError(tr("The clip changed before the move could be applied.")); return false;
    }
    if (move.slip) {
        const auto before = captureEditState();
        if (session::edits::slipClip(m_project, move.from, move.index, move.sourceOffset))
            commit("slip clip", move.from, before);
        return true;
    }
    const auto from = m_project.trackIndex(move.from), to = m_project.trackIndex(move.to);
    if (!from || !to) { setError(tr("Choose existing destination tracks.")); return false; }
    return applyClipTranslation(move.group, move.position - move.clip.timelineStart,
                              int(*to) - int(*from), move.from, move.index);
}

void Backend::cancelClipMove()
{
    m_clipMove.reset();
    m_clipMovePreview.clear();
    emit clipMoveChanged();
}

// --- broadcast metadata (W05) ---------------------------------------------------------

QString Backend::formatTimeReference(qint64 samples, int rate)
{
    if (rate <= 0 || samples < 0) return {};
    const double seconds = double(samples) / rate;
    const int h = int(seconds / 3600), m = int(seconds / 60) % 60;
    const double s = std::fmod(seconds, 60.0);
    return QStringLiteral("%1:%2:%3").arg(h, 2, 10, QLatin1Char('0')).arg(m, 2, 10, QLatin1Char('0'))
        .arg(QString::number(s, 'f', 3).rightJustified(6, QLatin1Char('0')));
}

// "hh:mm:ss.mmm" or a plain sample count; -1 when neither.
qint64 parseTimeReference(const QString &text, int rate)
{
    const QString t = text.trimmed();
    if (t.isEmpty()) return -1;
    bool plain = false;
    const qint64 samples = t.toLongLong(&plain);
    if (plain) return samples < 0 ? -1 : samples;
    const QStringList parts = t.split(QLatin1Char(':'));
    if (parts.size() != 3) return -1;
    bool okH = false, okM = false, okS = false;
    const int h = parts[0].toInt(&okH), m = parts[1].toInt(&okM);
    const double s = parts[2].toDouble(&okS);
    if (!okH || !okM || !okS || h < 0 || m < 0 || m > 59 || s < 0 || s >= 60) return -1;
    return qint64(std::llround((h * 3600.0 + m * 60.0 + s) * rate));
}

namespace {
}

void Backend::setImportPlacement(const QString &placement)
{
    const QString value = placement == QLatin1String("playhead") || placement == QLatin1String("timestamp") ? placement : QStringLiteral("start");
    if (value == m_importPlacement) return;
    m_importPlacement = value;
    emit importPlacementChanged();
}

QVariantMap Backend::projectMetadata() const
{
    const session::ProjectMetadata &pm = m_project.metadata();
    QVariantMap out;
    out["description"] = QString::fromStdString(pm.description);
    out["originator"] = QString::fromStdString(pm.originator);
    out["originatorReference"] = QString::fromStdString(pm.originatorReference);
    out["originationDate"] = QString::fromStdString(pm.originationDate);
    out["originationTime"] = QString::fromStdString(pm.originationTime);
    out["timeReferenceSet"] = pm.timeReferenceSet;
    out["timeReference"] = qint64(pm.timeReference);
    out["timeReferenceText"] = pm.timeReferenceSet ? formatTimeReference(pm.timeReference, static_cast<int>(m_project.sampleRate())) : QString();
    out["codingHistory"] = QString::fromStdString(pm.codingHistory);
    out["empty"] = pm.empty();
    return out;
}

bool Backend::setProjectMetadata(const QVariantMap &values)
{
    if (const auto deferred = deferAutomationEdit([this, values] { return setProjectMetadata(values); })) return *deferred;
    if (m_loading || m_saving) { setError(QStringLiteral("wait until the file has finished opening before editing metadata")); return false; }
    session::ProjectMetadata next = m_project.metadata();
    const auto text = [&](const char *key, const std::string &current) {
        return values.contains(QLatin1String(key)) ? values.value(QLatin1String(key)).toString().toStdString() : current;
    };
    next.description = text("description", next.description);
    next.originator = text("originator", next.originator);
    next.originatorReference = text("originatorReference", next.originatorReference);
    next.originationDate = text("originationDate", next.originationDate);
    next.originationTime = text("originationTime", next.originationTime);
    next.codingHistory = text("codingHistory", next.codingHistory);
    if (values.contains(QStringLiteral("timeReferenceText"))) {
        const QString t = values.value(QStringLiteral("timeReferenceText")).toString().trimmed();
        if (t.isEmpty()) { next.timeReferenceSet = false; next.timeReference = 0; }
        else {
            const qint64 samples = parseTimeReference(t, static_cast<int>(m_project.sampleRate()));
            if (samples < 0) { setError(QStringLiteral("the time reference must be hh:mm:ss.mmm or a sample count")); return false; }
            next.timeReferenceSet = true; next.timeReference = samples;
        }
    }
    static const QRegularExpression dateShape(QStringLiteral("^\\d{4}-\\d{2}-\\d{2}$")), timeShape(QStringLiteral("^\\d{2}:\\d{2}:\\d{2}$"));
    if (!next.originationDate.empty() && !dateShape.match(QString::fromStdString(next.originationDate)).hasMatch()) {
        setError(QStringLiteral("the origination date must be yyyy-mm-dd")); return false;
    }
    if (!next.originationTime.empty() && !timeShape.match(QString::fromStdString(next.originationTime)).hasMatch()) {
        setError(QStringLiteral("the origination time must be hh:mm:ss")); return false;
    }
    if (next == m_project.metadata()) { setError({}); return true; }
    const auto before = captureEditState();
    m_project.metadata() = std::move(next);
    commit("edit metadata", session::kInvalidTrack, before);
    setError({});
    emit metadataChanged();
    return true;
}

QVariantMap Backend::sourceMetadata(int track) const
{
    QVariantMap out{{"valid", false}};
    if (track < -1) return out;
    const auto *t = m_project.track(trackArg(track));
    if (!t) return out;
    std::set<session::SourceId> references;
    for (const auto &clip : t->clips) references.insert(clip.source);
    if (references.size() != 1) { out["sourceCount"] = qint64(references.size()); return out; }
    const auto *source = m_project.source(*references.begin());
    if (!source) return out;
    const session::AudioMetadata &p = source->provenance;
    const int rate = static_cast<int>(source->sampleRate ? source->sampleRate : m_project.sampleRate());
    out["valid"] = true;
    out["sourceCount"] = 1;
    out["present"] = !p.empty();
    out["hasBext"] = p.bext.present;
    out["description"] = QString::fromStdString(p.bext.description);
    out["originator"] = QString::fromStdString(p.bext.originator);
    out["originatorReference"] = QString::fromStdString(p.bext.originatorReference);
    out["recorded"] = p.bext.present && !(p.bext.originationDate.empty() && p.bext.originationTime.empty())
                      ? QString::fromStdString(p.bext.originationDate + " " + p.bext.originationTime).trimmed() : QString();
    out["timeReference"] = qint64(p.bext.timeReference);
    out["timeReferenceText"] = p.bext.present && p.bext.timeReference > 0 ? formatTimeReference(p.bext.timeReference, rate) : QString();
    out["codingHistory"] = QString::fromStdString(p.bext.codingHistory);
    out["loudness"] = p.bext.loudnessValue ? QString::number(*p.bext.loudnessValue, 'f', 1) + QStringLiteral(" LUFS") : QString();
    QVariantList markers;
    for (const auto &m : p.markers)
        markers.append(QVariantMap{{"position", qint64(m.position)}, {"length", qint64(m.length)}, {"label", QString::fromStdString(m.label)}});
    out["markers"] = markers;
    out["markerCount"] = qint64(p.markers.size());
    out["hasIxml"] = !p.ixml.empty();
    out["ixml"] = QString::fromStdString(p.ixml);
    QVariantMap strings;
    for (const auto &[k, v] : p.strings) strings[QString::fromStdString(k)] = QString::fromStdString(v);
    out["strings"] = strings;
    return out;
}

session::AudioMetadata Backend::exportMetadata(const QString &markers, std::uint32_t exportRate) const
{
    session::AudioMetadata out;
    const session::ProjectMetadata &pm = m_project.metadata();
    const std::uint32_t projectRate = m_project.sampleRate();
    out.bext.present = true;
    out.bext.description = pm.description;
    out.bext.originator = pm.originator.empty() ? "OmaAudio" : pm.originator;
    out.bext.originatorReference = pm.originatorReference;
    out.bext.originationDate = pm.originationDate;
    out.bext.originationTime = pm.originationTime;
    out.bext.timeReference = pm.timeReferenceSet ? session::rescaleFrames(pm.timeReference, projectRate, exportRate) : 0;
    out.bext.codingHistory = pm.codingHistory;
    if (!out.bext.codingHistory.empty() && out.bext.codingHistory.back() != '\n') out.bext.codingHistory += "\r\n";
    if (!pm.description.empty()) out.strings["title"] = pm.description;
    out.strings["software"] = "OmaAudio";
    std::vector<session::FileMarker> found;
    if (markers == QLatin1String("project")) {
        for (const auto &m : m_project.markers()) {
            session::FileMarker f;
            f.position = session::rescaleFrames(m.position, projectRate, exportRate);
            f.length = m.length > 0 ? session::rescaleFrames(m.end(), projectRate, exportRate) - f.position : 0;
            f.label = m.name;
            found.push_back(std::move(f));
        }
    } else if (markers == QLatin1String("source")) {
        // Through the clips: a source marker is where its audio plays now, and one
        // no clip covers is left out.
        for (const auto &track : m_project.tracks()) {
            for (const auto &clip : track.clips) {
                const auto *source = m_project.source(clip.source);
                if (!source || source->provenance.markers.empty()) continue;
                const std::uint32_t sourceRate = source->sampleRate ? source->sampleRate : projectRate;
                for (const auto &m : source->provenance.markers) {
                    const std::int64_t inProject = session::rescaleFrames(m.position, sourceRate, projectRate);
                    if (inProject < clip.sourceOffset || inProject >= clip.sourceOffset + clip.length) continue;
                    session::FileMarker f;
                    const std::int64_t timeline = clip.timelineStart + (inProject - clip.sourceOffset);
                    f.position = session::rescaleFrames(timeline, projectRate, exportRate);
                    f.length = m.length > 0 ? session::rescaleFrames(m.length, sourceRate, exportRate) : 0;
                    f.label = m.label;
                    found.push_back(std::move(f));
                }
            }
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const auto &a, const auto &b) { return a.position < b.position; });
    out.markers = std::move(found);
    return out;
}

QVariantMap Backend::exportPreview(const QVariantMap &spec) const
{
    QVariantMap out;
    const QString formatId = spec.value(QStringLiteral("format"), QStringLiteral("wav")).toString();
    const auto container = render::containerFromId(formatId.toStdString()).value_or(render::Container::Wav);
    const auto sampleFormat = render::sampleFormatFromBits(spec.value(QStringLiteral("bitDepth"), 24).toInt()).value_or(render::SampleFormat::Pcm24);
    const std::uint32_t exportRate = static_cast<std::uint32_t>(std::max(0, spec.value(QStringLiteral("sampleRate"), 0).toInt()));
    const std::uint32_t rate = exportRate == 0 ? m_project.sampleRate() : exportRate;
    const std::uint32_t channels = spec.value(QStringLiteral("channels"), 0).toInt() == 1 ? 1 : kOutputChannels;
    const QString markers = spec.value(QStringLiteral("markers"), QStringLiteral("none")).toString();
    const bool include = spec.value(QStringLiteral("includeMetadata"), false).toBool();
    // The mix's length at the export rate decides whether a WAV can hold it.
    std::uint64_t frames = 0;
    for (const auto &track : m_project.tracks())
        for (const auto &clip : track.clips) frames = std::max<std::uint64_t>(frames, static_cast<std::uint64_t>(clip.timelineStart + clip.length));
    frames = static_cast<std::uint64_t>(session::rescaleFrames(static_cast<std::int64_t>(frames), m_project.sampleRate(), rate));
    const auto written = render::containerForSize(container, frames, channels, sampleFormat);
    const bool wavFamily = written == render::Container::Wav || written == render::Container::Bwf || written == render::Container::Rf64;
    const bool metadata = wavFamily && (include || written == render::Container::Bwf);
    const session::AudioMetadata meta = exportMetadata(markers, rate);
    QStringList lines, omitted;
    lines << QStringLiteral("Container: %1%2").arg(QString::fromStdString(std::string(render::containerId(written))).toUpper(),
                                                   written != container ? QStringLiteral(" (the mix exceeds what a WAV can hold, so RF64 is written)") : QString());
    if (metadata) {
        lines << QStringLiteral("Broadcast information (bext): description \"%1\", originator \"%2\", reference \"%3\"")
                     .arg(QString::fromStdString(meta.bext.description), QString::fromStdString(meta.bext.originator), QString::fromStdString(meta.bext.originatorReference));
        lines << QStringLiteral("Origination: %1 %2").arg(meta.bext.originationDate.empty() ? QStringLiteral("(render date)") : QString::fromStdString(meta.bext.originationDate),
                                                          meta.bext.originationTime.empty() ? QStringLiteral("(render time)") : QString::fromStdString(meta.bext.originationTime));
        lines << QStringLiteral("Time reference: %1").arg(m_project.metadata().timeReferenceSet
                                                            ? formatTimeReference(meta.bext.timeReference, static_cast<int>(rate)) + QStringLiteral(" (%1 samples at %2 Hz)").arg(meta.bext.timeReference).arg(rate)
                                                            : QStringLiteral("none; the file starts at 0"));
        lines << QStringLiteral("Loudness fields: measured from the delivered audio at render time");
        lines << QStringLiteral("Coding history: %1one line for this render").arg(meta.bext.codingHistory.empty() ? QString() : QStringLiteral("what you entered, then "));
        lines << QStringLiteral("INFO: title from the description, software OmaAudio");
        if (m_project.metadata().description.empty()) omitted << QStringLiteral("description is empty");
        if (m_project.metadata().originatorReference.empty()) omitted << QStringLiteral("originator reference is empty");
        omitted << QStringLiteral("UMID is not written") << QStringLiteral("iXML is not written (imported iXML stays with its source)");
    } else if (wavFamily) {
        omitted << QStringLiteral("no broadcast information: turn on \"Include metadata\" or choose BWF");
    } else {
        omitted << QStringLiteral("%1 carries no broadcast information or markers").arg(formatId.toUpper());
    }
    if (wavFamily && markers != QLatin1String("none")) {
        int ranges = 0;
        for (const auto &m : meta.markers) if (m.length > 0) ++ranges;
        lines << QStringLiteral("Markers: %1 as cue points%2 (%3)").arg(meta.markers.size()).arg(ranges ? QStringLiteral(", %1 with a length").arg(ranges) : QString(),
                                                                           markers == QLatin1String("project") ? QStringLiteral("the project's markers") : QStringLiteral("source file markers mapped through the clips"));
        if (meta.markers.empty()) omitted << QStringLiteral("no markers to write");
    } else if (wavFamily) {
        omitted << QStringLiteral("no markers (Markers: None)");
    }
    out["container"] = QString::fromStdString(std::string(render::containerId(written)));
    out["upgraded"] = written != container;
    out["metadataWritten"] = metadata;
    out["markerCount"] = qint64(meta.markers.size());
    out["markers"] = markers;
    out["frames"] = qint64(frames);
    out["omitted"] = omitted;
    out["text"] = lines.join(QLatin1Char('\n')) + (omitted.isEmpty() ? QString() : QStringLiteral("\nNot written: ") + omitted.join(QStringLiteral("; ")));
    return out;
}
