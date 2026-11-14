// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "printmap.h"

#include "vectortile.h"

#include <QFont>
#include <QFontMetricsF>
#include <QPainter>
#include <QSet>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

using namespace Qt::StringLiterals;

namespace
{
/// Of the sphere that Web Mercator maps, in metres.
constexpr double EarthCircumference = 2 * std::numbers::pi * 6378137.0;

QPointF toWorld(GeoPoint point)
{
    const double sine = std::sin(std::clamp(point.lat, -85.0, 85.0) * Geo::DegToRad);
    return {(point.lon + 180) / 360, 0.5 - std::log((1 + sine) / (1 - sine)) / (4 * std::numbers::pi)};
}

enum Kind : quint8 {
    Wood,
    Grass,
    BuiltUp,
    Water,
    Waterway,
    Building,
    Square, ///< paved ground for walking: a pedestrian area, a pier
    Road,
    Rail,
    RoadName,
    Place,
};

/// From the widest to the narrowest.
enum RoadClass : quint8 {
    Motorway,
    Trunk,
    Primary,
    Secondary,
    Tertiary,
    Minor,
    Pedestrian,
    Service,
    Track,
    Footpath,
};

enum PlaceClass : quint8 {
    City,
    Town,
    Village,
    Quarter, ///< and whatever else is smaller than a village
};

// The layers of the OpenMapTiles schema that are drawn, in this order in the list.
enum Layer {
    Landcover,
    Landuse,
    WaterLayer,
    WaterwayLayer,
    BuildingLayer,
    Transportation,
    TransportationName,
    PlaceLayer,
};
const QStringList &layerNames()
{
    static const QStringList names{
        u"landcover"_s,
        u"landuse"_s,
        u"water"_s,
        u"waterway"_s,
        u"building"_s,
        u"transportation"_s,
        u"transportation_name"_s,
        u"place"_s,
    };
    return names;
}
const QStringList &propertyNames()
{
    static const QStringList names{u"class"_s, u"subclass"_s, u"brunnel"_s, u"ramp"_s, u"name"_s, u"name:latin"_s, u"ref"_s, u"rank"_s};
    return names;
}

/// The class of a road, or -1 for what is not drawn as one.
int roadClass(const QString &group, const QString &subclass)
{
    static const QHash<QString, RoadClass> classes{
        {u"motorway"_s, Motorway},
        {u"trunk"_s, Trunk},
        {u"primary"_s, Primary},
        {u"secondary"_s, Secondary},
        {u"tertiary"_s, Tertiary},
        {u"minor"_s, Minor},
        {u"raceway"_s, Minor},
        {u"service"_s, Service},
        {u"busway"_s, Service},
        {u"track"_s, Track},
    };
    if (group == "path"_L1) {
        if (subclass == "platform"_L1) {
            return -1;
        }
        return subclass == "pedestrian"_L1 ? Pedestrian : Footpath;
    }
    const auto found = classes.constFind(group);
    return found == classes.cend() ? -1 : int(*found);
}

// --- Colours. Paper is white whatever the colours of the desktop. ---
constexpr QRgb WoodColour = 0xffdde9d3;
constexpr QRgb GrassColour = 0xffecf3e4;
constexpr QRgb BuiltUpColour = 0xffeeeeee;
constexpr QRgb WaterColour = 0xffc6daec;
constexpr QRgb BuildingColour = 0xffdedbd6;
constexpr QRgb SquareColour = 0xfff1f1f1;
constexpr QRgb CasingColour = 0xff7a7a7a;
constexpr QRgb TunnelCasingColour = 0xffb4b4b4;
constexpr QRgb ThinRoadColour = 0xff969696;
constexpr QRgb FootpathColour = 0xff8c8c8c;
constexpr QRgb RailColour = 0xff5a5a5a;
constexpr QRgb TextColour = 0xff202020;
constexpr QRgb FrameColour = 0xff8c8c8c;

/// How a road is drawn. A width of 0 means not at all.
struct RoadStyle {
    double width = 0;
    QRgb fill = 0xffffffff;
    bool cased = false; ///< an outline around a fill, rather than a plain line
};

RoadStyle roadStyle(PrintMap::Style style, int zoom, RoadClass group, bool ramp)
{
    static constexpr std::array<QRgb, 10>
        fills{0xfff7cd98, 0xfff9dcae, 0xfffcecbb, 0xfffdf6d5, 0xffffffff, 0xffffffff, 0xfffafafa, 0xffffffff, 0xffffffff, 0xffffffff};
    RoadStyle result;
    result.fill = fills[group];
    if (style == PrintMap::Junction) {
        static constexpr std::array<double, 10> widths{2.6, 2.4, 2.2, 2.0, 1.8, 1.5, 1.1, 0.9, 0.7, 0.28};
        result.width = widths[group] * (ramp ? 0.75 : 1);
        result.cased = group != Footpath;
        return result;
    }
    // A whole route: the big roads as on a road map, the rest as thin lines
    // as far as the scale has room for them.
    static constexpr std::array<double, 10> widths{1.2, 1.05, 0.9, 0.7, 0.35, 0.22, 0.22, 0.15, 0.15, 0};
    static constexpr std::array<int, 10> fromZoom{0, 0, 0, 0, 0, 11, 11, 13, 13, 99};
    if (zoom >= fromZoom[group]) {
        // Where a map shows half a country its roads lie close together.
        const double thinner = zoom >= 10 ? 1 : zoom >= 8 ? 0.8 : 0.55;
        result.width = widths[group] * thinner * (ramp ? 0.75 : 1);
        result.cased = group <= Secondary;
    }
    return result;
}

constexpr double CasingWidth = 0.18; ///< on each side of a road
constexpr double RouteWidth = 1.2;
constexpr double RouteCasing = 0.35; ///< on each side

/// The part of a segment inside a rectangle, if any (Liang-Barsky).
bool clipSegment(QPointF &a, QPointF &b, const QRectF &rect)
{
    const double dx = b.x() - a.x();
    const double dy = b.y() - a.y();
    double t0 = 0;
    double t1 = 1;
    const auto edge = [&](double p, double q) {
        if (p == 0) {
            return q >= 0;
        }
        const double t = q / p;
        if (p < 0) {
            if (t > t1) {
                return false;
            }
            t0 = std::max(t0, t);
        } else {
            if (t < t0) {
                return false;
            }
            t1 = std::min(t1, t);
        }
        return true;
    };
    if (!edge(-dx, a.x() - rect.left()) || !edge(dx, rect.right() - a.x()) || !edge(-dy, a.y() - rect.top()) || !edge(dy, rect.bottom() - a.y())) {
        return false;
    }
    const QPointF start = a;
    a = start + QPointF(dx, dy) * t0;
    b = start + QPointF(dx, dy) * t1;
    return true;
}

/// The stretches of a line that lie inside a rectangle.
QList<QPolygonF> clipLine(const QPolygonF &line, const QRectF &rect)
{
    QList<QPolygonF> runs;
    bool open = false; // whether the last run ends where the next segment starts
    for (qsizetype i = 0; i + 1 < line.size(); ++i) {
        QPointF a = line[i];
        QPointF b = line[i + 1];
        if (!clipSegment(a, b, rect)) {
            open = false;
            continue;
        }
        if (open && a == line[i]) {
            runs.last().append(b);
        } else {
            runs.append(QPolygonF{a, b});
        }
        open = b == line[i + 1];
    }
    return runs;
}

/// The part of an area inside a rectangle (Sutherland-Hodgman). Where the
/// area is cut, its outline runs along the edge of the rectangle.
QPolygonF clipRing(const QPolygonF &ring, const QRectF &rect)
{
    if (rect.contains(ring.boundingRect())) {
        return ring;
    }
    QPolygonF result = ring;
    // Against each side in turn: which side, and where it is.
    const std::array<std::pair<int, double>, 4> sides{{{0, rect.left()}, {1, rect.right()}, {2, rect.top()}, {3, rect.bottom()}}};
    for (const auto &[side, limit] : sides) {
        const auto inside = [side = side, limit = limit](QPointF p) {
            return side == 0 ? p.x() >= limit : side == 1 ? p.x() <= limit : side == 2 ? p.y() >= limit : p.y() <= limit;
        };
        const auto crossing = [side = side, limit = limit](QPointF a, QPointF b) {
            if (side < 2) {
                return QPointF(limit, a.y() + (b.y() - a.y()) * (limit - a.x()) / (b.x() - a.x()));
            }
            return QPointF(a.x() + (b.x() - a.x()) * (limit - a.y()) / (b.y() - a.y()), limit);
        };
        const QPolygonF input = result;
        result.clear();
        for (qsizetype i = 0; i < input.size(); ++i) {
            const QPointF current = input[i];
            const QPointF previous = input[(i + input.size() - 1) % input.size()];
            if (inside(current)) {
                if (!inside(previous)) {
                    result.append(crossing(previous, current));
                }
                result.append(current);
            } else if (inside(previous)) {
                result.append(crossing(previous, current));
            }
        }
    }
    return result;
}

/// A line without the points that lie closer together than can be seen.
QPolygonF thinned(const QPolygonF &line)
{
    constexpr double Finest = 0.08;
    QPolygonF result;
    for (qsizetype i = 0; i < line.size(); ++i) {
        const bool end = i == 0 || i + 1 == line.size();
        if (end || std::hypot(line[i].x() - result.last().x(), line[i].y() - result.last().y()) >= Finest) {
            result.append(line[i]);
        }
    }
    return result;
}

/// Joins the lines that continue one another.
void joinLines(QList<QPolygonF> &lines)
{
    const auto same = [](QPointF a, QPointF b) {
        return std::abs(a.x() - b.x()) < 0.05 && std::abs(a.y() - b.y()) < 0.05;
    };
    for (qsizetype i = 0; i < lines.size(); ++i) {
        for (qsizetype j = i + 1; j < lines.size();) {
            QPolygonF &a = lines[i];
            QPolygonF b = lines[j];
            if (same(a.first(), b.first()) || same(a.last(), b.last())) {
                std::reverse(b.begin(), b.end());
            }
            if (same(a.last(), b.first())) {
                a += b;
            } else if (same(b.last(), a.first())) {
                a = b + a;
            } else {
                ++j;
                continue;
            }
            lines.removeAt(j);
            j = i + 1; // the longer line may now meet one that was passed over
        }
    }
}

bool lineCrosses(const QPolygonF &line, const QRectF &rect)
{
    for (qsizetype i = 0; i + 1 < line.size(); ++i) {
        QPointF a = line[i];
        QPointF b = line[i + 1];
        if (clipSegment(a, b, rect)) {
            return true;
        }
    }
    return false;
}

double lengthOf(const QPolygonF &line)
{
    double length = 0;
    for (qsizetype i = 0; i + 1 < line.size(); ++i) {
        length += std::hypot(line[i + 1].x() - line[i].x(), line[i + 1].y() - line[i].y());
    }
    return length;
}

/// The point at a distance from the start of a line, measured along it.
QPointF pointAt(const QPolygonF &line, double distance)
{
    for (qsizetype i = 0; i + 1 < line.size(); ++i) {
        const double length = std::hypot(line[i + 1].x() - line[i].x(), line[i + 1].y() - line[i].y());
        if (distance <= length && length > 0) {
            return line[i] + (line[i + 1] - line[i]) * (distance / length);
        }
        distance -= length;
    }
    return line.isEmpty() ? QPointF() : line.last();
}

/// Shortens a line by a distance at its end; returns false when it is not that long.
bool trimEnd(QPolygonF &line, double distance)
{
    if (lengthOf(line) <= distance) {
        return false;
    }
    while (line.size() >= 2) {
        const QPointF a = line[line.size() - 2];
        const QPointF b = line.last();
        const double length = std::hypot(b.x() - a.x(), b.y() - a.y());
        if (length > distance) {
            line.last() = b + (a - b) * (distance / length);
            return true;
        }
        distance -= length;
        line.removeLast();
    }
    return false;
}

QPen linePen(QRgb colour, double width, Qt::PenCapStyle cap = Qt::RoundCap)
{
    return QPen(QColor(colour), width, Qt::SolidLine, cap, Qt::RoundJoin);
}

QPainterPath areaPath(const QList<QPolygonF> &rings)
{
    QPainterPath path;
    for (const QPolygonF &ring : rings) {
        path.addPolygon(ring);
        path.closeSubpath();
    }
    return path;
}

void drawLines(QPainter *painter, const QList<QPolygonF> &lines)
{
    for (const QPolygonF &line : lines) {
        painter->drawPolyline(line);
    }
}
}

