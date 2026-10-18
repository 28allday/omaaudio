// OmaAudio Low Cut, Hum Removal and Width: small effects behind the Sound
// panel (its "Rumble and pops", "Mains hum" and "Width"), shipped with the
// application so they work without any optional plugin pack. None has latency,
// and all work at any project rate.
//
//   Low Cut      mono (a pair on a stereo track): a 24 dB/octave Butterworth
//                high-pass, two biquads in series.
//   Hum Removal  mono: narrow notches at the mains frequency and its harmonics,
//                each about 4 Hz wide, so the voice around them is left alone.
//   Width        stereo: scales the side (L − R) against the middle (L + R);
//                0 % is mono, 100 % unchanged, 200 % twice as wide.
#include <lv2/core/lv2.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace {

// Transposed direct form II; coefficients normalised by a0.
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;

    double process(double x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void clear() { z1 = z2 = 0; }

    void highPass(double rate, double frequency, double q)
    {
        const double w = 2 * std::numbers::pi * frequency / rate;
        const double alpha = std::sin(w) / (2 * q), c = std::cos(w), a0 = 1 + alpha;
        b0 = (1 + c) / 2 / a0; b1 = -(1 + c) / a0; b2 = b0;
        a1 = -2 * c / a0; a2 = (1 - alpha) / a0;
    }
    void notch(double rate, double frequency, double q)
    {
        const double w = 2 * std::numbers::pi * frequency / rate;
        const double alpha = std::sin(w) / (2 * q), c = std::cos(w), a0 = 1 + alpha;
        b0 = 1 / a0; b1 = -2 * c / a0; b2 = b0;
        a1 = b1; a2 = (1 - alpha) / a0;
    }
};

float clean(float x) { return std::isfinite(x) ? x : 0.0f; }

// --- Low Cut -------------------------------------------------------------------

namespace lowcut {
enum Port : std::uint32_t { In, Out, Frequency, PortCount };
constexpr float kMin = 20, kMax = 300, kDefault = 80;
// The two pole-pair Qs of a fourth-order Butterworth response.
constexpr std::array<double, 2> kQ{0.54119610014619698, 1.3065629648763766};

struct Plugin {
    float* ports[PortCount]{};
    double rate = 48000;
    float frequency = -1;           // the frequency the coefficients were made for
    std::array<Biquad, 2> stages{};

    void tune(float wanted)
    {
        if (wanted == frequency) return;
        frequency = wanted;
        for (std::size_t i = 0; i < stages.size(); ++i) stages[i].highPass(rate, wanted, kQ[i]);
    }
};

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*, const LV2_Feature* const*)
{
    if (!(rate >= 8000)) return nullptr;
    auto* p = new Plugin;
    p->rate = rate;
    return p;
}
void connect(LV2_Handle h, std::uint32_t port, void* data)
{
    if (port < PortCount) static_cast<Plugin*>(h)->ports[port] = static_cast<float*>(data);
}
void activate(LV2_Handle h)
{
    auto* p = static_cast<Plugin*>(h);
    for (auto& s : p->stages) s.clear();
    p->frequency = -1;
}
void run(LV2_Handle h, std::uint32_t count)
{
    auto& p = *static_cast<Plugin*>(h);
    const float* in = p.ports[In];
    float* out = p.ports[Out];
    if (!in || !out) return;
    const float wanted = p.ports[Frequency] ? clean(*p.ports[Frequency]) : kDefault;
    p.tune(std::clamp(wanted, kMin, std::min(kMax, float(p.rate * 0.45))));
    for (std::uint32_t f = 0; f < count; ++f) {
        double y = clean(in[f]);
        for (auto& s : p.stages) y = s.process(y);
        out[f] = float(y);
    }
}
void cleanup(LV2_Handle h) { delete static_cast<Plugin*>(h); }

const LV2_Descriptor descriptor{"urn:omaaudio:lowcut", instantiate, connect, activate, run, nullptr, cleanup, nullptr};
}  // namespace lowcut

// --- Hum Removal ---------------------------------------------------------------

namespace hum {
enum Port : std::uint32_t { In, Out, Mains, Harmonics, PortCount };
constexpr int kMaxHarmonics = 8;
constexpr double kWidthHz = 4;

struct Plugin {
    float* ports[PortCount]{};
    double rate = 48000;
    float mains = -1;
    int harmonics = -1;
    int active = 0;                 // notches below the Nyquist limit
    std::array<Biquad, kMaxHarmonics> notches{};

