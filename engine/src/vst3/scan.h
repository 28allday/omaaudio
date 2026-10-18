#pragma once

#include "module.h"
#include "catalogue.h"
#include <atomic>
#include <chrono>
#include <cstdint>

namespace omawave::vst3 {
struct ScanBus {
    bool input = false, event = false;
    std::uint32_t index = 0, channels = 0, type = 0, flags = 0;
    std::string name;
};
struct ScanParameter {
    std::uint32_t id = 0, steps = 0, flags = 0;
    double defaultValue = 0;
    std::string name, shortName, units;
};
struct ScanLayout {
    std::uint32_t channels = 0, sidechainChannels = 0;
};
struct ScanPlugin {
    ClassInfo info;
    bool sample32 = false;
    std::vector<ScanBus> buses;
    std::vector<ScanParameter> parameters;
    // At most mono and stereo, each with its preferred supported sidechain.
    // Default bus metadata above is retained independently of negotiation.
    std::vector<ScanLayout> layouts;
    // Metadata remains available when an individual effect cannot be inspected.
    std::string error;
};
struct ScanResult {
    bool ok = false, cancelled = false, timedOut = false;
    std::string path, error, diagnostic;
    std::vector<ScanPlugin> plugins;
};
inline constexpr std::size_t kScanBytes = 8 * 1024 * 1024;
inline constexpr std::size_t kScanClasses = 4096, kScanParameters = 16384;
// Synchronous control-worker operation; never loads a plugin in this process.
// Pass the absolute path of the matching built/packaged helper, not a shell command.
[[nodiscard]] ScanResult scanBundle(const std::string& helper, const std::string& bundle,
    const std::atomic<bool>* cancel = nullptr,
    std::chrono::milliseconds timeout = std::chrono::seconds(15));

struct BundlePaths {
    std::vector<std::string> paths, errors;
    bool cancelled = false, limited = false;
};
// Stable root priority then sorted paths; canonical deduplication, including
// directory symlinks/cycles. Tree/count/depth limits and cancellation are explicit.
[[nodiscard]] BundlePaths discoverBundles(const std::vector<std::string>& roots,
    const std::atomic<bool>* cancel = nullptr);

// Helper-only: call on a newly exec'd process main thread. No live audio/device.
[[nodiscard]] ScanResult inspectBundle(const std::string& bundle);
[[nodiscard]] std::string encodeScan(const ScanResult& result);
}
