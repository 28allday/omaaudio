#pragma once

#include "host/instance.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct clap_plugin_entry;
struct clap_plugin_factory;
struct clap_plugin;
struct clap_host;

namespace omawave::clap {

// Every CLAP plugin this box has, found by loading each `.clap` on the search
// path and asking its factory. Identity is "clap:" + the plugin id, so a CLAP
// insert and an LV2 insert can never be confused in a project file.
//
// The search path is CLAP's own: $CLAP_PATH (colon-separated), then ~/.clap,
// /usr/lib/clap and /usr/local/lib/clap. Describing a plugin instantiates it
// once to read its ports and parameters, so this is worker-thread work and
// runs once per process. Loaded modules stay loaded: an instance made from
// one may still be running.
class Catalogue {
public:
    Catalogue() = default;
    ~Catalogue();
    Catalogue(const Catalogue&) = delete;
    Catalogue& operator=(const Catalogue&) = delete;

    void scan();
    [[nodiscard]] bool scanned() const noexcept { return scanned_; }
    [[nodiscard]] const std::vector<host::PluginInfo>& plugins() const noexcept { return plugins_; }
    [[nodiscard]] const host::PluginInfo* find(const std::string& uri) const noexcept;

    // Creates a plugin for `uri` bound to `host`; nullptr when the plugin is not
    // in the catalogue or its factory refuses. The plugin is not yet initialised.
    [[nodiscard]] const clap_plugin* create(const std::string& uri, const clap_host* host) const noexcept;

    // The search path as it will be walked, for messages.
    [[nodiscard]] static std::vector<std::string> searchPath();

private:
    struct Module {
        std::string path;
        void* handle = nullptr;
        const clap_plugin_entry* entry = nullptr;
        const clap_plugin_factory* factory = nullptr;
    };
    void loadModule(const std::string& path);

    bool scanned_ = false;
    std::vector<Module> modules_;
    std::vector<host::PluginInfo> plugins_;
    std::map<std::string, std::pair<std::size_t, std::string>> moduleOfUri_;   // uri -> (module index, plugin id)
};

// The plugin id inside a "clap:" uri, empty for anything else.
[[nodiscard]] std::string pluginIdOf(const std::string& uri);

} // namespace omawave::clap