    void tune(float wantedMains, int wantedHarmonics)
    {
        if (wantedMains == mains && wantedHarmonics == harmonics) return;
        mains = wantedMains; harmonics = wantedHarmonics;
        active = 0;
        for (int k = 1; k <= harmonics; ++k) {
            const double f = mains * k;
            if (f >= rate * 0.45) break;
            notches[std::size_t(active)].notch(rate, f, f / kWidthHz);
            notches[std::size_t(active)].clear();
            ++active;
        }
    }
};

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*, const LV2_Feature* const*)
{
    if (!(rate >= 8000)) return nullptr;
    auto* p = new Plugin;
    p->rate = rate;
    return p;
}
void connect(LV2_Handle h, std::uint32_t port, void* data)
{
    if (port < PortCount) static_cast<Plugin*>(h)->ports[port] = static_cast<float*>(data);
}
void activate(LV2_Handle h)
{
    auto* p = static_cast<Plugin*>(h);
    p->mains = -1; p->harmonics = -1;
}
void run(LV2_Handle h, std::uint32_t count)
{
    auto& p = *static_cast<Plugin*>(h);
    const float* in = p.ports[In];
    float* out = p.ports[Out];
    if (!in || !out) return;
    // Mains is 50 or 60 Hz; anything else is read as the nearer of the two.
    const float mains = (p.ports[Mains] ? clean(*p.ports[Mains]) : 50.0f) >= 55.0f ? 60.0f : 50.0f;
    const int harmonics = std::clamp(int(std::lround(p.ports[Harmonics] ? clean(*p.ports[Harmonics]) : 4.0f)), 1, kMaxHarmonics);
    p.tune(mains, harmonics);
    for (std::uint32_t f = 0; f < count; ++f) {
        double y = clean(in[f]);
        for (int i = 0; i < p.active; ++i) y = p.notches[std::size_t(i)].process(y);
        out[f] = float(y);
    }
}
void cleanup(LV2_Handle h) { delete static_cast<Plugin*>(h); }

const LV2_Descriptor descriptor{"urn:omaaudio:hum", instantiate, connect, activate, run, nullptr, cleanup, nullptr};
}  // namespace hum

// --- Width ----------------------------------------------------------------------

namespace width {
enum Port : std::uint32_t { InL, InR, OutL, OutR, Width, PortCount };

struct Plugin {
    float* ports[PortCount]{};
    float width = 1;                // smoothed, 0..2
};

LV2_Handle instantiate(const LV2_Descriptor*, double, const char*, const LV2_Feature* const*) { return new Plugin; }
void connect(LV2_Handle h, std::uint32_t port, void* data)
{
    if (port < PortCount) static_cast<Plugin*>(h)->ports[port] = static_cast<float*>(data);
}
float wanted(const Plugin& p) { return p.ports[Width] ? std::clamp(clean(*p.ports[Width]), 0.0f, 200.0f) / 100.0f : 1.0f; }
void activate(LV2_Handle h) { auto* p = static_cast<Plugin*>(h); p->width = wanted(*p); }
void run(LV2_Handle h, std::uint32_t count)
{
    auto& p = *static_cast<Plugin*>(h);
    const float *l = p.ports[InL], *r = p.ports[InR];
    float *ol = p.ports[OutL], *orr = p.ports[OutR];
    if (!l || !r || !ol || !orr) return;
    // Width moves over the block, so a change never clicks.
    const float target = wanted(p);
    const float step = count ? (target - p.width) / float(count) : 0.0f;
    for (std::uint32_t f = 0; f < count; ++f) {
        p.width += step;
        const float a = clean(l[f]), b = clean(r[f]);
        const float mid = (a + b) * 0.5f, side = (a - b) * 0.5f * p.width;
        ol[f] = mid + side;
        orr[f] = mid - side;
    }
    p.width = target;
}
void cleanup(LV2_Handle h) { delete static_cast<Plugin*>(h); }

const LV2_Descriptor descriptor{"urn:omaaudio:width", instantiate, connect, activate, run, nullptr, cleanup, nullptr};
}  // namespace width

}  // namespace

extern "C" LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(std::uint32_t index)
{
    switch (index) {
    case 0: return &lowcut::descriptor;
    case 1: return &hum::descriptor;
    case 2: return &width::descriptor;
    default: return nullptr;
    }
}
