#pragma once

#include "host/editor.h"
#include "lv2/urid.h"
#include "lv2/world.h"

#include <lv2/ui/ui.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// An LV2 plug-in's own X11 interface (ui:X11UI), loaded beside the processor.
// LV2 controls belong to the host, so the interface writes through the same path
// as the rack's controls and is told about every control and meter change.
// Main thread only.
class Lv2UiEditor final : public omawave::host::EmbeddedEditor {
public:
    using Write = std::function<void(std::uint32_t port, float value)>;
    using Read = std::function<std::optional<float>(std::uint32_t port)>;
    [[nodiscard]] static bool available(omawave::lv2::World& world, const std::string& pluginUri);
    [[nodiscard]] static std::unique_ptr<Lv2UiEditor> create(omawave::lv2::World& world, omawave::lv2::UridMap& urids,
        const std::string& pluginUri, Write write, Read read, std::string& error);
    ~Lv2UiEditor() override;
    Lv2UiEditor(const Lv2UiEditor&) = delete;
    Lv2UiEditor& operator=(const Lv2UiEditor&) = delete;

    [[nodiscard]] bool openEditor(std::uint64_t parent, omawave::host::EditorHost&,
                                  omawave::host::EditorSize&, std::string& error) override;
    bool resizeEditor(std::uint32_t&, std::uint32_t&) noexcept override { return false; }
    void closeEditor() noexcept override;

private:
    Lv2UiEditor() = default;
    static void writeTrampoline(LV2UI_Controller, std::uint32_t port, std::uint32_t size,
                                std::uint32_t protocol, const void* buffer);
    static int resizeTrampoline(LV2UI_Feature_Handle, int width, int height);
    void tick();

    omawave::lv2::UridMap* urids_ = nullptr;
    std::string pluginUri_, uiUri_, binary_, bundle_;
    std::vector<std::uint32_t> ports_;
    Write write_;
    Read read_;
    void* library_ = nullptr;
    const LV2UI_Descriptor* descriptor_ = nullptr;
    LV2UI_Handle handle_ = nullptr;
    const LV2UI_Idle_Interface* idle_ = nullptr;
    omawave::host::EditorHost* host_ = nullptr;
    std::uint32_t timer_ = 0;
    std::uint32_t requestedWidth_ = 0, requestedHeight_ = 0;
    std::map<std::uint32_t, float> sent_;
    LV2UI_Resize resize_{};
};
