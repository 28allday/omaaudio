#include "resampler.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <limits>
#include <stdexcept>

namespace omawave::io {
namespace {

// Zeroth-order modified Bessel function of the first kind, for the Kaiser window.
double besselI0(double x)
{
    double sum = 1.0;
    double term = 1.0;
    const double halfSquared = (x * x) / 4.0;
    for (int k = 1; k < 64; ++k) {
        term *= halfSquared / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < sum * 1e-16)
            break;
    }
    return sum;
}

double sinc(double x)
{
    if (std::fabs(x) < 1e-12)
        return 1.0;
    const double px = std::numbers::pi * x;
    return std::sin(px) / px;
}

// ~90 dB stopband attenuation.
constexpr double kKaiserBeta = 8.6;

} // namespace

Resampler::Resampler(std::uint32_t sourceRate, std::uint32_t targetRate,
                     std::uint32_t tapsPerSide, std::uint32_t phases)
    : sourceRate_(sourceRate), targetRate_(targetRate),
      tapsPerSide_(tapsPerSide), phases_(phases),
      kernelWidth_(tapsPerSide * 2),
      ratio_(static_cast<double>(targetRate) / static_cast<double>(sourceRate))
{
    if (!sourceRate || !targetRate || !tapsPerSide || !phases || tapsPerSide > 4096 || phases > 65536)
        throw std::invalid_argument("invalid resampler rate or kernel dimensions");
    if (passthrough())
        return;
    // Lowering the cutoff stretches the filter in time by 1/ratio. With the same
    // number of taps its transition band would widen by as much, and content
    // just above the new Nyquist would fold back barely attenuated; the taps
    // grow with it, so the band stays as narrow as it is when upsampling.
    if (ratio_ < 1.0) {
        tapsPerSide_ = static_cast<std::uint32_t>(std::min(4096.0, std::ceil(tapsPerSide / ratio_)));
        kernelWidth_ = tapsPerSide_ * 2;
    }

    // Downsampling has to lower the cutoff below Nyquist of the *target* rate,
    // otherwise everything above it folds back as aliasing. Upsampling keeps the
    // source band intact.
    const double cutoff = ratio_ < 1.0 ? ratio_ * 0.95 : 0.95;

    kernel_.resize(static_cast<std::size_t>(phases_) * kernelWidth_);

    const double i0beta = besselI0(kKaiserBeta);
    const auto half = static_cast<double>(tapsPerSide_);

    for (std::uint32_t p = 0; p < phases_; ++p) {
        const double frac = static_cast<double>(p) / static_cast<double>(phases_);
        double sum = 0.0;

        for (std::uint32_t i = 0; i < kernelWidth_; ++i) {
            // Distance from the interpolation point to this tap, in source samples.
            const double x = (static_cast<double>(i) - half + 1.0) - frac;

            // Kaiser window, evaluated over the kernel's support.
            const double w = x / half;
            double window = 0.0;
            if (std::fabs(w) <= 1.0) {
                window = besselI0(kKaiserBeta * std::sqrt(1.0 - w * w)) / i0beta;
            }

            const double v = cutoff * sinc(cutoff * x) * window;
            kernel_[static_cast<std::size_t>(p) * kernelWidth_ + i] = static_cast<float>(v);
            sum += v;
        }

        // Normalise each phase to unity gain so DC survives conversion exactly.
        // Without this, different phases have slightly different gain and the output
        // develops a periodic amplitude ripple.
        if (sum > 1e-12) {
            for (std::uint32_t i = 0; i < kernelWidth_; ++i)
                kernel_[static_cast<std::size_t>(p) * kernelWidth_ + i] /=
                    static_cast<float>(sum);
        }
    }
}

std::size_t Resampler::outputFrames(std::size_t inputFrames) const noexcept
{
    if (passthrough())
        return inputFrames;
    return static_cast<std::size_t>(
        std::llround(static_cast<double>(inputFrames) * ratio_));
}

