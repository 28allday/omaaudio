#include "routing.h"
#include <algorithm>
#include <cmath>

namespace omawave::session {
RoutingGraph compileRouting(const Project& project)
{
    RoutingGraph result;
    const auto& tracks = project.tracks(); const auto count = tracks.size();
    std::vector<std::size_t> incoming(count, 0);
    const auto append = [&](std::size_t from, TrackId target, float gain, bool pre, bool key) {
        const auto to = project.trackIndex(target);
        if (!to || *to == from) return false;
        result.edges.push_back({from, *to, gain, pre, key}); ++incoming[*to]; return true;
    };
    for (std::size_t i = 0; i < count; ++i) {
        if (tracks[i].id == kInvalidTrack || tracks[i].id == kNoTrackOutput
            || std::any_of(tracks.begin(), tracks.begin() + static_cast<std::ptrdiff_t>(i),
                [&](const auto& track) { return track.id == tracks[i].id; })) {
            result.error = "routing requires distinct valid track ids"; return result;
        }
        const auto& routing = tracks[i].routing;
        result.active = result.active || !routing.direct();
        if (routing.output == kInvalidTrack) result.edges.push_back({i, count, 1, false, false});
        else if (routing.output != kNoTrackOutput && !append(i, routing.output, 1, false, false)) {
            result.error = "track output refers to itself or a missing track"; return result;
        }
        if (routing.sends.size() > kMaxTrackSends) { result.error = "a track may have at most eight sends"; return result; }
        std::vector<TrackId> sent;
        for (const auto& send : routing.sends) {
            if (!std::isfinite(send.gain) || send.gain < 0 || send.gain > 4
                || std::find(sent.begin(), sent.end(), send.target) != sent.end()
                || !append(i, send.target, send.gain, send.preFader, false)) {
                result.error = "send requires a distinct destination and a finite gain between zero and four"; return result;
            }
            sent.push_back(send.target);
        }
        if (routing.sidechain != kInvalidTrack) {
            const auto source = project.trackIndex(routing.sidechain);
            if (!source || !append(*source, tracks[i].id, 1, false, true)) {
                result.error = "sidechain refers to itself or a missing track"; return result;
            }
        }
    }
    for (std::size_t i = 0; i < count; ++i) if (incoming[i] == 0) result.order.push_back(i);
    for (std::size_t cursor = 0; cursor < result.order.size(); ++cursor)
        for (const auto& edge : result.edges)
            if (edge.from == result.order[cursor] && edge.to < count && --incoming[edge.to] == 0)
                result.order.push_back(edge.to);
    if (result.order.size() != count) { result.error = "routing would create a feedback cycle"; return result; }
    result.ok = true; return result;
}

void removeRoutingTarget(Project& project, TrackId removed)
{
    for (auto& track : project.tracks()) {
        if (track.routing.output == removed) track.routing.output = kInvalidTrack;
        if (track.routing.sidechain == removed) track.routing.sidechain = kInvalidTrack;
        std::erase_if(track.routing.sends, [removed](const auto& send) { return send.target == removed; });
    }
}
} // namespace omawave::session
