#include "backend.h"
#include "engine/mix_benchmark.h"

#include <QDateTime>
#include <QThreadPool>
#include <algorithm>

// Audio load (how much of each audio block playback uses, peaks, dropouts and the
// effect chains costing most) and the "Check this computer" measurement.

QVariantMap Backend::audioLoad() const
{
    const auto samples = m_meters.loadSamples();
    const bool active = samples != m_lastLoadSamples;
    m_lastLoadSamples = samples;
    const auto rate = double(m_output.sampleRate() ? m_output.sampleRate() : m_project.sampleRate());
    QVariantList heaviest;
    struct Cost { QString name; double share; int track; };
    std::vector<Cost> costs;
    for (const auto& [owner, chain] : m_chains) {
        if (!chain) continue;
        const double share = chain->nanosecondsPerFrame() * rate / 1e9;
        if (!(share > 0.0001)) continue;   // listed from 0.01% of real time
        QString name; int track = -1;
        if (owner.isMaster()) name = tr("Master bus effects");
        else if (owner.isTrack()) {
            const auto* lane = m_project.track(owner.trackId());
            name = lane ? QString::fromStdString(lane->name) : tr("Track");
            track = int(owner.trackId());
        } else continue;
        costs.push_back({name, share, track});
    }
    std::sort(costs.begin(), costs.end(), [](const Cost& a, const Cost& b) { return a.share > b.share; });
    for (std::size_t i = 0; i < std::min<std::size_t>(costs.size(), 6); ++i)
        heaviest.append(QVariantMap{{"name", costs[i].name}, {"percent", costs[i].share * 100}, {"track", costs[i].track}});
    return {{"active", active}, {"percent", double(m_meters.loadAverage()) * 100}, {"peak", double(m_meters.loadPeak()) * 100},
            {"dropouts", qulonglong(m_meters.dropouts())}, {"overloads", qulonglong(m_meters.overloads())},
            {"blockFrames", int(m_output.blockFrames())}, {"sampleRate", int(rate)}, {"heaviest", heaviest}};
}

void Backend::checkComputer()
{
    if (m_checkingComputer) return;
    m_checkingComputer = true;
    emit computerCheckChanged();
    const unsigned rate = m_output.sampleRate() ? m_output.sampleRate() : m_project.sampleRate();
    const unsigned block = m_output.blockFrames() ? m_output.blockFrames() : 256;
    auto cancel = m_computerCheckCancel;
    cancel->store(false);
    QThreadPool::globalInstance()->start([this, rate, block, cancel] {
        const auto check = engine::checkComputer(rate, block, 0.5, cancel.get());
        QMetaObject::invokeMethod(this, [this, check] {
            m_checkingComputer = false;
            if (m_shuttingDown) return;
            if (check.ok) {
                m_computerCheck = {{"ok", true}, {"sampleRate", int(check.rate)}, {"blockFrames", int(check.block)},
                    {"deadlineMs", check.deadlineUs / 1000.0}, {"share", check.share},
                    {"plainUs", check.plainUs}, {"automatedUs", check.automatedUs}, {"busUs", check.busUs},
                    {"plainTracks", int(check.plainTracks)}, {"automatedTracks", int(check.automatedTracks)},
                    {"busTracks", int(check.busTracks)}, {"maxTracks", maxTracks()},
                    {"checkedAt", QDateTime::currentDateTime().toString(Qt::ISODate)}};
            } else {
                m_computerCheck = {{"ok", false}, {"reason", check.cancelled ? tr("The check was cancelled.")
                                                               : tr("The check could not run: %1").arg(QString::fromStdString(check.error))}};
            }
            emit computerCheckChanged();
        }, Qt::QueuedConnection);
    });
}
