#include "pluginlibrary.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUrl>

#include <utility>

namespace {
constexpr int kLibraryLimit = 1024;
constexpr int kRecentLimit = 32;
constexpr qint64 kFileLimit = 4 * 1024 * 1024;
bool validUri(const QString &uri)
{
    const QUrl parsed(uri, QUrl::StrictMode);
    return !uri.isEmpty() && uri.size() <= 4096 && !uri.contains(QChar::Null)
        && !uri.contains('\n') && !uri.contains('\r')
        && parsed.isValid() && !parsed.scheme().isEmpty();
}
bool readList(const QJsonValue &value, int limit, QStringList &out)
{
    if (!value.isArray() || value.toArray().size() > limit)
        return false;
    for (const auto &item : value.toArray()) {
        if (!item.isString() || !validUri(item.toString()))
            return false;
        if (!out.contains(item.toString()))
            out.append(item.toString());
    }
    return true;
}
QString invalidUriError() { return QStringLiteral("A plugin needs a valid LV2 URI."); }
}

PluginLibrary::PluginLibrary(QString path) : m_path(std::move(path)) {}

QString PluginLibrary::load()
{
    QFile file(m_path);
    if (!file.exists()) {
        m_loadError.clear();
        return {};
    }
    if (!file.open(QIODevice::ReadOnly) || file.size() > kFileLimit)
        return m_loadError = QStringLiteral("Could not read the plugin library: %1").arg(m_path);
    const auto bytes = file.read(kFileLimit + 1);
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    const auto object = document.object();
    QStringList favorites, recent, disabled;
    if (bytes.size() > kFileLimit || error.error != QJsonParseError::NoError
        || !document.isObject() || object.value("version").toInt() != 1
        || !readList(object.value("favorites"), kLibraryLimit, favorites)
        || !readList(object.value("recent"), kRecentLimit, recent)
        || !readList(object.value("disabled"), kLibraryLimit, disabled))
        return m_loadError = QStringLiteral("The plugin library has an unsupported or invalid format: %1").arg(m_path);
    m_favorites = std::move(favorites);
    m_recent = std::move(recent);
    m_disabled = std::move(disabled);
    m_loadError.clear();
    return {};
}

QString PluginLibrary::save(QStringList favorites, QStringList recent, QStringList disabled)
{
    if (!m_loadError.isEmpty())
        return m_loadError;
    if (favorites.size() > kLibraryLimit || disabled.size() > kLibraryLimit)
        return QStringLiteral("The plugin library is limited to 1024 entries per list.");
    if (favorites == m_favorites && recent == m_recent && disabled == m_disabled)
        return {};
    const QJsonObject object{{"version", 1}, {"favorites", QJsonArray::fromStringList(favorites)},
                             {"recent", QJsonArray::fromStringList(recent)},
                             {"disabled", QJsonArray::fromStringList(disabled)}};
    const auto bytes = QJsonDocument(object).toJson();
    if (bytes.size() > kFileLimit)
        return QStringLiteral("The plugin library has reached its file size limit.");
    QSaveFile file(m_path);
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath()) || !file.open(QIODevice::WriteOnly)
        || file.write(bytes) != bytes.size() || !file.commit())
        return QStringLiteral("Could not save the plugin library: %1").arg(m_path);
    m_favorites = std::move(favorites);
    m_recent = std::move(recent);
    m_disabled = std::move(disabled);
    return {};
}

QString PluginLibrary::setFavorite(const QString &uri, bool favorite)
{
    if (!validUri(uri)) return invalidUriError();
    if (m_favorites.contains(uri) == favorite) return m_loadError;
    auto favorites = m_favorites;
    favorites.removeAll(uri);
    if (favorite) favorites.append(uri);
    return save(std::move(favorites), m_recent, m_disabled);
}

QString PluginLibrary::setEnabled(const QString &uri, bool enabled)
{
    if (!validUri(uri)) return invalidUriError();
    if (m_disabled.contains(uri) != enabled) return m_loadError;
    auto disabled = m_disabled;
    disabled.removeAll(uri);
    if (!enabled) disabled.append(uri);
    return save(m_favorites, m_recent, std::move(disabled));
}

QString PluginLibrary::remember(const QString &uri)
{
    if (!validUri(uri)) return invalidUriError();
    auto recent = m_recent;
    recent.removeAll(uri);
    recent.prepend(uri);
    while (recent.size() > kRecentLimit) recent.removeLast();
    return save(m_favorites, std::move(recent), m_disabled);
}

QString PluginLibrary::clearRecent() { return save(m_favorites, {}, m_disabled); }
