#include "pauses.h"

#include <algorithm>
#include <cmath>

namespace omawave::render {

namespace {
constexpr float kSilentDb = -120.0f;
// A breath: short, and well below speech, though above the pause level.
constexpr double kBreathSeconds = 0.6;
constexpr double kBreathAboveDb = 12.0;
}

void windowLevels(const float* const* planes, std::uint32_t channels, std::size_t frames,
                  std::uint32_t window, std::vector<float>& out)
{
    for (std::size_t at = 0; at < frames; at += window) {
        const auto n = std::min<std::size_t>(window, frames - at);
        double loudest = 0;
        for (std::uint32_t c = 0; c < channels; ++c) {
            if (!planes[c]) continue;
            double sum = 0;
            for (std::size_t i = 0; i < n; ++i) sum += double(planes[c][at + i]) * planes[c][at + i];
            loudest = std::max(loudest, sum / double(n));
        }
        out.push_back(loudest > 0 ? std::max(kSilentDb, float(10 * std::log10(loudest))) : kSilentDb);
    }
}

PauseScan findPauses(const std::vector<float>& levelsDb, session::FramePos start, session::FramePos end,
                     std::uint32_t window, double rate, const PauseSettings& settings)
{
    PauseScan scan;
    if (levelsDb.empty() || window == 0 || rate <= 0 || end <= start) return scan;

    // The background noise: the level of the quietest tenth of what is not digital
    // silence (a gap between clips says nothing about the room).
    std::vector<float> heard;
    for (const float level : levelsDb) if (level > kSilentDb + 20) heard.push_back(level);
    if (heard.size() >= std::max<std::size_t>(10, levelsDb.size() / 20)) {
        const auto tenth = heard.begin() + std::ptrdiff_t(heard.size() / 10);
        std::nth_element(heard.begin(), tenth, heard.end());
        scan.noiseFloorDb = *tenth;
    } else {
        scan.noiseFloorDb = -100;
    }
    scan.thresholdDb = settings.autoThreshold ? std::clamp(scan.noiseFloorDb + 10.0, -70.0, -20.0)
                                              : std::clamp(settings.thresholdDb, -90.0, 0.0);

    const auto count = levelsDb.size();
    std::vector<bool> quiet(count);
    for (std::size_t i = 0; i < count; ++i) quiet[i] = levelsDb[i] < scan.thresholdDb;

    // Without Keep breaths, a breath between two quiet stretches is part of the gap.
    if (!settings.keepBreaths) {
        const auto longest = std::size_t(std::ceil(kBreathSeconds * rate / window));
        for (std::size_t i = 0; i < count;) {
            if (quiet[i]) { ++i; continue; }
            std::size_t j = i;
            float loudest = kSilentDb;
            while (j < count && !quiet[j]) loudest = std::max(loudest, levelsDb[j++]);
            const bool between = i > 0 && j < count;
            if (between && j - i <= longest && loudest < scan.thresholdDb + kBreathAboveDb)
                std::fill(quiet.begin() + std::ptrdiff_t(i), quiet.begin() + std::ptrdiff_t(j), true);
            i = j;
        }
    }

    const auto shortest = session::FramePos(std::llround(settings.minSeconds * rate));
    for (std::size_t i = 0; i < count;) {
        if (!quiet[i]) { ++i; continue; }
        std::size_t j = i;
        while (j < count && quiet[j]) ++j;
        const auto from = start + session::FramePos(i) * window;
        const auto to = std::min(end, start + session::FramePos(j) * window);
        if (to - from >= shortest) scan.pauses.emplace_back(from, to);
        i = j;
    }
    return scan;
}

} // namespace omawave::render
