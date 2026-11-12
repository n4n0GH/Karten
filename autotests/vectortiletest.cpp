// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "vectortile.h"

#include <QTest>

using namespace Qt::StringLiterals;

namespace
{
// A minimal protobuf writer, enough to put a vector tile together.
QByteArray varint(quint64 value)
{
    QByteArray out;
    do {
        const char byte = char(value & 0x7f);
        value >>= 7;
        out.append(value ? char(byte | 0x80) : byte);
    } while (value);
    return out;
}

QByteArray number(int field, quint64 value)
{
    return varint(quint64(field) << 3) + varint(value);
}

QByteArray bytes(int field, const QByteArray &content)
{
    return varint(quint64(field) << 3 | 2) + varint(quint64(content.size())) + content;
}

quint64 zigzag(qint64 value)
{
    return (quint64(value) << 1) ^ quint64(value >> 63);
}

QByteArray packed(std::initializer_list<quint64> values)
{
    QByteArray out;
    for (const quint64 value : values) {
        out += varint(value);
    }
    return out;
}

/// A feature of the given geometry type at one position, with tag index pairs.
QByteArray feature(int type, int x, int y, std::initializer_list<quint64> tags)
{
    const quint64 moveToOnce = 1 | 1 << 3;
    return bytes(2, packed(tags)) + number(3, quint64(type)) + bytes(4, packed({moveToOnce, zigzag(x), zigzag(y)}));
}

QByteArray layer(const QByteArray &name, const QList<QByteArray> &features)
{
    QByteArray out = bytes(1, name);
    for (const QByteArray &entry : features) {
        out += bytes(2, entry);
    }
    out += bytes(3, "name") + bytes(3, "rank");
    out += bytes(4, bytes(1, "Café Zentral")) + bytes(4, number(4, 7)) + bytes(4, bytes(1, "Bahnhof"));
    out += number(5, 4096);
    return out;
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

    void survivesGarbage()
    {
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
