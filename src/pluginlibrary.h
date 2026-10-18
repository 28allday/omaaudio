#pragma once

#include <QString>
#include <QStringList>

// Personal catalogue choices, separate from document state and the LV2 world.
// GUI-thread only. Disk failure leaves the preceding in-memory state intact.
class PluginLibrary {
public:
    explicit PluginLibrary(QString path);
    [[nodiscard]] const QStringList &favorites() const { return m_favorites; }
    [[nodiscard]] const QStringList &recent() const { return m_recent; }
    [[nodiscard]] const QStringList &disabled() const { return m_disabled; }
    [[nodiscard]] QString load();
    [[nodiscard]] QString setFavorite(const QString &uri, bool favorite);
    [[nodiscard]] QString setEnabled(const QString &uri, bool enabled);
    [[nodiscard]] QString remember(const QString &uri);
    [[nodiscard]] QString clearRecent();

private:
    [[nodiscard]] QString save(QStringList favorites, QStringList recent, QStringList disabled);
    QString m_path;
    QString m_loadError;
    QStringList m_favorites, m_recent, m_disabled;
};
