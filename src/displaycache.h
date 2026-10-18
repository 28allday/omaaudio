#pragma once

#include <QStringList>
#include <QSet>
#include <QUrl>
#include <memory>

// Private, immutable display files. Producers and accepted image requests share
// batches; the final reader releases the files and, when appropriate, directory.
class DisplayCache {
    struct State;
public:
    class Batch {
    public:
        ~Batch();
        Batch(const Batch &) = delete;
        Batch &operator=(const Batch &) = delete;
        [[nodiscard]] QString token() const;
        [[nodiscard]] bool contains(const QString &path) const;
        [[nodiscard]] QUrl textureUrl(const QString &path) const;
    private:
        friend class DisplayCache;
        Batch(std::shared_ptr<State> state, quint64 id, QSet<QString> paths);
        std::shared_ptr<State> m_state;
        quint64 m_id;
        QSet<QString> m_paths;
    };
    using Lease = std::shared_ptr<const Batch>;
    struct Access {
        Lease lease;
        bool retired = false;
        QString error;
    };

    explicit DisplayCache(const QString &directoryTemplate);
    [[nodiscard]] QString path() const;
    [[nodiscard]] bool isValid() const;
    [[nodiscard]] QString errorString() const;
    [[nodiscard]] Lease adopt(const QStringList &paths, QString &error) const;
    [[nodiscard]] Access acquire(const QString &token) const;
    [[nodiscard]] qsizetype liveBatches() const;
private:
    std::shared_ptr<State> m_state;
};