size_t qHash(const PrintMap::TileId &id, size_t seed)
{
    return qHashMulti(seed, id.z, id.x, id.y);
}

struct PrintMap::Shape {
    Kind kind = Road;
    quint8 rank = 0; ///< RoadClass, PlaceClass, or 0 for a river among waterways
    qint8 level = 0; ///< -1 in a tunnel, 1 on a bridge
    bool ramp = false;
    int order = 0; ///< importance among places, the smaller the more
    /// As in the tile: from 0 to 1 across it. A tile keeps all that is
    /// alike in few shapes of many parts, hence the bounds of each part.
    QList<QPolygonF> parts;
    QList<QRectF> bounds;
    QString name;
};

struct PrintMap::Tile {
    QList<Shape> shapes;
};

/// A piece of text ready to be put somewhere: its outline around the origin.
struct PrintMap::Label {
    QPainterPath path;
    QRectF rect; ///< the room it takes up
};

// --- The view ---

QTransform PrintMap::View::worldToPaper() const
{
    const QPointF middle = toWorld(centre);
    // How many millimetres the whole world would be wide at this scale.
    const double scale = EarthCircumference * std::cos(centre.lat * Geo::DegToRad) / metresPerMm;
    QTransform transform;
    transform.translate(size.width() / 2, size.height() / 2);
    transform.rotate(-bearing);
    transform.scale(scale, scale);
    transform.translate(-middle.x(), -middle.y());
    return transform;
}

