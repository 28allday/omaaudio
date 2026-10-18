#include "lv2/urid.h"

namespace omawave::lv2 {

UridMap::UridMap()
{
    mapData_.handle = this;
    mapData_.map = &UridMap::mapCallback;
    mapFeature_.URI = LV2_URID__map;
    mapFeature_.data = &mapData_;

    unmapData_.handle = this;
    unmapData_.unmap = &UridMap::unmapCallback;
    unmapFeature_.URI = LV2_URID__unmap;
    unmapFeature_.data = &unmapData_;
}

LV2_URID UridMap::map(const char* uri)
{
    if (uri == nullptr || *uri == '\0')
        return 0;

    const std::lock_guard<std::mutex> lock(mutex_);

    const std::string_view key{uri};
    if (const auto it = ids_.find(key); it != ids_.end())
        return it->second;

    strings_.emplace_back(uri);
    const auto id = static_cast<LV2_URID>(strings_.size());   // 1-based; 0 means unmapped
    ids_.emplace(std::string_view{strings_.back()}, id);
    return id;
}

const char* UridMap::unmap(LV2_URID urid) const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (urid == 0 || urid > strings_.size())
        return nullptr;
    return strings_[urid - 1].c_str();
}

std::size_t UridMap::size() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return strings_.size();
}

LV2_URID UridMap::mapCallback(LV2_URID_Map_Handle handle, const char* uri)
{
    return static_cast<UridMap*>(handle)->map(uri);
}

const char* UridMap::unmapCallback(LV2_URID_Unmap_Handle handle, LV2_URID urid)
{
    return static_cast<const UridMap*>(handle)->unmap(urid);
}

} // namespace omawave::lv2
