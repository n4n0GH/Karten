// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "router.h"

#include <algorithm>
#include <limits>
#include <queue>

namespace
{
constexpr double CarSpeedFactor = 0.9; ///< share of the speed limit that is driven on average
constexpr double BikeSpeed = 18.0 / 3.6; ///< m/s
constexpr double FootSpeed = 5.0 / 3.6;
constexpr double MinBikeWeight = 0.85;

struct Cost {
    float seconds; ///< real travel time
    float weight; ///< what the search minimises: time scaled by how pleasant the road is
};

Cost edgeCost(const WayInfo &way, Mode mode, float length)
{
    double speed = 1;
    double factor = 1;
    switch (mode) {
    case Car:
        speed = std::max<double>(way.speed, 5) * CarSpeedFactor / 3.6;
        if (way.cls == HighwayClass::Service || way.cls == HighwayClass::LivingStreet) {
            factor = 1.3; // keep through traffic off access roads
        }
        break;
    case Bike:
        speed = BikeSpeed;
        switch (way.cls) {
        case HighwayClass::Trunk:
        case HighwayClass::TrunkLink:
            factor = 2.5;
            break;
        case HighwayClass::Primary:
        case HighwayClass::PrimaryLink:
            factor = 1.8;
            break;
        case HighwayClass::Secondary:
        case HighwayClass::SecondaryLink:
            factor = 1.4;
            break;
        case HighwayClass::Tertiary:
        case HighwayClass::TertiaryLink:
        case HighwayClass::Unclassified:
            factor = 1.1;
            break;
        case HighwayClass::Pedestrian:
        case HighwayClass::Footway:
            speed = BikeSpeed * 0.5;
            break;
        case HighwayClass::Path:
        case HighwayClass::Track:
            speed = BikeSpeed * 0.8;
            break;
        default:
            break;
        }
        if (way.flags & WayInfo::Unpaved) {
            speed *= 0.7;
        }
        if (way.flags & WayInfo::CycleFriendly) {
            factor = std::min(factor, MinBikeWeight);
        }
        break;
    case Foot:
        speed = FootSpeed;
        switch (way.cls) {
        case HighwayClass::Trunk:
        case HighwayClass::TrunkLink:
            factor = 1.6;
            break;
        case HighwayClass::Primary:
        case HighwayClass::PrimaryLink:
            factor = 1.3;
            break;
        case HighwayClass::Secondary:
        case HighwayClass::SecondaryLink:
            factor = 1.2;
            break;
        case HighwayClass::Tertiary:
        case HighwayClass::TertiaryLink:
        case HighwayClass::Unclassified:
            factor = 1.1;
            break;
        case HighwayClass::Steps:
            speed = FootSpeed * 0.6;
            break;
        default:
            break;
        }
        break;
    }
    const double seconds = length / speed;
    return {float(seconds), float(seconds * factor)};
}

/// Lower bound of the weight per metre for any edge, which keeps A* optimal.
double minWeightPerMetre(Mode mode)
{
    switch (mode) {
    case Car:
        return 3.6 / (130.0 * CarSpeedFactor);
    case Bike:
        return MinBikeWeight / BikeSpeed;
    case Foot:
        return 1.0 / FootSpeed;
    }
    return 0;
}
}

Maneuver::Type Maneuver::turnType(double angle)
{
    const double a = std::abs(angle);
    if (a < 20) {
        return Continue;
    }
    if (a < 55) {
        return angle > 0 ? SlightRight : SlightLeft;
    }
    if (a < 130) {
        return angle > 0 ? Right : Left;
    }
    if (a < 170) {
        return angle > 0 ? SharpRight : SharpLeft;
    }
    return UTurn;
}

Router::Router(const RoadGraph &graph)
    : m_graph(graph)
{
}