QPointF PrintMap::View::toPaper(GeoPoint point) const
{
    return worldToPaper().map(toWorld(point));
}

QList<PrintMap::TileId> PrintMap::View::tiles() const
{
    // A little beyond the edge, for what is drawn wider than it is.
    const QRectF world = worldToPaper().inverted().mapRect(QRectF(QPointF(0, 0), size).adjusted(-2, -2, 2, 2));
    const int count = 1 << zoom;
    const auto index = [count](double position) {
        return std::clamp(int(std::floor(position * count)), 0, count - 1);
    };
    QList<TileId> result;
    for (int x = index(world.left()); x <= index(world.right()); ++x) {
        for (int y = index(world.top()); y <= index(world.bottom()); ++y) {
            result.append({zoom, x, y});
        }
    }
    return result;
}

int PrintMap::View::zoomFor(double metresPerMm, double latitude, int minZoom, int maxZoom)
{
    // A tile is made to be shown about 512 pixels of a screen wide, which
    // is 135 mm. On paper a little more detail than that is welcome.
    constexpr double TileWidth = 135;
    const double tiles = EarthCircumference * std::cos(latitude * Geo::DegToRad) / (metresPerMm * TileWidth);
    return std::clamp(int(std::lround(std::log2(std::max(tiles, 1.0)) + 0.4)), minZoom, maxZoom);
}

