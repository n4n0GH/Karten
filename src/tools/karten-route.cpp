// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// Developer tool: builds a road graph from an .osm.pbf file and plans a route.
//
//   karten-route extract.osm.pbf LAT,LON LAT,LON [car|bike|foot]

#include "graphbuilder.h"
#include "router.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTextStream>

using namespace Qt::StringLiterals;

static bool parsePoint(const QString &text, GeoPoint &point)
{
    const QStringList parts = text.split(u',');
    bool okLat = false;
    bool okLon = false;
    if (parts.size() == 2) {
        point.lat = parts[0].toDouble(&okLat);
        point.lon = parts[1].toDouble(&okLon);
    }
    return okLat && okLon;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    const QStringList args = app.arguments();
    GeoPoint from;
    GeoPoint to;
    if (args.size() < 4 || !parsePoint(args[2], from) || !parsePoint(args[3], to)) {
        out << "Usage: karten-route extract.osm.pbf LAT,LON LAT,LON [car|bike|foot]\n";
        return 2;
    }
    const QString modeName = args.value(4, u"car"_s);
    const Mode mode = modeName == "bike"_L1 ? Bike : modeName == "foot"_L1 ? Foot : Car;

    QFile file(args[1]);
    if (!file.open(QIODevice::ReadOnly)) {
        out << "Cannot open " << args[1] << ": " << file.errorString() << '\n';
        return 1;
    }

    QElapsedTimer timer;
    timer.start();
    GraphBuilder builder;
    Osm::PbfReader reader;
    if (!reader.read(&file, &builder, Osm::PbfReader::Ways)) {
        out << "Read error: " << reader.errorString() << '\n';
        return 1;
    }
    builder.prepareNodes();
    file.seek(0);
    if (!reader.read(&file, &builder, Osm::PbfReader::Nodes)) {
        out << "Read error: " << reader.errorString() << '\n';
        return 1;
    }
    RoadGraph graph;
    std::vector<Place> places;
    builder.finish(&graph, &places);
    out << "Graph: " << graph.nodeCount() << " nodes, " << graph.edges.size() << " edges, " << graph.ways.size() << " ways, " << places.size()
        << " places, built in " << timer.restart() << " ms\n";

    const Route route = Router(graph).route(from, to, mode);
    const qint64 routeMs = timer.elapsed();
    if (!route.isValid()) {
        out << "No route found\n";
        return 1;
    }
    out << "Route: " << route.distance / 1000.0 << " km, " << route.duration / 60.0 << " min, " << route.path.size() << " points, found in " << routeMs
        << " ms\n";
    static const char *const names[] =
        {"Depart", "Continue", "Slight right", "Right", "Sharp right", "U-turn", "Sharp left", "Left", "Slight left", "Roundabout", "Arrive"};
    for (const Maneuver &m : route.maneuvers) {
        out << "  " << names[m.type];
        if (m.type == Maneuver::Roundabout) {
            out << " exit " << m.exit;
        }
        if (!m.street.isEmpty()) {
            out << " onto " << m.street;
        }
        out << " (" << qRound(m.distance) << " m)\n";
    }
    return 0;
}
