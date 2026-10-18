#include "videoframeitem.h"

#include "backend.h"

#include <QPainter>

VideoFrameItem::VideoFrameItem(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    setFillColor(Qt::black);
    setRenderTarget(QQuickPaintedItem::Image);
}

VideoFrameItem::~VideoFrameItem()
{
    watch(false);
}

QObject *VideoFrameItem::service() const { return m_backend; }

void VideoFrameItem::setService(QObject *service)
{
    auto *backend = qobject_cast<Backend *>(service);
    if (backend == m_backend) return;
    watch(false);
    if (m_backend) disconnect(m_backend, nullptr, this, nullptr);
    m_backend = backend;
    if (m_backend) {
        connect(m_backend, &Backend::videoFrameChanged, this, [this] { update(); emit frameChanged(); });
        connect(m_backend, &Backend::videoChanged, this, [this] { update(); emit frameChanged(); });
        // The Backend may go before the scene does; never call into a gone one.
        connect(m_backend, &QObject::destroyed, this, [this] { m_backend = nullptr; m_watching = false; });
    }
    if (isVisible()) watch(true);
    emit serviceChanged();
    update();
}

double VideoFrameItem::frameSeconds() const { return m_backend ? m_backend->videoFrameSeconds() : -1.0; }
bool VideoFrameItem::hasFrame() const { return m_backend && !m_backend->videoFrame().isNull(); }

void VideoFrameItem::watch(bool on)
{
    if (on == m_watching || !m_backend) { if (!m_backend) m_watching = false; return; }
    m_watching = on;
    m_backend->watchVideo(on ? 1 : -1);
}

void VideoFrameItem::itemChange(ItemChange change, const ItemChangeData &value)
{
    QQuickPaintedItem::itemChange(change, value);
    if (change == ItemVisibleHasChanged) watch(value.boolValue);
}

void VideoFrameItem::paint(QPainter *painter)
{
    if (!m_backend) return;
    const QImage &frame = m_backend->videoFrame();
    if (frame.isNull() || width() <= 0 || height() <= 0) return;
    // Letterboxed: the whole picture, its shape kept, centred.
    QRectF target(0, 0, width(), height());
    const double scale = std::min(width() / frame.width(), height() / frame.height());
    const QSizeF size(frame.width() * scale, frame.height() * scale);
    target = QRectF(QPointF((width() - size.width()) / 2, (height() - size.height()) / 2), size);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter->drawImage(target, frame);
}
