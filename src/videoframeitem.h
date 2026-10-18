#pragma once

#include <QQuickPaintedItem>

class Backend;

// The reference picture on screen: paints whatever frame the Backend holds,
// letterboxed on black, and tells the Backend while it is visible so the frame
// timer runs only when someone is looking. No decoding here; the Backend's
// decoder thread fills the frame and this repaints on its signal.
class VideoFrameItem : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QObject *service READ service WRITE setService NOTIFY serviceChanged)
    Q_PROPERTY(double frameSeconds READ frameSeconds NOTIFY frameChanged)
    Q_PROPERTY(bool hasFrame READ hasFrame NOTIFY frameChanged)

public:
    explicit VideoFrameItem(QQuickItem *parent = nullptr);
    ~VideoFrameItem() override;

    [[nodiscard]] QObject *service() const;
    void setService(QObject *service);
    [[nodiscard]] double frameSeconds() const;
    [[nodiscard]] bool hasFrame() const;

    void paint(QPainter *painter) override;

signals:
    void serviceChanged();
    void frameChanged();

protected:
    void itemChange(ItemChange change, const ItemChangeData &value) override;

private:
    void watch(bool on);

    Backend *m_backend = nullptr;
    bool m_watching = false;
};
