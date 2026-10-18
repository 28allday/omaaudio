#include "backend.h"
#include "resourcebudget.h"
#include "timeformat.h"
#include "engine/stream_cache.h"

#include <QFileInfo>
#include <QLocale>
#include <QStorageInfo>
#include <QThreadPool>
#include <QTimer>

#include <cmath>
#include <malloc.h>
#include <unistd.h>

namespace omaaudio::resources {
int systemMemoryMiB() noexcept
{
    const long long pages = ::sysconf(_SC_PHYS_PAGES), size = ::sysconf(_SC_PAGESIZE);
    if (pages <= 0 || size <= 0) return 4096;
    return int(std::min<long long>(pages * size / (1024 * 1024), 1 << 20));
}
}

using namespace omaaudio;

int Backend::systemMemoryMiB() const { return m_systemMemoryMiB; }
int Backend::maximumMemoryMiB() const { return resources::maximumMiB(m_systemMemoryMiB); }
int Backend::effectiveMemoryMiB() const { return resources::effectiveMiB(m_memoryMiB, m_systemMemoryMiB); }

std::size_t Backend::playbackCacheBytes() const
{
    return resources::Budget::forMemory(effectiveMemoryMiB()).playbackBytes();
}

int Backend::playbackCacheMiB() const
{
    return int(engine::StreamCache::Config::forBytes(playbackCacheBytes()).bytes() >> 20);
}

void Backend::setMemoryMiB(int mib)
{
    mib = resources::requestedMiB(mib, m_systemMemoryMiB);
    if (mib == m_memoryMiB) return;
    const auto before = playbackCacheBytes();
    m_memoryMiB = mib;
    emit memoryChanged();
    // The next plan builds a cache of the new size; the old one is retired with
    // the plan that borrowed it, by the normal handshake. Nothing to do while a
    // session is being replaced: its first plan reads the budget anyway.
    if (playbackCacheBytes() != before && m_currentPlan && m_currentPlan->streamCache && !m_loading) {
        publishPlan();
        emit storageChanged();
    }
}

// Finished jobs leave freed heap with the allocator. Hand it back once the
// session has been quiet for a moment, off the GUI thread.
void Backend::scheduleMemoryRelease()
{
    if (m_shuttingDown) return;
    if (!m_memoryReleaseTimer) {
        m_memoryReleaseTimer = new QTimer(this);
        m_memoryReleaseTimer->setSingleShot(true);
        m_memoryReleaseTimer->setInterval(2000);
        connect(m_memoryReleaseTimer, &QTimer::timeout, this, [this] {
            if (m_shuttingDown) return;
            if (m_busy.load(std::memory_order_acquire) || m_loading || m_saving) { m_memoryReleaseTimer->start(); return; }
#ifdef __GLIBC__
            QThreadPool::globalInstance()->start([] { ::malloc_trim(0); });
#endif
        });
    }
    m_memoryReleaseTimer->start();
}

// --- the spectral store ----------------------------------------------------------

void Backend::setSpectralStoreGiB(int gib)
{
    gib = std::clamp(gib, 1, 1024);
    if (gib == m_spectralStoreGiB) return;
    m_spectralStoreGiB = gib;
    emit spectralStoreChanged();
    refreshSpectralStore();
}

QString Backend::spectralStoreUsage() const
{
    if (m_spectralStoreBytes < 0) return tr("Measuring the spectral cache…");
    const auto used = QLocale().formattedDataSize(m_spectralStoreBytes);
    const auto held = m_spectralStoreEntries == 1 ? tr("1 display") : tr("%1 displays").arg(m_spectralStoreEntries);
    return m_spectralStoreFree < 0 ? tr("%1 used by %2").arg(used, held)
        : tr("%1 used by %2 · %3 free on disk").arg(used, held, QLocale().formattedDataSize(m_spectralStoreFree));
}

void Backend::refreshSpectralStore() { maintainSpectralStore(false); }
void Backend::clearSpectralStore() { maintainSpectralStore(true); }

void Backend::maintainSpectralStore(bool clear)
{
    if (m_shuttingDown) return;
    if (m_spectralStoreBusy) { m_spectralStoreAgain = true; m_spectralStoreClearPending |= clear; return; }
    const auto store = m_spectralStore;
    const qint64 limit = clear ? 0 : qint64(m_spectralStoreGiB) << 30;
    m_spectralStoreBusy = true;
    m_spectralStoreStatus = clear ? tr("Clearing the spectral cache…") : tr("Checking the spectral cache…");
    emit spectralStoreChanged();
    // The destructor joins this pool. The reply is addressed to this object, so
    // one still queued when the window closes is dropped.
    m_storePool.start([this, store, limit, clear] {
        const int removed = store.prune(limit);
        const auto usage = store.usage();
        QString volume = store.root();
        while (!QFileInfo::exists(volume) && QFileInfo(volume).absolutePath() != volume)
            volume = QFileInfo(volume).absolutePath();
        const QStorageInfo disk(volume);
        const qint64 free = disk.isValid() && disk.isReady() ? disk.bytesAvailable() : -1;
        QMetaObject::invokeMethod(this, [this, usage, free, removed, clear, limit] {
            m_spectralStoreBytes = usage.bytes; m_spectralStoreEntries = usage.entries; m_spectralStoreFree = free;
            m_spectralStoreBusy = false;
            m_spectralStoreStatus = usage.bytes > limit && clear ? tr("Some cached displays are being written by another window. Clear again in a moment.")
                : clear ? tr("Spectral cache cleared. Displays are rebuilt when needed.")
                : removed == 1 ? tr("Removed 1 older cached display.")
                : removed ? tr("Removed %1 older cached displays.").arg(removed) : QString();
            emit spectralStoreChanged();
            if (m_spectralStoreAgain) {
                const bool next = m_spectralStoreClearPending;
                m_spectralStoreAgain = m_spectralStoreClearPending = false;
                maintainSpectralStore(next);
            }
        }, Qt::QueuedConnection);
    });
}

