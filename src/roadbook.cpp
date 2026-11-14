// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "roadbook.h"

#include "tileserver.h"

#include <KLocalizedString>

#include <QFutureWatcher>
#include <QGuiApplication>
#include <QLocale>
#include <QPainter>
#include <QPointer>
#include <QPrintPreviewDialog>
#include <QPrinter>
#include <QSet>
#include <QTextDocumentFragment>
#include <QWindow>
#include <QtConcurrentRun>

#include <array>
#include <numbers>

using namespace Qt::StringLiterals;

namespace
{
Roadbook *s_instance = nullptr;

/// How many tiles are asked for at a time: as few as when an area is
/// downloaded, which the terms of the map sources are about.
constexpr int Requests = 2;
/// The overview map, in millimetres: as wide as a sheet has room for, and
/// as tall as the shape of the route asks for within these bounds.
constexpr double OverviewWidth = 180;
constexpr double OverviewMinHeight = 70;
constexpr double OverviewMaxHeight = 125;
constexpr double OverviewMargin = 9;

/// Web Mercator: how far south of the north edge of the world map a
/// latitude lies, from 0 to 1, and back.
double mercator(double latitude)
{
    return 0.5 - std::log(std::tan(std::numbers::pi / 4 + latitude * Geo::DegToRad / 2)) / (2 * std::numbers::pi);
}
double latitudeOf(double mercator)
{
    return std::atan(std::sinh(std::numbers::pi * (1 - 2 * mercator))) / Geo::DegToRad;
}

/// The point a distance along the route from one of its points, forwards
/// or backwards; the points passed on the way there are added to @p passed.
GeoPoint walk(const std::vector<GeoPoint> &path, size_t from, double metres, bool forwards, std::vector<GeoPoint> *passed = nullptr)
{
    GeoPoint here = path[from];
    for (size_t i = from; forwards ? i + 1 < path.size() : i > 0;) {
        i = forwards ? i + 1 : i - 1;
        const GeoPoint next = path[i];
        const double length = Geo::distance(here, next);
        if (length >= metres && length > 0) {
            // It ends between two points of the route.
            const double part = metres / length;
            here = {here.lat + (next.lat - here.lat) * part, here.lon + (next.lon - here.lon) * part};
            if (passed) {
                passed->push_back(here);
            }
            break;
        }
        metres -= length;
        here = next;
        if (passed) {
            passed->push_back(here);
        }
    }
    return here;
}

/// A round distance of at most @p metres for a scale bar, in the units
/// distances are shown in.
double roundDistance(double metres)
{
    static constexpr double Foot = 0.3048;
    static constexpr double Mile = 1609.344;
    static constexpr std::array metric{10.0, 20.0, 50.0, 100.0, 200.0, 500.0, 1e3, 2e3, 5e3, 1e4, 2e4, 5e4, 1e5, 2e5, 5e5};
    static constexpr std::array imperial{20 * Foot,
                                         50 * Foot,
                                         100 * Foot,
                                         200 * Foot,
                                         300 * Foot,
                                         0.1 * Mile,
                                         0.2 * Mile,
                                         0.5 * Mile,
                                         1 * Mile,
                                         2 * Mile,
                                         5 * Mile,
                                         10 * Mile,
                                         20 * Mile,
                                         50 * Mile,
                                         100 * Mile,
                                         200 * Mile,
                                         500 * Mile};
    double best = 0;
    if (QLocale().measurementSystem() == QLocale::MetricSystem) {
        for (const double candidate : metric) {
            best = candidate <= metres ? candidate : best;
        }
    } else {
        for (const double candidate : imperial) {
            best = candidate <= metres ? candidate : best;
        }
    }
    return best;
}

/// The distance from the start. An odometer is read to a tenth, so the
/// tenths stay also where a distance on its own is rounded to whole
/// kilometres or miles.
QString totalText(double metres)
{
    const QLocale locale;
    const bool metric = locale.measurementSystem() == QLocale::MetricSystem;
    const double amount = metres / (metric ? 1000.0 : 1609.344);
    if (amount < 100) {
        return RouteController::formatDistance(metres);
    }
    const QString number = locale.toString(amount, 'f', 1);
    return metric ? i18nc("distance in kilometres", "%1 km", number) : i18nc("distance in miles", "%1 mi", number);
}

QString modeText(RouteController::TravelMode mode)
{
    switch (mode) {
    case RouteController::Cycle:
        return i18nc("@info how a route is travelled", "by bicycle");
    case RouteController::Walk:
        return i18nc("@info how a route is travelled", "on foot");
    default:
        return i18nc("@info how a route is travelled", "by car");
    }
}
}

