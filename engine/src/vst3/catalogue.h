#pragma once

#include "host/instance.h"
#include <atomic>
#include <chrono>

namespace omawave::vst3 {
// Standard discovery roots; application callers need no SDK interface headers.
[[nodiscard]] std::vector<std::string> searchPath(const std::string& applicationFolder);
struct CatalogueOptions {
    std::string helper;
    std::vector<std::string> roots;
    std::chrono::milliseconds timeout = std::chrono::seconds(15); // per bundle
    std::size_t maxPlugins = 16384, maxBytes = 32 * 1024 * 1024;
};
struct CatalogueResult {
    bool ok = false, cancelled = false, limited = false;
    std::vector<host::PluginInfo> plugins;
    std::vector<std::string> errors;
};
// Worker-owned discovery; plugin code runs only in bounded child scanners.
// First class ID in root/path order wins. Cancel/limit returns no partial list.
[[nodiscard]] CatalogueResult scanCatalogue(const CatalogueOptions&, const std::atomic<bool>* cancel = nullptr);
}
