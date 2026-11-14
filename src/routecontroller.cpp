// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "routecontroller.h"

#include "extractimporter.h"
#include "network.h"
#include "offlinemanager.h"
#include "transitplanner.h"

#include <KConfigGroup>
#include <KLocalizedString>
#include <KSharedConfig>

#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <QtConcurrentRun>

using namespace Qt::StringLiterals;

namespace
{
RouteController *s_instance = nullptr;

/// The most recently used road graph stays in memory, as loading takes a while.
struct GraphCache {
    QMutex mutex;
    QString key;
    std::shared_ptr<const RoadGraph> graph;
};
Q_GLOBAL_STATIC(GraphCache, graphCache)

std::shared_ptr<const RoadGraph> loadGraph(const OfflineManager::RoutingRegion &region)
{
    const QString key = region.id + u'@' + QString::number(region.updated.toSecsSinceEpoch());
    const QMutexLocker lock(&graphCache->mutex);
    if (graphCache->key != key) {
        graphCache->key.clear();
        graphCache->graph.reset(); // free the old one before loading the next
        auto graph = std::make_shared<RoadGraph>();
        if (!graph->load(ExtractImporter::graphPath(region.directory))) {
            return nullptr;
        }
        graphCache->graph = graph;
        graphCache->key = key;
    }
    return graphCache->graph;
}

QString compassDirection(double bearing)
{
    switch (int(std::lround(bearing / 45.0)) % 8) {
    case 0:
        return i18nc("compass direction", "north");
    case 1:
        return i18nc("compass direction", "north-east");
    case 2:
        return i18nc("compass direction", "east");
    case 3:
        return i18nc("compass direction", "south-east");
    case 4:
        return i18nc("compass direction", "south");
    case 5:
        return i18nc("compass direction", "south-west");
    case 6:
        return i18nc("compass direction", "west");
    default:
        return i18nc("compass direction", "north-west");
    }
}

QString describe(const Maneuver &m)
{
    const bool named = !m.street.isEmpty();
    switch (m.type) {
    case Maneuver::Depart:
        return named ? i18nc("route instruction; %1 is a compass direction", "Head %1 on %2", compassDirection(m.bearing), m.street)
                     : i18nc("route instruction; %1 is a compass direction", "Head %1", compassDirection(m.bearing));
    case Maneuver::Continue:
        return named ? i18nc("route instruction", "Continue onto %1", m.street) : i18nc("route instruction", "Continue straight ahead");
    case Maneuver::SlightRight:
        return named ? i18nc("route instruction", "Keep right onto %1", m.street) : i18nc("route instruction", "Keep right");
    case Maneuver::Right:
        return named ? i18nc("route instruction", "Turn right onto %1", m.street) : i18nc("route instruction", "Turn right");
    case Maneuver::SharpRight:
        return named ? i18nc("route instruction", "Turn sharply right onto %1", m.street) : i18nc("route instruction", "Turn sharply right");
    case Maneuver::UTurn:
        return i18nc("route instruction", "Make a U-turn");
    case Maneuver::SharpLeft:
        return named ? i18nc("route instruction", "Turn sharply left onto %1", m.street) : i18nc("route instruction", "Turn sharply left");
    case Maneuver::Left:
        return named ? i18nc("route instruction", "Turn left onto %1", m.street) : i18nc("route instruction", "Turn left");
    case Maneuver::SlightLeft:
        return named ? i18nc("route instruction", "Keep left onto %1", m.street) : i18nc("route instruction", "Keep left");
    case Maneuver::Roundabout:
        if (m.exit > 0) {
            return named ? i18nc("route instruction; %1 is the exit number", "At the roundabout, take exit %1 onto %2", m.exit, m.street)
                         : i18nc("route instruction; %1 is the exit number", "At the roundabout, take exit %1", m.exit);
        }
        return i18nc("route instruction", "Enter the roundabout");
    case Maneuver::Arrive:
        return i18nc("route instruction", "Arrive at your destination");
    }
    return {};
}

/// The pictograms are Karten's own: see src/icons.
QString iconOf(Maneuver::Type type)
{
    switch (type) {
    case Maneuver::SlightRight:
        return u"turn-slight-right-symbolic"_s;
    case Maneuver::Right:
        return u"turn-right-symbolic"_s;
    case Maneuver::SharpRight:
        return u"turn-sharp-right-symbolic"_s;
    case Maneuver::UTurn:
        return u"turn-uturn-symbolic"_s;
    case Maneuver::SharpLeft:
        return u"turn-sharp-left-symbolic"_s;
    case Maneuver::Left:
        return u"turn-left-symbolic"_s;
    case Maneuver::SlightLeft:
        return u"turn-slight-left-symbolic"_s;
    case Maneuver::Roundabout:
        return u"turn-roundabout-symbolic"_s;
    case Maneuver::Arrive:
        return u"flag-symbolic"_s;
    default:
        return u"turn-straight-symbolic"_s;
    }
}

Maneuver::Type typeFromOsrm(const QString &type, const QString &modifier)
{
    if (type == "depart"_L1) {
        return Maneuver::Depart;
    }
    if (type == "arrive"_L1) {
        return Maneuver::Arrive;
    }
    if (type == "roundabout"_L1 || type == "rotary"_L1) {
        return Maneuver::Roundabout;
    }
    if (modifier == "uturn"_L1) {
        return Maneuver::UTurn;
    }
    if (modifier == "sharp right"_L1) {
        return Maneuver::SharpRight;
    }
    if (modifier == "right"_L1) {
        return Maneuver::Right;
    }
    if (modifier == "slight right"_L1) {
        return Maneuver::SlightRight;
    }
    if (modifier == "sharp left"_L1) {
        return Maneuver::SharpLeft;
    }
    if (modifier == "left"_L1) {
        return Maneuver::Left;
    }
    if (modifier == "slight left"_L1) {
        return Maneuver::SlightLeft;
    }
    return Maneuver::Continue;
}

}

