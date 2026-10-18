#include "elastic_processor.h"
#include "signalsmith-stretch.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace omawave::engine {
using session::FramePos;
struct ElasticProcessor::Impl {
    static constexpr int block = 256;
    const session::AudioBuffer& source;
    session::ElasticSpec spec;
    signalsmith::stretch::SignalsmithStretch<float> dsp{0};
    std::array<std::vector<float>,2> input;
    std::array<std::array<float,block>,2> buffer{};
    std::array<float*,2> in{}, out{};
    FramePos origin = -1, next = -1, buffered = -1;
    int lead = 0;
    double speed;
    Impl(const session::AudioBuffer& audio, session::ElasticSpec mapping, std::uint32_t rate)
        : source(audio), spec(mapping), speed(double(spec.inputFrames)/spec.outputFrames) {
        if (!spec.valid() || !audio.channels() || audio.channels() > 2 || rate < 8000 || rate > 192000
            || std::uint64_t(spec.inputStart) > audio.frames()
            || std::uint64_t(spec.inputFrames) > audio.frames()-std::size_t(spec.inputStart))
            throw std::runtime_error("invalid elastic processor input");
        int window = 1024;
        while (window < double(rate)*8192/48000) window *= 2;
        dsp.configure(int(audio.channels()),window,window/8,true);
        dsp.setTransposeSemitones(spec.semitones);
        lead = dsp.outputSeekLength(float(speed));
        for (unsigned c = 0; c < audio.channels(); ++c) {
            input[c].resize(std::max(lead,block*2+2));
            in[c] = input[c].data(); out[c] = buffer[c].data();
        }
    }
    FramePos mapped(FramePos output) const {
        const long double value = std::round(static_cast<long double>(output)*spec.inputFrames/spec.outputFrames);
        if (value < 0 || value > std::numeric_limits<FramePos>::max()-spec.inputStart-lead-block*2)
            throw std::runtime_error("elastic source position overflow");
        return FramePos(value);
    }
    void fill(FramePos at, int count) {
        for (unsigned c = 0; c < source.channels(); ++c) {
            const auto plane = source.plane(c);
            for (int f = 0; f < count; ++f) {
                const auto position = at+f;
                const float value = position >= 0 && position < spec.inputFrames
                    ? plane[std::size_t(spec.inputStart+position)] : 0;
                if (!std::isfinite(value)) throw std::runtime_error("elastic input contains non-finite samples");
                in[c][f] = value;
            }
        }
    }
    void seek(FramePos at) {
        if (source.diskStorage()) source.diskStorage()->releaseMappedPages();
        origin = next = at/2048*2048; buffered = -1;
        fill(mapped(origin),lead);
        dsp.outputSeek(in.data(),lead);
    }
    void generate() {
        if (next > std::numeric_limits<FramePos>::max()-block)
            throw std::runtime_error("elastic output position overflow");
        const auto first = mapped(next), end = mapped(next+block);
        fill(first+lead,int(end-first));
        dsp.process(in.data(),int(end-first),out.data(),block);
        for (unsigned c = 0; c < source.channels(); ++c)
            for (float v : buffer[c]) if (!std::isfinite(v))
                throw std::runtime_error("elastic processor produced non-finite samples");
        buffered = next; next += block;
        if (source.diskStorage() && next%65536 == 0) source.diskStorage()->releaseMappedPages();
    }
};
ElasticProcessor::ElasticProcessor(const session::AudioBuffer& input, session::ElasticSpec spec, std::uint32_t rate)
    : impl_(std::make_unique<Impl>(input,spec,rate)) {}
ElasticProcessor::~ElasticProcessor() = default;
void ElasticProcessor::render(FramePos start, std::uint32_t frames, float* const* output,
                              const std::atomic<bool>* cancel) {
    auto& p = *impl_;
    if (start < 0 || start > p.spec.outputFrames || frames > std::uint64_t(p.spec.outputFrames-start))
        throw std::runtime_error("elastic output range is outside the source");
    for (std::uint32_t done = 0; done < frames;) {
        if (cancel && cancel->load(std::memory_order_relaxed)) throw std::runtime_error("elastic processing cancelled");
        const auto at = start+done;
        if (p.origin < 0 || at < p.buffered || at > p.next) p.seek(at);
        while (p.buffered < 0 || at >= p.next) p.generate();
        const auto count = std::min<std::uint32_t>(frames-done,std::uint32_t(p.next-at));
        for (unsigned c = 0; c < p.source.channels(); ++c)
            std::copy_n(p.out[c]+(at-p.buffered),count,output[c]+done);
        done += count;
    }
}
} // namespace omawave::engine
