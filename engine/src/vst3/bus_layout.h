#pragma once

#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace omawave::vst3 {
struct AudioBusDescription {
    Steinberg::Vst::BusInfo info{};
    Steinberg::Vst::SpeakerArrangement arrangement = 0;
    bool selected = false;
};
struct AudioBusLayout {
    std::vector<AudioBusDescription> inputs, outputs;
    std::size_t mainInput = 0, mainOutput = 0, keyInput = 0;
};
// Initialized, inactive component on its control thread. No processing setup or
// activation. Throws on unsupported/invalid results; callers own the candidate.
// Shared by isolated discovery and real instance preparation, which revalidates
// the result after restoring state. keyInput == inputs.size() means no sidechain.
[[nodiscard]] AudioBusLayout negotiateAudioLayout(Steinberg::Vst::IComponent*,
    Steinberg::Vst::IAudioProcessor*, std::uint32_t channels, std::uint32_t sidechainChannels);
}
