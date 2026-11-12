// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QVariantMap>

/// Reads the labelled points out of a vector tile (Mapbox Vector Tile format),
/// to find out what is drawn at a spot on the map.
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

/// Unpacks gzip-compressed data; returns the data unchanged when it is not
/// compressed, and nothing when it is damaged.
QByteArray gunzip(const QByteArray &data);
}
