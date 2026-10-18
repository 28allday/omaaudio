#include "generate.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace omawave::session {
namespace {
constexpr FramePos kMaxFrames = FramePos(192000) * 60 * 60 * 6; // six hours at the highest rate

// Residual that removes the discontinuity of a naive step at phase 0 (PolyBLEP).
double blep(double phase, double increment)
{
    if (phase < increment) { const double t = phase / increment; return t + t - t * t - 1; }
    if (phase > 1 - increment) { const double t = (phase - 1) / increment; return t * t + t + t + 1; }
    return 0;
}
bool validCommon(FramePos frames, std::uint32_t channels, double db, std::string& error)
{
    if (frames <= 0 || frames > kMaxFrames) { error = "Choose a duration between one sample and six hours."; return false; }
    if (!channels || channels > 8) { error = "Choose one to eight channels."; return false; }
    if (!std::isfinite(db) || db > 0 || db < -120) { error = "Choose a level from -120 to 0 dBFS."; return false; }
    return true;
}
}

AudioBuffer generateTone(const ToneSpec& spec, std::string& error)
{
    error.clear();
    if (!validCommon(spec.frames, spec.channels, spec.amplitudeDb, error)) return {};
    const double nyquist = spec.sampleRate / 2.0;
    if (spec.sampleRate < 8000 || spec.sampleRate > 192000 || !std::isfinite(spec.frequency)
        || spec.frequency < 1 || spec.frequency >= nyquist || !std::isfinite(spec.endFrequency)
        || spec.endFrequency < 0 || (spec.endFrequency && (spec.endFrequency < 1 || spec.endFrequency >= nyquist))) {
        error = "Choose frequencies from 1 Hz up to half the sample rate."; return {};
    }
    AudioBuffer out(spec.channels, std::size_t(spec.frames));
    const double amplitude = std::pow(10.0, spec.amplitudeDb / 20.0);
    const double ratio = spec.endFrequency ? spec.endFrequency / spec.frequency : 1;
    // Shapes with harmonics are drawn 8x oversampled (PolyBLEP) and decimated
    // through a long windowed-sinc low-pass, so nothing folds back below Nyquist.
    const int over = spec.shape == ToneShape::Sine ? 1 : 8;
    std::vector<double> taps;
    if (over > 1) {
        const int half = over * 48;
        taps.resize(std::size_t(2 * half + 1));
        const double cutoff = 0.46 / over; // of the oversampled rate
        double sum = 0;
        for (int i = -half; i <= half; ++i) {
            const double x = double(i);
            const double sinc = i == 0 ? 2 * cutoff : std::sin(2 * M_PI * cutoff * x) / (M_PI * x);
            const double n = double(i + half) / (2 * half);
            const double window = 0.35875 - 0.48829 * std::cos(2 * M_PI * n) + 0.14128 * std::cos(4 * M_PI * n)
                - 0.01168 * std::cos(6 * M_PI * n); // Blackman-Harris
            taps[std::size_t(i + half)] = sinc * window;
            sum += taps[std::size_t(i + half)];
        }
        for (auto& t : taps) t /= sum;
    }
    const auto half = taps.empty() ? 0 : (taps.size() - 1) / 2;
    std::vector<double> history(taps.empty() ? 1 : taps.size(), 0.0);
    std::size_t head = 0;
    double phase = 0, triangle = 0;
    const FramePos total = spec.frames * over + FramePos(half); // run on to flush the filter delay
    FramePos produced = 0;
    for (FramePos step = 0; step < total; ++step) {
        const FramePos f = std::min<FramePos>(step / over, spec.frames - 1);
        // Logarithmic sweep: the instantaneous frequency moves evenly in octaves.
        const double frequency = spec.endFrequency
            ? spec.frequency * std::pow(ratio, double(f) / std::max<FramePos>(1, spec.frames - 1)) : spec.frequency;
        const double increment = frequency / (double(spec.sampleRate) * over);
        double value = 0;
        if (step < spec.frames * over) switch (spec.shape) {
        case ToneShape::Sine: value = std::sin(2 * M_PI * phase); break;
        case ToneShape::Sawtooth: value = 2 * phase - 1 - blep(phase, increment); break;
        case ToneShape::Square:
        case ToneShape::Triangle: {
            double square = phase < 0.5 ? 1 : -1;
            square += blep(phase, increment);
            square -= blep(std::fmod(phase + 0.5, 1.0), increment);
            if (spec.shape == ToneShape::Square) value = square;
            else {
                // Leaky integration of the band-limited square gives a triangle.
                triangle = increment * square * 4 + (1 - 0.0005 / over) * triangle;
                value = triangle;
            }
            break;
        }
        }
        phase += increment;
        if (phase >= 1) phase -= std::floor(phase);
        double sample = value;
        if (over > 1) {
            history[head] = value;
            head = (head + 1) % history.size();
            if (step < FramePos(half) || (step - FramePos(half)) % over) continue;
            sample = 0;
            for (std::size_t t = 0; t < taps.size(); ++t)
                sample += taps[t] * history[(head + t) % history.size()];
        }
        if (produced >= spec.frames) break;
        const float scaled = float(sample * amplitude);
        for (std::uint32_t c = 0; c < spec.channels; ++c) out.plane(c)[std::size_t(produced)] = scaled;
        ++produced;
    }
    return out;
}

AudioBuffer generateNoise(const NoiseSpec& spec, std::string& error)
{
    error.clear();
    if (!validCommon(spec.frames, spec.channels, spec.amplitudeDb, error)) return {};
    AudioBuffer out(spec.channels, std::size_t(spec.frames));
    const double target = std::pow(10.0, spec.amplitudeDb / 20.0);
    for (std::uint32_t c = 0; c < spec.channels; ++c) {
        std::mt19937_64 random(spec.seed * 0x9E3779B97F4A7C15ULL + c);
        std::normal_distribution<double> gauss(0.0, 1.0);
        double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0, brown = 0;
        auto plane = out.plane(c);
        long double squares = 0;
        for (FramePos f = 0; f < spec.frames; ++f) {
            const double white = gauss(random);
            double value = white;
            if (spec.colour == NoiseColour::Pink) {
                // Paul Kellet's refined pink filter (-3 dB/octave).
                b0 = 0.99886 * b0 + white * 0.0555179; b1 = 0.99332 * b1 + white * 0.0750759;
                b2 = 0.96900 * b2 + white * 0.1538520; b3 = 0.86650 * b3 + white * 0.3104856;
                b4 = 0.55000 * b4 + white * 0.5329522; b5 = -0.7616 * b5 - white * 0.0168980;
                value = b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362;
                b6 = white * 0.115926;
            } else if (spec.colour == NoiseColour::Brown) {
                brown = (brown + 0.02 * white) / 1.02; // -6 dB/octave, leaky to stay bounded
                value = brown;
            }
            plane[std::size_t(f)] = float(value);
            squares += value * value;
        }
        const double rms = std::sqrt(double(squares / spec.frames));
        const double gain = rms > 0 ? target / rms : 0;
        for (auto& sample : plane) sample = float(std::clamp(sample * gain, -1.0, 1.0));
    }
    return out;
}

} // namespace omawave::session
