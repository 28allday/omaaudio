#pragma once
#include "filebatch.h"

// One lifetime owner for a versioned queue and immutable native-state sidecars.
// An explicit empty root means an in-memory test queue; no fallback after I/O errors.
class FileBatchStore {
public:
    explicit FileBatchStore(QString root);
    ~FileBatchStore();
    QVariantMap status() const;
    bool load(std::vector<FileBatchProcessor::Job>&, int& nextJob, int& nextBatch);
    bool save(const std::vector<FileBatchProcessor::Job>&, int nextJob, int nextBatch, QString& error);
    // Explicit recovery for an unreadable queue this process owns: the damaged
    // manifest is set aside (three kept), unreferenced debris removed, and the
    // queue starts empty. Refused while another process holds the queue.
    bool reset(QString& error);

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
