#include "plugineditor.h"

#include <QSocketNotifier>
#include <QTimer>

#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>

#include <cstdlib>
#include <cstring>
#include <string>

namespace {
xcb_atom_t atom(xcb_connection_t* connection, const char* name)
{
    const auto cookie = xcb_intern_atom(connection, 0, std::uint16_t(std::strlen(name)), name);
    xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(connection, cookie, nullptr);
    const xcb_atom_t result = reply ? reply->atom : XCB_ATOM_NONE;
    std::free(reply);
    return result;
}
} // namespace

std::unique_ptr<PluginEditorWindow> PluginEditorWindow::open(omawave::host::EmbeddedEditor& processor,
                                                             const QString& title, QString& error)
{
    error.clear();
    if (qEnvironmentVariableIsEmpty("DISPLAY")) {
        error = QObject::tr("Effect windows need X11 support (XWayland). It is not available in this session.");
        return nullptr;
    }
    std::unique_ptr<PluginEditorWindow> self(new PluginEditorWindow());
    int screenNumber = 0;
    self->connection_ = xcb_connect(nullptr, &screenNumber);
    if (!self->connection_ || xcb_connection_has_error(self->connection_)) {
        error = QObject::tr("Could not reach the X11 display for the effect window.");
        return nullptr;
    }
    auto roots = xcb_setup_roots_iterator(xcb_get_setup(self->connection_));
    for (int i = 0; i < screenNumber && roots.rem; ++i) xcb_screen_next(&roots);
    xcb_screen_t* screen = roots.data;
    if (!screen) { error = QObject::tr("The X11 display has no screen for the effect window."); return nullptr; }

    auto* c = self->connection_;
    self->window_ = xcb_generate_id(c);
    const std::uint32_t values[] = {screen->black_pixel, XCB_EVENT_MASK_STRUCTURE_NOTIFY};
    xcb_create_window(c, XCB_COPY_FROM_PARENT, self->window_, screen->root, 0, 0, 640, 400, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
    const auto utf8 = title.toUtf8();
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, self->window_, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                        std::uint32_t(utf8.size()), utf8.constData());
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, self->window_, atom(c, "_NET_WM_NAME"), atom(c, "UTF8_STRING"), 8,
                        std::uint32_t(utf8.size()), utf8.constData());
    static constexpr char kClass[] = "omaaudio-effect\0OmaAudio";
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, self->window_, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
                        sizeof kClass, kClass);
    self->protocols_ = atom(c, "WM_PROTOCOLS");
    self->deleteWindow_ = atom(c, "WM_DELETE_WINDOW");
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, self->window_, self->protocols_, XCB_ATOM_ATOM, 32, 1,
                        &self->deleteWindow_);
    const xcb_atom_t dialog = atom(c, "_NET_WM_WINDOW_TYPE_DIALOG");
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, self->window_, atom(c, "_NET_WM_WINDOW_TYPE"), XCB_ATOM_ATOM, 32, 1, &dialog);
    xcb_flush(c);

    self->events_ = new QSocketNotifier(xcb_get_file_descriptor(c), QSocketNotifier::Read, self.get());
    connect(self->events_, &QSocketNotifier::activated, self.get(), &PluginEditorWindow::pump);

    omawave::host::EditorSize size;
    std::string reason;
    self->processor_ = &processor;
    if (!processor.openEditor(self->window_, *self, size, reason)) {
        self->processor_ = nullptr;
        error = QString::fromStdString(reason);
        return nullptr;
    }
    self->resizable_ = size.resizable;
    self->applySize(size.width, size.height, !size.resizable);
    xcb_map_window(c, self->window_);
    xcb_flush(c);
    return self;
}

PluginEditorWindow::~PluginEditorWindow()
{
    closing_ = true;
    // The effect lets go of its window (and its timers) before the window goes.
    if (processor_) processor_->closeEditor();
    for (auto& [id, timer] : timers_) delete timer;
    for (auto& [id, notifier] : descriptors_) delete notifier;
    timers_.clear(); descriptors_.clear();
    delete events_;
    if (connection_) {
        if (window_) xcb_destroy_window(connection_, window_);
        xcb_flush(connection_);
        xcb_disconnect(connection_);
    }
}

