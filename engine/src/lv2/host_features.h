#pragma once

#include "lv2/urid.h"

#include <lv2/core/lv2.h>
#include <lv2/log/log.h>
#include <lv2/options/options.h>
#include <lv2/worker/worker.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace omawave::lv2 {

// The features handed to every plugin at instantiation.
//
// `features.h` lists what Omawave claims to provide; this is where the claims are made
// good. The two must agree: a host that advertises a feature it does not supply is
// worse than one that supplies nothing, because the plugin stops defending itself.
//
// Owned by whatever owns the instances -- the pointers handed to
// lilv_plugin_instantiate must outlive the instance, and several of them point into
// this object.
class HostFeatures {
public:
    // `maxBlock` is the largest number of frames a single run() will be given. It is
    // reported as bufsz:maxBlockLength so a plugin can size its own buffers once, and
    // it is why bufsize:boundedBlockLength can honestly be claimed: the chain works in
    // sub-blocks of this size whatever PipeWire hands the callback.
    HostFeatures(UridMap& urids, double sampleRate, std::uint32_t maxBlock);

    HostFeatures(const HostFeatures&) = delete;
    HostFeatures& operator=(const HostFeatures&) = delete;
    HostFeatures(HostFeatures&&) = delete;
    HostFeatures& operator=(HostFeatures&&) = delete;

    // Null-terminated, as lilv_plugin_instantiate expects. Stable for the life of this
    // object.
    [[nodiscard]] const LV2_Feature* const* array() const noexcept
    {
        return features_.data();
    }

    [[nodiscard]] UridMap& urids() const noexcept { return urids_; }
    [[nodiscard]] double sampleRate() const noexcept { return sampleRate_; }
    [[nodiscard]] std::uint32_t maxBlock() const noexcept { return maxBlock_; }

    // How large an atom port's buffer is unless the plugin asks for more through
    // rsz:minimumSize. Nothing sends atoms yet -- Phase 5 hosts effects, not
    // instruments -- but the buffers still have to exist and be well formed, because
    // an unconnected atom port is a null dereference inside the plugin and 315 of the
    // 360 plugins installed on this box have one.
    static constexpr std::uint32_t kSequenceSize = 8192;

    // URIDs everything needs, mapped once here rather than in each instance.
    struct Urids {
        LV2_URID atomSequence = 0;
        LV2_URID atomChunk = 0;
        LV2_URID atomFloat = 0;
        LV2_URID atomInt = 0;
        LV2_URID paramSampleRate = 0;
        LV2_URID bufMinBlockLength = 0;
        LV2_URID bufMaxBlockLength = 0;
        LV2_URID bufSequenceSize = 0;
    };

    [[nodiscard]] const Urids& ids() const noexcept { return ids_; }

private:
    static int logPrintf(LV2_Log_Handle handle, LV2_URID type, const char* fmt, ...)
        LV2_LOG_FUNC(3, 4);
    static int logVprintf(LV2_Log_Handle handle, LV2_URID type, const char* fmt,
                          va_list args);

    UridMap& urids_;
    double sampleRate_;
    std::uint32_t maxBlock_;
    Urids ids_;

    LV2_Log_Log log_{};
    LV2_Feature logFeature_{};

    // Options are read during instantiate, but a plugin is free to keep the pointer,
    // so the array lives as long as this object does.
    std::array<LV2_Options_Option, 5> options_{};
    LV2_Feature optionsFeature_{};

    LV2_Feature boundedBlockFeature_{};

    float sampleRateValue_ = 0.0f;
    std::int32_t minBlockValue_ = 0;
    std::int32_t maxBlockValue_ = 0;
    std::int32_t sequenceSizeValue_ = 0;

    std::vector<const LV2_Feature*> features_;
};

} // namespace omawave::lv2
