#pragma once

#include "uuid.h"
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace omawave::session {

// Opaque, immutable control-thread data. Project/Undo copies share the payload;
// no running plugin, module handle or mutable buffer belongs in a snapshot.
class PluginState {
public:
    static constexpr std::size_t kStreamLimit = 64 * 1024 * 1024;
    static constexpr std::size_t kProjectLimit = 256 * 1024 * 1024;
    static constexpr std::size_t kReferenceLimit = 4096;
    static constexpr std::size_t kUriLimit = 4096;
    using Bytes = std::vector<std::uint8_t>;
    PluginState() = default;
    // With no identity supplied, generate one from the existing system CSPRNG.
    // Explicit identities are for loading; storage rejects conflicting reuse.
    static std::optional<PluginState> create(std::string uri, Bytes processor,
        std::optional<Bytes> controller, std::string& error, std::optional<Uuid> identity = {});
    explicit operator bool() const noexcept { return bool(data_); }
    const Uuid& id() const noexcept;
    const std::string& uri() const noexcept;
    std::span<const std::uint8_t> processor() const noexcept;
    std::optional<std::span<const std::uint8_t>> controller() const noexcept;
    std::size_t bytes() const noexcept;
    // Content equality, independent of the storage UUID. Separate captures of
    // unchanged state must not force a rebuild or an Undo entry.
    bool operator==(const PluginState&) const noexcept;
    static bool validUri(std::string_view) noexcept;
private:
    struct Data { Uuid id; std::string uri; Bytes processor; std::optional<Bytes> controller; };
    explicit PluginState(std::shared_ptr<const Data> data) : data_(std::move(data)) {}
    std::shared_ptr<const Data> data_;
};

// Worker/control-side aggregate guard. Repeated references share a payload;
// the same UUID with different content is refused. Never use this in run().
class PluginStateBudget {
public:
    explicit PluginStateBudget(std::size_t bytes = PluginState::kProjectLimit,
        std::size_t references = PluginState::kReferenceLimit) : limit_(bytes), maxReferences_(references) {}
    bool add(const PluginState&, std::string& error);
    std::size_t bytes() const noexcept { return used_; }
private:
    std::map<std::array<std::uint8_t,16>, PluginState> seen_;
    std::size_t limit_, maxReferences_, used_ = 0, references_ = 0;
};
}
