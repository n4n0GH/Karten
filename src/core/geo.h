// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cmath>
#include <numbers>

/// A WGS84 position in degrees.
struct GeoPoint {
    double lat = 0.0;
    double lon = 0.0;
};

namespace Geo
{
constexpr double EarthRadius = 6371008.8; // metres
constexpr double DegToRad = std::numbers::pi / 180.0;

/// Great-circle distance in metres.
inline double distance(GeoPoint a, GeoPoint b)
{
    const double dLat = (b.lat - a.lat) * DegToRad;
    const double dLon = (b.lon - a.lon) * DegToRad;
    const double s1 = std::sin(dLat / 2);
    const double s2 = std::sin(dLon / 2);
    const double h = s1 * s1 + std::cos(a.lat * DegToRad) * std::cos(b.lat * DegToRad) * s2 * s2;
    return 2 * EarthRadius * std::asin(std::sqrt(h));
}

/// Initial bearing from @p a to @p b in degrees, clockwise from north, in [0, 360).
inline double bearing(GeoPoint a, GeoPoint b)
{
    const double lat1 = a.lat * DegToRad;
    const double lat2 = b.lat * DegToRad;
    const double dLon = (b.lon - a.lon) * DegToRad;
    const double y = std::sin(dLon) * std::cos(lat2);
    const double x = std::cos(lat1) * std::sin(lat2) - std::sin(lat1) * std::cos(lat2) * std::cos(dLon);
    const double deg = std::atan2(y, x) / DegToRad;
    return deg < 0 ? deg + 360.0 : deg;
}

/// Signed difference @p to - @p from, normalised to (-180, 180]. Positive is clockwise (a right turn).
inline double turnAngle(double from, double to)
{
    double d = std::fmod(to - from, 360.0);
    if (d > 180.0) {
        d -= 360.0;
    } else if (d <= -180.0) {
        d += 360.0;
    }
    return d;
}
}
