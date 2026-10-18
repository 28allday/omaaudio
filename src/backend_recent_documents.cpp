#include "backend.h"
#include <QFileInfo>
#include <algorithm>

QVariantList Backend::recentDocuments() const
{
    QVariantList out;
    for (const auto &entry : m_recentDocuments.entries()) {
        const auto name = QFileInfo(entry.path).fileName();
        out.append(QVariantMap{{"path", entry.path}, {"project", entry.project}, {"name", name.isEmpty() ? entry.path : name}});
    }
    return out;
}

bool Backend::reloadRecentDocuments()
{
    m_recentDocumentsError = m_recentDocuments.load();
    emit recentDocumentsChanged();
    return m_recentDocumentsError.isEmpty();
}

void Backend::rememberDocument(const QString &path, bool project)
{
    // Recovered capture spools are temporary; a later project save retires them.
    if (!project && std::any_of(m_recordedSpools.cbegin(), m_recordedSpools.cend(),
        [&](const auto &spool) { return spool.path == path; })) return;
    // History is optional metadata. Its errors must never abort the primary
    // open/save or the interface's pending Save-and-continue action.
    m_recentDocumentsError = m_recentDocuments.remember(path, project);
    emit recentDocumentsChanged();
}

bool Backend::forgetRecentDocument(const QString &path, bool project)
{
    m_recentDocumentsError = m_recentDocuments.forget(path, project);
    emit recentDocumentsChanged();
    return m_recentDocumentsError.isEmpty();
}

bool Backend::clearRecentDocuments()
{
    m_recentDocumentsError = m_recentDocuments.clear();
    emit recentDocumentsChanged();
    return m_recentDocumentsError.isEmpty();
}
