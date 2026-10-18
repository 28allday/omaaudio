#include "bus_layout.h"
#include <pluginterfaces/vst/vstspeaker.h>
#include <bit>
#include <stdexcept>

namespace omawave::vst3 {
using namespace Steinberg;
using namespace Steinberg::Vst;
namespace {
void require(bool valid, const char* message) { if (!valid) throw std::runtime_error(message); }
SpeakerArrangement arrangement(std::uint32_t channels) {
    return channels == 1 ? SpeakerArr::kMono : SpeakerArr::kStereo;
}
}
AudioBusLayout negotiateAudioLayout(IComponent* component, IAudioProcessor* processor,
    std::uint32_t channels, std::uint32_t sidechainChannels)
{
    require(component && processor && (channels == 1 || channels == 2) && sidechainChannels <= 2,
        "Invalid VST3 bus configuration");
    AudioBusLayout layout;
    const auto read = [&](BusDirection direction, int32 index, AudioBusDescription& bus) {
        require(component->getBusInfo(kAudio, direction, index, bus.info) == kResultOk
            && bus.info.mediaType == kAudio && bus.info.direction == direction
            && bus.info.channelCount >= 0 && bus.info.channelCount <= 64
            && (bus.info.busType == kMain || bus.info.busType == kAux), "Invalid VST3 audio bus metadata");
        require(processor->getBusArrangement(direction, index, bus.arrangement) == kResultOk
            && std::popcount(bus.arrangement) == bus.info.channelCount, "VST3 channel count and speaker arrangement disagree");
    };
    const auto buses = [&](BusDirection direction, auto& list, std::size_t& main) {
        const int32 count = component->getBusCount(kAudio, direction);
        require(count > 0 && count <= 64, "VST3 effect needs a bounded main input and output bus");
        list.resize(count); main = list.size();
        for (int32 i = 0; i < count; ++i) {
            read(direction, i, list[i]);
            if (list[i].info.busType == kMain) {
                require(main == list.size() && !(list[i].info.flags & BusInfo::kIsControlVoltage),
                    "VST3 effect needs one non-control-voltage main bus per direction");
                main = i; list[i].selected = true;
            }
        }
        require(main < list.size(), "VST3 effect has no main audio bus");
    };
    buses(kInput, layout.inputs, layout.mainInput); buses(kOutput, layout.outputs, layout.mainOutput);
    layout.keyInput = layout.inputs.size();
    if (sidechainChannels) {
        for (std::size_t i = 0; i < layout.inputs.size(); ++i)
            if (layout.inputs[i].info.busType == kAux && !(layout.inputs[i].info.flags & BusInfo::kIsControlVoltage)) {
                layout.keyInput = i; layout.inputs[i].selected = true; break;
            }
        require(layout.keyInput < layout.inputs.size(), "VST3 effect has no audio sidechain bus");
    }
    std::vector<SpeakerArrangement> in, out;
    for (const auto& bus : layout.inputs) in.push_back(bus.arrangement);
    for (const auto& bus : layout.outputs) out.push_back(bus.arrangement);
    in[layout.mainInput] = out[layout.mainOutput] = arrangement(channels);
    if (layout.keyInput < in.size()) in[layout.keyInput] = arrangement(sidechainChannels);
    const auto negotiated = processor->setBusArrangements(in.data(), in.size(), out.data(), out.size());
    require(negotiated == kResultOk || negotiated == kResultFalse, "VST3 bus negotiation failed");
    // A false return may still adapt selected buses successfully. Inspect the
    // actual configuration; do not trust either result code alone.
    const auto verify = [&](BusDirection direction, auto& list, const auto& requested) {
        require(component->getBusCount(kAudio, direction) == static_cast<int32>(list.size()),
            "VST3 changed its bus count during negotiation");
        for (std::size_t i = 0; i < list.size(); ++i) {
            const auto type = list[i].info.busType;
            read(direction, i, list[i]);
            require(list[i].info.busType == type, "VST3 changed its bus roles during negotiation");
            if (list[i].selected) require(list[i].arrangement == requested[i]
                && !(list[i].info.flags & BusInfo::kIsControlVoltage), "VST3 effect refused the requested main or sidechain layout");
        }
    };
    verify(kInput, layout.inputs, in); verify(kOutput, layout.outputs, out);
    return layout;
}
}
