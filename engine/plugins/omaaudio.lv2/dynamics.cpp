// OmaAudio Limit Peaks and Change Level: the two level effects shipped with the
// application, so a track can be made louder or kept under a ceiling without any
// optional plugin pack. Both take a mono or a stereo track as one instance: the
// right-hand ports are optional and left unconnected on a mono track.
//
//   Limit Peaks   a lookahead peak limiter. The signal is delayed by 5 ms and the
//                 gain starts falling that long before a peak arrives, so no output
//                 sample rises above the ceiling and nothing is clipped. Sample
//                 peak, not true peak (no oversampling). Stereo channels share one
//                 gain, so the image does not move. The 5 ms is reported as latency;
//                 below the ceiling the gain is exactly 1 and the output is the input,
//                 delayed.
//   Change Level  a plain gain, glided over about 10 ms so a change never clicks.
//                 No latency; at 0 dB the output is the input, bit for bit.
#include <lv2/core/lv2.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <new>
#include <vector>

namespace {

float clean(float x) { return std::isfinite(x) ? x : 0.0f; }
double fromDb(double db) { return std::pow(10.0, db / 20.0); }

// --- Limit Peaks ---------------------------------------------------------------

namespace limit {
enum Port : std::uint32_t { InL, InR, OutL, OutR, Ceiling, Release, Reduction, Latency, PortCount };
constexpr float kCeilingMin = -24, kCeilingMax = 0, kCeilingDefault = -1;
constexpr float kReleaseMin = 10, kReleaseMax = 1000, kReleaseDefault = 100;
constexpr float kReductionMax = 60;
constexpr double kLookahead = 0.005;            // seconds
// A release this close to unity is unity: it lets the gain reach exactly 1, so the
// output is the input again rather than a hair quieter for ever.
constexpr double kUnitySnap = 1e-6;

// How it works, for a lookahead of L frames. `need` is the gain each input frame
// asks for (ceiling / peak, or 1). The minimum of `need` over the last L + 1 frames
// covers every peak the delay line still holds; the release lets that rise slowly;
// and an L + 1 frame moving average turns its steps into ramps. The frame leaving
// the delay line was the newest frame of every window that average spans, so the
// gain it gets is never more than it needed: the ceiling holds exactly.
struct Plugin {
    float* ports[PortCount]{};
    double rate = 48000;
    std::uint32_t lookahead = 0;                // L, the reported latency
    std::uint32_t window = 0;                   // L + 1

    // All sized once in instantiate, for this rate's lookahead; run() never allocates.
    std::vector<float> delayL, delayR;          // L frames each
    std::vector<std::uint64_t> minAt;           // monotonic queue: frame numbers...
    std::vector<double> minNeed;                // ...and the gain each asked for
    std::vector<double> held;                   // the last L + 1 released gains
    std::uint32_t delayAt = 0, heldAt = 0;
    std::uint32_t minHead = 0, minSize = 0;
    std::uint64_t frame = 0;
    double released = 1;                        // the gain after the release
    double sum = 0;                             // of `held`
    std::uint32_t belowUnity = 0;               // entries of `held` under 1

    void reset()
    {
        std::fill(delayL.begin(), delayL.end(), 0.0f);
        std::fill(delayR.begin(), delayR.end(), 0.0f);
        std::fill(held.begin(), held.end(), 1.0);
        delayAt = heldAt = minHead = minSize = 0;
        frame = 0;
        released = 1;
        sum = double(window);
        belowUnity = 0;
    }

    // The smallest `need` of the last L + 1 frames, after adding this frame's.
    double windowMinimum(double need)
    {
        const std::uint32_t capacity = window;
        while (minSize && minNeed[(minHead + minSize - 1) % capacity] >= need) --minSize;
        const std::uint32_t tail = (minHead + minSize) % capacity;
        minAt[tail] = frame; minNeed[tail] = need; ++minSize;
        while (minAt[minHead] + window <= frame) { minHead = (minHead + 1) % capacity; --minSize; }
        return minNeed[minHead];
    }

