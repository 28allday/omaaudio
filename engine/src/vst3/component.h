#pragma once
#include "module.h"
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstmessage.h>
#include <cstdint>
#include <optional>
#include <span>

namespace omawave::vst3 {
struct ComponentState {
    std::vector<std::uint8_t> processor;
    std::optional<std::vector<std::uint8_t>> controller;
    bool operator==(const ComponentState&) const = default;
};

// Control-thread component/controller lifetime and state, shared by the future
// scanner and render adapter. Does not activate or process. The adapter must
// stop/deactivate before destruction; restore is for an unpublished candidate.
class Component final {
public:
    static std::unique_ptr<Component> create(std::shared_ptr<Module>, const std::string& classId,
        Steinberg::FUnknown* context, Steinberg::Vst::IComponentHandler* handler, std::string& error);
    ~Component();
    Component(const Component&) = delete;
    Component& operator=(const Component&) = delete;
    [[nodiscard]] Steinberg::Vst::IComponent* component() const noexcept { return component_.get(); }
    [[nodiscard]] Steinberg::Vst::IAudioProcessor* processor() const noexcept { return processor_.get(); }
    [[nodiscard]] Steinberg::Vst::IEditController* controller() const noexcept { return controller_.get(); }
    [[nodiscard]] std::optional<ComponentState> capture(std::string& error);
    [[nodiscard]] bool restore(const ComponentState&, std::string& error);
    [[nodiscard]] bool restore(std::span<const std::uint8_t> processor,
        std::optional<std::span<const std::uint8_t>> controller, std::string& error);
private:
    Component() = default;
    std::shared_ptr<Module> module_;
    Steinberg::IPtr<Steinberg::FUnknown> context_;
    Steinberg::IPtr<Steinberg::Vst::IComponentHandler> handler_;
    Steinberg::IPtr<Steinberg::Vst::IComponent> component_;
    Steinberg::IPtr<Steinberg::Vst::IAudioProcessor> processor_;
    Steinberg::IPtr<Steinberg::Vst::IEditController> controller_;
    Steinberg::IPtr<Steinberg::Vst::IConnectionPoint> componentConnection_, controllerConnection_;
    bool initialized_ = false, controllerInitialized_ = false;
    bool componentConnected_ = false, controllerConnected_ = false;
    bool handlerInstalled_ = false;
};
}
