// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QVariantMap>

#include <QPointF>

/// Reads vector tiles (Mapbox Vector Tile format): the labelled points, to
/// find out what is drawn at a spot on the map, and the shapes, to draw a
/// map of its own for printing.
namespace VectorTile
{
struct PointFeature {
    QString layer;
    /// Position within the tile, from 0 (west, north) to 1 (east, south).
    double x = 0;
    double y = 0;
    QVariantMap properties;
};

/// The point features of the named layers. Anything malformed is left out.
QList<PointFeature> pointFeatures(const QByteArray &tile, const QStringList &layers);

struct Feature {
    enum Type {
        Point = 1,
        Line = 2,
        Polygon = 3,
    };
    Type type = Point;
    /// Which of the layers asked for it belongs to, counted from 0.
    int layer = 0;
    /// Positions within the tile, from 0 (west, north) to 1 (east, south);
    /// shapes reach into the margin around the tile. A point feature has one
    /// part with its points, a line one part per stretch, and a polygon one
    /// per ring: holes follow the outline they are cut out of.
    QList<QList<QPointF>> parts;
    QVariantMap properties;
};

/// The features of the named layers, in the order of the tile. Of their
/// properties only those named in @p keys are read. Anything malformed is
/// left out.
QList<Feature> features(const QByteArray &tile, const QStringList &layers, const QStringList &keys);

/// Unpacks gzip-compressed data; returns the data unchanged when it is not
/// compressed, and nothing when it is damaged.
QByteArray gunzip(const QByteArray &data);
}