// --- Tiles ---

PrintMap::PrintMap(const QString &fontFamily)
    : m_fontFamily(fontFamily)
    , m_tiles(24)
{
}

PrintMap::~PrintMap() = default;

void PrintMap::addTile(const TileId &id, const QByteArray &data)
{
    m_data.insert(id, data);
    m_tiles.remove(id);
}

const PrintMap::Tile *PrintMap::tile(const TileId &id)
{
    if (const Tile *known = m_tiles.object(id)) {
        return known;
    }
    const auto data = m_data.constFind(id);
    if (data == m_data.cend()) {
        return nullptr;
    }
    auto *tile = new Tile;
    const QList<VectorTile::Feature> features = VectorTile::features(VectorTile::gunzip(*data), layerNames(), propertyNames());
    for (const VectorTile::Feature &feature : features) {
        const QString group = feature.properties.value(u"class"_s).toString();
        const QString subclass = feature.properties.value(u"subclass"_s).toString();
        const QString brunnel = feature.properties.value(u"brunnel"_s).toString();
        Shape shape;
        shape.level = brunnel == "tunnel"_L1 ? -1 : brunnel == "bridge"_L1 ? 1 : 0;
        bool wanted = false;
        switch (feature.layer) {
        case Landcover:
            shape.kind = group == "wood"_L1 ? Wood : Grass;
            wanted = feature.type == VectorTile::Feature::Polygon && (group == "wood"_L1 || group == "grass"_L1);
            break;
        case Landuse:
            shape.kind = BuiltUp;
            wanted = feature.type == VectorTile::Feature::Polygon
                && (group == "residential"_L1 || group == "commercial"_L1 || group == "industrial"_L1 || group == "retail"_L1);
            break;
        case WaterLayer:
            shape.kind = Water;
            // What flows underground is not seen from the road.
            wanted = feature.type == VectorTile::Feature::Polygon && shape.level >= 0;
            break;
        case WaterwayLayer:
            shape.kind = Waterway;
            shape.rank = group == "river"_L1 || group == "canal"_L1 ? 0 : 1;
            wanted = feature.type == VectorTile::Feature::Line && shape.level >= 0;
            break;
        case BuildingLayer:
            shape.kind = Building;
            wanted = feature.type == VectorTile::Feature::Polygon;
            break;
        case Transportation: {
            if (feature.type == VectorTile::Feature::Polygon) {
                shape.kind = Square;
                wanted = group == "path"_L1 || group == "pier"_L1;
                break;
            }
            if (group == "rail"_L1) {
                shape.kind = Rail;
                // Trams share the road, and the underground cannot be seen.
                wanted = feature.type == VectorTile::Feature::Line && subclass != "tram"_L1 && shape.level >= 0;
                break;
            }
            const int road = roadClass(group, subclass);
            shape.kind = Road;
            shape.rank = quint8(std::max(road, 0));
            shape.ramp = feature.properties.value(u"ramp"_s).toInt() != 0;
            wanted = feature.type == VectorTile::Feature::Line && road >= 0;
            break;
        }
        case TransportationName: {
            shape.kind = RoadName;
            const int road = roadClass(group, subclass);
            shape.rank = quint8(std::max(road, 0));
            shape.name = feature.properties.value(u"name"_s).toString();
            if (shape.name.isEmpty()) {
                shape.name = feature.properties.value(u"name:latin"_s).toString();
            }
            if (shape.name.isEmpty()) {
                shape.name = feature.properties.value(u"ref"_s).toString(); // motorways often have a number only
            }
            wanted = feature.type == VectorTile::Feature::Line && road >= 0 && !shape.name.isEmpty();
            break;
        }
        case PlaceLayer: {
            shape.kind = Place;
            shape.rank = group == "city"_L1 ? City : group == "town"_L1 ? Town : group == "village"_L1 ? Village : Quarter;
            shape.order = feature.properties.value(u"rank"_s).toInt();
            shape.name = feature.properties.value(u"name"_s).toString();
            if (shape.name.isEmpty()) {
                shape.name = feature.properties.value(u"name:latin"_s).toString();
            }
            const QPointF at = feature.parts.first().first();
            // Outside of its own tile a place is a copy of the one next door.
            wanted = feature.type == VectorTile::Feature::Point && !shape.name.isEmpty() && group != "country"_L1 && group != "state"_L1
                && group != "continent"_L1 && group != "province"_L1 && at.x() >= 0 && at.x() < 1 && at.y() >= 0 && at.y() < 1;
            break;
        }
        }
        if (!wanted) {
            continue;
        }
        const bool area = feature.type == VectorTile::Feature::Polygon;
        for (const QList<QPointF> &part : feature.parts) {
            if (part.size() < (area ? 3 : feature.type == VectorTile::Feature::Line ? 2 : 1)) {
                continue;
            }
            QPointF least = part.first();
            QPointF most = part.first();
            for (const QPointF &point : part) {
                least = {std::min(least.x(), point.x()), std::min(least.y(), point.y())};
                most = {std::max(most.x(), point.x()), std::max(most.y(), point.y())};
            }
            shape.parts.append(QPolygonF(part));
            // Never empty, also for a point or a line straight along an axis.
            shape.bounds.append(QRectF(least, most).adjusted(-1e-9, -1e-9, 1e-9, 1e-9));
        }
        if (shape.parts.isEmpty()) {
            continue;
        }
        tile->shapes.append(std::move(shape));
    }
    m_tiles.insert(id, tile);
    return m_tiles.object(id);
}

