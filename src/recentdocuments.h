#pragma once
#include <QList>
#include <QString>

struct RecentDocument {
    QString path;
    bool project = false;
    bool operator==(const RecentDocument &) const = default;
};

class RecentDocuments {
public:
    explicit RecentDocuments(QString path);
    [[nodiscard]] const QList<RecentDocument> &entries() const { return m_entries; }
    [[nodiscard]] QString load();
    [[nodiscard]] QString remember(const QString &path, bool project);
    [[nodiscard]] QString forget(const QString &path, bool project);
    [[nodiscard]] QString clear();
private:
    [[nodiscard]] QString mutate(const RecentDocument &document, int operation);
    QString m_path;
    QList<RecentDocument> m_entries;
};
