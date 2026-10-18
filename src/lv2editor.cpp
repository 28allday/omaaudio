#include "lv2editor.h"

#include <lilv/lilv.h>
#include <xcb/xcb.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>

namespace {
struct UiChoice {
    std::string uri, binary, bundle;
    std::vector<std::uint32_t> ports;
};

// The first X11 interface the plug-in declares, with its control ports.
std::optional<UiChoice> findUi(omawave::lv2::World& world, const std::string& pluginUri)
{
    const LilvPlugin* plugin = world.lilvPlugin(pluginUri);
    if (!plugin) return std::nullopt;
    LilvWorld* lilv = world.raw();
    LilvNode* x11 = lilv_new_uri(lilv, LV2_UI__X11UI);
    LilvNode* control = lilv_new_uri(lilv, LV2_CORE__ControlPort);
    std::optional<UiChoice> result;
    LilvUIs* uis = lilv_plugin_get_uis(plugin);
    LILV_FOREACH(uis, it, uis) {
        const LilvUI* ui = lilv_uis_get(uis, it);
        if (!lilv_ui_is_a(ui, x11)) continue;
        char* binary = lilv_file_uri_parse(lilv_node_as_uri(lilv_ui_get_binary_uri(ui)), nullptr);
        char* bundle = lilv_file_uri_parse(lilv_node_as_uri(lilv_ui_get_bundle_uri(ui)), nullptr);
        if (binary && bundle) {
            UiChoice choice{lilv_node_as_uri(lilv_ui_get_uri(ui)), binary, bundle, {}};
            std::string& path = choice.bundle;
            if (!path.empty() && path.back() != '/') path += '/';
            for (std::uint32_t i = 0; i < lilv_plugin_get_num_ports(plugin); ++i)
                if (lilv_port_is_a(plugin, lilv_plugin_get_port_by_index(plugin, i), control)) choice.ports.push_back(i);
            result = std::move(choice);
        }
        lilv_free(binary); lilv_free(bundle);
        if (result) break;
    }
    lilv_uis_free(uis);
    lilv_node_free(control);
    lilv_node_free(x11);
    return result;
}
} // namespace

bool Lv2UiEditor::available(omawave::lv2::World& world, const std::string& pluginUri)
{
    return findUi(world, pluginUri).has_value();
}

std::unique_ptr<Lv2UiEditor> Lv2UiEditor::create(omawave::lv2::World& world, omawave::lv2::UridMap& urids,
    const std::string& pluginUri, Write write, Read read, std::string& error)
{
    auto choice = findUi(world, pluginUri);
    if (!choice) { error = "this effect has no X11 window of its own"; return nullptr; }
    std::unique_ptr<Lv2UiEditor> self(new Lv2UiEditor());
    self->urids_ = &urids;
    self->pluginUri_ = pluginUri;
    self->uiUri_ = choice->uri;
    self->binary_ = choice->binary;
    self->bundle_ = choice->bundle;
    self->ports_ = std::move(choice->ports);
    self->write_ = std::move(write);
    self->read_ = std::move(read);
    return self;
}

Lv2UiEditor::~Lv2UiEditor()
{
    closeEditor();
    if (library_) dlclose(library_);
}

void Lv2UiEditor::writeTrampoline(LV2UI_Controller controller, std::uint32_t port, std::uint32_t size,
                                  std::uint32_t protocol, const void* buffer)
{
    auto* self = static_cast<Lv2UiEditor*>(controller);
    // Only float control changes; atom/event protocols are not routed to the processor.
    if (!self || protocol != 0 || size != sizeof(float) || !buffer) return;
    float value;
    std::memcpy(&value, buffer, sizeof value);
    if (!std::isfinite(value)) return;
    self->sent_[port] = value; // already shown by the interface itself
    if (self->write_) self->write_(port, value);
}

int Lv2UiEditor::resizeTrampoline(LV2UI_Feature_Handle handle, int width, int height)
{
    auto* self = static_cast<Lv2UiEditor*>(handle);
    if (!self || width <= 0 || height <= 0) return 1;
    self->requestedWidth_ = std::uint32_t(width); self->requestedHeight_ = std::uint32_t(height);
    if (self->host_ && self->handle_) return self->host_->requestEditorResize(self->requestedWidth_, self->requestedHeight_) ? 0 : 1;
    return 0; // remembered for the window's first size
}

