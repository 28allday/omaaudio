#include "scan.h"
#include "component.h"
#include "bus_layout.h"
#include "host_context.h"
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace omawave::vst3 {
using namespace Steinberg;
using namespace Steinberg::Vst;
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void budget(std::size_t& used, std::size_t count) {
    require(count <= kScanBytes - used, "VST3 inspection exceeds the metadata size limit"); used += count;
}
std::string hex(std::string_view text) {
    if (text.empty()) return "-";
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result; result.reserve(text.size() * 2);
    for (unsigned char c : text) { result += digits[c >> 4]; result += digits[c & 15]; }
    return result;
}
std::string decimal(double value) {
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
        std::chars_format::general, std::numeric_limits<double>::max_digits10);
    require(result.ec == std::errc{}, "VST3 parameter value could not be encoded");
    return {buffer.data(), result.ptr};
}
}

ScanResult inspectBundle(const std::string& bundle)
{
    ScanResult result;
    try {
        auto module = Module::open(bundle, result.error);
        if (!module) return result;
        result.path = module->path();
        std::size_t used = 0;
        for (const auto& metadata : module->classes()) {
            if (metadata.category != kVstAudioEffectClass) continue;
            budget(used, sizeof(ScanPlugin) + 256 + 2 * (metadata.id.size() + metadata.name.size()
                + metadata.category.size() + metadata.subcategories.size() + metadata.vendor.size()
                + metadata.version.size() + metadata.sdkVersion.size()));
            ScanPlugin plugin; plugin.info = metadata;
            auto component = Component::create(module, metadata.id, module->context(), nullptr, plugin.error);
            if (component) {
                try {
                    plugin.sample32 = component->processor()->canProcessSampleSize(kSample32) == kResultOk;
                    auto* processor = component->component();
                    for (const auto media : {kAudio, kEvent}) for (const auto direction : {kInput, kOutput}) {
                        const auto count = processor->getBusCount(media, direction);
                        require(count >= 0 && count <= 64, "VST3 bus count exceeds the supported limit");
                        for (int32 index = 0; index < count; ++index) {
                            BusInfo info{};
                            require(processor->getBusInfo(media, direction, index, info) == kResultOk,
                                "VST3 bus metadata could not be read");
                            require(info.mediaType == media && info.direction == direction
                                && info.channelCount >= 0 && info.channelCount <= 256
                                && (info.busType == kMain || info.busType == kAux), "VST3 bus metadata is invalid");
                            ScanBus bus{direction == kInput, media == kEvent,
                                static_cast<uint32>(index), static_cast<uint32>(info.channelCount),
                                static_cast<uint32>(info.busType), info.flags, utf8(info.name, std::size(info.name))};
                            budget(used, sizeof(bus) + 128 + 2 * bus.name.size());
                            plugin.buses.push_back(std::move(bus));
                        }
                    }
                    if (auto* controller = component->controller()) {
                        const auto count = controller->getParameterCount();
                        require(count >= 0 && count <= static_cast<int32>(kScanParameters),
                            "VST3 parameter count exceeds the supported limit");
                        std::set<ParamID> identities;
                        for (int32 index = 0; index < count; ++index) {
                            ParameterInfo info{};
                            require(controller->getParameterInfo(index, info) == kResultOk,
                                "VST3 parameter metadata could not be read");
                            require(info.id <= kMaxParamId && identities.insert(info.id).second
                                && info.stepCount >= 0 && std::isfinite(info.defaultNormalizedValue)
                                && info.defaultNormalizedValue >= 0 && info.defaultNormalizedValue <= 1,
                                "VST3 parameter identity, steps or default is invalid");
                            ScanParameter parameter{info.id, static_cast<uint32>(info.stepCount),
                                static_cast<uint32>(info.flags), info.defaultNormalizedValue,
                                utf8(info.title, std::size(info.title)), utf8(info.shortTitle, std::size(info.shortTitle)),
                                utf8(info.units, std::size(info.units))};
                            budget(used, sizeof(parameter) + 128 + 2 * (parameter.name.size() + parameter.shortName.size() + parameter.units.size()));
                            plugin.parameters.push_back(std::move(parameter));
                        }
                    }
                } catch (const std::exception& error) {
                    plugin.error = error.what(); plugin.buses.clear(); plugin.parameters.clear();
                }
            }
            component.reset();
            if (plugin.error.empty() && plugin.sample32) {
                std::vector<std::uint32_t> keys{0};
                for (const auto& bus : plugin.buses) {
                    if (bus.input && !bus.event && bus.type == kAux && !(bus.flags & BusInfo::kIsControlVoltage)) {
                        const auto preferred = bus.channels == 2 ? 2u : 1u;
                        keys = {preferred, preferred == 1 ? 2u : 1u, 0}; break;
                    }
                }
                for (std::uint32_t channels : {1u, 2u}) for (auto key : keys) {
                    // A rejected request may mutate the component. Every trial
                    // starts fresh, with no processing setup or activation.
                    std::string error;
                    auto trial = Component::create(module, metadata.id, module->context(), nullptr, error);
                    if (!trial) continue;
                    try {
                        (void)negotiateAudioLayout(trial->component(), trial->processor(), channels, key);
                        plugin.layouts.push_back({channels, key}); break;
                    } catch (...) { /* Another main/key arrangement may still work. */ }
                }
                budget(used, plugin.layouts.size() * (sizeof(ScanLayout) + 32));
            }
            result.plugins.push_back(std::move(plugin));
        }
        result.ok = true;
    } catch (const std::exception& error) {
        result.plugins.clear(); result.error = error.what();
    }
    return result;
}

std::string encodeScan(const ScanResult& result)
{
    std::string output = "OMAAUDIO-VST3-SCAN-2\n";
    const auto line = [&](const std::string& text) {
        require(text.size() <= 16384 && text.size() + 1 <= kScanBytes - output.size(),
            "VST3 scan result exceeds the protocol limit"); output += text; output += '\n';
    };
    if (!result.ok) { line("ERR " + hex(result.error.substr(0, 1024))); line("END 0"); return output; }
    line("M " + hex(result.path));
    for (const auto& plugin : result.plugins) {
        const auto& p = plugin.info;
        line("P " + p.id + " " + hex(p.name) + " " + hex(p.category) + " " + hex(p.subcategories)
            + " " + hex(p.vendor) + " " + hex(p.version) + " " + hex(p.sdkVersion) + " " + hex(plugin.error)
            + " " + std::to_string(plugin.sample32) + " " + std::to_string(plugin.buses.size())
            + " " + std::to_string(plugin.parameters.size()) + " " + std::to_string(plugin.layouts.size()));
        for (const auto& layout : plugin.layouts)
            line("L " + std::to_string(layout.channels) + " " + std::to_string(layout.sidechainChannels));
        for (const auto& b : plugin.buses)
            line("B " + std::to_string(b.input) + " " + std::to_string(b.event) + " " + std::to_string(b.index)
                + " " + std::to_string(b.channels) + " " + std::to_string(b.type) + " " + std::to_string(b.flags) + " " + hex(b.name));
        for (const auto& p : plugin.parameters)
            line("C " + std::to_string(p.id) + " " + std::to_string(p.steps) + " " + std::to_string(p.flags)
                + " " + decimal(p.defaultValue) + " " + hex(p.name) + " " + hex(p.shortName) + " " + hex(p.units));
        line("E");
    }
    line("END " + std::to_string(result.plugins.size()));
    return output;
}
}
