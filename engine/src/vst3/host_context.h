#pragma once

#include <pluginterfaces/vst/ivsthostapplication.h>
#include <cstddef>
#include <string>

namespace omawave::vst3 {
// Main/control-thread services. Messages and typed attributes copy their values;
// they are never allocated or edited by the audio callback.
[[nodiscard]] Steinberg::IPtr<Steinberg::Vst::IHostApplication> makeHostContext();
// Bounded UTF-16 metadata conversion; malformed surrogate pairs become U+FFFD.
[[nodiscard]] std::string utf8(const Steinberg::char16*, std::size_t units);
}
