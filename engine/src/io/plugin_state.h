#pragma once
#include "session/plugin_state.h"
#include <atomic>
#include <filesystem>
#include <functional>

namespace omawave::io {
// Stable, bounded little-endian container with a CRC for accidental corruption.
// The UUID names the file; the URI binds it to the corresponding plugin instance.
inline constexpr std::size_t kPluginStateHeader = 52;
inline constexpr std::size_t kPluginStateFileLimit = kPluginStateHeader + session::PluginState::kUriLimit
    + session::PluginState::kStreamLimit * 2 + 4;
std::vector<std::uint8_t> encodePluginState(const session::PluginState&);
std::optional<session::PluginState> decodePluginState(std::span<const std::uint8_t>,
    const session::Uuid&, std::string_view expectedUri, std::string& error);

// Both operations require the caller's bundle lock. Files are immutable: a save
// never replaces an existing state file, even if it has the requested UUID.
std::string writePluginState(const std::filesystem::path& root, const session::PluginState&,
    const std::atomic<bool>* cancel = nullptr, const std::function<void(float)>& progress = {});
std::optional<session::PluginState> readPluginState(const std::filesystem::path& root,
    const session::Uuid&, std::string_view expectedUri, std::size_t remainingBytes, std::string& error);
}
