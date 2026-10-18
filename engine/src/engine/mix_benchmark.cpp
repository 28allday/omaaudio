#include "mix_benchmark.h"

#include "render_plan.h"
#include "transport.h"
#include "session/project.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

namespace omawave::engine {

MixTiming measureMixing(const MixScenario& s, const std::atomic<bool>* cancel)
{
    MixTiming out;
    if (s.tracks == 0 || s.tracks > Transport::kMaxTracks || s.block == 0 || s.block > 8192
        || s.rate < 8000 || s.rate > 192000 || s.blocks < 10 || (s.bus && s.tracks < 2)) {
        out.error = "invalid benchmark scenario";
        return out;
    }
    try {
        session::Project project(s.rate);
        session::AudioBuffer audio(2, s.rate);
        for (unsigned c = 0; c < 2; ++c)
            for (unsigned f = 0; f < s.rate; ++f)
                audio.plane(c)[f] = .01f * std::sin(float(f) * (.031f + float(c) * .007f));
        const auto source = project.addSource("generated", "", std::move(audio));
        for (unsigned t = 0; t < s.tracks; ++t) {
            const auto id = project.addTrack("track");
            auto& track = *project.track(id);
            if (!s.bus || t + 1 < s.tracks) {
                session::Clip clip;
                clip.source = source; clip.length = s.rate; clip.fadeIn = 137; clip.fadeOut = 211;
                track.clips.push_back(clip);
            }
            track.gain = .5f; track.pan = float(t % 7) / 4 - .75f;
            track.volumeAutomation.enabled = s.points > 0;
            track.panAutomation.enabled = s.points > 0;
            for (unsigned p = 0; p < s.points; ++p) {
                const auto at = session::FramePos(std::uint64_t(p) * s.rate / s.points);
                session::AutomationPoint volume, pan;
                volume.frame = pan.frame = at;
                volume.value = .5 + double((p + t) % 11) / 20;
                pan.value = double((p + t) % 17) / 8 - 1;
                track.volumeAutomation.points.push_back(volume);
                track.panAutomation.points.push_back(pan);
            }
        }
        if (s.bus)
            for (unsigned t = 0; t + 1 < s.tracks; ++t)
                project.tracks()[t].routing.output = project.tracks().back().id;
        auto plan = buildRenderPlan(project, 2);
        Transport::Queue queue;
        MeterChannel meters;
        Transport transport(queue, meters, s.rate);
        if (!queue.push(Command::setPlan(&plan, 1)) || !queue.push(Command::setLoop(true, 0, s.rate))
            || !queue.push(Command::play())) { out.error = "benchmark command queue full"; return out; }
        std::vector<float> left(s.block), right(s.block);
        float* output[]{left.data(), right.data()};
        for (unsigned i = 0; i < 50; ++i) transport.process(output, 2, s.block);
        std::vector<double> times;
        times.reserve(s.blocks);
        for (unsigned i = 0; i < s.blocks; ++i) {
            if ((i & 63) == 0 && cancel && cancel->load(std::memory_order_relaxed)) { out.error = "cancelled"; return out; }
            const auto begin = std::chrono::steady_clock::now();
            transport.process(output, 2, s.block);
            const auto end = std::chrono::steady_clock::now();
            times.push_back(std::chrono::duration<double, std::micro>(end - begin).count());
        }
        if (!transport.playing()) { out.error = "the benchmark session stopped"; return out; }
        std::sort(times.begin(), times.end());
        out.p50Us = times[(times.size() - 1) / 2];
        out.p99Us = times[(times.size() - 1) * 99 / 100];
        out.maxUs = times.back();
        out.ok = true;
    } catch (const std::exception& error) {
        out.error = error.what();
    }
    return out;
}

ComputerCheck checkComputer(unsigned rate, unsigned block, double share, const std::atomic<bool>* cancel)
{
    ComputerCheck out;
    out.rate = std::clamp(rate, 8000u, 192000u);
    out.block = std::clamp(block, 16u, 8192u);
    out.share = std::clamp(share, 0.05, 1.0);
    out.deadlineUs = double(out.block) * 1e6 / out.rate;
    // Each kind measured at a small and a larger count: the difference is the cost
    // of one more track, independent of the fixed per-block overhead.
    constexpr unsigned small = 8, large = 64, points = 200;
    const auto perTrack = [&](unsigned pts, bool bus, double& fixed) -> double {
        const auto a = measureMixing({small, out.block, pts, out.rate, 600, bus}, cancel);
        const auto b = measureMixing({large, out.block, pts, out.rate, 600, bus}, cancel);
        if (!a.ok || !b.ok) { out.error = !a.ok ? a.error : b.error; return -1; }
        const double each = std::max(0.0, (b.p99Us - a.p99Us) / double(large - small));
        fixed = std::max(0.0, a.p99Us - each * small);
        return each;
    };
    const auto fits = [&](double each, double fixed) -> unsigned {
        const double budget = out.share * out.deadlineUs - fixed;
        if (budget <= 0) return 0;
        if (each <= 0) return 9999;
        return unsigned(std::min(9999.0, std::floor(budget / each)));
    };
    double fixed = 0;
    out.plainUs = perTrack(0, false, fixed);
    if (out.plainUs < 0) { out.cancelled = out.error == "cancelled"; return out; }
    out.plainTracks = fits(out.plainUs, fixed);
    out.automatedUs = perTrack(points, false, fixed);
    if (out.automatedUs < 0) { out.cancelled = out.error == "cancelled"; return out; }
    out.automatedTracks = fits(out.automatedUs, fixed);
    out.busUs = perTrack(points, true, fixed);
    if (out.busUs < 0) { out.cancelled = out.error == "cancelled"; return out; }
    out.busTracks = fits(out.busUs, fixed);
    out.ok = true;
    return out;
}

} // namespace omawave::engine
