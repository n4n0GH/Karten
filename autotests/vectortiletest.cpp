// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "vectortile.h"

#include "tilewriter.h"

#include <QTest>

using namespace Qt::StringLiterals;

using namespace TileWriter;

namespace
{
/// A feature of the given geometry type at one position, with tag index pairs.
QByteArray feature(int type, int x, int y, std::initializer_list<quint64> tags)
{
    const quint64 moveToOnce = 1 | 1 << 3;
    return bytes(2, packed(tags)) + number(3, quint64(type)) + bytes(4, packed({moveToOnce, zigzag(x), zigzag(y)}));
}

/// A layer of such features, with the keys and values their tags refer to.
QByteArray layer(const QByteArray &name, const QList<QByteArray> &features)
{
    return TileWriter::layer(name, features, {"name", "rank"}, {text("Café Zentral"), number(4, 7), text("Bahnhof")});
}
}

class VectorTileTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void readsPointsOfWantedLayers()
    {
        constexpr int Point = 1;
        constexpr int Line = 2;
        const QByteArray tile =
            bytes(3, layer("poi", {feature(Point, 1024, 3072, {0, 0, 1, 1}), feature(Line, 5, 5, {0, 2}), feature(Point, -8, 4100, {0, 2})}))
            + bytes(3, layer("water", {feature(Point, 1, 1, {0, 0})}));

        const QList<VectorTile::PointFeature> points = VectorTile::pointFeatures(tile, {u"poi"_s});
        QCOMPARE(points.size(), 2);
        QCOMPARE(points[0].layer, u"poi"_s);
        QCOMPARE(points[0].x, 0.25);
        QCOMPARE(points[0].y, 0.75);
        QCOMPARE(points[0].properties.value(u"name"_s).toString(), u"Café Zentral"_s);
        QCOMPARE(points[0].properties.value(u"rank"_s).toInt(), 7);
        // Positions may lie in the margin around the tile.
        QVERIFY(points[1].x < 0);
        QVERIFY(points[1].y > 1);
        QCOMPARE(points[1].properties.value(u"name"_s).toString(), u"Bahnhof"_s);
    }

    void readsLinesAndAreas()
    {
        // A road of two stretches, and a lake with an island in it.
        // One after the other: each continues from where the last ended.
        QPoint cursor;
        QByteArray road = path({{0, 0}, {2048, 0}, {2048, 2048}}, false, cursor);
        road += path({{4096, 4096}, {0, 4096}}, false, cursor);
        cursor = {};
        QByteArray lake = path({{1024, 1024}, {3072, 1024}, {3072, 3072}, {1024, 3072}}, true, cursor);
        lake += path({{1536, 1536}, {1536, 2560}, {2560, 2560}}, true, cursor);
        const QByteArray tile =
            bytes(3, TileWriter::layer("transportation", {shape(2, road, {0, 0, 1, 1})}, {"class", "name"}, {text("primary"), text("Ring")}))
            + bytes(3, TileWriter::layer("water", {shape(3, lake, {0, 0})}, {"class"}, {text("lake")}))
            + bytes(3, TileWriter::layer("building", {shape(3, lake, {})}, {}, {}));

        const QList<VectorTile::Feature> features = VectorTile::features(tile, {u"water"_s, u"transportation"_s}, {u"class"_s});
        QCOMPARE(features.size(), 2);

        // In the order of the tile; the layer is the place in the list asked for.
        QCOMPARE(features[0].type, VectorTile::Feature::Line);
        QCOMPARE(features[0].layer, 1);
        QCOMPARE(features[0].parts.size(), 2);
        QCOMPARE(features[0].parts[0], (QList<QPointF>{{0, 0}, {0.5, 0}, {0.5, 0.5}}));
        QCOMPARE(features[0].parts[1], (QList<QPointF>{{1, 1}, {0, 1}}));
        QCOMPARE(features[0].properties.value(u"class"_s).toString(), u"primary"_s);
        // Only the properties asked for are read.
        QVERIFY(!features[0].properties.contains(u"name"_s));

        QCOMPARE(features[1].type, VectorTile::Feature::Polygon);
        QCOMPARE(features[1].layer, 0);
        QCOMPARE(features[1].parts.size(), 2);
        QCOMPARE(features[1].parts[0].size(), 4);
        QCOMPARE(features[1].parts[1], (QList<QPointF>{{0.375, 0.375}, {0.375, 0.625}, {0.625, 0.625}}));
        QCOMPARE(features[1].properties.value(u"class"_s).toString(), u"lake"_s);
    }

    void survivesGarbage()
    {
        // A line that claims more points than there is data for.
        const QByteArray cut = bytes(3, TileWriter::layer("water", {shape(2, packed({1 | 1 << 3, 0, 0, 2 | 1000 << 3, 2, 2}), {})}, {}, {}));
        QVERIFY(VectorTile::features(cut, {u"water"_s}, {}).isEmpty());
        QVERIFY(VectorTile::features("not a vector tile at all", {u"poi"_s}, {}).isEmpty());

        QVERIFY(VectorTile::pointFeatures("not a vector tile at all", {u"poi"_s}).isEmpty());
        QVERIFY(VectorTile::pointFeatures(QByteArray(64, '\xff'), {u"poi"_s}).isEmpty());
        QVERIFY(VectorTile::pointFeatures({}, {u"poi"_s}).isEmpty());
    }

    void unpacksGzip()
    {
        const QByteArray packedText = QByteArray::fromHex("1f8b08000000000002fff34e2c2a49cd53284b4d2ec92f5228c9cc495528492d2e01001c3ffe1617000000");
        QCOMPARE(VectorTile::gunzip(packedText), QByteArray("Karten vector tile test"));
        QCOMPARE(VectorTile::gunzip("plain"), QByteArray("plain"));
        QVERIFY(VectorTile::gunzip(packedText.left(20)).isEmpty());
    }
};

QTEST_GUILESS_MAIN(VectorTileTest)

#include "vectortiletest.moc"