void PluginEditorWindow::raise()
{
    if (!connection_ || !window_) return;
    const std::uint32_t above = XCB_STACK_MODE_ABOVE;
    xcb_map_window(connection_, window_);
    xcb_configure_window(connection_, window_, XCB_CONFIG_WINDOW_STACK_MODE, &above);
    xcb_flush(connection_);
}

void PluginEditorWindow::applySize(std::uint32_t width, std::uint32_t height, bool fixed)
{
    width_ = std::max<std::uint32_t>(1, width); height_ = std::max<std::uint32_t>(1, height);
    const std::uint32_t geometry[] = {width_, height_};
    xcb_configure_window(connection_, window_, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, geometry);
    xcb_size_hints_t hints{};
    xcb_icccm_size_hints_set_size(&hints, 0, int32_t(width_), int32_t(height_));
    if (fixed) {
        xcb_icccm_size_hints_set_min_size(&hints, int32_t(width_), int32_t(height_));
        xcb_icccm_size_hints_set_max_size(&hints, int32_t(width_), int32_t(height_));
    }
    xcb_icccm_set_wm_normal_hints(connection_, window_, &hints);
    xcb_flush(connection_);
}

void PluginEditorWindow::pump()
{
    while (xcb_generic_event_t* event = xcb_poll_for_event(connection_)) {
        const auto type = event->response_type & ~0x80;
        if (type == XCB_CLIENT_MESSAGE) {
            const auto* message = reinterpret_cast<xcb_client_message_event_t*>(event);
            if (message->type == protocols_ && message->data.data32[0] == deleteWindow_ && !closing_) {
                closing_ = true;
                QTimer::singleShot(0, this, [this] { emit closed(); });
            }
        } else if (type == XCB_CONFIGURE_NOTIFY && resizable_ && processor_) {
            const auto* configure = reinterpret_cast<xcb_configure_notify_event_t*>(event);
            std::uint32_t width = configure->width, height = configure->height;
            if ((width != width_ || height != height_) && processor_->resizeEditor(width, height)) {
                width_ = width; height_ = height;
                if (width != configure->width || height != configure->height) applySize(width, height, false);
            }
        }
        std::free(event);
    }
    if (xcb_connection_has_error(connection_) && !closing_) {
        closing_ = true;
        QTimer::singleShot(0, this, [this] { emit closed(); });
    }
}

bool PluginEditorWindow::requestEditorResize(std::uint32_t width, std::uint32_t height)
{
    if (!width || !height || width > 16384 || height > 16384) return false;
    applySize(width, height, !resizable_);
    return true;
}

void PluginEditorWindow::editorClosed()
{
    if (closing_) return;
    closing_ = true;
    QTimer::singleShot(0, this, [this] { emit closed(); });
}

std::uint32_t PluginEditorWindow::addEditorTimer(std::uint32_t milliseconds, Callback callback)
{
    const auto id = ++nextId_;
    auto* timer = new QTimer(this);
    timer->setInterval(int(std::min<std::uint32_t>(milliseconds, 60000)));
    connect(timer, &QTimer::timeout, this, [this, id, callback = std::move(callback)] {
        if (!closing_ && timers_.contains(id)) callback();
    });
    timer->start();
    timers_.emplace(id, timer);
    return id;
}

void PluginEditorWindow::removeEditorTimer(std::uint32_t id)
{
    // A plug-in may unregister from inside its own callback.
    if (const auto found = timers_.find(id); found != timers_.end()) {
        found->second->stop(); found->second->deleteLater(); timers_.erase(found);
    }
}

std::uint32_t PluginEditorWindow::addEditorDescriptor(int fd, Callback callback)
{
    if (fd < 0) return 0;
    const auto id = ++nextId_;
    auto* notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    connect(notifier, &QSocketNotifier::activated, this, [this, id, callback = std::move(callback)] {
        if (!closing_ && descriptors_.contains(id)) callback();
    });
    descriptors_.emplace(id, notifier);
    return id;
}

void PluginEditorWindow::removeEditorDescriptor(std::uint32_t id)
{
    if (const auto found = descriptors_.find(id); found != descriptors_.end()) {
        found->second->setEnabled(false); found->second->deleteLater(); descriptors_.erase(found);
    }
}
