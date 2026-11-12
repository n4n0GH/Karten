// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "roadgraph.h"

#include <atomic>

struct Maneuver {
    enum Type {
        Depart,
        Continue,
        SlightRight,
        Right,
        SharpRight,
        UTurn,
        SharpLeft,
        Left,
        SlightLeft,
        Roundabout,
        Arrive,
    };
    Type type = Continue;
    int exit = 0; ///< roundabout exit number, counted from 1
    QString street; ///< the road this manoeuvre leads onto, may be empty
    double bearing = 0; ///< direction of travel after the manoeuvre, degrees from north
    double distance = 0; ///< metres until the next manoeuvre
    double duration = 0; ///< seconds until the next manoeuvre
    quint32 pathIndex = 0; ///< where on Route::path the manoeuvre happens

    /// Classifies a change of direction in degrees, positive to the right.
    static Type turnType(double angle);
};

struct Route {
    std::vector<GeoPoint> path;
    std::vector<Maneuver> maneuvers;
    double distance = 0; ///< metres
    double duration = 0; ///< seconds

    bool isValid() const
    {
        return !path.empty();
    }
};

/// Shortest-time routing on a RoadGraph using A*.
class Router
{
public:
    explicit Router(const RoadGraph &graph);

    /// How far a start or destination may be from the nearest usable road.
    static constexpr double SnapRadius = 1000.0;

    /// Returns an invalid route when an end point is not near a road, when
    /// there is no connection, or when @p cancel becomes true.
    Route route(GeoPoint from, GeoPoint to, Mode mode, const std::atomic_bool *cancel = nullptr) const;

private:
    Route buildRoute(const std::vector<quint32> &edgePath, quint32 start, Mode mode) const;
    const RoadGraph &m_graph;
};
