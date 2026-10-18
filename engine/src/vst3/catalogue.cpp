#include "catalogue.h"
#include "scan.h"
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <filesystem>
#include <algorithm>
#include <set>
#include <unistd.h>

namespace omawave::vst3 {
namespace {
using namespace Steinberg::Vst;
bool category(const std::string& list, std::string_view item) {
    std::size_t from = 0;
    while (from < list.size()) {
        const auto end = list.find('|', from);
        if (std::string_view(list).substr(from, end == list.npos ? end : end - from) == item) return true;
        if (end == list.npos) break;
        from = end + 1;
    }
    return false;
}
host::PluginInfo describe(const ScanPlugin& source, const std::string& path) {
    host::PluginInfo p;
    p.format = host::Format::Vst3; p.uri = "vst3:" + source.info.id; p.path = path;
    p.name = source.info.name; p.author = source.info.vendor; p.className = source.info.subcategories;
    p.hasStateInterface = true; p.unusableReason = source.error;
    unsigned defaultIn = 0, defaultOut = 0;
    for (const auto& bus : source.buses) {
        if (bus.event || (bus.flags & BusInfo::kIsControlVoltage)) continue;
        if (bus.type == kMain) {
            if (bus.input) defaultIn = bus.channels;
            else defaultOut = bus.channels;
        }
    }
    for (const auto& parameter : source.parameters)
        if (!(parameter.flags & (ParameterInfo::kIsReadOnly | ParameterInfo::kIsHidden))) ++p.controlIn;
    for (const auto& layout : source.layouts) {
        p.audioConfigurations.push_back({layout.channels, layout.sidechainChannels});
        p.sidechainIn = std::max(p.sidechainIn, layout.sidechainChannels);
    }
    if (!p.audioConfigurations.empty()) {
        const auto preferred = std::find_if(p.audioConfigurations.begin(), p.audioConfigurations.end(),
            [&](const auto& layout) { return defaultIn == defaultOut && layout.channels == defaultIn; });
        if (preferred != p.audioConfigurations.end()) std::rotate(p.audioConfigurations.begin(), preferred, preferred + 1);
        p.requiredIn = p.requiredOut = p.fits(1) ? 1 : 2;
        p.totalIn = p.totalOut = p.fits(2) ? 2 : 1;
        p.layout = p.fits(1) ? (p.fits(2) ? host::Layout::MonoOrStereo : host::Layout::Mono) : host::Layout::Stereo;
    }
    if (p.unusableReason.empty()) {
        if (category(source.info.subcategories, PlugType::kInstrument) || category(source.info.subcategories, PlugType::kOnlyARA))
            p.unusableReason = "requires an unsupported instrument or ARA host";
        else if (!source.sample32) p.unusableReason = "does not support 32-bit audio";
        else if (p.audioConfigurations.empty()) p.unusableReason = "does not accept a supported mono or stereo main bus arrangement";
    }
    p.usable = p.unusableReason.empty();
    return p;
}
}

CatalogueResult scanCatalogue(const CatalogueOptions& options, const std::atomic<bool>* cancel) {
    CatalogueResult result;
    const auto cancelled = [&] { return cancel && cancel->load(std::memory_order_relaxed); };
    if (cancelled()) { result.cancelled = true; return result; }
    if (options.helper.empty() || !std::filesystem::path(options.helper).is_absolute()
        || options.helper.find('\0') != options.helper.npos || options.helper.size() > 4096
        || options.timeout <= std::chrono::milliseconds::zero() || options.timeout > std::chrono::minutes(1)
        || !options.maxPlugins || options.maxPlugins > 16384 || !options.maxBytes || options.maxBytes > 32 * 1024 * 1024) {
        result.errors.emplace_back("Invalid VST3 catalogue options"); return result;
    }
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(options.helper, fileError) || ::access(options.helper.c_str(), X_OK) != 0) {
        result.errors.emplace_back("VST3 scanner helper is unavailable or not executable"); return result;
    }
    const auto paths = discoverBundles(options.roots, cancel);
    result.errors = paths.errors; result.cancelled = paths.cancelled; result.limited = paths.limited;
    if (result.cancelled || result.limited) return result;
    std::set<std::string> identities;
    std::size_t bytes = 0;
    for (const auto& path : paths.paths) {
        auto scan = scanBundle(options.helper, path, cancel, options.timeout);
        if (scan.cancelled || cancelled()) { result.cancelled = true; result.plugins.clear(); return result; }
        if (!scan.ok) {
            if (result.errors.size() < 128) result.errors.push_back(path.substr(0, 512) + ": " + scan.error.substr(0, 1024));
            continue;
        }
        for (const auto& source : scan.plugins) {
            if (!identities.insert(source.info.id).second) continue;
            auto p = describe(source, scan.path);
            const auto size = sizeof(p) + p.uri.size() + p.path.size() + p.name.size() + p.author.size()
                + p.className.size() + p.unusableReason.size() + p.audioConfigurations.size() * sizeof(host::AudioConfiguration);
            if (result.plugins.size() == options.maxPlugins || size > options.maxBytes - bytes) {
                result.limited = true; result.plugins.clear();
                result.errors.emplace_back("VST3 catalogue exceeds its plugin or metadata limit"); return result;
            }
            bytes += size; result.plugins.push_back(std::move(p));
        }
    }
    if (cancelled()) { result.cancelled = true; result.plugins.clear(); return result; }
    result.ok = true; return result;
}
}
