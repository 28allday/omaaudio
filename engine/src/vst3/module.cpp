#include "module.h"
#include "host_context.h"
#include <pluginterfaces/base/funknownimpl.h>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <map>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace omawave::vst3 {
using namespace Steinberg;
namespace {
template<std::size_t N> std::string bounded(const char (&text)[N])
{ return std::string(text, ::strnlen(text, N)); }
struct Modules {
    std::map<std::string, std::weak_ptr<Module>> live;
    std::vector<std::shared_ptr<Module>> retained;
};
Modules& modules() { static Modules value; return value; }
}

bool onMainThread() noexcept { return ::syscall(SYS_gettid) == ::getpid(); }
bool parseClassId(std::string_view text, FUID& id) noexcept
{
    if (text.size() != 32 || !std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
    })) return false;
    char buffer[33]{};
    std::memcpy(buffer, text.data(), text.size());
    FUID candidate;
    if (!candidate.fromString(buffer) || !candidate.isValid()) return false;
    id = candidate; return true;
}
std::string classIdString(const TUID id)
{
    char text[33]{}; FUID::fromTUID(id).toString(text); return text;
}
std::shared_ptr<Module> Module::open(const std::string& bundle, std::string& error)
{
    error.clear();
    if (!onMainThread()) { error = "VST3 module loading requires the process main thread"; return {}; }
    if (bundle.empty() || bundle.find('\0') != std::string::npos) {
        error = "invalid VST3 bundle path"; return {};
    }
    std::error_code ec;
    const auto canonical = std::filesystem::canonical(bundle, ec);
    if (ec || canonical.extension() != ".vst3" || !std::filesystem::is_directory(canonical, ec)) {
        error = "VST3 bundle is not a readable .vst3 directory"; return {};
    }
    const auto path = canonical.string();
    auto& cache = modules();
    if (const auto found = cache.live.find(path); found != cache.live.end())
        if (auto module = found->second.lock()) {
            error = module->error_;
            return error.empty() ? module : std::shared_ptr<Module>{};
        }
    auto module = std::shared_ptr<Module>(new Module);
    const bool loaded = module->load(path, module->error_);
    // The factory's non-discardable promise still applies when later metadata
    // is invalid. Remember that failed load rather than entering it again.
    if (loaded || module->nonDiscardable_) {
        cache.live[path] = module;
        if (module->nonDiscardable_) cache.retained.push_back(module);
    }
    error = module->error_;
    return loaded ? module : std::shared_ptr<Module>{};
}
bool Module::load(const std::string& bundle, std::string& error)
{
    struct utsname machine{};
    if (::uname(&machine) != 0) { error = "could not determine VST3 machine architecture"; return false; }
    const std::filesystem::path root(bundle);
    const auto binary = root / "Contents" / (std::string(machine.machine) + "-linux") / (root.stem().string() + ".so");
    handle_ = ::dlopen(binary.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle_) { error = "could not load VST3 binary: " + std::string(::dlerror()); return false; }
    const auto entry = reinterpret_cast<bool (*)(void*)>(::dlsym(handle_, "ModuleEntry"));
    exit_ = reinterpret_cast<bool (*)()>(::dlsym(handle_, "ModuleExit"));
    const auto factory = reinterpret_cast<IPluginFactory* (*)()>(::dlsym(handle_, "GetPluginFactory"));
    if (!entry || !exit_ || !factory) { error = "VST3 binary lacks ModuleEntry, ModuleExit or GetPluginFactory"; return false; }
    if (!entry(handle_)) { error = "VST3 ModuleEntry failed"; return false; }
    entered_ = true;
    factory_ = owned(factory());
    if (!factory_) { error = "VST3 GetPluginFactory returned no factory"; return false; }
    context_ = makeHostContext();
    const auto unicode = U::cast<IPluginFactory3>(factory_);
    // Some factories expose version 3 for Unicode but ignore host context.
    if (unicode) (void)unicode->setHostContext(context_);
    PFactoryInfo info;
    if (factory_->getFactoryInfo(&info) != kResultOk) { error = "VST3 factory metadata failed"; return false; }
    nonDiscardable_ = (info.flags & PFactoryInfo::kComponentNonDiscardable) != 0;
    const int32 count = factory_->countClasses();
    if (count < 0 || count > 4096) { error = "VST3 class count exceeds the supported limit"; return false; }
    const auto extended = U::cast<IPluginFactory2>(factory_);
    for (int32 index = 0; index < count; ++index) {
        PClassInfo basic;
        ClassInfo value;
        value.vendor = bounded(info.vendor);
        if (factory_->getClassInfo(index, &basic) == kResultOk) {
            value.id = classIdString(basic.cid);
            value.name = bounded(basic.name); value.category = bounded(basic.category);
        }
        if (extended) {
            PClassInfo2 detail;
            if (extended->getClassInfo2(index, &detail) == kResultOk) {
                const auto id = classIdString(detail.cid);
                if (!value.id.empty() && id != value.id) { error = "VST3 class metadata identities disagree"; return false; }
                value.id = id;
                value.name = bounded(detail.name); value.category = bounded(detail.category);
                value.subcategories = bounded(detail.subCategories); value.version = bounded(detail.version);
                value.sdkVersion = bounded(detail.sdkVersion);
                if (detail.vendor[0]) value.vendor = bounded(detail.vendor);
            }
        }
        if (unicode) {
            PClassInfoW detail;
            if (unicode->getClassInfoUnicode(index, &detail) == kResultOk) {
                const auto id = classIdString(detail.cid);
                if (!value.id.empty() && id != value.id) { error = "VST3 Unicode metadata identities disagree"; return false; }
                value.id = id;
                value.name = utf8(detail.name, std::size(detail.name)); value.category = bounded(detail.category);
                value.subcategories = bounded(detail.subCategories); value.version = utf8(detail.version, std::size(detail.version));
                value.sdkVersion = utf8(detail.sdkVersion, std::size(detail.sdkVersion));
                if (detail.vendor[0]) value.vendor = utf8(detail.vendor, std::size(detail.vendor));
            }
        }
        if (value.id.empty()) { error = "VST3 class metadata failed"; return false; }
        FUID validated;
        if (!parseClassId(value.id, validated)) { error = "VST3 class has a nil identity"; return false; }
        if (std::any_of(classes_.begin(), classes_.end(), [&](const auto& other) { return other.id == value.id; })) {
            error = "VST3 factory reports duplicate class identities"; return false;
        }
        classes_.push_back(std::move(value));
    }
    path_ = bundle; return true;
}
Module::~Module()
{
    assert(onMainThread());
    factory_ = nullptr;
    if (entered_ && exit_) (void)exit_();
    if (handle_) ::dlclose(handle_);
}
}