session::AudioBuffer Resampler::process(const session::AudioBuffer& in, const std::atomic<bool>* cancel) const
{
    if (cancel && cancel->load(std::memory_order_relaxed)) return {};
    if (passthrough() || in.empty())
        return in;

    const long double wanted = static_cast<long double>(in.frames()) * targetRate_ / sourceRate_;
    if (!sourceRate_ || !targetRate_ || wanted > std::numeric_limits<std::int64_t>::max()
        || wanted > std::numeric_limits<std::size_t>::max() / sizeof(float) / in.channels())
        throw std::length_error("resampled audio exceeds addressable storage");
    const std::size_t outFrames = outputFrames(in.frames());
    std::shared_ptr<session::DiskAudio> backing;
    session::AudioBuffer out;
    if (outFrames * in.channels() * sizeof(float) >= session::AudioBuffer::kDiskThreshold)
        backing = session::DiskAudio::create(in.channels(), outFrames);
    else out = session::AudioBuffer(in.channels(), outFrames);
    constexpr std::size_t blockFrames = 8192;
    std::vector<float> block(backing ? blockFrames : 0);

    const auto inFrames = static_cast<std::int64_t>(in.frames());
    const auto half = static_cast<std::int64_t>(tapsPerSide_);

    for (std::uint32_t c = 0; c < in.channels(); ++c) {
        const std::span<const float> src = in.plane(c);
        for (std::size_t first = 0; first < outFrames; first += blockFrames) {
        const auto count = std::min(blockFrames, outFrames - first);
        const auto dst = backing ? std::span<float>(block).first(count) : out.plane(c).subspan(first, count);
        if (cancel && cancel->load(std::memory_order_relaxed)) return {};
        // A silent gap has no convolution to do. Include every input tap needed
        // by this output block, so filter tails at either edge are still kept.
        const auto sourceFirst = std::max<std::int64_t>(0,
            static_cast<std::int64_t>(std::floor(static_cast<double>(first) / ratio_)) - half + 1);
        const auto sourceLast = std::min(inFrames,
            static_cast<std::int64_t>(std::floor(static_cast<double>(first + count - 1) / ratio_)) + half + 1);
        const bool silent = sourceFirst >= sourceLast || std::all_of(src.begin() + sourceFirst, src.begin() + sourceLast,
            [](float value) { return value == 0; });
        if (silent) std::fill(dst.begin(), dst.end(), 0);
        else for (std::size_t o = first; o < first + count; ++o) {
            if ((o & 4095) == 0 && cancel && cancel->load(std::memory_order_relaxed)) return {};
            // Where this output sample sits in the source timeline.
            const double srcPos = static_cast<double>(o) / ratio_;
            const auto base = static_cast<std::int64_t>(std::floor(srcPos));
            const double frac = srcPos - static_cast<double>(base);

            const auto phase = static_cast<std::uint32_t>(
                frac * static_cast<double>(phases_));
            const std::uint32_t p = phase < phases_ ? phase : phases_ - 1;

            double acc = 0.0;
            for (std::uint32_t i = 0; i < kernelWidth_; ++i) {
                const std::int64_t idx = base - half + 1 + static_cast<std::int64_t>(i);
                // Edges are treated as silence rather than clamped, so a file does not
                // gain an artificial DC step at its start and end.
                if (idx < 0 || idx >= inFrames)
                    continue;
                acc += static_cast<double>(src[static_cast<std::size_t>(idx)])
                     * static_cast<double>(tap(p, i));
            }
            dst[o - first] = static_cast<float>(acc);
        }
        if (backing) backing->writePlane(c, first, dst);
        if (in.diskStorage()) in.diskStorage()->releaseMappedPages();
        }
    }
    if (backing) { backing->seal(); out = session::AudioBuffer::fromDisk(std::move(backing)); }
    if (in.diskStorage()) in.diskStorage()->releaseMappedPages();
    return out;
}

} // namespace omawave::io
