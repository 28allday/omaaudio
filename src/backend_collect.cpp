#include "backend.h"
#include "io/project_collect.h"

#include <QDir>
#include <QFileInfo>
#include <QTimer>
#include <QUrl>
#include <array>

namespace {
constexpr std::array<const char*, 6> phases{"preparing", "audio", "video", "metadata", "inventory", "publish"};
QString collectionPath(const QString& value)
{
    if (value.isEmpty() || value.contains(QChar::Null)) return {};
    const QUrl url(value, QUrl::StrictMode);
    QString path = value;
    if (value.startsWith("file:", Qt::CaseInsensitive)) {
        if (!url.isValid() || !url.isLocalFile() || (!url.host().isEmpty() && url.host().compare("localhost", Qt::CaseInsensitive) != 0)
            || !url.userInfo().isEmpty() || url.port(-1) != -1 || url.hasQuery() || url.hasFragment()) return {};
        auto local = url; local.setHost({}); path = local.toLocalFile();
    } else if (!url.scheme().isEmpty() && value.contains("://")) return {};
    if (path.isEmpty() || path.contains(QChar::Null) || path.endsWith('/')) return {};
    const auto name = path.section('/', -1); if (name == "." || name == "..") return {};
    if (!path.endsWith(".omaaudio") && !path.endsWith(".omawave")) path += ".omaaudio";
    return QDir::isAbsolutePath(path) ? path : QDir::currentPath() + '/' + path;
}
}

QVariantMap Backend::collectionStatus() const
{
    auto result = m_collectionStatus;
    if (m_collecting) {
        result["phase"] = QString::fromLatin1(phases.at(m_collectPhase.load(std::memory_order_relaxed)));
        result["progress"] = double(m_collectProgress.load(std::memory_order_relaxed));
    }
    return result;
}

bool Backend::collectProject(const QString& path)
{
    if (m_shuttingDown || m_collecting || m_loading || recording()) {
        setError(tr("Finish opening or recording, and wait for any active collection before collecting the project.")); return false;
    }
    const auto target = collectionPath(path);
    const QFileInfo info(target);
    if (target.isEmpty() || info.exists() || info.isSymLink()) {
        setError(tr("Choose a new local project path. Collection never replaces an existing file or folder.")); return false;
    }
    if (m_collectThread.joinable()) m_collectThread.join();
    m_cancelCollect.store(false, std::memory_order_relaxed);
    m_collectPhase.store(0, std::memory_order_relaxed); m_collectProgress.store(0, std::memory_order_relaxed);
    m_collectionStatus = {{"state", "collecting"}, {"path", target}, {"published", false}};
    m_collecting = true; setError({});
    if (!m_collectTimer) {
        m_collectTimer = new QTimer(this); m_collectTimer->setInterval(100);
        connect(m_collectTimer, &QTimer::timeout, this, &Backend::collectionChanged);
    }
    m_collectTimer->start();
    requestTimelineSnapshot(
        [this] { return m_cancelCollect.load(std::memory_order_relaxed) || m_shuttingDown; },
        [this, target](DeliverySnapshot snapshot, QString error, bool cancelled) {
            if (cancelled || !error.isEmpty()) {
                m_collectTimer->stop(); m_collecting = false;
                m_collectionStatus = {{"state", cancelled ? "cancelled" : "failed"},
                    {"path", target}, {"published", false}, {"error", error}, {"progress", 0.0}};
                if (!error.isEmpty()) setError(error);
                emit collectionChanged();
                return;
            }
            startCollectedSnapshot(std::move(snapshot), target);
        }, /*clipEffects=*/false); // a collected project keeps its editable clip effects
    emit collectionChanged();
    return true;
}

void Backend::startCollectedSnapshot(DeliverySnapshot captured, const QString& target)
{
    auto snapshot = std::move(captured.project);
    auto view = std::move(captured.view);
    m_collectThread = std::thread([this, snapshot, view, target] {
        omawave::io::SaveOptions options; options.cancel = &m_cancelCollect;
        options.progress = [this](const std::string& phase, float progress) {
            for (std::size_t index = 1; index < phases.size(); ++index) if (phase == phases[index]) {
                m_collectProgress.store(progress, std::memory_order_relaxed);
                m_collectPhase.store(index, std::memory_order_relaxed); break;
            }
        };
        const auto result = omawave::io::collectProject(snapshot, view, target.toStdString(), options);
        QMetaObject::invokeMethod(this, [this, result] {
            if (m_collectThread.joinable()) m_collectThread.join();
            if (m_shuttingDown) return;
            m_collectTimer->stop(); m_collecting = false;
            m_collectionStatus = {{"state", result.ok ? "done" : result.cancelled ? "cancelled" : "failed"},
                {"path", QString::fromStdString(result.path)}, {"published", result.published},
                {"error", QString::fromStdString(result.error)}, {"sources", qulonglong(result.sources)},
                {"plugins", qulonglong(result.plugins)}, {"video", result.video}, {"progress", result.ok ? 1.0 : 0.0}};
            if (!result.ok) setError(result.published
                ? tr("The collected project exists at %1, but its final disk sync failed: %2").arg(QString::fromStdString(result.path), QString::fromStdString(result.error))
                : QString::fromStdString(result.error));
            emit collectionChanged();
        }, Qt::QueuedConnection);
    });
}

void Backend::cancelCollection()
{
    m_cancelCollect.store(true, std::memory_order_relaxed);
    pumpDeliverySnapshots();
}
