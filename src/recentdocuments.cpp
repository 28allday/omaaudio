#include "recentdocuments.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <utility>

namespace {
constexpr int kEntryLimit = 20;
constexpr qint64 kFileLimit = 512 * 1024;
bool validPath(const QString &path) { return !path.isEmpty() && path.size() <= 16384 && QDir::isAbsolutePath(path) && !path.contains(QChar::Null); }
}

RecentDocuments::RecentDocuments(QString path) : m_path(std::move(path)) {}

QString RecentDocuments::load()
{
    QFile file(m_path);
    if (!file.exists()) { m_entries.clear(); return {}; }
    if (!file.open(QIODevice::ReadOnly) || file.size() > kFileLimit)
        return QStringLiteral("Could not read recent documents: %1").arg(m_path);
    const auto bytes = file.read(kFileLimit + 1);
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(bytes, &error);
    const auto object = doc.object();
    const auto invalid = [&] { return QStringLiteral("Recent documents have an invalid or unsupported format: %1").arg(m_path); };
    if (bytes.size() > kFileLimit || error.error != QJsonParseError::NoError || !doc.isObject()
        || object.value("version") != QJsonValue(1) || !object.value("documents").isArray()
        || object.value("documents").toArray().size() > kEntryLimit) return invalid();
    QList<RecentDocument> entries;
    for (const auto &value : object.value("documents").toArray()) {
        if (!value.isObject()) return invalid();
        const auto row = value.toObject();
        if (!row.value("path").isString() || !row.value("project").isBool() || !validPath(row.value("path").toString())) return invalid();
        RecentDocument entry{row.value("path").toString(), row.value("project").toBool()};
        if (entries.contains(entry)) return invalid();
        entries.append(std::move(entry));
    }
    m_entries = std::move(entries); return {};
}

QString RecentDocuments::remember(const QString &path, bool project)
{
    if (path.isEmpty() || path.contains(QChar::Null)) return QStringLiteral("This document has no valid local path to remember.");
    // Keep symlink/../ semantics. No existence probe or canonicalization here.
    const auto absolute = QDir::isAbsolutePath(path) ? path : QDir::currentPath() + '/' + path;
    return mutate({absolute, project}, 1);
}
QString RecentDocuments::forget(const QString &path, bool project) { return mutate({path, project}, 2); }
QString RecentDocuments::clear() { return mutate({}, 0); }

QString RecentDocuments::mutate(const RecentDocument &document, int operation)
{
    if (operation && !validPath(document.path)) return QStringLiteral("Choose a valid recent document.");
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath())) return QStringLiteral("Could not create the recent-documents directory.");
    QLockFile lock(m_path + QStringLiteral(".lock"));
    if (!lock.tryLock(0)) return QStringLiteral("Recent documents are busy or unavailable. Try again after the other instance finishes.");
    if (const auto error = load(); !error.isEmpty()) return error;
    auto entries = m_entries;
    if (!operation) entries.clear();
    else {
        entries.removeAll(document);
        if (operation == 1) entries.prepend(document);
        while (entries.size() > kEntryLimit) entries.removeLast();
    }
    if (entries == m_entries) return {};
    QJsonArray rows;
    for (const auto &entry : entries) rows.append(QJsonObject{{"path", entry.path}, {"project", entry.project}});
    const auto bytes = QJsonDocument(QJsonObject{{"version", 1}, {"documents", rows}}).toJson();
    if (bytes.size() > kFileLimit) return QStringLiteral("The recent-documents file has reached its size limit.");
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return QStringLiteral("Could not save recent documents: %1").arg(m_path);
    m_entries = std::move(entries); return {};
}
