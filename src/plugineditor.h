#pragma once

#include "host/editor.h"
#include "host/instance.h"

#include <QObject>
#include <QString>

#include <map>
#include <memory>

class QSocketNotifier;
class QTimer;
struct xcb_connection_t;

// One effect's own editor, in its own window. Plug-in editors on Linux draw into
// X11 windows, so this window lives on the X server (XWayland under a Wayland
// desktop) through a private connection; OmaAudio's Qt windows are unaffected.
// Main thread only. Close (destroy) it before its processor is destroyed.
class PluginEditorWindow final : public QObject, public omawave::host::EditorHost {
    Q_OBJECT
public:
    [[nodiscard]] static std::unique_ptr<PluginEditorWindow> open(omawave::host::EmbeddedEditor& editor,
                                                                  const QString& title, QString& error);
    ~PluginEditorWindow() override;
    PluginEditorWindow(const PluginEditorWindow&) = delete;
    PluginEditorWindow& operator=(const PluginEditorWindow&) = delete;

    [[nodiscard]] omawave::host::EmbeddedEditor* editor() const noexcept { return processor_; }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    void raise();
    // The effect lets go of its editor now; the window itself may go later.
    void releaseProcessor() noexcept { if (processor_) processor_->closeEditor(); processor_ = nullptr; }

    bool requestEditorResize(std::uint32_t width, std::uint32_t height) override;
    void editorClosed() override;
    std::uint32_t addEditorTimer(std::uint32_t milliseconds, Callback) override;
    void removeEditorTimer(std::uint32_t id) override;
    std::uint32_t addEditorDescriptor(int fd, Callback) override;
    void removeEditorDescriptor(std::uint32_t id) override;

signals:
    // The person closed the window, or the effect closed its own editor.
    void closed();

private:
    PluginEditorWindow() = default;
    void pump();
    void applySize(std::uint32_t width, std::uint32_t height, bool fixed);
    xcb_connection_t* connection_ = nullptr;
    std::uint32_t window_ = 0, protocols_ = 0, deleteWindow_ = 0;
    std::uint32_t width_ = 0, height_ = 0;
    bool resizable_ = false, closing_ = false;
    QSocketNotifier* events_ = nullptr;
    omawave::host::EmbeddedEditor* processor_ = nullptr;
    std::uint32_t nextId_ = 0;
    std::map<std::uint32_t, QTimer*> timers_;
    std::map<std::uint32_t, QSocketNotifier*> descriptors_;
};
