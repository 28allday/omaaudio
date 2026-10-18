#include "backend.h"
#include "session/routing.h"
#include <cmath>

QVariantMap Backend::routingTarget(int track) const
{
    const auto* target = m_project.track(trackArg(track));
    if (!target) return {};
    const auto& routing = target->routing;
    QVariantList sends;
    for (const auto& send : routing.sends) sends.append(QVariantMap{{"target",static_cast<int>(send.target)},
        {"db",send.gain > 0 ? 20 * std::log10(send.gain) : -120.0}, {"pre",send.preFader}});
    int keySlots = 0;
    if (const auto chain = m_chains.find(session::EffectOwner::track(target->id)); chain != m_chains.end() && chain->second)
        for (std::size_t i = 0; i < chain->second->size(); ++i) keySlots += chain->second->hasSidechain(i);
    return {{"track",static_cast<int>(target->id)}, {"stamp",automationStamp()},
        {"output",routing.output == session::kInvalidTrack ? -2 : routing.output == session::kNoTrackOutput ? -3 : static_cast<int>(routing.output)},
        {"sidechain",routing.sidechain == session::kInvalidTrack ? -1 : static_cast<int>(routing.sidechain)},
        {"sends",sends}, {"keySlots",keySlots}};
}

bool Backend::setTrackRouting(int track,int output,int sidechain,const QVariantList& sends,const QString& stamp)
{
    if (const auto deferred = deferAutomationEdit([this, track, output, sidechain, sends, stamp] { return setTrackRouting(track, output, sidechain, sends, stamp); })) return *deferred;
    if (recording() || m_loading || m_projectLoadActive) {
        setError(QStringLiteral("Wait for recording or opening to finish before changing routes.")); return false;
    }
    if (!stamp.isEmpty() && !automationStampCurrent(stamp)) {
        setError(QStringLiteral("The session changed; reload the routing settings before applying.")); return false;
    }
    const auto id = trackArg(track);
    if (!m_project.track(id) || (output < 0 && output != -2 && output != -3) || sidechain < -1
        || sends.size() > static_cast<qsizetype>(session::kMaxTrackSends)) {
        setError(QStringLiteral("Choose a valid track, output and sidechain, with at most eight sends.")); return false;
    }
    session::TrackRouting routing;
    routing.output = output == -2 ? session::kInvalidTrack : output == -3 ? session::kNoTrackOutput : static_cast<session::TrackId>(output);
    routing.sidechain = sidechain < 0 ? session::kInvalidTrack : static_cast<session::TrackId>(sidechain);
    for (const auto& value : sends) {
        const auto send = value.toMap(); bool validTarget = false, validDb = false;
        const auto target = send.value("target").toInt(&validTarget); const auto db = send.value("db").toDouble(&validDb);
        if (!validTarget || target < 0 || !validDb || !std::isfinite(db) || db < -120 || db > 12.041199826559248
            || send.value("pre").metaType().id() != QMetaType::Bool) {
            setError(QStringLiteral("Each send needs a destination, pre/post mode and a level from −120 to +12 dB.")); return false;
        }
        routing.sends.push_back({static_cast<session::TrackId>(target),db <= -120 ? 0.0f : static_cast<float>(std::pow(10.0,db/20)),send.value("pre").toBool()});
    }
    auto proposed = m_project; proposed.track(id)->routing = routing;
    const auto graph = session::compileRouting(proposed);
    if (!graph.ok) { setError(QString::fromStdString(graph.error)); return false; }
    if (m_project.track(id)->routing == routing) { setError({}); return true; }
    const auto before = captureEditState(); m_project.track(id)->routing = std::move(routing);
    commit("change routing",id,before); setError({}); return true;
}