    // The gain for the frame leaving the delay line now.
    double gain(double need, double releaseCoefficient)
    {
        const double target = windowMinimum(need);
        if (target <= released) {
            released = target;                  // down at once: the average is the attack
        } else {
            released = target - (target - released) * releaseCoefficient;
            if (target - released < kUnitySnap) released = target;
        }
        const double old = held[heldAt];
        if (old < 1) --belowUnity;
        if (released < 1) ++belowUnity;
        held[heldAt] = released;
        heldAt = heldAt + 1 == window ? 0 : heldAt + 1;
        sum += released - old;
        if (!belowUnity) { sum = double(window); return 1; }   // exact unity, no drift
        return std::min(1.0, sum / double(window));
    }
};

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*, const LV2_Feature* const*)
{
    if (!(rate >= 8000 && rate <= 1536000)) return nullptr;
    auto* p = new (std::nothrow) Plugin;
    if (!p) return nullptr;
    try {
        p->rate = rate;
        p->lookahead = std::uint32_t(std::lround(rate * kLookahead));
        p->window = p->lookahead + 1;
        p->delayL.assign(p->lookahead, 0.0f);
        p->delayR.assign(p->lookahead, 0.0f);
        p->minAt.assign(p->window, 0);
        p->minNeed.assign(p->window, 1.0);
        p->held.assign(p->window, 1.0);
    } catch (...) {
        delete p;
        return nullptr;
    }
    p->reset();
    return p;
}
void connect(LV2_Handle h, std::uint32_t port, void* data)
{
    if (port < PortCount) static_cast<Plugin*>(h)->ports[port] = static_cast<float*>(data);
}
void report(Plugin& p, float reduction)
{
    if (p.ports[Latency]) *p.ports[Latency] = float(p.lookahead);
    if (p.ports[Reduction]) *p.ports[Reduction] = reduction;
}
void activate(LV2_Handle h)
{
    auto* p = static_cast<Plugin*>(h);
    p->reset();
    report(*p, 0);
}
void run(LV2_Handle h, std::uint32_t count)
{
    auto& p = *static_cast<Plugin*>(h);
    report(p, 0);
    const float *inL = p.ports[InL], *inR = p.ports[InR];
    float *outL = p.ports[OutL], *outR = p.ports[OutR];
    if (!inL || !outL) return;
    const bool stereo = inR && outR;
    const double ceiling = fromDb(std::clamp(p.ports[Ceiling] ? clean(*p.ports[Ceiling]) : kCeilingDefault,
                                             kCeilingMin, kCeilingMax));
    const double releaseMs = std::clamp(p.ports[Release] ? clean(*p.ports[Release]) : kReleaseDefault,
                                        kReleaseMin, kReleaseMax);
    const double releaseCoefficient = std::exp(-1.0 / (releaseMs * 0.001 * p.rate));
    double lowest = 1;
    for (std::uint32_t f = 0; f < count; ++f, ++p.frame) {
        // Both inputs are read before either output is written: in place is fine.
        const float l = clean(inL[f]);
        const float r = stereo ? clean(inR[f]) : 0.0f;
        const double peak = std::max(std::fabs(double(l)), std::fabs(double(r)));
        const double need = peak > ceiling ? ceiling / peak : 1.0;
        const double g = p.gain(need, releaseCoefficient);
        lowest = std::min(lowest, g);
        float dl = l, dr = r;
        if (p.lookahead) {
            dl = p.delayL[p.delayAt]; p.delayL[p.delayAt] = l;
            dr = p.delayR[p.delayAt]; p.delayR[p.delayAt] = r;
            p.delayAt = p.delayAt + 1 == p.lookahead ? 0 : p.delayAt + 1;
        }
        outL[f] = g == 1 ? dl : float(double(dl) * g);
        if (stereo) outR[f] = g == 1 ? dr : float(double(dr) * g);
    }
    report(p, lowest < 1 ? std::min(kReductionMax, float(-20 * std::log10(lowest))) : 0.0f);
}
void cleanup(LV2_Handle h) { delete static_cast<Plugin*>(h); }

const LV2_Descriptor descriptor{"urn:omaaudio:limit", instantiate, connect, activate, run, nullptr, cleanup, nullptr};
}  // namespace limit

// --- Change Level --------------------------------------------------------------

namespace gain {
enum Port : std::uint32_t { InL, InR, OutL, OutR, Gain, PortCount };
constexpr float kMin = -48, kMax = 24, kDefault = 0;
constexpr double kGlide = 0.010;                // seconds, the time constant
constexpr double kSnap = 1e-7;                  // relative: close enough is there

struct Plugin {
    float* ports[PortCount]{};
    double coefficient = 0;                     // one-pole glide per frame
    float db = kDefault;                        // what `target` was made from
    double target = 1, current = 1;

    void aim()
    {
        const float wanted = std::clamp(ports[Gain] ? clean(*ports[Gain]) : kDefault, kMin, kMax);
        if (wanted == db) return;
        db = wanted;
        target = wanted == 0 ? 1.0 : fromDb(wanted);
    }
};

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*, const LV2_Feature* const*)
{
    if (!(rate >= 8000)) return nullptr;
    auto* p = new (std::nothrow) Plugin;
    if (!p) return nullptr;
    p->coefficient = std::exp(-1.0 / (kGlide * rate));
    return p;
}
void connect(LV2_Handle h, std::uint32_t port, void* data)
{
    if (port < PortCount) static_cast<Plugin*>(h)->ports[port] = static_cast<float*>(data);
}
void activate(LV2_Handle h)
{
    auto* p = static_cast<Plugin*>(h);
    p->db = kDefault; p->target = 1;
    p->aim();
    p->current = p->target;                     // starts where it is asked to be
}
void run(LV2_Handle h, std::uint32_t count)
{
    auto& p = *static_cast<Plugin*>(h);
    const float *inL = p.ports[InL], *inR = p.ports[InR];
    float *outL = p.ports[OutL], *outR = p.ports[OutR];
    if (!inL || !outL) return;
    const bool stereo = inR && outR;
    p.aim();
    for (std::uint32_t f = 0; f < count; ++f) {
        if (p.current != p.target) {
            p.current = p.target + (p.current - p.target) * p.coefficient;
            if (std::fabs(p.current - p.target) <= kSnap * p.target) p.current = p.target;
        }
        const float l = clean(inL[f]);
        const float r = stereo ? clean(inR[f]) : 0.0f;
        outL[f] = p.current == 1 ? l : float(double(l) * p.current);
        if (stereo) outR[f] = p.current == 1 ? r : float(double(r) * p.current);
    }
}
void cleanup(LV2_Handle h) { delete static_cast<Plugin*>(h); }

const LV2_Descriptor descriptor{"urn:omaaudio:gain", instantiate, connect, activate, run, nullptr, cleanup, nullptr};
}  // namespace gain

}  // namespace

extern "C" LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(std::uint32_t index)
{
    switch (index) {
    case 0: return &limit::descriptor;
    case 1: return &gain::descriptor;
    default: return nullptr;
    }
}
