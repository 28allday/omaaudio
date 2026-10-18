#pragma once

#include <lv2/core/lv2.h>       // LV2_Feature
#include <lv2/urid/urid.h>

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace omawave::lv2 {

// The host's URID map: strings in, small integers out, one table for the whole
// process.
//
// One table, not one per plugin, and that is load-bearing. A saved plugin state is
// written in terms of URIDs, and restoring it maps the same strings again; two plugins
// that both talk about the same URI have to agree on its number or a state saved by one
// is nonsense to the other. Sharing the table is what makes that true by construction.
//
// Ids start at 1. Zero is the LV2 convention for "not mapped", so it must never be
// handed out.
//
// THREADING. Mapping takes a mutex. The audio thread must never call it -- LV2 puts
// urid:map in the instantiation threading class and nothing in our render path needs a
// URI. The lock exists because instantiation happens on a worker thread while the loop
// thread may be reading the catalogue, not because the audio thread is a caller.
class UridMap {
public:
    UridMap();

    UridMap(const UridMap&) = delete;
    UridMap& operator=(const UridMap&) = delete;
    UridMap(UridMap&&) = delete;
    UridMap& operator=(UridMap&&) = delete;

    // Stable for the life of the map: mapping the same string twice gives the same id.
    [[nodiscard]] LV2_URID map(const char* uri);

    // Nullptr for an id this map never handed out.
    [[nodiscard]] const char* unmap(LV2_URID urid) const;

    [[nodiscard]] std::size_t size() const;

    // Features to hand to lilv_plugin_instantiate. The pointers stay valid for the
    // life of the map, which outlives every instance by construction -- the App owns
    // one map and every chain in it.
    [[nodiscard]] const LV2_Feature* mapFeature() const noexcept { return &mapFeature_; }
    [[nodiscard]] const LV2_Feature* unmapFeature() const noexcept
    {
        return &unmapFeature_;
    }

private:
    static LV2_URID mapCallback(LV2_URID_Map_Handle handle, const char* uri);
    static const char* unmapCallback(LV2_URID_Unmap_Handle handle, LV2_URID urid);

    mutable std::mutex mutex_;

    // The strings are owned here and the lookup keys are views of them, so the
    // container must never move an element it has already handed a view of. A deque
    // never does; a vector would, and a short-enough URI living in a small-string
    // buffer would then have the view pointing into the moved-from string.
    std::deque<std::string> strings_;
    std::unordered_map<std::string_view, LV2_URID> ids_;

    LV2_URID_Map mapData_{};
    LV2_URID_Unmap unmapData_{};
    LV2_Feature mapFeature_{};
    LV2_Feature unmapFeature_{};
};

} // namespace omawave::lv2