// --- Drawing ---

PrintMap::Label PrintMap::makeLabel(const QString &text, double size, bool bold) const
{
    // Set large and scaled down: the outlines are the same at any size,
    // and it keeps the hinting for screens out of them.
    constexpr int Large = 100;
    QFont font(m_fontFamily);
    font.setPixelSize(Large);
    font.setBold(bold);
    font.setHintingPreference(QFont::PreferNoHinting);
    const QFontMetricsF metrics(font);
    const double width = metrics.horizontalAdvance(text);
    QPainterPath path;
    path.addText(0, 0, font, text);

    const double factor = size / Large;
    QTransform transform;
    transform.scale(factor, factor);
    // The capitals sit around the middle.
    transform.translate(-width / 2, metrics.capHeight() / 2);
    Label label;
    label.path = transform.map(path);
    const double halfWidth = width * factor / 2 + 0.5;
    const double halfHeight = size * 0.55;
    label.rect = QRectF(-halfWidth, -halfHeight, 2 * halfWidth, 2 * halfHeight);
    return label;
}

void PrintMap::paint(QPainter *painter, const View &view, Style style, const Overlay &overlay)
{
    const QRectF frame(QPointF(0, 0), view.size);
    const QTransform worldToPaper = view.worldToPaper();

    // Everything in sight, on the paper.
    struct Drawn {
        const Shape *shape;
        QList<QPolygonF> parts;
    };
    std::array<QList<Drawn>, Place + 1> drawn;
    // A little beyond the edge, for what is drawn wider than it is.
    const QRectF sight = frame.adjusted(-3, -3, 3, 3);
    const QList<TileId> ids = view.tiles();
    // The tiles in the cache must stay there while their shapes are in use.
    m_tiles.setMaxCost(std::max<qsizetype>(24, ids.size()));
    for (const TileId &id : ids) {
        const Tile *tile = this->tile(id);
        if (!tile) {
            continue;
        }
        const double count = 1 << id.z;
        const QTransform tileToPaper = QTransform(1 / count, 0, 0, 1 / count, id.x / count, id.y / count) * worldToPaper;
        for (const Shape &shape : tile->shapes) {
            const bool shown = style == Junction ? shape.kind != BuiltUp && shape.kind != Place
                                                 : shape.kind != Building && shape.kind != Square && shape.kind != Grass && shape.kind != RoadName;
            if (!shown) {
                continue;
            }
            const bool area = shape.kind <= Square && shape.kind != Waterway;
            Drawn entry{&shape, {}};
            for (qsizetype i = 0; i < shape.parts.size(); ++i) {
                if (!tileToPaper.mapRect(shape.bounds[i]).intersects(sight)) {
                    continue;
                }
                // Only what is in sight goes onto the paper, and no finer
                // than can be seen: a print job is easily many megabytes.
                const QPolygonF part = tileToPaper.map(shape.parts[i]);
                if (shape.kind == Place || shape.kind == RoadName) {
                    entry.parts.append(part);
                } else if (area) {
                    const QPolygonF ring = thinned(clipRing(part, sight));
                    if (ring.size() >= 3) {
                        entry.parts.append(ring);
                    }
                } else {
                    const QList<QPolygonF> pieces = clipLine(part, sight);
                    for (const QPolygonF &piece : pieces) {
                        entry.parts.append(thinned(piece));
                    }
                }
            }
            if (!entry.parts.isEmpty()) {
                drawn[shape.kind].append(std::move(entry));
            }
        }
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->fillRect(frame, Qt::white);

    // Ground.
    painter->setPen(Qt::NoPen);
    const auto fillAll = [&](Kind kind, QRgb colour) {
        painter->setBrush(QColor(colour));
        for (const Drawn &entry : std::as_const(drawn[kind])) {
            painter->drawPath(areaPath(entry.parts));
        }
    };
    fillAll(BuiltUp, BuiltUpColour);
    fillAll(Grass, GrassColour);
    fillAll(Wood, WoodColour);
    fillAll(Water, WaterColour);
    painter->setBrush(Qt::NoBrush);
    for (const Drawn &entry : std::as_const(drawn[Waterway])) {
        const bool river = entry.shape->rank == 0;
        if (style == Overview && !river) {
            continue;
        }
        painter->setPen(linePen(WaterColour, style == Junction ? (river ? 1.2 : 0.5) : 0.5));
        drawLines(painter, entry.parts);
    }
    painter->setPen(Qt::NoPen);
    fillAll(Square, SquareColour);
    fillAll(Building, BuildingColour);
    painter->setBrush(Qt::NoBrush);

    // Roads: what is below first, and within a level the outlines of all
    // before the insides of any, which is what joins them at a junction.
    QList<Drawn> &roads = drawn[Road];
    std::stable_sort(roads.begin(), roads.end(), [](const Drawn &a, const Drawn &b) {
        return a.shape->rank > b.shape->rank; // the bigger roads on top
    });
    for (int level = -1; level <= 1; ++level) {
        for (const Drawn &entry : std::as_const(roads)) {
            const RoadStyle look = roadStyle(style, view.zoom, RoadClass(entry.shape->rank), entry.shape->ramp);
            if (entry.shape->level != level || look.width <= 0 || !look.cased) {
                continue;
            }
            // On a bridge the outline ends with the bridge instead of bulging over what it leads to.
            painter->setPen(linePen(level < 0 ? TunnelCasingColour : CasingColour, look.width + 2 * CasingWidth, level > 0 ? Qt::FlatCap : Qt::RoundCap));
            drawLines(painter, entry.parts);
        }
        for (const Drawn &entry : std::as_const(roads)) {
            const RoadStyle look = roadStyle(style, view.zoom, RoadClass(entry.shape->rank), entry.shape->ramp);
            if (entry.shape->level != level || look.width <= 0) {
                continue;
            }
            if (look.cased) {
                painter->setPen(linePen(look.fill, look.width));
            } else if (entry.shape->rank == Footpath) {
                QPen pen = linePen(FootpathColour, look.width, Qt::FlatCap);
                pen.setDashPattern({4, 3});
                painter->setPen(pen);
            } else {
                painter->setPen(linePen(ThinRoadColour, look.width));
            }
            drawLines(painter, entry.parts);
        }
        for (const Drawn &entry : std::as_const(drawn[Rail])) {
            if (entry.shape->level != level) {
                continue;
            }
            // The dashes of a railway line on a map.
            const double width = style == Junction ? 0.7 : 0.4;
            painter->setPen(linePen(RailColour, width, Qt::FlatCap));
            drawLines(painter, entry.parts);
            QPen dashes = linePen(0xffffffff, width * 0.55, Qt::FlatCap);
            dashes.setDashPattern({5, 5});
            painter->setPen(dashes);
            drawLines(painter, entry.parts);
        }
    }

    // The route.
    QPolygonF route;
    for (const GeoPoint &point : overlay.line) {
        const QPointF at = worldToPaper.map(toWorld(point));
        // Finer than can be seen only makes the print job bigger.
        if (route.isEmpty() || std::hypot(at.x() - route.last().x(), at.y() - route.last().y()) > 0.15) {
            route.append(at);
        } else if (&point == &overlay.line.back() && route.size() > 1) {
            route.last() = at; // it ends where it ends
        }
    }
    constexpr double ArrowLength = 3.8;
    constexpr double ArrowHalfWidth = 2.0;
    QPolygonF arrow;
    QPolygonF line = route;
    if (overlay.end == Arrow && line.size() >= 2) {
        // The head points the way the line ends.
        QPointF tip = line.last();
        QPointF direction;
        for (qsizetype i = line.size() - 2; i >= 0 && direction.isNull(); --i) {
            direction = tip - line[i];
        }
        const double length = std::hypot(direction.x(), direction.y());
        if (length > 0) {
            direction /= length;
            // It needs as much of the line going that way to sit on: put
            // across a corner just before the end, it would point askew.
            // Where there is less, the line is drawn out a little.
            double straight = 0;
            for (qsizetype i = line.size() - 1; i > 0 && straight < ArrowLength; --i) {
                const QPointF piece = line[i] - line[i - 1];
                const double pieceLength = std::hypot(piece.x(), piece.y());
                if (QPointF::dotProduct(piece, direction) < 0.94 * pieceLength) {
                    break; // more than some 20 degrees off
                }
                straight += pieceLength;
            }
            if (straight < ArrowLength) {
                tip += direction * (ArrowLength - straight);
                line.last() = tip;
            }
            // The line ends inside the head, which then comes to a clean point.
            trimEnd(line, ArrowLength * 0.8);
            const QPointF across(-direction.y(), direction.x());
            const QPointF base = tip - direction * ArrowLength;
            arrow = {tip, base + across * ArrowHalfWidth, base - across * ArrowHalfWidth};
        }
    }
    if (line.size() >= 2) {
        painter->setPen(linePen(0xffffffff, RouteWidth + 2 * RouteCasing));
        painter->drawPolyline(line);
        painter->setPen(linePen(0xff000000, RouteWidth));
        painter->drawPolyline(line);
    }
    if (!arrow.isEmpty()) {
        painter->setPen(QPen(Qt::white, RouteCasing, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->setBrush(Qt::black);
        painter->drawPolygon(arrow);
    }
    const auto drawEnd = [&](End end, QPointF at) {
        if (end == Dot) {
            painter->setPen(QPen(Qt::white, RouteCasing));
            painter->setBrush(Qt::black);
            painter->drawEllipse(at, 1.4, 1.4);
        } else if (end == Target) {
            painter->setPen(QPen(Qt::black, 0.55));
            painter->setBrush(Qt::white);
            painter->drawEllipse(at, 2.0, 2.0);
            painter->setPen(Qt::NoPen);
            painter->setBrush(Qt::black);
            painter->drawEllipse(at, 0.95, 0.95);
        }
    };
    if (!route.isEmpty()) {
        drawEnd(overlay.start, route.first());
        drawEnd(overlay.end, route.last());
    }

    // Room that is taken, in which no name is put.
    QList<QPolygonF> taken;
    constexpr double MarkerRadius = 2.7;
    constexpr double CompassRadius = 2.6;
    const QPointF compass(frame.right() - CompassRadius - 1.2, frame.top() + CompassRadius + 1.2);
    taken.append(QPolygonF(QRectF(compass.x() - CompassRadius, compass.y() - CompassRadius, 2 * CompassRadius, 2 * CompassRadius)));
    const double barLength = overlay.scaleBar / view.metresPerMm;
    const QPointF barStart(frame.left() + 2, frame.bottom() - 2);
    Label barLabel;
    if (barLength > 0) {
        barLabel = makeLabel(overlay.scaleBarLabel, 2.3, false);
        taken.append(QPolygonF(QRectF(barStart.x() - 1, barStart.y() - 5, std::max(barLength, barLabel.rect.width()) + 2, 6.5)));
    }
    for (const Marker &marker : overlay.markers) {
        const QPointF at = worldToPaper.map(toWorld(marker.position));
        taken.append(QPolygonF(QRectF(at.x() - MarkerRadius, at.y() - MarkerRadius, 2 * MarkerRadius, 2 * MarkerRadius)));
    }

    const auto drawLabel = [&](const Label &label, const QTransform &placement, QRgb colour) {
        const QPainterPath path = placement.map(label.path);
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(Qt::white, 0.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->drawPath(path);
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(colour));
        painter->drawPath(path);
    };
    enum Fit {
        Fits,
        OnRoute,
        NoRoom,
    };
    // Whether a label can go at a place: within the map, clear of the
    // others, and not across the route, which it would cut in two.
    const auto fit = [&](const Label &label, const QTransform &placement) {
        const QPolygonF outline = placement.map(QPolygonF(label.rect));
        for (const QPointF &corner : outline) {
            if (!frame.adjusted(0.5, 0.5, -0.5, -0.5).contains(corner)) {
                return NoRoom;
            }
        }
        for (const QPolygonF &other : std::as_const(taken)) {
            if (outline.intersects(other)) {
                return NoRoom;
            }
        }
        const double clearance = RouteWidth / 2 + RouteCasing;
        const QPolygonF local = placement.inverted().map(route);
        return lineCrosses(local, label.rect.adjusted(-clearance, -clearance, clearance, clearance)) ? OnRoute : Fits;
    };

    if (style == Junction) {
        // Street names, along the streets.
        struct Candidate {
            const Shape *shape;
            QPolygonF run;
            double length;
            bool preferred;
        };
        // A street comes in pieces: cut at the edges of the tiles and
        // wherever something about it changes. Its name wants the whole.
        struct Street {
            const Shape *shape;
            QList<QPolygonF> lines;
        };
        QHash<QString, Street> streets;
        for (const Drawn &entry : std::as_const(drawn[RoadName])) {
            Street &street = streets[entry.shape->name];
            if (!street.shape || entry.shape->rank < street.shape->rank) {
                street.shape = entry.shape;
            }
            street.lines.append(entry.parts);
        }
        QList<Candidate> candidates;
        for (Street &street : streets) {
            joinLines(street.lines);
            const bool preferred = overlay.streets.contains(street.shape->name);
            for (const QPolygonF &line : std::as_const(street.lines)) {
                const QList<QPolygonF> runs = clipLine(line, frame.adjusted(1, 1, -1, -1));
                for (const QPolygonF &run : runs) {
                    candidates.append({street.shape, run, lengthOf(run), preferred});
                }
            }
        }
        std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
            if (a.preferred != b.preferred) {
                return a.preferred;
            }
            if (a.shape->rank != b.shape->rank) {
                return a.shape->rank < b.shape->rank;
            }
            return a.length > b.length;
        });
        QSet<QString> named;
        for (const Candidate &candidate : std::as_const(candidates)) {
            if (named.contains(candidate.shape->name) || named.size() >= 8) {
                continue;
            }
            const double beside = roadStyle(style, view.zoom, RoadClass(candidate.shape->rank), false).width / 2;
            bool placed = false;
            // Smaller rather than not at all, where the street is short.
            for (const double size : {candidate.preferred ? 2.9 : 2.6, 2.3}) {
                const Label label = makeLabel(candidate.shape->name, size, candidate.preferred);
                const double width = label.rect.width();
                if (candidate.length < width + 1) {
                    continue;
                }
                for (const double fraction : {0.5, 0.3, 0.7, 0.15, 0.85}) {
                    const double middle = std::clamp(candidate.length * fraction, width / 2, candidate.length - width / 2);
                    const QPointF from = pointAt(candidate.run, middle - width / 2);
                    const QPointF to = pointAt(candidate.run, middle + width / 2);
                    const QPointF centre = (from + to) / 2;
                    const QPointF onRoad = pointAt(candidate.run, middle);
                    // Text is straight; where the street bends away from under it, it does not belong.
                    if (std::hypot(onRoad.x() - centre.x(), onRoad.y() - centre.y()) > label.rect.height() * 0.5) {
                        continue;
                    }
                    double angle = std::atan2(to.y() - from.y(), to.x() - from.x()) / Geo::DegToRad;
                    if (angle > 90 || angle < -90) {
                        angle += 180; // never upside down
                    }
                    // On the street, or next to it where the route runs along it.
                    const double aside = beside + label.rect.height() / 2 + 0.3;
                    for (const double offset : {0.0, -aside, aside}) {
                        QTransform placement;
                        placement.translate(centre.x(), centre.y());
                        placement.rotate(angle);
                        placement.translate(0, offset);
                        const Fit result = fit(label, placement);
                        if (result == Fits) {
                            drawLabel(label, placement, TextColour);
                            taken.append(placement.map(QPolygonF(label.rect)));
                            named.insert(candidate.shape->name);
                            placed = true;
                        }
                        if (result != OnRoute) {
                            break;
                        }
                    }
                    if (placed) {
                        break;
                    }
                }
                if (placed) {
                    break;
                }
            }
        }
    } else {
        // Names of places, the important ones first.
        QList<Drawn> &places = drawn[Place];
        std::stable_sort(places.begin(), places.end(), [](const Drawn &a, const Drawn &b) {
            return std::pair(a.shape->rank, a.shape->order) < std::pair(b.shape->rank, b.shape->order);
        });
        int count = 0;
        for (const Drawn &entry : std::as_const(places)) {
            const quint8 group = entry.shape->rank;
            if (count >= 40 || (group == Village && view.zoom < 10) || (group == Quarter && view.zoom < 12)) {
                continue;
            }
            static constexpr std::array<double, 4> sizes{3.6, 3.1, 2.7, 2.5};
            const Label label = makeLabel(entry.shape->name, sizes[group], group <= Town);
            const QPointF at = entry.parts.first().first();
            const double aside = label.rect.height() / 2 + RouteWidth;
            for (const double offset : {0.0, -aside, aside}) {
                const QTransform placement = QTransform::fromTranslate(at.x(), at.y() + offset);
                const Fit result = fit(label, placement);
                if (result == Fits) {
                    drawLabel(label, placement, TextColour);
                    taken.append(placement.map(QPolygonF(label.rect)));
                    ++count;
                }
                if (result != OnRoute) {
                    break;
                }
            }
        }
    }

    // Stops, each with its letter.
    for (const Marker &marker : overlay.markers) {
        const QPointF at = worldToPaper.map(toWorld(marker.position));
        painter->setPen(QPen(Qt::white, 0.4));
        painter->setBrush(Qt::black);
        painter->drawEllipse(at, MarkerRadius, MarkerRadius);
        const Label letter = makeLabel(marker.label, 3.4, true);
        painter->setPen(Qt::NoPen);
        painter->setBrush(Qt::white);
        painter->drawPath(letter.path.translated(at));
    }

    // Which way is north: a compass needle, its dark half pointing there.
    painter->setPen(QPen(QColor(FrameColour), 0.15));
    painter->setBrush(Qt::white);
    painter->drawEllipse(compass, CompassRadius, CompassRadius);
    painter->save();
    painter->translate(compass);
    painter->rotate(-view.bearing);
    painter->setPen(QPen(Qt::black, 0.15, Qt::SolidLine, Qt::RoundCap, Qt::MiterJoin));
    painter->drawPolygon(QPolygonF{{-0.8, 0}, {0, 2.0}, {0.8, 0}});
    painter->setBrush(Qt::black);
    painter->drawPolygon(QPolygonF{{-0.8, 0}, {0, -2.0}, {0.8, 0}});
    painter->restore();

    if (barLength > 0) {
        const QPolygonF bar{barStart + QPointF(0, -1), barStart, barStart + QPointF(barLength, 0), barStart + QPointF(barLength, -1)};
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(Qt::white, 1.0, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
        painter->drawPolyline(bar);
        painter->setPen(QPen(Qt::black, 0.35, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
        painter->drawPolyline(bar);
        drawLabel(barLabel, QTransform::fromTranslate(barStart.x() - barLabel.rect.left(), barStart.y() - 3.2), TextColour);
    }
    painter->restore();
}
