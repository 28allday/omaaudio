#include "catalogue.h"

#include "clap/probe_host.h"

#include <clap/clap.h>

#include <dlfcn.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace omawave::clap {

namespace {

constexpr const char* kPrefix = "clap:";

std::string lower(std::string text)
{
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// The class the browser groups by, from CLAP's feature list.
std::string classOf(const clap_plugin_descriptor_t& d)
{
    if (d.features == nullptr) return "Plugin";
    std::string found;
    for (const char* const* f = d.features; *f != nullptr; ++f) {
        const std::string feature = *f;
        if (feature == CLAP_PLUGIN_FEATURE_INSTRUMENT) return "Instrument Plugin";
        if (feature == CLAP_PLUGIN_FEATURE_COMPRESSOR) found = "Compressor Plugin";
        else if (feature == CLAP_PLUGIN_FEATURE_LIMITER) found = "Limiter Plugin";
        else if (feature == CLAP_PLUGIN_FEATURE_EQUALIZER) found = "Equaliser Plugin";
        else if (feature == CLAP_PLUGIN_FEATURE_REVERB) found = "Reverb Plugin";
        else if (feature == CLAP_PLUGIN_FEATURE_DELAY) found = "Delay Plugin";
        else if (feature == CLAP_PLUGIN_FEATURE_DISTORTION) found = "Distortion Plugin";
        else if (feature == CLAP_PLUGIN_FEATURE_FILTER) found = "Filter Plugin";
        else if (feature == CLAP_PLUGIN_FEATURE_UTILITY) found = "Utility Plugin";
        else if (feature == CLAP_PLUGIN_FEATURE_ANALYZER) found = "Analyser Plugin";
        else if (feature == CLAP_PLUGIN_FEATURE_RESTORATION) found = "Restoration Plugin";
        else if (feature == CLAP_PLUGIN_FEATURE_MASTERING) found = "Mastering Plugin";
        else if (feature == CLAP_PLUGIN_FEATURE_GATE) found = "Gate Plugin";
        else if (feature == CLAP_PLUGIN_FEATURE_EXPANDER) found = "Expander Plugin";
        else if (feature == CLAP_PLUGIN_FEATURE_DEESSER) found = "De-esser Plugin";
        else if (feature == CLAP_PLUGIN_FEATURE_AUDIO_EFFECT && found.empty()) found = "Audio Effect Plugin";
    }
    return found.empty() ? "Plugin" : found;
}

} // namespace

std::string pluginIdOf(const std::string& uri)
{
    return uri.rfind(kPrefix, 0) == 0 ? uri.substr(std::strlen(kPrefix)) : std::string();
}

std::vector<std::string> Catalogue::searchPath()
{
    std::vector<std::string> out;
    if (const char* env = std::getenv("CLAP_PATH"); env != nullptr && *env != '\0') {
        std::string rest = env;
        while (!rest.empty()) {
            const auto colon = rest.find(':');
            const std::string part = rest.substr(0, colon);
            if (!part.empty()) out.push_back(part);
            if (colon == std::string::npos) break;
            rest.erase(0, colon + 1);
        }
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0')
        out.push_back(std::string(home) + "/.clap");
    out.emplace_back("/usr/lib/clap");
    out.emplace_back("/usr/local/lib/clap");
    return out;
}

Catalogue::~Catalogue()
{
    for (Module& m : modules_) {
        if (m.entry != nullptr && m.entry->deinit != nullptr) m.entry->deinit();
        // Modules are not dlclose'd: an instance made from one may still be
        // retired rather than destroyed, and a library unloaded under it is a
        // crash the process cannot see coming.
    }
}

void Catalogue::loadModule(const std::string& path)
{
    void* handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) return;
    auto* entry = static_cast<const clap_plugin_entry_t*>(::dlsym(handle, "clap_entry"));
    if (entry == nullptr || !clap_version_is_compatible(entry->clap_version)
        || entry->init == nullptr || !entry->init(path.c_str())) {
        ::dlclose(handle);
        return;
    }
    auto* factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (factory == nullptr) {
        if (entry->deinit != nullptr) entry->deinit();
        ::dlclose(handle);
        return;
    }
    const std::size_t index = modules_.size();
    modules_.push_back(Module{path, handle, entry, factory});

    const uint32_t count = factory->get_plugin_count(factory);
    for (uint32_t i = 0; i < count; ++i) {
        const clap_plugin_descriptor_t* d = factory->get_plugin_descriptor(factory, i);
        if (d == nullptr || d->id == nullptr || *d->id == '\0') continue;
        host::PluginInfo info;
        info.format = host::Format::Clap;
        info.uri = std::string(kPrefix) + d->id;
        info.name = d->name != nullptr && *d->name ? d->name : d->id;
        info.author = d->vendor != nullptr ? d->vendor : "";
        info.className = classOf(*d);
        info.path = path;
        if (moduleOfUri_.count(info.uri) != 0) continue;   // the first of a duplicate id wins

        // Ports and parameters need an instance; a plugin that will not make one is
        // listed as unusable with that reason.
        ProbeHost probe;
        const clap_plugin_t* plugin = factory->create_plugin(factory, probe.raw(), d->id);
        if (plugin == nullptr || !plugin->init(plugin)) {
            if (plugin != nullptr) plugin->destroy(plugin);
            info.unusableReason = "the plugin could not be created";
            plugins_.push_back(info);
            moduleOfUri_[info.uri] = {index, d->id};
            continue;
        }
        auto* ports = static_cast<const clap_plugin_audio_ports_t*>(plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
        auto* params = static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
        info.hasStateInterface = plugin->get_extension(plugin, CLAP_EXT_STATE) != nullptr;
        // The first main pair is the track's audio and the first other input is
        // the key. Any further port is fed silence or drained by the instance.
        std::uint32_t mainIn = 0, mainOut = 0, sidechain = 0;
        if (ports != nullptr) {
            for (bool input : {true, false}) {
                const uint32_t n = ports->count(plugin, input);
                bool haveMain = false, haveKey = false;
                for (uint32_t p = 0; p < n; ++p) {
                    clap_audio_port_info_t pi{};
                    if (!ports->get(plugin, p, input, &pi)) continue;
                    const bool main = (pi.flags & CLAP_AUDIO_PORT_IS_MAIN) != 0 || p == 0;
                    if (main && !haveMain) { haveMain = true; (input ? mainIn : mainOut) = pi.channel_count; }
                    else if (input && !haveKey) { haveKey = true; sidechain = pi.channel_count; }
                }
            }
        }
        info.requiredIn = info.totalIn = mainIn;
        info.requiredOut = info.totalOut = mainOut;
        info.sidechainIn = sidechain;
        if (params != nullptr) {
            const uint32_t n = params->count(plugin);
            for (uint32_t p = 0; p < n; ++p) {
                clap_param_info_t pi{};
                if (params->get_info(plugin, p, &pi) && (pi.flags & CLAP_PARAM_IS_READONLY) == 0 && (pi.flags & CLAP_PARAM_IS_HIDDEN) == 0)
                    ++info.controlIn;
            }
        }
        if (mainIn == 1 && mainOut == 1) info.layout = host::Layout::Mono;
        else if (mainIn == 2 && mainOut == 2) info.layout = host::Layout::Stereo;
        else info.layout = host::Layout::Unsupported;
        if (ports == nullptr) info.unusableReason = "the plugin has no audio ports";
        else if (info.layout == host::Layout::Unsupported)
            info.unusableReason = "its main audio port is " + std::to_string(mainIn) + " in, " + std::to_string(mainOut)
                                + " out; a mono or stereo pair is needed";
        info.usable = info.unusableReason.empty();
        plugin->destroy(plugin);
        plugins_.push_back(info);
        moduleOfUri_[info.uri] = {index, d->id};
    }
}

void Catalogue::scan()
{
    modules_.clear();
    plugins_.clear();
    moduleOfUri_.clear();
    std::error_code ec;
    for (const std::string& root : searchPath()) {
        if (!std::filesystem::is_directory(root, ec)) continue;
        std::vector<std::string> files;
        for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec) && lower(it->path().extension().string()) == ".clap") files.push_back(it->path().string());
        }
        std::sort(files.begin(), files.end());
        for (const std::string& file : files) loadModule(file);
    }
    std::stable_sort(plugins_.begin(), plugins_.end(), [](const host::PluginInfo& a, const host::PluginInfo& b) {
        return lower(a.name) < lower(b.name);
    });
    scanned_ = true;
}

const host::PluginInfo* Catalogue::find(const std::string& uri) const noexcept
{
    for (const host::PluginInfo& p : plugins_) if (p.uri == uri) return &p;
    return nullptr;
}

const clap_plugin* Catalogue::create(const std::string& uri, const clap_host* host) const noexcept
{
    const auto it = moduleOfUri_.find(uri);
    if (it == moduleOfUri_.end() || it->second.first >= modules_.size()) return nullptr;
    const Module& m = modules_[it->second.first];
    if (m.factory == nullptr) return nullptr;
    return m.factory->create_plugin(m.factory, host, it->second.second.c_str());
}

} // namespace omawave::clap
