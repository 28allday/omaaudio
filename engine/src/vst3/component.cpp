#include "component.h"
#include "stream.h"
#include <pluginterfaces/vst/ivsthostapplication.h>
#include <cassert>

namespace omawave::vst3 {
using namespace Steinberg;
using namespace Steinberg::Vst;

std::unique_ptr<Component> Component::create(std::shared_ptr<Module> module, const std::string& classId,
    FUnknown* context, IComponentHandler* handler, std::string& error)
{
    error.clear();
    if (!onMainThread()) { error = "VST3 creation requires the process main thread"; return {}; }
    FUID id;
    if (!module || !parseClassId(classId, id) || !context || !U::cast<IHostApplication>(context)) {
        error = "VST3 creation requires a module, class identity and host context"; return {};
    }
    auto result = std::unique_ptr<Component>(new Component);
    result->module_ = std::move(module); result->context_ = context; result->handler_ = handler;
    auto* factory = result->module_->factory();
    IComponent* raw = nullptr;
    if (factory->createInstance(id.toTUID(), IComponent::iid.toTUID(), reinterpret_cast<void**>(&raw)) != kResultOk || !raw) {
        if (raw) raw->release();
        error = "VST3 processor could not be created"; return {};
    }
    result->component_ = owned(raw);
    alignas(16) TUID controllerId{};
    const bool hasControllerId = raw->getControllerClassId(controllerId) == kResultOk;
    result->controller_ = U::cast<IEditController>(raw);
    result->processor_ = U::cast<IAudioProcessor>(raw);
    if (!result->processor_) { error = "VST3 component has no audio processor"; return {}; }
    if (raw->initialize(context) != kResultOk) { error = "VST3 processor initialization failed"; return {}; }
    result->initialized_ = true;
    if (!result->controller_ && hasControllerId) {
        IEditController* controller = nullptr;
        if (factory->createInstance(controllerId, IEditController::iid.toTUID(), reinterpret_cast<void**>(&controller)) != kResultOk || !controller) {
            if (controller) controller->release();
            error = "VST3 controller could not be created"; return {};
        }
        result->controller_ = owned(controller);
        if (controller->initialize(context) != kResultOk) { error = "VST3 controller initialization failed"; return {}; }
        result->controllerInitialized_ = true;
    }
    if (result->controller_) {
        if (handler && result->controller_->setComponentHandler(handler) != kResultOk) {
            error = "VST3 controller rejected the component handler"; return {};
        }
        result->handlerInstalled_ = handler != nullptr;
        // Combined components were initialized above and must not be connected to
        // themselves or initialized/terminated a second time as a controller.
        if (result->controllerInitialized_) {
            result->componentConnection_ = U::cast<IConnectionPoint>(raw);
            result->controllerConnection_ = U::cast<IConnectionPoint>(result->controller_);
            if (result->componentConnection_ && result->controllerConnection_) {
                if (result->componentConnection_->connect(result->controllerConnection_) != kResultOk) {
                    error = "VST3 processor connection failed"; return {};
                }
                result->componentConnected_ = true;
                if (result->controllerConnection_->connect(result->componentConnection_) != kResultOk) {
                    error = "VST3 controller connection failed"; return {};
                }
                result->controllerConnected_ = true;
            }
        }
        auto initial = owned(new StateStream);
        if (raw->getState(initial) == kResultOk) {
            if (initial->failed()) { error = "VST3 initial state exceeds the stream limit"; return {}; }
            (void)initial->seek(0, IBStream::kIBSeekSet, nullptr);
            if (result->controller_->setComponentState(initial) != kResultOk || initial->failed()) {
                error = "VST3 controller rejected the initial processor state"; return {};
            }
        } else if (initial->failed()) { error = "VST3 initial state stream failed"; return {}; }
    }
    return result;
}

Component::~Component()
{
    assert(onMainThread());
    if (controllerConnected_) (void)controllerConnection_->disconnect(componentConnection_);
    if (componentConnected_) (void)componentConnection_->disconnect(controllerConnection_);
    controllerConnection_ = nullptr; componentConnection_ = nullptr;
    if (handlerInstalled_) (void)controller_->setComponentHandler(nullptr);
    if (controllerInitialized_) (void)controller_->terminate();
    if (initialized_) (void)component_->terminate();
    controller_ = nullptr; processor_ = nullptr; component_ = nullptr;
}

std::optional<ComponentState> Component::capture(std::string& error)
{
    error.clear();
    if (!onMainThread()) { error = "VST3 state capture requires the process main thread"; return {}; }
    auto processor = owned(new StateStream);
    if (component_->getState(processor) != kResultOk || processor->failed()) {
        error = "VST3 processor state could not be captured within the size limit"; return {};
    }
    ComponentState state{processor->bytes(), std::nullopt};
    if (controller_) {
        auto controller = owned(new StateStream);
        const auto code = controller_->getState(controller);
        if (controller->failed() || (code != kResultOk && code != kNotImplemented && code != kResultFalse)) {
            error = "VST3 controller state could not be captured"; return {};
        }
        if (code == kResultOk) state.controller = controller->bytes();
    }
    return state;
}

bool Component::restore(const ComponentState& state, std::string& error)
{
    return restore(state.processor, state.controller
        ? std::optional<std::span<const std::uint8_t>>{*state.controller} : std::nullopt, error);
}

bool Component::restore(std::span<const std::uint8_t> processorBytes,
    std::optional<std::span<const std::uint8_t>> controllerBytes, std::string& error)
{
    error.clear();
    if (!onMainThread()) { error = "VST3 state restore requires the process main thread"; return false; }
    if (processorBytes.size() > StateStream::kLimit || (controllerBytes && controllerBytes->size() > StateStream::kLimit)) {
        error = "VST3 state exceeds the size limit"; return false;
    }
    if (controllerBytes && !controller_) { error = "VST3 controller state has no matching controller"; return false; }
    auto processor = owned(new StateStream(std::vector<std::uint8_t>(processorBytes.begin(), processorBytes.end())));
    if (component_->setState(processor) != kResultOk || processor->failed()) {
        error = "VST3 processor rejected its saved state"; return false;
    }
    if (controller_) {
        (void)processor->seek(0, IBStream::kIBSeekSet, nullptr);
        if (controller_->setComponentState(processor) != kResultOk || processor->failed()) {
            error = "VST3 controller rejected saved processor state"; return false;
        }
        if (controllerBytes) {
            auto controller = owned(new StateStream(std::vector<std::uint8_t>(controllerBytes->begin(), controllerBytes->end())));
            if (controller_->setState(controller) != kResultOk || controller->failed()) {
                error = "VST3 controller rejected its saved state"; return false;
            }
        }
    }
    return true;
}
}