/// Converts a response of the OSRM HTTP API. Returns an invalid journey on error.
RouteController::Journey RouteController::parseOsrm(const QByteArray &json)
{
    Journey journey;
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    if (root["code"_L1].toString() != "Ok"_L1) {
        return journey;
    }
    const QJsonObject first = root["routes"_L1][0].toObject();
    const QJsonArray coordinates = first["geometry"_L1]["coordinates"_L1].toArray();
    journey.path.reserve(size_t(coordinates.size()));
    for (const QJsonValue &c : coordinates) {
        journey.path.push_back({c[1].toDouble(), c[0].toDouble()});
    }

    quint32 searchFrom = 0;
    const QJsonArray legs = first["legs"_L1].toArray();
    for (const QJsonValue &legValue : legs) {
        Leg leg;
        leg.distance = legValue["distance"_L1].toDouble();
        leg.duration = legValue["duration"_L1].toDouble();
        const QJsonArray steps = legValue["steps"_L1].toArray();
        for (const QJsonValue &step : steps) {
            const QJsonObject maneuver = step["maneuver"_L1].toObject();
            const QString type = maneuver["type"_L1].toString();
            const double distance = step["distance"_L1].toDouble();
            const double duration = step["duration"_L1].toDouble();
            // Leaving a roundabout belongs to the instruction for entering it.
            if ((type == "exit roundabout"_L1 || type == "exit rotary"_L1) && !leg.maneuvers.empty()) {
                leg.maneuvers.back().distance += distance;
                leg.maneuvers.back().duration += duration;
                continue;
            }
            Maneuver m;
            m.type = typeFromOsrm(type, maneuver["modifier"_L1].toString());
            m.exit = maneuver["exit"_L1].toInt();
            m.street = step["name"_L1].toString();
            if (m.street.isEmpty()) {
                m.street = step["ref"_L1].toString(); // motorways often have a number only
            }
            m.bearing = maneuver["bearing_after"_L1].toDouble();
            m.distance = distance;
            m.duration = duration;
            // The service reports where each step happens rather than an index.
            const QJsonArray location = maneuver["location"_L1].toArray();
            const GeoPoint at{location[1].toDouble(), location[0].toDouble()};
            double best = std::numeric_limits<double>::max();
            for (quint32 i = searchFrom; i < journey.path.size(); ++i) {
                const double d = std::abs(journey.path[i].lat - at.lat) + std::abs(journey.path[i].lon - at.lon);
                if (d < best) {
                    best = d;
                    m.pathIndex = i;
                }
                if (d < 1e-7) {
                    break;
                }
            }
            searchFrom = m.pathIndex;
            leg.maneuvers.push_back(m);
        }
        if (!leg.maneuvers.empty()) {
            journey.legs.push_back(std::move(leg));
        }
    }
    if (journey.legs.empty()) {
        journey.path.clear();
    }
    return journey;
}

