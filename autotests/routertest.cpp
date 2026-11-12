// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "router.h"
#include "graphbuilder.h"

#include <QBuffer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

using namespace Qt::StringLiterals;

namespace
{
constexpr double BaseLat = 48.0;
constexpr double BaseLon = 11.0;
constexpr double Step = 0.001; // about 111 m north-south, 74 m east-west
constexpr int Size = 15;

qint64 nodeId(int row, int col)
{
    return 1000 + row * Size + col;
}

GeoPoint nodePoint(int row, int col)
{
    return {BaseLat + row * Step, BaseLon + col * Step};
}

Osm::Tags tags(std::initializer_list<std::pair<const char *, const char *>> list)
{
    Osm::Tags result;
    for (const auto &[key, value] : list) {
        result.push_back({key, value});
    }
    return result;
}

/// A 15×15 grid of residential streets. Rows are named "Row N", columns
/// "Column N". The callbacks can add further ways and their nodes.
void buildGrid(RoadGraph *graph,
               std::vector<Place> *places,
               const std::function<void(GraphBuilder &)> &extraWays = {},
               const std::function<void(GraphBuilder &)> &extraNodes = {})
{
    GraphBuilder builder;
    qint64 wayId = 1;
    std::vector<QByteArray> names;
    for (int i = 0; i < Size; ++i) {
        std::vector<qint64> row;
        std::vector<qint64> column;
        for (int j = 0; j < Size; ++j) {
            row.push_back(nodeId(i, j));
            column.push_back(nodeId(j, i));
        }
        names.push_back("Row " + QByteArray::number(i));
        builder.way(wayId++, row, tags({{"highway", "residential"}, {"name", names.back().constData()}}));
        names.push_back("Column " + QByteArray::number(i));
        builder.way(wayId++, column, tags({{"highway", "residential"}, {"name", names.back().constData()}}));
    }
    if (extraWays) {
        extraWays(builder);
    }
    builder.prepareNodes();
    for (int i = 0; i < Size; ++i) {
        for (int j = 0; j < Size; ++j) {
            const GeoPoint p = nodePoint(i, j);
            builder.node(nodeId(i, j), p.lat, p.lon, {});
        }
    }
    if (extraNodes) {
        extraNodes(builder);
    }
    builder.node(1, BaseLat + 0.0005, BaseLon + 0.0005, tags({{"place", "village"}, {"name", "Gridville"}}));
    builder.node(2, BaseLat + 0.0007, BaseLon + 0.0007, tags({{"amenity", "cafe"}, {"name", "Corner Café"}}));
    builder.finish(graph, places);
}
}

class RouterTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testGeo()
    {
        QCOMPARE(qRound(Geo::bearing({48, 11}, {49, 11})), 0);
        QCOMPARE(qRound(Geo::bearing({48, 11}, {48, 12})), 90);
        QCOMPARE(Geo::turnAngle(350, 10), 20.0);
        QCOMPARE(Geo::turnAngle(10, 350), -20.0);
        // One degree of latitude is about 111.2 km.
        QVERIFY(std::abs(Geo::distance({48, 11}, {49, 11}) - 111195) < 50);
    }

    void testGraph()
    {
        RoadGraph graph;
        std::vector<Place> places;
        buildGrid(&graph, &places);
        QCOMPARE(graph.nodeCount(), quint32(Size * Size));
        QCOMPARE(graph.edges.size(), size_t(2 * 2 * Size * (Size - 1)));

        const qint64 nearest = graph.nearestNode({BaseLat + 3 * Step + 0.0001, BaseLon + 4 * Step - 0.0001}, Car, 500);
        QVERIFY(nearest >= 0);
        const GeoPoint p = graph.point(quint32(nearest));
        QVERIFY(Geo::distance(p, nodePoint(3, 4)) < 1);
        QCOMPARE(graph.nearestNode({BaseLat + 1, BaseLon}, Car, 500), -1);

        // Place search data: the village, the café and one entry per street.
        int streets = 0;
        bool village = false;
        bool cafe = false;
        for (const Place &place : places) {
            streets += place.kind == Place::Street;
            village = village || (place.kind == Place::Village && place.name == u"Gridville");
            cafe = cafe || (place.kind == Place::Poi && place.type == u"cafe" && place.name == u"Corner Café");
        }
        QVERIFY(village);
        QVERIFY(cafe);
        QCOMPARE(streets, 2 * Size);
    }

    void testPlaceDetails()
    {
        // A library drawn as the outline of its building, and a bakery as a point.
        RoadGraph graph;
        std::vector<Place> places;
        buildGrid(
            &graph,
            &places,
            [](GraphBuilder &builder) {
                builder.way(900,
                            {5001, 5002, 5003, 5004, 5001},
                            tags({{"building", "yes"},
                                  {"amenity", "library"},
                                  {"name", "Town Library"},
                                  {"opening_hours", "Mo-Fr 09:00-18:00"},
                                  {"wikidata", "Q42"},
                                  {"roof:shape", "flat"}}));
                // An outline without a name is not a place.
                builder.way(901, {5001, 5002, 5003}, tags({{"building", "yes"}, {"amenity", "library"}}));
            },
            [](GraphBuilder &builder) {
                builder.node(5001, BaseLat + 0.0010, BaseLon + 0.0010, {});
                builder.node(5002, BaseLat + 0.0010, BaseLon + 0.0014, {});
                builder.node(5003, BaseLat + 0.0012, BaseLon + 0.0014, {});
                builder.node(5004, BaseLat + 0.0012, BaseLon + 0.0010, {});
                builder.node(5010, BaseLat + 0.002, BaseLon + 0.002, tags({{"shop", "bakery"}, {"name", "Bakery"}, {"phone", "+423 000 00 00"}}));
            });

        int libraries = 0;
        bool bakery = false;
        for (const Place &place : places) {
            if (place.name == u"Town Library") {
                ++libraries;
                QCOMPARE(place.type, u"library"_s);
                // Halfway between two opposite corners.
                QVERIFY(std::abs(place.lat - (BaseLat + 0.0011)) < 1e-9);
                QVERIFY(std::abs(place.lon - (BaseLon + 0.0012)) < 1e-9);
                const QJsonObject details = QJsonDocument::fromJson(place.tags).object();
                QCOMPARE(details.value("opening_hours"_L1).toString(), u"Mo-Fr 09:00-18:00"_s);
                QCOMPARE(details.value("wikidata"_L1).toString(), u"Q42"_s);
                QVERIFY(!details.contains("roof:shape"_L1));
                QVERIFY(!details.contains("name"_L1));
            } else if (place.name == u"Bakery") {
                bakery = QJsonDocument::fromJson(place.tags).object().value("phone"_L1).toString() == u"+423 000 00 00";
            } else if (place.name == u"Corner Café") {
                QVERIFY(place.tags.isEmpty());
            }
        }
        QCOMPARE(libraries, 1);
        QVERIFY(bakery);
    }

    void testRoute()
    {
        RoadGraph graph;
        std::vector<Place> places;
        buildGrid(&graph, &places);
        const Router router(graph);

        // East along row 0, then north along the last column: one right-angle turn.
        const Route route = router.route(nodePoint(0, 0), nodePoint(Size - 1, Size - 1), Car);
        QVERIFY(route.isValid());
        const double expected = Geo::distance(nodePoint(0, 0), nodePoint(0, Size - 1)) + Geo::distance(nodePoint(0, Size - 1), nodePoint(Size - 1, Size - 1));
        QVERIFY(std::abs(route.distance - expected) < 5);
        QCOMPARE(route.path.size(), size_t(2 * Size - 1));
        QCOMPARE(route.maneuvers.front().type, Maneuver::Depart);
        QCOMPARE(route.maneuvers.back().type, Maneuver::Arrive);
        QVERIFY(route.duration > 0);

        // A straight line down one street has no turns at all.
        const Route straight = router.route(nodePoint(5, 0), nodePoint(5, Size - 1), Foot);
        QVERIFY(straight.isValid());
        QCOMPARE(straight.maneuvers.size(), size_t(2));
        QCOMPARE(straight.maneuvers.front().street, u"Row 5"_s);
        QCOMPARE(qRound(straight.maneuvers.front().bearing), 90);
        // Walking pace is 5 km/h.
        QVERIFY(std::abs(straight.duration - straight.distance / (5.0 / 3.6)) < 1);

        // Start equals destination.
        const Route nowhere = router.route(nodePoint(2, 2), nodePoint(2, 2), Bike);
        QVERIFY(nowhere.isValid());
        QCOMPARE(nowhere.distance, 0.0);
    }

    void testTurnDirection()
    {
        QCOMPARE(Maneuver::turnType(90), Maneuver::Right);
        QCOMPARE(Maneuver::turnType(-90), Maneuver::Left);
        QCOMPARE(Maneuver::turnType(5), Maneuver::Continue);
        QCOMPARE(Maneuver::turnType(-40), Maneuver::SlightLeft);
        QCOMPARE(Maneuver::turnType(150), Maneuver::SharpRight);
        QCOMPARE(Maneuver::turnType(179), Maneuver::UTurn);

        // A dead-end street leaving the east edge of the grid at row 7.
        const GeoPoint spurEnd{BaseLat + 7 * Step, BaseLon + (Size + 1) * Step};
        RoadGraph graph;
        std::vector<Place> places;
        buildGrid(
            &graph,
            &places,
            [](GraphBuilder &builder) {
                builder.way(900, {nodeId(7, Size - 1), 5000, 5001}, tags({{"highway", "residential"}, {"name", "Spur"}}));
            },
            [spurEnd](GraphBuilder &builder) {
                builder.node(5000, spurEnd.lat, spurEnd.lon - Step, {});
                builder.node(5001, spurEnd.lat, spurEnd.lon, {});
            });
        const Router router(graph);

        // Northbound on the last column, the spur is a right turn.
        Route route = router.route(nodePoint(0, Size - 1), spurEnd, Car);
        QVERIFY(route.isValid());
        QCOMPARE(route.maneuvers.size(), size_t(3));
        QCOMPARE(route.maneuvers[1].type, Maneuver::Right);
        QCOMPARE(route.maneuvers[1].street, u"Spur"_s);
        QCOMPARE(route.maneuvers[1].pathIndex, quint32(7));
        QVERIFY(std::abs(route.maneuvers[1].distance - Geo::distance(nodePoint(7, Size - 1), spurEnd)) < 1);

        // Southbound it is a left turn.
        route = router.route(nodePoint(Size - 1, Size - 1), spurEnd, Bike);
        QVERIFY(route.isValid());
        QCOMPARE(route.maneuvers.size(), size_t(3));
        QCOMPARE(route.maneuvers[1].type, Maneuver::Left);
    }

    void testOneWayAndAccess()
    {
        // West of the grid: a fast one-way road from the north-west corner to
        // the south-west corner. East of it: stairs joining the two east corners.
        const GeoPoint fastMiddle{BaseLat + 7 * Step, BaseLon - Step};
        const GeoPoint stairsMiddle{BaseLat + 7 * Step, BaseLon + Size * Step};
        RoadGraph graph;
        std::vector<Place> places;
        buildGrid(
            &graph,
            &places,
            [](GraphBuilder &builder) {
                builder.way(900,
                            {nodeId(Size - 1, 0), 5000, nodeId(0, 0)},
                            tags({{"highway", "primary"}, {"oneway", "yes"}, {"name", "Fast Lane"}, {"maxspeed", "100"}}));
                builder.way(901, {nodeId(0, Size - 1), 5001, nodeId(Size - 1, Size - 1)}, tags({{"highway", "steps"}, {"name", "Stairs"}}));
            },
            [=](GraphBuilder &builder) {
                builder.node(5000, fastMiddle.lat, fastMiddle.lon, {});
                builder.node(5001, stairsMiddle.lat, stairsMiddle.lon, {});
            });
        const Router router(graph);

        // Southbound, a car takes the fast one-way road.
        const Route south = router.route(nodePoint(Size - 1, 0), nodePoint(0, 0), Car);
        QVERIFY(south.isValid());
        QCOMPARE(south.maneuvers.front().street, u"Fast Lane"_s);
        QCOMPARE(south.path.size(), size_t(3));

        // Northbound it must not, so it follows column 0 instead.
        const Route north = router.route(nodePoint(0, 0), nodePoint(Size - 1, 0), Car);
        QVERIFY(north.isValid());
        QCOMPARE(north.maneuvers.front().street, u"Column 0"_s);
        QCOMPARE(north.path.size(), size_t(Size));

        // Pedestrians may walk the one-way road against the traffic.
        const Route walk = router.route(fastMiddle, nodePoint(Size - 1, 0), Foot);
        QVERIFY(walk.isValid());
        QCOMPARE(walk.path.size(), size_t(2));

        // The stairs are for pedestrians only: a car starting there snaps to
        // the nearest road it can drive on.
        const Route onFoot = router.route(stairsMiddle, nodePoint(Size - 1, Size - 1), Foot);
        QCOMPARE(onFoot.path.size(), size_t(2));
        const Route byCar = router.route(stairsMiddle, nodePoint(Size - 1, Size - 1), Car);
        QVERIFY(byCar.isValid());
        QCOMPARE(byCar.maneuvers.front().street, u"Column 14"_s);
    }

    void testSaveLoad()
    {
        RoadGraph graph;
        std::vector<Place> places;
        buildGrid(&graph, &places);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(u"graph.bin"_s);
        QVERIFY(graph.save(path));

        RoadGraph loaded;
        QString error;
        QVERIFY2(loaded.load(path, &error), qPrintable(error));
        QCOMPARE(loaded.nodeCount(), graph.nodeCount());
        QCOMPARE(loaded.edges.size(), graph.edges.size());
        const Route a = Router(graph).route(nodePoint(1, 1), nodePoint(9, 12), Bike);
        const Route b = Router(loaded).route(nodePoint(1, 1), nodePoint(9, 12), Bike);
        QVERIFY(a.isValid());
        QCOMPARE(a.distance, b.distance);
        QCOMPARE(a.maneuvers.size(), b.maneuvers.size());

        // A truncated file is rejected rather than read out of bounds.
        QFile file(path);
        QVERIFY(file.resize(file.size() - 10));
        QVERIFY(!loaded.load(path, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(loaded.nodeCount(), quint32(0));
    }

    void testPbfRejectsGarbage()
    {
        QByteArray garbage(1000, 'x');
        QBuffer buffer(&garbage);
        QVERIFY(buffer.open(QIODevice::ReadOnly));
        Osm::Handler handler;
        Osm::PbfReader reader;
        QVERIFY(!reader.read(&buffer, &handler, Osm::PbfReader::Nodes | Osm::PbfReader::Ways));
        QVERIFY(!reader.errorString().isEmpty());
    }
};

QTEST_GUILESS_MAIN(RouterTest)

#include "routertest.moc"
