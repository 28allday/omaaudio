#pragma once
#include "project.h"
#include <cstddef>
#include <string>
#include <vector>

namespace omawave::session {
struct RouteEdge {
    std::size_t from = 0, to = 0; // to == track count means master
    float gain = 1;
    bool preFader = false, sidechain = false;
};
struct RoutingGraph {
    bool ok = false, active = false;
    std::string error;
    std::vector<std::size_t> order;
    std::vector<RouteEdge> edges;
};
// Main/worker thread only. Never repairs an invalid graph silently.
[[nodiscard]] RoutingGraph compileRouting(const Project& project);
// Part of the caller's track-removal undo transaction.
void removeRoutingTarget(Project& project, TrackId removed);
} // namespace omawave::session
