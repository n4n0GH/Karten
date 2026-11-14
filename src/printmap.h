// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "geo.h"

#include <QCache>
#include <QHash>
#include <QPainterPath>
#include <QPolygonF>
#include <QSizeF>
#include <QStringList>
#include <QTransform>

#include <vector>

class QPainter;

/// Draws maps for paper: light, with little ink, the roads as outlines and
/// the route in black on top, so that it reads at a glance and survives a
/// black-and-white printer.
///
/// The map on the screen is MapLibre's and needs a window to draw in; this
/// one is drawn from the same vector tiles with a painter, which makes it
/// sharp at any resolution. It understands the OpenMapTiles schema. Where
/// there are no such tiles, a map is its route on white.
///
/// All measures on paper are millimetres. An object of this class is used
/// by one thread at a time.
class PrintMap
{
public:
    struct TileId {
        int z = 0;
        int x = 0;
        int y = 0; ///< counted from the north
        friend bool operator==(const TileId &, const TileId &) = default;
    };

    /// What one map shows.
    struct View {
        GeoPoint centre;
        double metresPerMm = 10; ///< at the centre
        double bearing = 0; ///< the compass direction that points up, in degrees
        QSizeF size;
        int zoom = 14; ///< of the tiles to draw from

        /// From Web Mercator coordinates (0 to 1) to the paper, the top
        /// left corner of the map being its origin.
        QTransform worldToPaper() const;
        QPointF toPaper(GeoPoint point) const;
        /// The tiles the map is drawn from.
        QList<TileId> tiles() const;
        /// The zoom level whose tiles hold the detail a map of this scale wants.
        static int zoomFor(double metresPerMm, double latitude, int minZoom, int maxZoom);
    };

    enum Style {
        Junction, ///< a few hundred metres, with buildings, paths and street names
        Overview, ///< a whole route, with the bigger roads and the names of places
    };

    /// How a line ends.
    enum End {
        Plain,
        Dot, ///< where it comes from
        Arrow, ///< where it goes on to
        Target, ///< the destination
    };
    struct Marker {
        GeoPoint position;
        QString label;
    };
    /// What is drawn on top of a map.
    struct Overlay {
        std::vector<GeoPoint> line; ///< the route, or the stretch of it to show
        End start = Plain;
        End end = Plain;
        QList<Marker> markers;
        /// Streets whose names matter here; they are labelled first.
        QStringList streets;
        double scaleBar = 0; ///< metres, 0 for none
        QString scaleBarLabel;
    };

    /// @p fontFamily is what the labels are set in.
    explicit PrintMap(const QString &fontFamily);
    ~PrintMap();

    /// Adds a tile as the map source sent it.
    void addTile(const TileId &id, const QByteArray &data);

    /// Draws a map of the size of its view at the painter's origin, one
    /// unit of the painter being a millimetre. Parts of it lie outside:
    /// clip to the size of the view.
    void paint(QPainter *painter, const View &view, Style style, const Overlay &overlay);

private:
    struct Shape;
    struct Tile;
    struct Label;
    const Tile *tile(const TileId &id);
    Label makeLabel(const QString &text, double size, bool bold) const;

    QString m_fontFamily;
    QHash<TileId, QByteArray> m_data;
    /// Taken apart, tiles are much larger; only the last few are kept so.
    QCache<TileId, Tile> m_tiles;
};

size_t qHash(const PrintMap::TileId &id, size_t seed = 0);
