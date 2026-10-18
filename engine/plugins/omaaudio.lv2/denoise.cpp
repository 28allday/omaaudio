// OmaAudio Voice Cleanup: RNNoise as a live, mono LV2 effect, so background
// noise in speech can be reduced on the track and adjusted at any time (the
// Sound panel's "Background noise"). A stereo track runs it as a pair.
//
// RNNoise works on 480-sample frames at 48 kHz and answers 960 samples late
// (the same model delay as Voice Cleanup), so the output is 1440 samples behind
// the input. That is reported as latency and the dry signal is delayed to match,
// so Amount blends two aligned signals. Other project rates are refused: the
// project rate is fixed and nothing resamples inside the chain; at those rates
// the Sound panel applies Voice Cleanup instead.
#include <lv2/core/lv2.h>
#include <rnnoise.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace {

constexpr std::uint32_t kFrame = 480;
constexpr std::uint32_t kModelDelay = 2 * kFrame;
constexpr std::uint32_t kLatency = kFrame + kModelDelay;
// RNNoise expects samples on the 16-bit scale.
constexpr float kScale = 32768.0f;

enum Port : std::uint32_t { In, Out, Amount, Latency, PortCount };

struct Denoise {
    float* ports[PortCount]{};
    DenoiseState* model = nullptr;
    std::array<float, kFrame> input{};      // the frame being collected
    std::array<float, kFrame> output{};     // the model's last frame, played out
    std::array<float, kLatency> dry{};      // the input, delayed to the model's output
    std::uint32_t at = 0;                   // position in the current frame
    std::uint32_t dryAt = 0;
    float amount = 1.0f;                    // smoothed Amount, 0..1

    void reset()
    {
        input.fill(0); output.fill(0); dry.fill(0);
        at = 0; dryAt = 0;
        if (model) rnnoise_destroy(model);
        model = rnnoise_create(nullptr);
    }
};

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*, const LV2_Feature* const*)
{
    if (rate != 48000.0 || rnnoise_get_frame_size() != int(kFrame)) return nullptr;
    auto* d = new Denoise;
    d->model = rnnoise_create(nullptr);
    if (!d->model) { delete d; return nullptr; }
    return d;
}

void connect(LV2_Handle handle, std::uint32_t port, void* data)
{
    if (port < PortCount) static_cast<Denoise*>(handle)->ports[port] = static_cast<float*>(data);
}

void activate(LV2_Handle handle)
{
    auto* d = static_cast<Denoise*>(handle);
    d->reset();
    d->amount = d->ports[Amount] ? std::clamp(*d->ports[Amount], 0.0f, 100.0f) / 100.0f : 1.0f;
}

void run(LV2_Handle handle, std::uint32_t count)
{
    auto& d = *static_cast<Denoise*>(handle);
    const float* in = d.ports[In];
    float* out = d.ports[Out];
    if (d.ports[Latency]) *d.ports[Latency] = float(kLatency);
    if (!in || !out || !d.model) return;
    // Amount moves over the block, so a change never clicks.
    const float target = d.ports[Amount] ? std::clamp(*d.ports[Amount], 0.0f, 100.0f) / 100.0f : 1.0f;
    const float step = count ? (target - d.amount) / float(count) : 0.0f;
    for (std::uint32_t f = 0; f < count; ++f) {
        float x = in[f];
        if (!std::isfinite(x)) x = 0.0f;     // one bad sample must not poison the model
        const float dry = d.dry[d.dryAt];
        d.dry[d.dryAt] = x;
        d.dryAt = (d.dryAt + 1) % kLatency;
        const float wet = d.output[d.at] / kScale;
        d.input[d.at] = x * kScale;
        d.amount += step;
        out[f] = dry + (wet - dry) * d.amount;
        if (++d.at == kFrame) {
            (void)rnnoise_process_frame(d.model, d.output.data(), d.input.data());
            d.at = 0;
        }
    }
    d.amount = target;
}

void cleanup(LV2_Handle handle)
{
    auto* d = static_cast<Denoise*>(handle);
    if (d->model) rnnoise_destroy(d->model);
    delete d;
}

const LV2_Descriptor descriptor{"urn:omaaudio:denoise", instantiate, connect, activate, run, nullptr, cleanup, nullptr};

}  // namespace

extern "C" LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(std::uint32_t index)
{
    return index == 0 ? &descriptor : nullptr;
}