RouteController::RouteController(OfflineManager *offline, QObject *parent)
    : QObject(parent)
    , m_offline(offline)
    , m_transit(new TransitPlanner(this))
{
    s_instance = this;
    connect(m_transit, &TransitPlanner::finished, this, [this] {
        if (m_mode != Transit) {
            return;
        }
        if (!m_transit->errorText().isEmpty()) {
            setError(m_transit->errorText());
            return;
        }
        m_connections = m_transit->connections();
        showConnection(0);
    });
    connect(offline, &OfflineManager::routingRegionsChanged, this, [this] {
        {
            const QMutexLocker lock(&graphCache->mutex);
            graphCache->key.clear();
            graphCache->graph.reset();
        }
        if (complete()) {
            recalculate(); // newly downloaded data may make this route work offline
        }
    });
}

RouteController::~RouteController()
{
    if (m_cancel) {
        m_cancel->store(true);
    }
    if (m_previewCancel) {
        m_previewCancel->store(true);
    }
}

RouteController *RouteController::create(QQmlEngine *, QJSEngine *)
{
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

QGeoCoordinate RouteController::from() const
{
    return m_stops.first().coordinate;
}

QGeoCoordinate RouteController::to() const
{
    return m_stops.last().coordinate;
}

QString RouteController::fromName() const
{
    return m_stops.first().name;
}

QString RouteController::toName() const
{
    return m_stops.last().name;
}

QVariantList RouteController::stops() const
{
    QVariantList list;
    for (const Stop &stop : m_stops) {
        list.append(QVariantMap{
            {u"name"_s, stop.name},
            {u"latitude"_s, stop.coordinate.latitude()},
            {u"longitude"_s, stop.coordinate.longitude()},
            {u"valid"_s, stop.coordinate.isValid()},
        });
    }
    return list;
}

QVariantList RouteController::detours() const
{
    QVariantList list;
    for (qsizetype stop = 0; stop < m_stops.size(); ++stop) {
        for (qsizetype index = 0; index < m_stops[stop].detours.size(); ++index) {
            list.append(QVariantMap{
                {u"latitude"_s, m_stops[stop].detours[index].latitude()},
                {u"longitude"_s, m_stops[stop].detours[index].longitude()},
                {u"stop"_s, stop},
                {u"index"_s, index},
            });
        }
    }
    return list;
}

bool RouteController::complete() const
{
    return std::all_of(m_stops.cbegin(), m_stops.cend(), [](const Stop &stop) {
        return stop.coordinate.isValid();
    });
}

RouteController::TravelMode RouteController::mode() const
{
    return m_mode;
}

bool RouteController::busy() const
{
    return m_busy;
}

bool RouteController::hasRoute() const
{
    return !m_path.isEmpty();
}

QVariantList RouteController::path() const
{
    return m_path;
}

QGeoRectangle RouteController::bounds() const
{
    return m_bounds;
}

QVariantList RouteController::connections() const
{
    return m_connections;
}

int RouteController::connection() const
{
    return m_connection;
}

void RouteController::setConnection(int connection)
{
    if (connection != m_connection && connection >= 0 && connection < m_connections.size()) {
        showConnection(connection);
    }
}

QDateTime RouteController::transitTime() const
{
    return m_transitTime;
}

bool RouteController::arriveBy() const
{
    return m_arriveBy;
}

void RouteController::setTransitTime(const QDateTime &time, bool arriveBy)
{
    // Arriving "now" makes no sense; leaving now is what an unset time means.
    const bool by = arriveBy && time.isValid();
    if (time == m_transitTime && by == m_arriveBy) {
        return;
    }
    m_transitTime = time;
    m_arriveBy = by;
    Q_EMIT transitTimeChanged();
    if (m_mode == Transit) {
        recalculate();
    }
}

void RouteController::planTransit()
{
    // Timetables go from one place to another; stops on the way and detours
    // are for roads.
    setBusy(true);
    m_transit->plan(m_stops.first().coordinate, m_stops.first().name, m_stops.last().coordinate, m_stops.last().name, m_transitTime, m_arriveBy);
}

void RouteController::showConnection(int connection)
{
    const TransitPlanner::Details details = m_transit->details(connection);
    m_connection = connection;
    m_path = details.path;
    m_bounds = details.bounds;
    m_legs = details.legs;
    m_durationText = details.duration;
    m_distanceText = details.distance;
    // Nothing to take hold of: a connection cannot be bent.
    m_routePoints.clear();
    m_parts.clear();
    m_waypointPositions.clear();
    m_isOffline = false;
    m_errorText.clear();
    setBusy(false);
    Q_EMIT routeChanged();
}

QVariantList RouteController::legs() const
{
    return m_legs;
}

QString RouteController::distanceText() const
{
    return m_distanceText;
}

QString RouteController::durationText() const
{
    return m_durationText;
}

bool RouteController::offline() const
{
    return m_isOffline;
}

QString RouteController::errorText() const
{
    return m_errorText;
}

RouteController::Itinerary RouteController::itinerary() const
{
    return {m_routePoints, m_parts};
}

QString RouteController::formatDistance(double metres)
{
    const QLocale locale;
    if (locale.measurementSystem() == QLocale::MetricSystem) {
        if (metres < 995) {
            const int rounded = metres < 100 ? int(std::lround(metres / 5) * 5) : int(std::lround(metres / 10) * 10);
            return i18nc("distance in metres", "%1 m", rounded);
        }
        const double km = metres / 1000.0;
        return i18nc("distance in kilometres", "%1 km", locale.toString(km, 'f', km < 100 ? 1 : 0));
    }
    const double feet = metres / 0.3048;
    if (feet < 500) {
        return i18nc("distance in feet", "%1 ft", int(std::lround(feet / 10) * 10));
    }
    const double miles = metres / 1609.344;
    return i18nc("distance in miles", "%1 mi", locale.toString(miles, 'f', miles < 100 ? 1 : 0));
}

QString RouteController::formatDuration(double seconds)
{
    const int minutes = std::max(1, int(std::lround(seconds / 60.0)));
    if (minutes < 60) {
        return i18nc("duration in minutes", "%1 min", minutes);
    }
    return i18nc("duration in hours and minutes", "%1 h %2 min", minutes / 60, minutes % 60);
}

void RouteController::setMode(TravelMode mode)
{
    if (m_mode != mode) {
        m_mode = mode;
        clearPreview(); // it was for the other way of travelling
        Q_EMIT modeChanged();
        recalculate();
    }
}

void RouteController::setFrom(const QGeoCoordinate &coordinate, const QString &name)
{
    setStop(0, coordinate, name);
}

void RouteController::setTo(const QGeoCoordinate &coordinate, const QString &name)
{
    setStop(int(m_stops.size()) - 1, coordinate, name);
}

void RouteController::setStop(int index, const QGeoCoordinate &coordinate, const QString &name)
{
    if (index < 0 || index >= m_stops.size()) {
        return;
    }
    // Detours shaped the way to and from the old place.
    m_stops[index] = {coordinate, name, {}};
    if (index > 0) {
        m_stops[index - 1].detours.clear();
    }
    Q_EMIT endpointsChanged();
    recalculate();
}

void RouteController::addStop()
{
    // The public routing server takes a limited number of places at once.
    constexpr int MaxStops = 10;
    if (m_stops.size() >= MaxStops) {
        return;
    }
    m_stops.append(Stop());
    Q_EMIT endpointsChanged();
    recalculate();
}

void RouteController::removeStop(int index)
{
    if (index < 0 || index >= m_stops.size()) {
        return;
    }
    if (index > 0) {
        m_stops[index - 1].detours.clear();
    }
    if (m_stops.size() > 2) {
        m_stops.removeAt(index);
    } else {
        m_stops[index] = Stop();
    }
    Q_EMIT endpointsChanged();
    recalculate();
}

void RouteController::nameEndpoint(double latitude, double longitude, const QString &name)
{
    const QGeoCoordinate position(latitude, longitude);
    bool changed = false;
    for (Stop &stop : m_stops) {
        if (stop.coordinate == position) {
            stop.name = name;
            changed = true;
        }
    }
    if (changed) {
        Q_EMIT endpointsChanged();
    }
}

void RouteController::reorderStops(const QList<int> &order)
{
    QList<int> sorted = order;
    std::sort(sorted.begin(), sorted.end());
    bool valid = order.size() == m_stops.size();
    bool unchanged = true;
    for (int i = 0; valid && i < sorted.size(); ++i) {
        valid = sorted[i] == i;
        unchanged = unchanged && order[i] == i;
    }
    if (!valid || unchanged) {
        return;
    }
    QList<Stop> stops;
    for (const int index : order) {
        stops.append({m_stops[index].coordinate, m_stops[index].name, {}});
    }
    m_stops = stops;
    Q_EMIT endpointsChanged();
    recalculate();
}

bool RouteController::insertDetour(QList<Stop> &stops, const QGeoCoordinate &onRoute, const QGeoCoordinate &through) const
{
    if (m_routePoints.empty() || m_waypointPositions.empty() || !through.isValid()) {
        return false;
    }
    // Which stretch of the route was taken hold of.
    const GeoPoint grabbed{onRoute.latitude(), onRoute.longitude()};
    quint32 nearest = 0;
    double best = std::numeric_limits<double>::max();
    for (quint32 i = 0; i < m_routePoints.size(); ++i) {
        const double distance = Geo::distance(grabbed, m_routePoints[i]);
        if (distance < best) {
            best = distance;
            nearest = i;
        }
    }
    qsizetype stretch = 0;
    for (size_t i = 0; i < m_waypointPositions.size(); ++i) {
        if (m_waypointPositions[i] <= nearest) {
            stretch = qsizetype(i);
        }
    }
    // Stretches are counted across stops and detours alike.
    for (Stop &stop : stops) {
        if (stretch <= stop.detours.size()) {
            stop.detours.insert(stretch, through);
            return true;
        }
        stretch -= stop.detours.size() + 1;
    }
    return false;
}

void RouteController::addDetour(const QGeoCoordinate &onRoute, const QGeoCoordinate &through)
{
    if (insertDetour(m_stops, onRoute, through)) {
        Q_EMIT endpointsChanged();
        recalculate();
    }
}

void RouteController::previewDetour(const QGeoCoordinate &onRoute, const QGeoCoordinate &through)
{
    QList<Stop> stops = m_stops;
    if (complete() && insertDetour(stops, onRoute, through)) {
        startPreview(stops);
    }
}

void RouteController::previewMovedDetour(int stop, int index, const QGeoCoordinate &through)
{
    if (complete() && stop >= 0 && stop < m_stops.size() && index >= 0 && index < m_stops[stop].detours.size() && through.isValid()) {
        QList<Stop> stops = m_stops;
        stops[stop].detours[index] = through;
        startPreview(stops);
    }
}

void RouteController::startPreview(const QList<Stop> &stops)
{
    m_previewWanted = placesOf(stops);
    if (!m_previewRunning) {
        runPreview();
    }
}

void RouteController::runPreview()
{
    // One at a time, always for the latest position: the pointer moves
    // faster than routes can be planned, online at least.
    const Places places = std::exchange(m_previewWanted, {});
    if (places.empty()) {
        return;
    }
    m_previewRunning = true;
    m_previewCancel = std::make_shared<std::atomic_bool>(false);
    plan(places, m_previewCancel, [this, places](const Outcome &outcome) {
        m_previewRunning = false;
        m_previewPlaces = places;
        m_previewOutcome = outcome;
        m_previewPath.clear();
        for (const GeoPoint &p : outcome.journey.path) {
            m_previewPath.append(QVariant::fromValue(QGeoCoordinate(p.lat, p.lon)));
        }
        Q_EMIT previewChanged();
        runPreview();
    });
}

void RouteController::clearPreview()
{
    m_previewWanted.clear();
    if (m_previewCancel) {
        m_previewCancel->store(true);
        m_previewCancel.reset();
    }
    m_previewRunning = false;
    m_previewPlaces.clear();
    m_previewOutcome = {};
    if (!m_previewPath.isEmpty()) {
        m_previewPath.clear();
        Q_EMIT previewChanged();
    }
}

QVariantList RouteController::previewPath() const
{
    return m_previewPath;
}

RouteController::Places RouteController::placesOf(const QList<Stop> &stops)
{
    Places places;
    for (const Stop &stop : stops) {
        places.push_back({stop.coordinate.latitude(), stop.coordinate.longitude()});
        for (const QGeoCoordinate &detour : stop.detours) {
            places.push_back({detour.latitude(), detour.longitude()});
        }
    }
    return places;
}

void RouteController::moveDetour(int stop, int index, const QGeoCoordinate &through)
{
    if (stop >= 0 && stop < m_stops.size() && index >= 0 && index < m_stops[stop].detours.size() && through.isValid()) {
        m_stops[stop].detours[index] = through;
        Q_EMIT endpointsChanged();
        recalculate();
    }
}

void RouteController::removeDetour(int stop, int index)
{
    if (stop >= 0 && stop < m_stops.size() && index >= 0 && index < m_stops[stop].detours.size()) {
        m_stops[stop].detours.removeAt(index);
        Q_EMIT endpointsChanged();
        recalculate();
    }
}

double RouteController::distanceToRoute(const QGeoCoordinate &position) const
{
    // On a flat sheet around the position, which is exact enough nearby.
    const double metresPerLatitude = Geo::EarthRadius * Geo::DegToRad;
    const double metresPerLongitude = metresPerLatitude * std::cos(position.latitude() * Geo::DegToRad);
    const auto flat = [&](GeoPoint p) {
        return std::pair{(p.lon - position.longitude()) * metresPerLongitude, (p.lat - position.latitude()) * metresPerLatitude};
    };
    double best = std::numeric_limits<double>::infinity();
    for (size_t i = 0; i + 1 < m_routePoints.size(); ++i) {
        const auto [ax, ay] = flat(m_routePoints[i]);
        const auto [bx, by] = flat(m_routePoints[i + 1]);
        const double dx = bx - ax;
        const double dy = by - ay;
        const double length = dx * dx + dy * dy;
        // The nearest point of the segment to the origin.
        const double t = length > 0 ? std::clamp(-(ax * dx + ay * dy) / length, 0.0, 1.0) : 0.0;
        best = std::min(best, std::hypot(ax + t * dx, ay + t * dy));
    }
    return best;
}

void RouteController::swap()
{
    // The detours of each stretch stay with it, in the opposite direction.
    QList<QList<QGeoCoordinate>> detours;
    for (const Stop &stop : std::as_const(m_stops)) {
        detours.prepend(stop.detours);
        std::reverse(detours.first().begin(), detours.first().end());
    }
    std::reverse(m_stops.begin(), m_stops.end());
    for (qsizetype i = 0; i < m_stops.size(); ++i) {
        // What was attached to a stop led to the one after it.
        m_stops[i].detours = i + 1 < detours.size() ? detours[i + 1] : QList<QGeoCoordinate>();
    }
    Q_EMIT endpointsChanged();
    recalculate();
}

void RouteController::clear()
{
    m_stops = QList<Stop>(2);
    Q_EMIT endpointsChanged();
    recalculate();
}

void RouteController::setBusy(bool busy)
{
    if (m_busy != busy) {
        m_busy = busy;
        Q_EMIT busyChanged();
    }
}

void RouteController::setError(const QString &error)
{
    m_path.clear();
    m_legs.clear();
    m_connections.clear();
    m_connection = 0;
    m_routePoints.clear();
    m_parts.clear();
    m_waypointPositions.clear();
    m_bounds = {};
    m_distanceText.clear();
    m_durationText.clear();
    m_errorText = error;
    setBusy(false);
    Q_EMIT routeChanged();
}

void RouteController::setRoute(const Journey &journey, bool offline)
{
    m_path.clear();
    m_path.reserve(qsizetype(journey.path.size()));
    double south = 90;
    double north = -90;
    double west = 180;
    double east = -180;
    for (const GeoPoint &p : journey.path) {
        m_path.append(QVariant::fromValue(QGeoCoordinate(p.lat, p.lon)));
        south = std::min(south, p.lat);
        north = std::max(north, p.lat);
        west = std::min(west, p.lon);
        east = std::max(east, p.lon);
    }
    m_bounds = QGeoRectangle(QGeoCoordinate(north, west), QGeoCoordinate(south, east));

    m_connections.clear();
    m_connection = 0;
    m_routePoints = journey.path;
    m_waypointPositions.clear();
    for (const Leg &part : journey.legs) {
        m_waypointPositions.push_back(part.maneuvers.front().pathIndex);
    }

    // The router plans from each stop or detour to the next. What is shown
    // goes from stop to stop, so join the parts that only pass a detour.
    std::vector<Leg> legs;
    size_t expected = 0;
    for (qsizetype i = 0; i + 1 < m_stops.size(); ++i) {
        expected += size_t(m_stops[i].detours.size()) + 1;
    }
    if (expected == journey.legs.size()) {
        size_t next = 0;
        for (qsizetype i = 0; i + 1 < m_stops.size(); ++i) {
            Leg joined;
            for (qsizetype part = 0; part <= m_stops[i].detours.size(); ++part) {
                const Leg &piece = journey.legs[next++];
                joined.distance += piece.distance;
                joined.duration += piece.duration;
                if (joined.maneuvers.empty()) {
                    joined.maneuvers = piece.maneuvers;
                    continue;
                }
                // Passing a detour is neither an arrival nor a departure.
                if (joined.maneuvers.back().type == Maneuver::Arrive) {
                    joined.maneuvers.pop_back();
                }
                for (Maneuver maneuver : piece.maneuvers) {
                    if (maneuver.type == Maneuver::Depart) {
                        maneuver.type = Maneuver::Continue;
                    }
                    joined.maneuvers.push_back(maneuver);
                }
            }
            legs.push_back(std::move(joined));
        }
    } else {
        legs = journey.legs;
    }

    m_legs.clear();
    m_parts.clear();
    double distance = 0;
    double duration = 0;
    for (size_t index = 0; index < legs.size(); ++index) {
        const Leg &leg = legs[index];
        // Each leg ends at the stop after the one it starts from.
        const QString target = m_stops.value(qsizetype(index) + 1).name;
        const bool last = index + 1 == legs.size();
        const QString title =
            target.isEmpty() ? i18nc("@title part of a route", "Directions") : i18nc("@title part of a route; %1 is a place", "To %1", target);
        Part part{title, leg.distance, leg.duration, {}};
        QVariantList steps;
        for (const Maneuver &m : leg.maneuvers) {
            const quint32 pathIndex = quint32(std::min<size_t>(m.pathIndex, journey.path.size() - 1));
            const GeoPoint at = journey.path[pathIndex];
            QString text = describe(m);
            if (m.type == Maneuver::Arrive && !last && !target.isEmpty()) {
                text = i18nc("route instruction; %1 is a stop on the way", "Arrive at %1", target);
            }
            part.steps.push_back({m.type, text, m.street, m.type == Maneuver::Arrive ? 0 : m.distance, pathIndex});
            steps.append(QVariantMap{
                {u"text"_s, text},
                {u"distance"_s, m.type == Maneuver::Arrive ? QString() : formatDistance(m.distance)},
                {u"icon"_s, iconOf(m.type)},
                {u"latitude"_s, at.lat},
                {u"longitude"_s, at.lon},
            });
        }
        m_legs.append(QVariantMap{
            {u"title"_s, title},
            {u"distance"_s, formatDistance(leg.distance)},
            {u"duration"_s, formatDuration(leg.duration)},
            {u"steps"_s, steps},
        });
        m_parts.push_back(std::move(part));
        distance += leg.distance;
        duration += leg.duration;
    }
    m_distanceText = formatDistance(distance);
    m_durationText = formatDuration(duration);
    m_isOffline = offline;
    m_errorText.clear();
    setBusy(false);
    Q_EMIT routeChanged();
}

void RouteController::recalculate()
{
    if (m_cancel) {
        m_cancel->store(true);
        m_cancel.reset();
    }
    m_transit->clear();
    if (!complete()) {
        setError({});
        return;
    }
    if (m_mode == Transit) {
        planTransit();
        return;
    }
    const Places places = placesOf(m_stops);
    const auto finish = [this](const Outcome &outcome) {
        if (outcome.journey.isValid()) {
            setRoute(outcome.journey, outcome.offline);
        } else if (outcome.serviceAnswered) {
            // The connection is fine: there is no way through.
            setError(i18nc("@info", "No route was found between these places."));
        } else if (outcome.hadOfflineData) {
            setError(i18nc("@info", "No route was found in the downloaded data, and the online routing service could not be reached."));
        } else {
            setError(i18nc("@info", "The online routing service could not be reached. Download this region to plan routes offline."));
        }
    };

    // A detour that was tried out just now has been planned already.
    const bool previewed =
        places.size() == m_previewPlaces.size() && std::equal(places.cbegin(), places.cend(), m_previewPlaces.cbegin(), [](GeoPoint a, GeoPoint b) {
            return a.lat == b.lat && a.lon == b.lon;
        });
    if (previewed && m_previewOutcome.journey.isValid()) {
        const Outcome outcome = m_previewOutcome;
        clearPreview();
        finish(outcome);
        return;
    }

    setBusy(true);
    m_cancel = std::make_shared<std::atomic_bool>(false);
    plan(places, m_cancel, finish);
}

void RouteController::plan(const Places &places, const Cancel &cancel, const std::function<void(const Outcome &)> &done)
{
    QList<OfflineManager::RoutingRegion> candidates;
    const QList<OfflineManager::RoutingRegion> regions = m_offline->routingRegions();
    for (const OfflineManager::RoutingRegion &region : regions) {
        const bool coversAll = std::all_of(places.cbegin(), places.cend(), [&region](GeoPoint p) {
            return p.lat >= region.south && p.lat <= region.north && p.lon >= region.west && p.lon <= region.east;
        });
        if (coversAll) {
            candidates.append(region);
        }
    }
    if (candidates.isEmpty()) {
        planOnline(places, cancel, false, done);
        return;
    }

    const Mode mode = m_mode == Drive ? Car : m_mode == Cycle ? Bike : Foot;
    auto *watcher = new QFutureWatcher<Journey>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [=, this] {
        watcher->deleteLater();
        if (cancel->load()) {
            return;
        }
        const Journey journey = watcher->result();
        if (journey.isValid()) {
            done({journey, true, true, false});
        } else {
            planOnline(places, cancel, true, done);
        }
    });
    watcher->setFuture(QtConcurrent::run([candidates, places, mode, cancel] {
        for (const OfflineManager::RoutingRegion &region : candidates) {
            const std::shared_ptr<const RoadGraph> graph = loadGraph(region);
            if (!graph || cancel->load()) {
                continue;
            }
            // One leg after the other, joined into a single line.
            Journey journey;
            for (size_t i = 0; i + 1 < places.size(); ++i) {
                const Route route = Router(*graph).route(places[i], places[i + 1], mode, cancel.get());
                if (!route.isValid()) {
                    journey = {};
                    break;
                }
                // The end of one leg is the start of the next.
                const size_t skip = journey.path.empty() ? 0 : 1;
                const quint32 offset = quint32(journey.path.size() - skip);
                journey.path.insert(journey.path.end(), route.path.begin() + qsizetype(skip), route.path.end());
                Leg leg;
                leg.distance = route.distance;
                leg.duration = route.duration;
                leg.maneuvers = route.maneuvers;
                for (Maneuver &maneuver : leg.maneuvers) {
                    maneuver.pathIndex += offset;
                }
                journey.legs.push_back(std::move(leg));
            }
            if (journey.isValid()) {
                return journey;
            }
        }
        return Journey();
    }));
}

