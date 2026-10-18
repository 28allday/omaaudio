#pragma once

#include <array>
#include <string_view>

#include <lv2/buf-size/buf-size.h>
#include <lv2/log/log.h>
#include <lv2/options/options.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>

namespace omawave::lv2 {

// Every host feature Omawave provides.
//
// This list is the contract in both directions. A plugin that *requires* something not
// on it is refused at discovery rather than instantiated and crashed, and anything
// added here has to actually be implemented -- a host that advertises a feature it does
// not supply is worse than one that supplies nothing, because the plugin stops
// defending itself.
//
// Measured against the 360 plugins installed on the target box, the required features
// in the wild are exactly: urid:map (272 plugins), worker:schedule (7),
// bufsize:boundedBlockLength (6) and options:options (6). Nothing installed here is
// refused for want of a feature.
inline constexpr std::array kProvidedFeatures{
    std::string_view{LV2_URID__map},
    std::string_view{LV2_URID__unmap},
    std::string_view{LV2_LOG__log},
    std::string_view{LV2_OPTIONS__options},
    std::string_view{LV2_BUF_SIZE__boundedBlockLength},
    std::string_view{LV2_WORKER__schedule},
};

// Deliberately absent, and each for a reason:
//
//   bufsize:fixedBlockLength / powerOf2BlockLength -- PipeWire may change the quantum
//     under us, so neither can be promised. `boundedBlockLength` can: the chain works
//     in sub-blocks of a fixed maximum whatever the callback is handed.
//   state:threadSafeRestore -- a restore happens with the transport stopped, so
//     claiming it would be claiming something we do not do.
//   state:makePath / state:mapPath -- instantiation is given neither. They were
//     listed here once without being supplied, which is exactly the case the
//     paragraph above warns about; a plugin that requires them is refused instead.
//   state:loadDefaultState -- nothing implements it yet. Nothing installed on the
//     target box requires it either.

[[nodiscard]] constexpr bool isProvidedFeature(std::string_view uri) noexcept
{
    for (const std::string_view provided : kProvidedFeatures) {
        if (provided == uri)
            return true;
    }
    return false;
}

} // namespace omawave::lv2
