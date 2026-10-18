#pragma once

#include <QObject>
#include <QProcessEnvironment>
#include <QStringList>
#include <atomic>

class QQuickWindow;

namespace omaaudio::graphics {
inline constexpr auto probeVariable = "OMAAUDIO_INTERNAL_GRAPHICS_PROBE";
inline constexpr auto restartVariable = "OMAAUDIO_INTERNAL_GRAPHICS_RESTART";
inline constexpr auto recoveryVariable = "OMAAUDIO_INTERNAL_GRAPHICS_RECOVERY";

enum class Choice { ProbeVulkan, Software, Explicit };
Choice startupChoice(const QProcessEnvironment &environment, const QString &platform,
                     bool restarting);
void useSoftware();
// Executes only in an isolated child, before PipeWire/Backend construction.
int probeVulkan();
bool runProbe(const QString &executable, const QStringList &arguments,
              const QProcessEnvironment &environment, QString &diagnostic,
              int timeoutMs = 5000);
// Called only after all Qt, audio and session objects have been destroyed.
int restartInSoftware(const QString &executable, const QStringList &arguments,
                      const QString &checkpoint);

class GraphicsGuard final : public QObject {
    Q_OBJECT
public:
    explicit GraphicsGuard(QQuickWindow *window, QObject *parent = nullptr);
    void disarm();
signals:
    void failed(const QString &message);
protected:
    bool eventFilter(QObject *object, QEvent *event) override;
private:
    void report(const QString &message);
    QQuickWindow *m_window;
    std::atomic_bool m_armed{true};
    std::atomic_bool m_presented{false};
    std::atomic_bool m_exposed{false};
    std::atomic_bool m_reported{false};
};
} // namespace omaaudio::graphics