Route Router::route(GeoPoint from, GeoPoint to, Mode mode, const std::atomic_bool *cancel) const
{
    const RoadGraph &g = m_graph;
    const qint64 startNode = g.nearestNode(from, mode, SnapRadius);
    const qint64 endNode = g.nearestNode(to, mode, SnapRadius);
    if (startNode < 0 || endNode < 0) {
        return {};
    }
    const quint32 start = quint32(startNode);
    const quint32 goal = quint32(endNode);

    constexpr quint32 NoEdge = std::numeric_limits<quint32>::max();
    const quint32 n = g.nodeCount();
    std::vector<float> best(n, std::numeric_limits<float>::max());
    std::vector<quint32> viaEdge(n, NoEdge);
    std::vector<bool> closed(n, false);

    // Equirectangular distance to the goal: cheap, and never longer than the
    // real distance once scaled down slightly.
    const GeoPoint goalPoint = g.point(goal);
    const double metresPerLat = Geo::EarthRadius * Geo::DegToRad;
    const double metresPerLon = metresPerLat * std::cos(goalPoint.lat * Geo::DegToRad);
    const double bound = minWeightPerMetre(mode) * 0.995;
    const auto heuristic = [&](quint32 node) {
        const double dy = (g.lat[node] * 1e-7 - goalPoint.lat) * metresPerLat;
        const double dx = (g.lon[node] * 1e-7 - goalPoint.lon) * metresPerLon;
        return float(std::sqrt(dx * dx + dy * dy) * bound);
    };

    using Entry = std::pair<float, quint32>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;
    best[start] = 0;
    queue.emplace(heuristic(start), start);
    quint32 settled = 0;
    bool found = false;

    while (!queue.empty()) {
        const quint32 node = queue.top().second;
        queue.pop();
        if (closed[node]) {
            continue;
        }
        closed[node] = true;
        if (node == goal) {
            found = true;
            break;
        }
        if (cancel && (++settled & 0xfff) == 0 && cancel->load(std::memory_order_relaxed)) {
            return {};
        }
        for (quint32 e = g.first[node]; e < g.first[node + 1]; ++e) {
            const Edge &edge = g.edges[e];
            if (!(edge.access() & mode) || closed[edge.target]) {
                continue;
            }
            const float candidate = best[node] + edgeCost(g.ways[edge.way()], mode, edge.length).weight;
            if (candidate < best[edge.target]) {
                best[edge.target] = candidate;
                viaEdge[edge.target] = e;
                queue.emplace(candidate + heuristic(edge.target), edge.target);
            }
        }
    }
    if (!found) {
        return {};
    }

    std::vector<quint32> edgePath;
    for (quint32 node = goal; node != start;) {
        const quint32 e = viaEdge[node];
        edgePath.push_back(e);
        // The source of an edge is the node whose edge range contains it.
        node = quint32(std::upper_bound(g.first.begin(), g.first.end(), e) - g.first.begin()) - 1;
    }
    std::reverse(edgePath.begin(), edgePath.end());
    return buildRoute(edgePath, start, mode);
}

Route Router::buildRoute(const std::vector<quint32> &edgePath, quint32 start, Mode mode) const
{
    const RoadGraph &g = m_graph;
    Route route;
    route.path.reserve(edgePath.size() + 1);
    route.path.push_back(g.point(start));

    Maneuver current;
    current.type = Maneuver::Depart;
    if (!edgePath.empty()) {
        const Edge &firstEdge = g.edges[edgePath.front()];
        current.street = g.wayName(firstEdge.way());
        current.bearing = Geo::bearing(g.point(start), g.point(firstEdge.target));
    }

    bool inRoundabout = false;
    int exitsPassed = 0;
    quint32 node = start; // the node the current edge starts from
    for (size_t i = 0; i < edgePath.size(); ++i) {
        const Edge &edge = g.edges[edgePath[i]];
        const WayInfo &way = g.ways[edge.way()];
        const bool roundabout = way.flags & WayInfo::Roundabout;

        if (i > 0) {
            const Edge &previous = g.edges[edgePath[i - 1]];
            const WayInfo &previousWay = g.ways[previous.way()];
            const GeoPoint here = g.point(node);
            const double bearingOut = Geo::bearing(here, g.point(edge.target));

            Maneuver next;
            bool emitNext = false;
            if (roundabout && !inRoundabout) {
                inRoundabout = true;
                exitsPassed = 0;
                next.type = Maneuver::Roundabout;
                emitNext = true;
            } else if (roundabout) {
                // Still circulating: count the exits we drive past.
                for (quint32 e = g.first[node]; e < g.first[node + 1]; ++e) {
                    const Edge &other = g.edges[e];
                    if ((other.access() & mode) && !(g.ways[other.way()].flags & WayInfo::Roundabout)) {
                        ++exitsPassed;
                    }
                }
            } else if (inRoundabout) {
                // Leaving: complete the roundabout manoeuvre we are still in.
                inRoundabout = false;
                current.exit = exitsPassed + 1;
                current.street = g.wayName(edge.way());
                current.bearing = bearingOut;
            } else {
                const quint32 from = quint32(std::upper_bound(g.first.begin(), g.first.end(), edgePath[i - 1]) - g.first.begin()) - 1;
                const double angle = Geo::turnAngle(Geo::bearing(g.point(from), here), bearingOut);
                int branches = 0;
                for (quint32 e = g.first[node]; e < g.first[node + 1]; ++e) {
                    branches += g.edges[e].touches(mode) ? 1 : 0;
                }
                const bool junction = branches > 2;
                const bool renamed = way.name != previousWay.name;
                const Maneuver::Type type = Maneuver::turnType(angle);
                if ((junction && type != Maneuver::Continue) || (renamed && std::abs(angle) > 35) || (renamed && junction && way.name != 0)) {
                    next.type = type;
                    emitNext = true;
                }
            }
            if (emitNext) {
                route.maneuvers.push_back(current);
                current = next;
                current.street = g.wayName(edge.way());
                current.bearing = bearingOut;
                current.pathIndex = quint32(route.path.size() - 1);
            }
        }

        const Cost cost = edgeCost(way, mode, edge.length);
        current.distance += edge.length;
        current.duration += cost.seconds;
        route.distance += edge.length;
        route.duration += cost.seconds;
        route.path.push_back(g.point(edge.target));
        node = edge.target;
    }
    if (inRoundabout) {
        current.exit = exitsPassed + 1; // the destination is on the roundabout
    }
    route.maneuvers.push_back(current);

    Maneuver arrive;
    arrive.type = Maneuver::Arrive;
    arrive.pathIndex = quint32(route.path.size() - 1);
    route.maneuvers.push_back(arrive);
    return route;
}