// --- the time display --------------------------------------------------------------

void Backend::setTimeDisplay(const QString &key)
{
    const QString valid = timeformat::validKey(key);
    if (valid == m_timeDisplay) return;
    m_timeDisplay = valid;
    emit timeDisplayChanged();
}

// The display in force, with "the video's rate" resolved against the picture
// this project carries, if it has been read.
timeformat::Display Backend::currentTimeDisplay() const
{
    if (m_timeDisplay == QLatin1String("barsbeats") && m_project.musicalTime().enabled) {
        timeformat::Display display;
        display.mode = timeformat::Mode::BarsBeats;
        display.music = m_project.musicalTime();
        return display;
    }
    const bool picture = !m_project.video().empty() && m_videoInfo.ok && m_videoProbedPath == m_project.video().path;
    return timeformat::displayFor(m_timeDisplay, picture ? m_videoInfo.rate : video::FrameRate{0, 1}, m_project.video().dropFrame);
}

QStringList Backend::timeDisplays() const { return timeformat::keys(); }
QString Backend::timeDisplayLabel(const QString &key) const { return timeformat::label(key); }
QString Backend::timeDisplayHint() const
{
    if (m_timeDisplay == QLatin1String("barsbeats") && !m_project.musicalTime().enabled)
        return tr("Enable musical timing in Project properties to count bars and beats. Showing clock time until then.");
    return timeformat::hint(currentTimeDisplay());
}
QString Backend::durationDisplayHint() const
{
    return currentTimeDisplay().mode == timeformat::Mode::BarsBeats
        ? timeformat::hint(currentTimeDisplay(), true) : timeDisplayHint();
}
QVariantMap Backend::musicalTime() const
{
    const auto &music = m_project.musicalTime();
    return {{"enabled", music.enabled}, {"tempo", music.tempoMilli / 1000.0},
        {"beats", music.beats}, {"unit", music.unit}, {"ticksPerBeat", music.ticksPerBeat()}};
}
bool Backend::setMusicalTime(bool enabled, double tempo, int beats, int unit, const QString &stamp)
{
    if (const auto deferred = deferAutomationEdit([this, enabled, tempo, beats, unit, stamp] { return setMusicalTime(enabled, tempo, beats, unit, stamp); })) return *deferred;
    if (m_loading || m_saving || recording() || sampleEditing() || m_repairing || storageBusy()) {
        setError(tr("Finish loading, saving, recording or processing before changing musical timing.")); return false;
    }
    if (!stamp.isEmpty() && !automationStampCurrent(stamp)) {
        setError(tr("The project changed. Reload its properties before applying musical timing.")); return false;
    }
    if (!std::isfinite(tempo) || tempo < 1 || tempo > 480) {
        setError(tr("Choose a tempo from 1 to 480 quarter notes per minute.")); return false;
    }
    const session::MusicalTime next{enabled, std::int32_t(std::lround(tempo * 1000)), beats, unit};
    if (!next.valid()) { setError(tr("Choose 1–32 beats and a note denominator of 1, 2, 4, 8, 16 or 32.")); return false; }
    if (next == m_project.musicalTime()) { setError({}); return true; }
    const auto before = captureEditState();
    m_project.musicalTime() = next;
    commit("edit musical timing", session::kInvalidTrack, before);
    setError({});
    return true;
}
qint64 Backend::musicalTickAt(qint64 samples) const { return m_project.musicalTime().tickAt(samples, m_project.sampleRate()); }
qint64 Backend::sampleAtMusicalTick(qint64 tick) const { return m_project.musicalTime().sampleAt(tick, m_project.sampleRate()); }
QString Backend::formatDuration(qint64 samples) const
{
    return timeformat::format(samples, m_project.sampleRate(), currentTimeDisplay(), true);
}
qint64 Backend::parseDuration(const QString &text) const
{
    return timeformat::parse(text, m_project.sampleRate(), currentTimeDisplay(), true);
}
qint64 Backend::timeStep() const { return timeformat::step(m_project.sampleRate(), currentTimeDisplay()); }

QString Backend::formatTime(qint64 samples) const
{
    return timeformat::format(samples, m_project.sampleRate(), currentTimeDisplay());
}
qint64 Backend::parseTime(const QString &text) const
{
    return timeformat::parse(text, m_project.sampleRate(), currentTimeDisplay());
}
qint64 Backend::advanceTime(qint64 samples, int steps) const
{
    return timeformat::advance(samples, steps, m_project.sampleRate(), currentTimeDisplay());
}
QString Backend::formatShortLength(qint64 samples) const
{
    return timeformat::shortLength(samples, m_project.sampleRate(), currentTimeDisplay());
}
QString Backend::formatPerPixel(double samplesPerPixel) const
{
    return timeformat::perPixel(samplesPerPixel, m_project.sampleRate(), currentTimeDisplay());
}