bool Lv2UiEditor::openEditor(std::uint64_t parent, omawave::host::EditorHost& host,
                             omawave::host::EditorSize& size, std::string& error)
{
    if (handle_) { error = "the effect's window is already open"; return false; }
    if (!library_) {
        library_ = dlopen(binary_.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!library_) { error = std::string("could not load the effect's interface: ") + dlerror(); return false; }
        const auto function = reinterpret_cast<LV2UI_DescriptorFunction>(dlsym(library_, "lv2ui_descriptor"));
        for (std::uint32_t index = 0; function && !descriptor_; ++index) {
            const LV2UI_Descriptor* candidate = function(index);
            if (!candidate) break;
            if (candidate->URI && uiUri_ == candidate->URI) descriptor_ = candidate;
        }
        if (!descriptor_) { error = "the effect's interface library does not describe it"; return false; }
    }
    resize_.handle = this;
    resize_.ui_resize = &Lv2UiEditor::resizeTrampoline;
    const LV2_Feature parentFeature{LV2_UI__parent, reinterpret_cast<void*>(static_cast<std::uintptr_t>(parent))};
    const LV2_Feature resizeFeature{LV2_UI__resize, &resize_};
    const LV2_Feature idleFeature{LV2_UI__idleInterface, nullptr};
    const LV2_Feature* features[] = {urids_->mapFeature(), urids_->unmapFeature(), &parentFeature,
                                     &resizeFeature, &idleFeature, nullptr};
    LV2UI_Widget widget = nullptr;
    requestedWidth_ = requestedHeight_ = 0;
    handle_ = descriptor_->instantiate(descriptor_, pluginUri_.c_str(), bundle_.c_str(),
                                       &Lv2UiEditor::writeTrampoline, this, &widget, features);
    if (!handle_) { error = "the effect's interface could not open"; return false; }
    host_ = &host;
    idle_ = descriptor_->extension_data
        ? static_cast<const LV2UI_Idle_Interface*>(descriptor_->extension_data(LV2_UI__idleInterface)) : nullptr;
    std::uint32_t width = requestedWidth_, height = requestedHeight_;
    if ((!width || !height) && widget) {
        // The interface sized its own window without asking; read it.
        if (xcb_connection_t* connection = xcb_connect(nullptr, nullptr); connection && !xcb_connection_has_error(connection)) {
            const auto cookie = xcb_get_geometry(connection, xcb_drawable_t(reinterpret_cast<std::uintptr_t>(widget)));
            if (xcb_get_geometry_reply_t* reply = xcb_get_geometry_reply(connection, cookie, nullptr)) {
                width = reply->width; height = reply->height; std::free(reply);
            }
            xcb_disconnect(connection);
        } else if (connection) xcb_disconnect(connection);
    }
    size = {width ? width : 480, height ? height : 320, false};
    sent_.clear();
    tick(); // every control's current value, before the first paint
    timer_ = host.addEditorTimer(33, [this] { tick(); });
    return true;
}

void Lv2UiEditor::tick()
{
    if (!handle_) return;
    if (idle_ && idle_->idle && idle_->idle(handle_) != 0) {
        // The interface closed itself.
        if (host_) host_->editorClosed();
        return;
    }
    if (!descriptor_->port_event || !read_) return;
    for (const auto port : ports_) {
        const auto value = read_(port);
        if (!value) continue;
        const auto known = sent_.find(port);
        if (known != sent_.end() && known->second == *value) continue;
        sent_[port] = *value;
        const float current = *value;
        descriptor_->port_event(handle_, port, sizeof current, 0, &current);
    }
}

void Lv2UiEditor::closeEditor() noexcept
{
    if (!handle_) return;
    if (host_ && timer_) host_->removeEditorTimer(timer_);
    timer_ = 0;
    if (descriptor_->cleanup) descriptor_->cleanup(handle_);
    handle_ = nullptr; host_ = nullptr; idle_ = nullptr;
}
