#include "displaycache.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QTemporaryDir>
#include <limits>

struct DisplayCache::State {
    explicit State(const QString &pattern) : directory(pattern) {}
    QTemporaryDir directory;
    QMutex mutex;
    quint64 issued = 0;
    QHash<quint64, std::weak_ptr<const Batch>> batches;
    QSet<QString> ownedPaths;
};

DisplayCache::DisplayCache(const QString &directoryTemplate)
    : m_state(std::make_shared<State>(directoryTemplate)) {}
QString DisplayCache::path() const { return m_state->directory.path(); }
bool DisplayCache::isValid() const { return m_state->directory.isValid(); }
QString DisplayCache::errorString() const { return m_state->directory.errorString(); }

DisplayCache::Batch::Batch(std::shared_ptr<State> state, quint64 id, QSet<QString> paths)
    : m_state(std::move(state)), m_id(id), m_paths(std::move(paths)) {}
DisplayCache::Batch::~Batch()
{
    // Keep deletion and ownership removal under the same lock: another producer
    // must not adopt a filename while its preceding owner is unlinking it.
    QMutexLocker lock(&m_state->mutex);
    for (const auto &path : m_paths) {
        QFile::remove(path);
        m_state->ownedPaths.remove(path);
    }
    m_state->batches.remove(m_id);
}
QString DisplayCache::Batch::token() const { return QString::number(m_id); }
bool DisplayCache::Batch::contains(const QString &path) const { return m_paths.contains(path); }
QUrl DisplayCache::Batch::textureUrl(const QString &path) const
{
    if (!contains(path)) return {};
    const auto request = QJsonDocument(QJsonObject{{"asset", token()}, {"texture", path}}).toJson(QJsonDocument::Compact);
    return QUrl(QStringLiteral("image://audio-texture/") + QString::fromLatin1(QUrl::toPercentEncoding(QString::fromUtf8(request))));
}

DisplayCache::Lease DisplayCache::adopt(const QStringList &paths, QString &error) const
{
    error.clear();
    if (!isValid()) { error = errorString(); return {}; }
    if (paths.isEmpty() || paths.size() > 4096) { error = "Invalid display-cache batch size."; return {}; }
    // Declare the lease before the lock so a failed allocation/registration
    // cannot destroy a constructed batch while this thread still holds the lock.
    Lease batch;
    QMutexLocker lock(&m_state->mutex);
    QSet<QString> unique;
    qsizetype characters = 0;
    for (const auto &path : paths) {
        const QFileInfo file(path);
        characters += path.size();
        if (path.size() > 16384 || characters > 16 * 1024 * 1024 || path.contains(QChar::Null)
            || !file.isAbsolute() || file.absolutePath() != m_state->directory.path()
            || path != QDir(m_state->directory.path()).filePath(file.fileName())
            || !file.isFile() || file.isSymLink() || unique.contains(path)
            || m_state->ownedPaths.contains(path)) {
            error = "Display-cache files must be unique regular files inside their private directory.";
            return {};
        }
        unique.insert(path);
    }
    if (m_state->issued == std::numeric_limits<quint64>::max()) {
        error = "Display-cache generation limit reached."; return {};
    }
    const auto id = ++m_state->issued;
    batch = Lease(new Batch(m_state, id, unique));
    m_state->batches.insert(id, batch);
    m_state->ownedPaths.unite(unique);
    return batch;
}

DisplayCache::Access DisplayCache::acquire(const QString &token) const
{
    bool valid = !token.isEmpty() && token.size() <= 20 && token.front() != '0';
    for (const auto ch : token) valid = valid && ch >= '0' && ch <= '9';
    bool parsed = false;
    const quint64 id = token.toULongLong(&parsed);
    if (!valid || !parsed) return {{}, false, "Invalid display-cache generation."};
    QMutexLocker lock(&m_state->mutex);
    if (id > m_state->issued) return {{}, false, "Unknown display-cache generation."};
    const auto entry = m_state->batches.constFind(id);
    if (entry != m_state->batches.cend()) {
        if (auto lease = entry.value().lock()) return {std::move(lease), false, {}};
    }
    return {{}, true, {}};
}
qsizetype DisplayCache::liveBatches() const
{
    QMutexLocker lock(&m_state->mutex);
    return m_state->batches.size();
}
