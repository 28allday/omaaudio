#pragma once

#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivsthostapplication.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace omawave::vst3 {

[[nodiscard]] bool onMainThread() noexcept;
// Strict 32-hex-digit class ID; rejects nil IDs, embedded NULs and suffixes.
[[nodiscard]] bool parseClassId(std::string_view, Steinberg::FUID&) noexcept;
[[nodiscard]] std::string classIdString(const Steinberg::TUID);

struct ClassInfo {
    std::string id, name, category, subcategories, vendor, version, sdkVersion;
};

// Linux bundle ownership. All calls and the last release belong to the process's
// main thread. Scanners therefore use child processes; offline preparation must
// dispatch to its control thread. Components keep shared ownership of the module.
class Module final {
public:
    static std::shared_ptr<Module> open(const std::string& bundle, std::string& error);
    ~Module();
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
    [[nodiscard]] const std::vector<ClassInfo>& classes() const noexcept { return classes_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] Steinberg::IPluginFactory* factory() const noexcept { return factory_.get(); }
    [[nodiscard]] Steinberg::Vst::IHostApplication* context() const noexcept { return context_.get(); }
private:
    Module() = default;
    bool load(const std::string&, std::string&);
    void* handle_ = nullptr;
    bool entered_ = false;
    bool nonDiscardable_ = false;
    bool (*exit_)() = nullptr;
    Steinberg::IPtr<Steinberg::IPluginFactory> factory_;
    Steinberg::IPtr<Steinberg::Vst::IHostApplication> context_;
    std::string path_, error_;
    std::vector<ClassInfo> classes_;
};
}
