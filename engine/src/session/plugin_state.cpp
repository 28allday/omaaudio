#include "plugin_state.h"
#include <algorithm>
#include <utility>

namespace omawave::session {
bool PluginState::validUri(std::string_view uri) noexcept {
    if (uri.empty() || uri.size() > kUriLimit || uri.find(':') == std::string_view::npos) return false;
    // TOML metadata must round-trip the same identity stored in the binary
    // container. Reject invalid UTF-8, overlong forms and surrogate code points
    // before a serializer can replace or omit their bytes.
    for (std::size_t at = 0; at < uri.size();) {
        const auto first = static_cast<unsigned char>(uri[at++]);
        if (first <= 32 || first == 127) return false;
        if (first < 128) continue;
        unsigned continuation = 0; std::uint32_t value = 0, minimum = 0;
        if (first >= 0xC2 && first <= 0xDF) { continuation = 1; value = first & 0x1F; minimum = 0x80; }
        else if (first >= 0xE0 && first <= 0xEF) { continuation = 2; value = first & 0x0F; minimum = 0x800; }
        else if (first >= 0xF0 && first <= 0xF4) { continuation = 3; value = first & 0x07; minimum = 0x10000; }
        else return false;
        if (continuation > uri.size() - at) return false;
        for (unsigned n = 0; n < continuation; ++n) {
            const auto next = static_cast<unsigned char>(uri[at++]);
            if ((next & 0xC0) != 0x80) return false;
            value = (value << 6) | (next & 0x3F);
        }
        if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) return false;
    }
    return true;
}
std::optional<PluginState> PluginState::create(std::string uri, Bytes processor,
    std::optional<Bytes> controller, std::string& error, std::optional<Uuid> identity)
{
    error.clear();
    if (!validUri(uri) || processor.size() > kStreamLimit || (controller && controller->size() > kStreamLimit)) {
        error = "invalid plugin state identity or stream size"; return {};
    }
    const auto id = identity.value_or(Uuid{});
    if (identity && id.isNull()) { error = "plugin state needs a nonzero identity"; return {}; }
    const auto chosen = identity ? id : Uuid::generate();
    if (chosen.isNull()) { error = "could not generate plugin state identity"; return {}; }
    return PluginState(std::make_shared<const Data>(Data{chosen, std::move(uri), std::move(processor), std::move(controller)}));
}
const Uuid& PluginState::id() const noexcept { static const Uuid empty; return data_ ? data_->id : empty; }
const std::string& PluginState::uri() const noexcept { static const std::string empty; return data_ ? data_->uri : empty; }
std::span<const std::uint8_t> PluginState::processor() const noexcept { return data_ ? std::span<const std::uint8_t>(data_->processor) : std::span<const std::uint8_t>{}; }
std::optional<std::span<const std::uint8_t>> PluginState::controller() const noexcept {
    if (!data_ || !data_->controller) return {};
    return std::span<const std::uint8_t>(*data_->controller);
}
std::size_t PluginState::bytes() const noexcept { return data_ ? data_->processor.size() + (data_->controller ? data_->controller->size() : 0) : 0; }
bool PluginState::operator==(const PluginState& other) const noexcept {
    if (data_ == other.data_) return true;
    return data_ && other.data_ && data_->uri == other.data_->uri
        && data_->processor == other.data_->processor && data_->controller == other.data_->controller;
}
bool PluginStateBudget::add(const PluginState& state, std::string& error) {
    if (!state || references_ == maxReferences_) { error = "invalid or excessive plugin state references"; return false; }
    const auto found = seen_.find(state.id().bytes);
    if (found != seen_.end()) {
        if (found->second != state) { error = "plugin state identity has conflicting content"; return false; }
    } else {
        if (state.bytes() > limit_ - used_) { error = "plugin states exceed the project size limit"; return false; }
        seen_.emplace(state.id().bytes, state); used_ += state.bytes();
    }
    ++references_; return true;
}
}
