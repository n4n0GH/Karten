// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "printmap.h"
#include "roadbookdocument.h"

#include "tilewriter.h"

#include <QBuffer>
#include <QGuiApplication>
#include <QImage>
#include <QPageRanges>
#include <QPainter>
#include <QPdfWriter>
#include <QTest>

#include <numbers>

using namespace Qt::StringLiterals;
using namespace TileWriter;

namespace
{
constexpr PrintMap::TileId Tile{14, 8640, 5560}; // somewhere in Germany

/// The middle of the tile.
GeoPoint tileCentre()
{
    const double count = 1 << Tile.z;
    const double y = (Tile.y + 0.5) / count;
    return {std::atan(std::sinh(std::numbers::pi * (1 - 2 * y))) * 180 / std::numbers::pi, (Tile.x + 0.5) / count * 360 - 180};
}

/// A map of 40 mm at 10 m to the millimetre, as of a junction.
PrintMap::View junctionView(double bearing = 0)
{
    PrintMap::View view;
    view.centre = tileCentre();
    view.metresPerMm = 10;
    view.bearing = bearing;
    view.size = QSizeF(40, 40);
    view.zoom = Tile.z;
    return view;
}

GeoPoint north(GeoPoint from, double metres)
{
    return {from.lat + metres / 111320.0, from.lon};
}

GeoPoint east(GeoPoint from, double metres)
{
    return {from.lat, from.lon + metres / (111320.0 * std::cos(from.lat * Geo::DegToRad))};
}

/// A main road from west to east through the middle of the tile, and a
/// pond to the south-east of the middle.
QByteArray tileData()
{
    QPoint cursor;
    const QByteArray road = path({{0, 2048}, {4096, 2048}}, false, cursor);
    cursor = {};
    const QByteArray pond = path({{2250, 2250}, {2450, 2250}, {2450, 2450}, {2250, 2450}}, true, cursor);
    return bytes(3, layer("transportation", {shape(2, road, {0, 0})}, {"class"}, {text("primary")}))
        + bytes(3, layer("water", {shape(3, pond, {0, 0})}, {"class"}, {text("pond")}));
}

/// Draws a map at 10 pixels to the millimetre.
QImage draw(PrintMap &map, const PrintMap::View &view, const PrintMap::Overlay &overlay)
{
    QImage image(400, 400, QImage::Format_RGB32);
    image.fill(Qt::red);
    QPainter painter(&image);
    painter.scale(10, 10);
    map.paint(&painter, view, PrintMap::Junction, overlay);
    return image;
}

RoadbookDocument documentOf(int rows, int sections = 1)
{
    RoadbookDocument document;
    document.title = u"Start → Destination"_s;
    document.subtitle = u"12 km · 20 min · by car"_s;
    document.attribution = u"Map data © OpenStreetMap contributors"_s;
    for (int s = 0; s < sections; ++s) {
        RoadbookDocument::Section section;
        section.title = u"To a stop"_s;
        section.detail = u"6 km · 10 min"_s;
        for (int r = 0; r < rows; ++r) {
            section.rows.append({u"1.2 km"_s, u"+ 300 m"_s, u"Turn left onto Long Street"_s, u"Continue for 2 km"_s, QPicture()});
        }
        document.sections.append(section);
    }
    return document;
}

/// How many pages the roadbook has on a sheet, and how many of them a PDF file of it gets.
std::pair<int, int> pagesOf(const RoadbookDocument &document, QPageLayout::Orientation orientation, const QString &ranges = {})
{
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    int pages = 0;
    {
        QPdfWriter writer(&buffer);
        writer.setPageSize(QPageSize(QPageSize::A4));
        writer.setPageOrientation(orientation);
        writer.setPageMargins(QMarginsF(12, 12, 12, 12), QPageLayout::Millimeter);
        writer.setPageRanges(QPageRanges::fromString(ranges));
        pages = document.paint(&writer);
    }
    // Each page is an object of the type Page; their list is one of the type Pages.
    const QByteArray &file = buffer.data();
    int written = 0;
    for (qsizetype at = file.indexOf("/Type /Page"); at >= 0; at = file.indexOf("/Type /Page", at + 1)) {
        written += file.mid(at + 11, 1) != "s" ? 1 : 0;
    }
    return {pages, written};
}
}

class PrintTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void mapsPositionsOntoPaper()
    {
        const GeoPoint centre = tileCentre();
        const PrintMap::View northUp = junctionView();
        const auto near = [](QPointF a, QPointF b) {
            return std::abs(a.x() - b.x()) < 0.05 && std::abs(a.y() - b.y()) < 0.05;
        };
        QVERIFY(near(northUp.toPaper(centre), QPointF(20, 20)));
        // 100 m are 10 mm; north is up and east is to the right.
        QVERIFY(near(northUp.toPaper(north(centre, 100)), QPointF(20, 10)));
        QVERIFY(near(northUp.toPaper(east(centre, 100)), QPointF(30, 20)));

        // Travelling east, what lies ahead is up and north is to the left.
        const PrintMap::View eastUp = junctionView(90);
        QVERIFY(near(eastUp.toPaper(east(centre, 100)), QPointF(20, 10)));
        QVERIFY(near(eastUp.toPaper(north(centre, 100)), QPointF(10, 20)));
    }

    void choosesTiles()
    {
        // A map of 400 m lies well within the tile around its middle, however it is turned.
        QCOMPARE(junctionView().tiles(), QList<PrintMap::TileId>{Tile});
        QCOMPARE(junctionView(45).tiles(), QList<PrintMap::TileId>{Tile});

        PrintMap::View wide = junctionView();
        wide.metresPerMm = 100;
        QCOMPARE(wide.tiles().size(), 9);
        QVERIFY(wide.tiles().contains(Tile));

        // The closest tiles for a junction, coarser ones for a country, and never beyond what there is.
        QCOMPARE(PrintMap::View::zoomFor(10, 50, 0, 14), 14);
        QCOMPARE(PrintMap::View::zoomFor(5, 50, 0, 14), 14);
        const int country = PrintMap::View::zoomFor(4000, 50, 0, 14);
        QVERIFY(country >= 5 && country <= 7);
        QCOMPARE(PrintMap::View::zoomFor(4000, 50, 8, 14), 8);
    }

    void drawsTheMapUnderTheRoute()
    {
        const GeoPoint centre = tileCentre();
        PrintMap::Overlay overlay;
        overlay.line = {north(centre, -150), centre, north(centre, 150)};
        overlay.start = PrintMap::Dot;
        overlay.end = PrintMap::Arrow;

        PrintMap map(QGuiApplication::font().family());
        map.addTile(Tile, tileData());
        const QImage image = draw(map, junctionView(), overlay);
        const QColor paper(Qt::white);
        // The route runs from south to north through the middle, in black.
        QCOMPARE(image.pixelColor(200, 200), QColor(Qt::black));
        QCOMPARE(image.pixelColor(200, 300), QColor(Qt::black));
        // The road crosses it from west to east and is not white inside; beside it is paper.
        const QColor road = image.pixelColor(60, 200);
        QVERIFY(road != paper && road != QColor(Qt::black));
        QCOMPARE(image.pixelColor(340, 200), road);
        QCOMPARE(image.pixelColor(60, 120), paper);
        // The pond: 12 mm east and south of the middle at this scale.
        const QColor water = image.pixelColor(330, 330);
        QVERIFY(water != paper && water != road);
        QVERIFY(water.blue() > water.red());

        // Turned to face east, the road runs up the map and the route across.
        const QImage turned = draw(map, junctionView(90), overlay);
        QCOMPARE(turned.pixelColor(200, 60), road);
        QCOMPARE(turned.pixelColor(100, 200), QColor(Qt::black));
        QCOMPARE(turned.pixelColor(330, 70), water);

        // Without tiles there is the route on paper.
        PrintMap empty(QGuiApplication::font().family());
        const QImage plain = draw(empty, junctionView(), overlay);
        QCOMPARE(plain.pixelColor(200, 200), QColor(Qt::black));
        QCOMPARE(plain.pixelColor(60, 200), paper);
        QCOMPARE(plain.pixelColor(330, 330), paper);

        // Nor does a tile that is not one do any harm.
        PrintMap broken(QGuiApplication::font().family());
        broken.addTile(Tile, "not a vector tile at all");
        QCOMPARE(draw(broken, junctionView(), overlay).pixelColor(60, 200), paper);
    }

    void laysInstructionsOutOnPages()
    {
        // On A4 upright six instructions fit a page, and five the first, under the head.
        QCOMPARE(pagesOf(documentOf(5), QPageLayout::Portrait), std::pair(1, 1));
        QCOMPARE(pagesOf(documentOf(6), QPageLayout::Portrait), std::pair(2, 2));
        QCOMPARE(pagesOf(documentOf(11), QPageLayout::Portrait), std::pair(2, 2));
        QCOMPARE(pagesOf(documentOf(12), QPageLayout::Portrait), std::pair(3, 3));
        // On its side there are two columns of four, of three on the first page.
        QCOMPARE(pagesOf(documentOf(6), QPageLayout::Landscape), std::pair(1, 1));
        QCOMPARE(pagesOf(documentOf(14), QPageLayout::Landscape), std::pair(2, 2));
        QCOMPARE(pagesOf(documentOf(15), QPageLayout::Landscape), std::pair(3, 3));
        // A heading for each part of the route, when there are several, takes room too.
        QCOMPARE(pagesOf(documentOf(5, 2), QPageLayout::Portrait), std::pair(2, 2));
        QCOMPARE(pagesOf(documentOf(6, 2), QPageLayout::Portrait), std::pair(3, 3));
    }

    void printsThePagesAskedFor()
    {
        // Still three pages, of which the second and third are written.
        QCOMPARE(pagesOf(documentOf(12), QPageLayout::Portrait, u"2-3"_s), std::pair(3, 2));
        QCOMPARE(pagesOf(documentOf(12), QPageLayout::Portrait, u"3"_s), std::pair(3, 1));
    }

    void drawsNothingOfNothing()
    {
        QCOMPARE(pagesOf(RoadbookDocument(), QPageLayout::Portrait), std::pair(1, 1));
    }
};

int main(int argc, char **argv)
{
    // Text and painting need the application, but no display.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    PrintTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "printtest.moc"
