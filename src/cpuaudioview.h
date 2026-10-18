#pragma once

#include "displaycache.h"
#include <QCache>
#include <QImage>
#include <QJsonObject>
#include <QMutex>
#include <QQuickAsyncImageProvider>
#include <QThreadPool>
#include <atomic>
#include <optional>

// CPU reference for the two audio display shaders. Consumes immutable display
// caches, never audio sources. CPU views and raw GPU textures share this bounded
// decoded cache. Provider requests hold file leases before joining a work queue.
class CpuAudioRenderer {
public:
    QImage render(const QJsonObject &request, QSize size, const std::atomic_bool &cancel,
                  QString &error);
    QImage load(const QString &path, QString &error);
    int cachedKiB();
private:
    QMutex m_mutex;
    QCache<QString, QImage> m_cache{32 * 1024}; // KiB, 32 MiB of retained decoded data
};

class CpuAudioViewProvider final : public QQuickAsyncImageProvider {
public:
    explicit CpuAudioViewProvider(std::optional<DisplayCache> cache = std::nullopt,
        std::shared_ptr<CpuAudioRenderer> renderer = std::make_shared<CpuAudioRenderer>());
    ~CpuAudioViewProvider() override;
    QQuickImageResponse *requestImageResponse(const QString &id, const QSize &size) override;
private:
    std::optional<DisplayCache> m_cache;
    std::shared_ptr<CpuAudioRenderer> m_renderer;
    QThreadPool m_pool;
};

class AudioTextureProvider final : public QQuickAsyncImageProvider {
public:
    explicit AudioTextureProvider(DisplayCache cache,
        std::shared_ptr<CpuAudioRenderer> renderer = std::make_shared<CpuAudioRenderer>());
    ~AudioTextureProvider() override;
    QQuickImageResponse *requestImageResponse(const QString &id, const QSize &size) override;
private:
    DisplayCache m_cache;
    std::shared_ptr<CpuAudioRenderer> m_renderer;
    QThreadPool m_pool;
};
