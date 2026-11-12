// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "geo.h"

#include <QByteArray>
#include <QString>

#include <vector>

/// Travel modes, usable as bit flags.
enum Mode : quint8 {
    Car = 1,
    Bike = 2,
    Foot = 4,
};

enum class HighwayClass : quint8 {
    Motorway,
    MotorwayLink,
    Trunk,
    TrunkLink,
    Primary,
    PrimaryLink,
    Secondary,
    SecondaryLink,
    Tertiary,
    TertiaryLink,
    Unclassified,
    Residential,
    LivingStreet,
    Service,
    Track,
    Pedestrian,
    Footway,
    Cycleway,
    Path,
    Steps,
};

struct WayInfo {
    enum Flag : quint8 {
        Roundabout = 1,
        CycleFriendly = 2,
        Unpaved = 4,
    };
    quint32 name = 0; ///< index into the name table, 0 is the empty name
    HighwayClass cls = HighwayClass::Unclassified;
    quint8 speed = 0; ///< km/h for motor traffic
    quint8 flags = 0;
    quint8 reserved = 0;
};

/// A directed edge. Every road segment is stored once per direction, even when
/// a direction is closed to all modes, so that junction shapes stay intact.
struct Edge {
    quint32 target = 0;
    quint32 wayAccess = 0; ///< way index << 6 | reverse access << 3 | access
    float length = 0; ///< metres

    quint32 way() const
    {
        return wayAccess >> 6;
    }
    quint8 access() const
    {
        return wayAccess & 7;
    }
    quint8 reverseAccess() const
    {
        return (wayAccess >> 3) & 7;
    }
    bool touches(quint8 mode) const
    {
        return ((wayAccess | (wayAccess >> 3)) & mode) != 0;
    }
};

/// A routable road network in compressed sparse row form. Every OSM node on a
/// road is a graph node.
class RoadGraph
{
public:
    std::vector<qint32> lat; ///< 1e-7 degrees
    std::vector<qint32> lon;
    std::vector<quint32> first; ///< edges of node n are [first[n], first[n + 1])
    std::vector<Edge> edges;
    std::vector<WayInfo> ways;
    std::vector<quint32> nameOffsets; ///< nameOffsets[i]..nameOffsets[i + 1] in nameData
    QByteArray nameData; ///< UTF-8

    double minLat = 0;
    double minLon = 0;
    double maxLat = 0;
    double maxLon = 0;

    quint32 nodeCount() const
    {
        return quint32(lat.size());
    }
    GeoPoint point(quint32 node) const
    {
        return {lat[node] * 1e-7, lon[node] * 1e-7};
    }
    QString wayName(quint32 way) const;

    bool save(const QString &path, QString *error = nullptr) const;
    bool load(const QString &path, QString *error = nullptr);

    /// Computes the bounding box and the spatial index. Called by load().
    void buildIndex();

    /// Nearest node that has a segment usable by @p mode, or -1 when there is
    /// none within @p maxMetres.
    qint64 nearestNode(GeoPoint p, quint8 mode, double maxMetres) const;

private:
    double m_cell = 0.005;
    int m_cols = 0;
    int m_rows = 0;
    std::vector<quint32> m_cellFirst;
    std::vector<quint32> m_cellNodes;
};