/// A roadbook in the making.
struct Roadbook::Preparation {
    RoadbookDocument document;
    QList<Job> maps; ///< the overview, then one for each instruction
    std::shared_ptr<PrintMap> map;
    TileServer::TileSource source;
    QList<PrintMap::TileId> tiles;
    qsizetype next = 0; ///< the first tile not asked for yet
    int inFlight = 0;
    bool pumping = false;
    bool missing = false;
    std::function<void(const RoadbookDocument &, bool)> done;
};

Roadbook::Roadbook(RouteController *routes, TileServer *server, QObject *parent)
    : QObject(parent)
    , m_routes(routes)
    , m_server(server)
{
    s_instance = this;
    connect(routes, &RouteController::routeChanged, this, &Roadbook::availableChanged);
}

Roadbook *Roadbook::create(QQmlEngine *, QJSEngine *)
{
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

bool Roadbook::available() const
{
    return m_routes->itinerary().isValid();
}

bool Roadbook::busy() const
{
    return m_busy;
}

void Roadbook::setBusy(bool busy)
{
    if (m_busy != busy) {
        m_busy = busy;
        Q_EMIT busyChanged();
    }
}

void Roadbook::prepare(const std::function<void(const RoadbookDocument &, bool)> &done)
{
    const RouteController::Itinerary route = m_routes->itinerary();
    if (m_busy || !route.isValid()) {
        done(RoadbookDocument(), false);
        return;
    }
    const auto preparation = std::make_shared<Preparation>();
    preparation->done = done;
    preparation->map = std::make_shared<PrintMap>(QGuiApplication::font().family());
    const QList<TileServer::TileSource> sources = m_server->tileSources();
    for (const TileServer::TileSource &source : sources) {
        if (source.vector) {
            preparation->source = source;
            break;
        }
    }
    const TileServer::TileSource &source = preparation->source;
    const auto zoomFor = [&source](double metresPerMm, double latitude) {
        return PrintMap::View::zoomFor(metresPerMm, latitude, source.minZoom, source.maxZoom);
    };
    const auto scaleBar = [](PrintMap::Overlay &overlay, double metres) {
        overlay.scaleBar = roundDistance(metres);
        overlay.scaleBarLabel = overlay.scaleBar > 0 ? RouteController::formatDistance(overlay.scaleBar) : QString();
    };

    // --- What it says ---
    RoadbookDocument &document = preparation->document;
    const QVariantList stops = m_routes->stops();
    QStringList stopTexts;
    QList<PrintMap::Marker> markers;
    for (qsizetype i = 0; i < stops.size(); ++i) {
        const QVariantMap stop = stops[i].toMap();
        const QString letter(QChar(char16_t(u'A' + i)));
        stopTexts.append(
            i18nc("@info a stop of a printed route; %1 is its letter on the map, %2 its name", "%1: %2", letter, stop.value(u"name"_s).toString()));
        markers.append({{stop.value(u"latitude"_s).toDouble(), stop.value(u"longitude"_s).toDouble()}, letter});
    }
    document.title = i18nc("@title of a printed route; %1 is where it starts, %2 where it ends", "%1 → %2", m_routes->fromName(), m_routes->toName());
    document.subtitle = i18nc("@info summary of a printed route: its length, how long it takes, how it is travelled",
                              "%1 · %2 · %3",
                              m_routes->distanceText(),
                              m_routes->durationText(),
                              modeText(m_routes->mode()));
    document.stops = stopTexts.join(i18nc("separates the stops of a printed route", " · "));
    document.attribution = QTextDocumentFragment::fromHtml(m_server->attribution()).toPlainText().simplified();
    if (document.attribution.isEmpty()) {
        document.attribution = i18nc("@info map attribution on a printed page", "Map data © OpenStreetMap contributors");
    }

    // --- The map of the whole route ---
    double south = 90;
    double north = -90;
    double west = 180;
    double east = -180;
    for (const GeoPoint &point : route.path) {
        south = std::min(south, point.lat);
        north = std::max(north, point.lat);
        west = std::min(west, point.lon);
        east = std::max(east, point.lon);
    }
    {
        PrintMap::View view;
        view.centre = {latitudeOf((mercator(south) + mercator(north)) / 2), (west + east) / 2};
        // The size of the route on the ground, at the scale of the middle of the map.
        const double metresPerWorld = 2 * std::numbers::pi * 6378137.0 * std::cos(view.centre.lat * Geo::DegToRad);
        const double across = std::max(1.0, (east - west) / 360 * metresPerWorld);
        const double down = std::max(1.0, (mercator(south) - mercator(north)) * metresPerWorld);
        const double height = std::clamp(OverviewWidth * down / across, OverviewMinHeight, OverviewMaxHeight);
        view.size = QSizeF(OverviewWidth, height);
        // Nearer than a junction map is of no use, however short the route.
        view.metresPerMm = std::max({across / (OverviewWidth - 2 * OverviewMargin), down / (height - 2 * OverviewMargin), 4.0});
        view.zoom = zoomFor(view.metresPerMm, view.centre.lat);
        PrintMap::Overlay overlay;
        overlay.line = route.path;
        overlay.markers = markers;
        scaleBar(overlay, view.metresPerMm * 35);
        document.overviewSize = view.size;
        preparation->maps.append({view, PrintMap::Overview, overlay});
    }

    // --- The instructions, each with the map of its junction ---
    // By car things come up faster and junctions are bigger.
    const double metresPerMm = m_routes->mode() == RouteController::Drive ? 10 : m_routes->mode() == RouteController::Cycle ? 7 : 5;
    const double across = RoadbookDocument::MapSize * metresPerMm;
    document.legend = i18nc("@info explains a printed roadbook; %1 is a distance",
                            "Each map shows %1 from side to side, with the direction of travel pointing up. "
                            "Large figures are the distance from the start, small ones the distance from the instruction before.",
                            RouteController::formatDistance(across));
    double total = 0;
    double interval = 0;
    bool first = true;
    QString street;
    for (const RouteController::Part &part : route.parts) {
        RoadbookDocument::Section section;
        section.title = part.title;
        section.detail = i18nc("@info length of a part of a route and how long it takes",
                               "%1 · %2",
                               RouteController::formatDistance(part.distance),
                               RouteController::formatDuration(part.duration));
        for (const RouteController::Step &step : part.steps) {
            RoadbookDocument::Row row;
            row.total = totalText(total);
            // Setting off again from a stop is no distance from arriving there.
            if (!first && interval > 0) {
                row.interval = i18nc("@info distance from the instruction before, in a printed roadbook", "+ %1", RouteController::formatDistance(interval));
            }
            row.text = step.text;
            if (step.distance > 0) {
                row.follow = i18nc("@info follows an instruction of a printed roadbook; %1 is a distance",
                                   "Continue for %1",
                                   RouteController::formatDistance(step.distance));
            }
            section.rows.append(row);

            const size_t at = std::min<size_t>(step.pathIndex, route.path.size() - 1);
            const bool departing = step.type == Maneuver::Depart;
            const bool arriving = step.type == Maneuver::Arrive;
            PrintMap::View view;
            view.centre = route.path[at];
            view.metresPerMm = metresPerMm;
            view.size = QSizeF(RoadbookDocument::MapSize, RoadbookDocument::MapSize);
            view.zoom = zoomFor(metresPerMm, view.centre.lat);
            // Up is the way one arrives at the junction; from a little
            // back, as the last metres may already bend into the turn.
            const double approach = across * 0.06;
            if (at == 0 || departing) {
                view.bearing = Geo::bearing(view.centre, walk(route.path, at, approach, true));
            } else {
                view.bearing = Geo::bearing(walk(route.path, at, approach, false), view.centre);
            }
            // The route from where it enters the map to where it leaves it,
            // as far as it concerns this instruction.
            const double reach = across * 0.42;
            PrintMap::Overlay overlay;
            std::vector<GeoPoint> before;
            if (!departing) {
                walk(route.path, at, reach, false, &before);
            }
            overlay.line.assign(before.rbegin(), before.rend());
            overlay.line.push_back(view.centre);
            if (!arriving) {
                walk(route.path, at, reach, true, &overlay.line);
            }
            overlay.start = PrintMap::Dot;
            overlay.end = arriving ? PrintMap::Target : PrintMap::Arrow;
            overlay.streets = {step.street, street};
            overlay.streets.removeAll(QString());
            scaleBar(overlay, metresPerMm * 12);
            preparation->maps.append({view, PrintMap::Junction, overlay});

            total += step.distance;
            interval = step.distance;
            first = false;
            if (!step.street.isEmpty()) {
                street = step.street;
            }
        }
        document.sections.append(section);
    }

    // --- The tiles under the maps ---
    if (source.vector) {
        QSet<PrintMap::TileId> wanted;
        for (const Job &job : std::as_const(preparation->maps)) {
            const QList<PrintMap::TileId> tiles = job.view.tiles();
            for (const PrintMap::TileId &id : tiles) {
                if (!wanted.contains(id)) {
                    wanted.insert(id);
                    preparation->tiles.append(id);
                }
            }
        }
    }
    // No more than the map source permits in one download. The overview
    // and the first instructions come first.
    if (preparation->tiles.size() > m_server->maxPackTiles()) {
        preparation->tiles.resize(m_server->maxPackTiles());
        preparation->missing = true;
    }
    setBusy(true);
    fetch(preparation);
}

void Roadbook::fetch(const std::shared_ptr<Preparation> &preparation)
{
    // Tiles that are on disk are answered at once, from within the call:
    // hence a loop that such answers do not start a second time.
    if (preparation->pumping) {
        return;
    }
    preparation->pumping = true;
    while (preparation->inFlight < Requests && preparation->next < preparation->tiles.size()) {
        const PrintMap::TileId id = preparation->tiles[preparation->next++];
        ++preparation->inFlight;
        const int row = preparation->source.tms ? (1 << id.z) - 1 - id.y : id.y;
        m_server->fetchTile(preparation->source.id, id.z, id.x, row, [this, preparation, id](const QByteArray &data, TileServer::Status status) {
            if (status == TileServer::Ok) {
                preparation->map->addTile(id, data);
            } else if (status == TileServer::Unavailable) {
                preparation->missing = true;
            }
            --preparation->inFlight;
            fetch(preparation);
        });
    }
    preparation->pumping = false;
    if (preparation->inFlight == 0 && preparation->next >= preparation->tiles.size()) {
        draw(preparation);
    }
}

void Roadbook::draw(const std::shared_ptr<Preparation> &preparation)
{
    auto *watcher = new QFutureWatcher<QList<QPicture>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, preparation] {
        watcher->deleteLater();
        const QList<QPicture> pictures = watcher->result();
        RoadbookDocument &document = preparation->document;
        qsizetype next = 0;
        document.overview = pictures.value(next++);
        for (RoadbookDocument::Section &section : document.sections) {
            for (RoadbookDocument::Row &row : section.rows) {
                row.map = pictures.value(next++);
            }
        }
        setBusy(false);
        preparation->done(document, preparation->missing);
    });
    // From here on the map belongs to the worker.
    watcher->setFuture(QtConcurrent::run([maps = preparation->maps, map = preparation->map] {
        QList<QPicture> pictures;
        pictures.reserve(maps.size());
        for (const Job &job : maps) {
            QPicture picture;
            QPainter painter(&picture);
            map->paint(&painter, job.view, job.style, job.overlay);
            painter.end();
            pictures.append(picture);
        }
        return pictures;
    }));
}

