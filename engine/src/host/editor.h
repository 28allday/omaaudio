#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace omawave::host {

// What an embedded native plug-in editor needs from the window that holds it.
// Everything here runs on the main thread, which also owns the plug-in's
// controller; the window host turns timers and descriptors into its own event
// loop and keeps the window sized to the editor.
class EditorHost {
public:
    virtual ~EditorHost() = default;
    using Callback = std::function<void()>;
    // The editor asks for a new size. True when the window took it.
    virtual bool requestEditorResize(std::uint32_t width, std::uint32_t height) = 0;
    // The plug-in closed its own editor (CLAP gui.closed); the window goes too.
    virtual void editorClosed() = 0;
    // Repeating timers and readable descriptors for plug-in run loops (VST3
    // Linux::IRunLoop, CLAP timer-support/posix-fd-support). Zero is "none".
    virtual std::uint32_t addEditorTimer(std::uint32_t milliseconds, Callback) = 0;
    virtual void removeEditorTimer(std::uint32_t id) = 0;
    virtual std::uint32_t addEditorDescriptor(int fd, Callback) = 0;
    virtual void removeEditorDescriptor(std::uint32_t id) = 0;
};

struct EditorSize {
    std::uint32_t width = 0, height = 0;
    bool resizable = false;
};

// Anything that can draw into a window the host owns: a processor's own
// editor (host::Instance) or a separately loaded plug-in UI (LV2 X11UI).
class EmbeddedEditor {
public:
    virtual ~EmbeddedEditor() = default;
    [[nodiscard]] virtual bool openEditor(std::uint64_t parent, EditorHost&, EditorSize&, std::string& error) = 0;
    virtual bool resizeEditor(std::uint32_t& width, std::uint32_t& height) noexcept = 0;
    virtual void closeEditor() noexcept = 0;
};

} // namespace omawave::host
