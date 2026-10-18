#pragma once

#include "render/file_batch.h"
#include "lv2/host_features.h"
#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QStringList>
#include <atomic>
#include <functional>
#include <memory>
#include <thread>

class FileBatchStore;

// Independent of project/Undo state. GUI owns the queue and plugin lifecycle;
// one private worker owns the current file's decode and offline processing.
class FileBatchProcessor : public QObject {
public:
    struct Options {
        QString directory, suffix = "-processed", effects = "No effects";
        omawave::render::ExportSpec output;
        std::vector<omawave::session::Insert> inserts;
        QVariantMap settings;
        bool effectsCaptured = true;
    };
    using ChainBuilder = std::function<std::unique_ptr<omawave::lv2::Chain>(
        const std::vector<omawave::session::Insert>&, omawave::lv2::HostFeatures&, unsigned, std::string&)>;
    FileBatchProcessor(omawave::lv2::UridMap& urids, ChainBuilder builder,
                       std::function<void()> changed, QObject* parent, QString storageRoot);
    ~FileBatchProcessor() override;
    static QVariantList preview(const QStringList& paths, const Options& options, QString& error);
    int enqueue(const QStringList& paths, Options options, QString& error);
    bool awaitingEffects(int batch) const;
    void prepareEffects(int batch, Options options, const QString& error, bool cancelled);
    QVariantList jobs() const;
    bool busy() const;
    QVariantMap status() const;
    bool resume(int job, QString& error);
    void cancel(int job = -1, int batch = -1);
    bool retry(int job, QString& error);
    void clearFinished();
    // Recovery for an unreadable or damaged queue: starts an empty queue, keeping
    // the damaged file aside. Refused while a file is processing.
    bool resetQueue(QString& error);
    void shutdown();
private:
    friend class FileBatchStore;
    struct Job {
        int id = -1, batch = -1;
        QString input, output, state = "queued", message, warning;
        float progress = 0;
        bool cancelRequested = false;
        QVariantMap report;
        std::shared_ptr<const Options> options;
    };
    Job* find(int id);
    void pump();
    void finishRead();
    void finishWrite();
    void complete(QString state, QString message);
    void notify();
    bool persist(const std::vector<Job>& jobs, int nextJob, int nextBatch, QString& error);
    bool available(QString& error) const;
    void persistenceFailed();
    void applyWriteResult(Job& job);
    QString retryProblem(const Job& job) const;
    std::unique_ptr<FileBatchStore> m_store;
    omawave::lv2::UridMap& m_urids;
    ChainBuilder m_builder;
    std::function<void()> m_changed;
    QTimer m_timer;
    std::vector<Job> m_jobs;
    int m_nextJob = 1, m_nextBatch = 1, m_active = -1;
    bool m_closing = false;
    std::atomic<bool> m_cancel{false};
    std::atomic<float> m_progress{0};
    std::thread m_worker;
    omawave::render::PreparedFile m_prepared;
    omawave::render::ExportResult m_result;
    std::unique_ptr<omawave::lv2::HostFeatures> m_features;
    std::unique_ptr<omawave::lv2::Chain> m_chain;
};
