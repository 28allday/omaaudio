#pragma once

#include <QCoreApplication>
#include <QEvent>
#include <QMouseEvent>
#include <QObject>
#include <QWindow>

// Passive input observation for contextual help. Observe the window dispatch
// once, before controls handle the event; never consume input or steal a grab.
// ShortcutOverride also covers keys consumed by the application's shortcuts.
class KeyWatcher final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool pointerPressed READ pointerPressed NOTIFY pointerPressedChanged)
public:
    explicit KeyWatcher(QObject *parent = nullptr) : QObject(parent)
    {
        if (QCoreApplication::instance()) QCoreApplication::instance()->installEventFilter(this);
    }
    bool pointerPressed() const { return m_pointerPressed; }

signals:
    void interactionStarted();
    void pointerMoved();
    void pointerPressedChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (qobject_cast<QWindow *>(watched)) {
            switch (event->type()) {
            case QEvent::MouseButtonPress:
            case QEvent::MouseButtonDblClick:
                setPointerPressed(true);
                emit interactionStarted();
                break;
            case QEvent::MouseButtonRelease:
                setPointerPressed(static_cast<QMouseEvent *>(event)->buttons() != Qt::NoButton);
                break;
            case QEvent::MouseMove:
                setPointerPressed(static_cast<QMouseEvent *>(event)->buttons() != Qt::NoButton);
                if (!m_pointerPressed) emit pointerMoved();
                break;
            case QEvent::KeyPress:
            case QEvent::ShortcutOverride:
            case QEvent::Wheel:
            case QEvent::TouchBegin:
                emit interactionStarted();
                break;
            case QEvent::WindowDeactivate:
                setPointerPressed(false);
                emit interactionStarted();
                break;
            default:
                break;
            }
        } else if (event->type() == QEvent::InputMethod) {
            // Composed text goes directly to the focused item.
            emit interactionStarted();
        }
        return QObject::eventFilter(watched, event);
    }

private:
    void setPointerPressed(bool pressed)
    {
        if (m_pointerPressed == pressed) return;
        m_pointerPressed = pressed;
        emit pointerPressedChanged();
    }
    bool m_pointerPressed = false;
};
