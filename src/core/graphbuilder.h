// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pbfreader.h"
#include "roadgraph.h"

#include <QHash>

/// A named thing that can be found by the offline search.
struct Place {
    enum Kind : quint8 {
        City,
        Town,
        Village,
        Suburb,
        Locality,
        Street,
        Poi,
    };
    QString name;
    QString type; ///< raw OSM value for points of interest, e.g. "cafe"
    /// What else is worth showing about a point of interest, such as opening
    /// hours: a JSON object of OSM tags, or empty.
    QByteArray tags;
    Kind kind = Poi;
    double lat = 0;
    double lon = 0;
};

/// Turns OSM data into a RoadGraph and a list of searchable places.
///
/// Feed all ways first, call prepareNodes(), feed all nodes, then finish().
/// With a PBF file that means two passes, because files list nodes before ways.
class GraphBuilder : public Osm::Handler
{
public:
    void way(qint64 id, const std::vector<qint64> &refs, const Osm::Tags &tags) override;
    void prepareNodes();
    void node(qint64 id, double lat, double lon, const Osm::Tags &tags) override;
    void finish(RoadGraph *graph, std::vector<Place> *places);

private:
    struct RawWay {
        size_t firstRef;
        quint32 refCount;
        quint8 forward; ///< mode bits allowed along the way direction
        quint8 backward;
    };
    /// A point of interest drawn as an outline, such as a building. Two of
    /// its corners, one from each half, give its position.
    struct PlaceWay {
        Place place;
        qint64 refs[2];
        double lat[2];
        double lon[2];
        quint8 found = 0;
    };
    struct PlaceRef {
        qint64 node;
        quint32 way;
        quint8 corner;
    };
    quint32 nameIndex(QByteArrayView name);
    /// Fills in everything but the position; false when the tags do not
    /// describe a place worth finding.
    static bool describePlace(const Osm::Tags &tags, Place *place);

    std::vector<RawWay> m_ways;
    std::vector<WayInfo> m_wayInfo;
    std::vector<qint64> m_refs;
    std::vector<qint64> m_nodeIds; ///< sorted ids of all nodes used by roads
    std::vector<qint32> m_lat;
    std::vector<qint32> m_lon;
    QHash<QByteArray, quint32> m_nameIndex;
    std::vector<quint32> m_nameOffsets{0, 0};
    QByteArray m_nameData;
    std::vector<Place> m_places;
    std::vector<PlaceWay> m_placeWays;
    std::vector<PlaceRef> m_placeRefs; ///< sorted by node once all ways are known
};