void RouteController::planOnline(const Places &places, const Cancel &cancel, bool hadOfflineData, const std::function<void(const Outcome &)> &done)
{
    // The public routing server allows one request per second.
    const qint64 wait = m_lastOnlineRequest.isValid() ? std::max<qint64>(0, 1100 - m_lastOnlineRequest.elapsed()) : 0;
    m_lastOnlineRequest.start();
    QTimer::singleShot(wait, this, [=, this] {
        if (cancel->load()) {
            return;
        }
        // Configurable so that the service can be switched without an update.
        const KConfigGroup config = KSharedConfig::openConfig()->group(u"Services"_s);
        const QString base = config.readEntry("RoutingUrl", u"https://routing.openstreetmap.de/routed-%1/route/v1/driving/"_s);
        const QString profile = m_mode == Drive ? u"car"_s : m_mode == Cycle ? u"bike"_s : u"foot"_s;
        QStringList coordinates;
        for (const GeoPoint &place : places) {
            coordinates.append(u"%1,%2"_s.arg(place.lon, 0, 'f', 6).arg(place.lat, 0, 'f', 6));
        }
        const QString url = (base.contains("%1"_L1) ? base.arg(profile) : base) + coordinates.join(u';') + u"?overview=full&geometries=geojson&steps=true"_s;
        QNetworkReply *reply = Network::manager()->get(Network::request(QUrl(url)));
        connect(reply, &QNetworkReply::finished, this, [=] {
            reply->deleteLater();
            if (cancel->load()) {
                return;
            }
            const QByteArray body = reply->readAll();
            Outcome outcome;
            outcome.journey = parseOsrm(body);
            outcome.hadOfflineData = hadOfflineData;
            outcome.serviceAnswered = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() >= 400 || !body.isEmpty();
            done(outcome);
        });
    });
}