void Roadbook::print(QWindow *window)
{
    prepare([this, parent = QPointer<QWindow>(window)](const RoadbookDocument &document, bool mapsMissing) {
        if (document.sections.isEmpty()) {
            return;
        }
        if (mapsMissing) {
            Q_EMIT notice(i18nc("@info", "Parts of the map could not be loaded. Where it is missing, the roadbook shows the route alone."));
        }
        auto *printer = new QPrinter(QPrinter::HighResolution);
        printer->setDocName(document.title);
        printer->setPageMargins(QMarginsF(12, 12, 12, 12), QPageLayout::Millimeter);

        // Shows the pages as they will be, and prints them or saves them as a file.
        auto *dialog = new QPrintPreviewDialog(printer);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(i18nc("@title:window", "Print Roadbook"));
        connect(dialog, &QPrintPreviewDialog::paintRequested, dialog, [document](QPrinter *target) {
            document.paint(target);
        });
        connect(dialog, &QObject::destroyed, [printer] {
            delete printer;
        });
        if (parent) {
            // A dialog of widgets on a window of Qt Quick: they are only related as windows.
            dialog->winId();
            dialog->windowHandle()->setTransientParent(parent);
            dialog->setWindowModality(Qt::WindowModal);
            dialog->resize(parent->size().boundedTo(QSize(1000, 1100)));
        }
        dialog->open();
    });
}
