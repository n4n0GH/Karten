// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "transitplanner.h"

#include "network.h"
#include "routecontroller.h"

#include <KLocalizedString>
#include <KPublicTransport/Backend>
#include <KPublicTransport/JourneyReply>
#include <KPublicTransport/JourneyRequest>
#include <KPublicTransport/Line>
#include <KPublicTransport/Location>
#include <KPublicTransport/Manager>
#include <KPublicTransport/Path>
#include <KPublicTransport/Stopover>

#include <QLocale>
#include <QNetworkAccessManager>

using namespace KPublicTransport;
using namespace Qt::StringLiterals;

namespace
{
QString clock(const QDateTime &time)
{
    // In the time of the place, which is how timetables are read.
    return QLocale().toString(time.time(), QLocale::ShortFormat);
}

/// The time to go by: what is expected now, if the service says.
QDateTime departureOf(const JourneySection &section)
{
    return section.hasExpectedDepartureTime() ? section.expectedDepartureTime() : section.scheduledDepartureTime();
}

QDateTime arrivalOf(const JourneySection &section)
{
    return section.hasExpectedArrivalTime() ? section.expectedArrivalTime() : section.scheduledArrivalTime();
}

QString delayNote(int minutes)
{
    return minutes > 0 ? i18ncp("delay of a train or bus, appended to a time", " (+%1 min)", " (+%1 min)", minutes) : QString();
}

QVariantMap step(const QString &text, const QString &note, const QString &icon, const Location &at)
{
    return {
        {u"text"_s, text},
        {u"distance"_s, note}, // the column on the right: a time here
        {u"icon"_s, icon},
        {u"latitude"_s, at.latitude()},
        {u"longitude"_s, at.longitude()},
    };
}

QString stopName(const Location &location, const QString &fallback)
{
    return location.name().isEmpty() ? fallback : location.name();
}
}

TransitPlanner::TransitPlanner(QObject *parent)
    : QObject(parent)
{
}

TransitPlanner::~TransitPlanner() = default;

Manager *TransitPlanner::manager()
{
    if (!m_manager) {
        m_manager = new Manager(this);
        // Requests that say who is asking, as the service requires.
        m_manager->setNetworkAccessManager(Network::identifyingManager(this));
        // One service only: see the class documentation.
        m_manager->setBackendsEnabledByDefault(false);
        for (const Backend &backend : m_manager->backends()) {
            if (backend.name() == "Transitous"_L1) {
                m_manager->setBackendEnabled(backend.identifier(), true);
            }
        }
    }
    return m_manager;
}

bool TransitPlanner::isBusy() const
{
    return m_reply != nullptr;
}

QString TransitPlanner::errorText() const
{
    return m_errorText;
}

void TransitPlanner::clear()
{
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    m_journeys.clear();
    m_errorText.clear();
}

void TransitPlanner::plan(const QGeoCoordinate &from,
                          const QString &fromName,
                          const QGeoCoordinate &to,
                          const QString &toName,
                          const QDateTime &when,
                          bool arriveBy)
{
    clear();
    m_fromName = fromName;
    m_toName = toName;

    Location start;
    start.setCoordinate(from.latitude(), from.longitude());
    start.setName(fromName);
    Location destination;
    destination.setCoordinate(to.latitude(), to.longitude());
    destination.setName(toName);

    JourneyRequest request(start, destination);
    request.setDateTime(when.isValid() ? when : QDateTime::currentDateTime());
    request.setDateTimeMode(arriveBy ? JourneyRequest::Arrival : JourneyRequest::Departure);
    request.setIncludeIntermediateStops(true);
    request.setIncludePaths(true);
    request.setMaximumResults(6);

    m_reply = manager()->queryJourney(request);
    connect(m_reply, &Reply::finished, this, [this, reply = m_reply] {
        reply->deleteLater();
        if (reply != m_reply) {
            return;
        }
        m_reply = nullptr;
        m_journeys = reply->takeResult();
        // Walking all the way is not what was asked for.
        std::erase_if(m_journeys, [](const Journey &journey) {
            const auto &sections = journey.sections();
            return std::none_of(sections.cbegin(), sections.cend(), [](const JourneySection &section) {
                return section.mode() == JourneySection::PublicTransport;
            });
        });
        std::stable_sort(m_journeys.begin(), m_journeys.end(), [](const Journey &a, const Journey &b) {
            return a.scheduledDepartureTime() < b.scheduledDepartureTime();
        });

        m_errorText.clear();
        if (m_journeys.empty()) {
            switch (reply->error()) {
            case Reply::NetworkError:
                m_errorText = i18nc("@info", "The public transport service could not be reached. Connections need an internet connection.");
                break;
            case Reply::NoBackend:
                m_errorText = i18nc("@info", "There is no public transport information for these places.");
                break;
            default:
                m_errorText = i18nc("@info", "No public transport connection was found between these places at that time.");
            }
        }
        Q_EMIT finished();
    });
}

QVariantList TransitPlanner::connections() const
{
    QVariantList list;
    for (const Journey &journey : m_journeys) {
        QStringList lines;
        for (const JourneySection &section : journey.sections()) {
            if (section.mode() == JourneySection::PublicTransport) {
                const QString name = section.route().line().name();
                lines.append(name.isEmpty() ? section.route().line().modeString() : name);
            }
        }
        lines.removeAll(QString());
        const QDateTime departure = journey.hasExpectedDepartureTime() ? journey.expectedDepartureTime() : journey.scheduledDepartureTime();
        const QDateTime arrival = journey.hasExpectedArrivalTime() ? journey.expectedArrivalTime() : journey.scheduledArrivalTime();
        list.append(QVariantMap{
            {u"departure"_s, clock(departure)},
            {u"arrival"_s, clock(arrival)},
            {u"duration"_s, RouteController::formatDuration(double(departure.secsTo(arrival)))},
            {u"changes"_s, journey.numberOfChanges()},
            {u"lines"_s, lines},
            {u"delayed"_s, journey.departureDelay() > 0 || journey.arrivalDelay() > 0},
        });
    }
    return list;
}

TransitPlanner::Details TransitPlanner::details(int connection) const
{
    Details details;
    if (connection < 0 || size_t(connection) >= m_journeys.size()) {
        return details;
    }
    const Journey &journey = m_journeys[size_t(connection)];

    double south = 90;
    double north = -90;
    double west = 180;
    double east = -180;
    const auto addPoint = [&](double latitude, double longitude) {
        if (std::isnan(latitude) || std::isnan(longitude)) {
            return;
        }
        details.path.append(QVariant::fromValue(QGeoCoordinate(latitude, longitude)));
        south = std::min(south, latitude);
        north = std::max(north, latitude);
        west = std::min(west, longitude);
        east = std::max(east, longitude);
    };

    const QString stopIcon = u"transit-stop-symbolic"_s;
    const auto &sections = journey.sections();
    for (size_t index = 0; index < sections.size(); ++index) {
        const JourneySection &section = sections[index];
        // The line on the map: the way as the service gives it, or else
        // straight from stop to stop.
        bool drawn = false;
        const Path way = section.path();
        for (const PathSection &part : way.sections()) {
            for (const QPointF &point : part.path()) {
                addPoint(point.y(), point.x());
                drawn = true;
            }
        }
        if (!drawn) {
            addPoint(section.from().latitude(), section.from().longitude());
            for (const Stopover &stop : section.intermediateStops()) {
                addPoint(stop.stopPoint().latitude(), stop.stopPoint().longitude());
            }
            addPoint(section.to().latitude(), section.to().longitude());
        }

        const QDateTime departure = departureOf(section);
        const QDateTime arrival = arrivalOf(section);
        const QString duration = RouteController::formatDuration(double(std::max<qint64>(60, departure.secsTo(arrival))));
        // The ends of the journey are the places the user chose, by the names
        // they chose them by; the service only has placeholders for those.
        const QString from = index == 0 && !m_fromName.isEmpty() ? m_fromName : stopName(section.from(), i18nc("@item a place without a name", "the start"));
        const QString to =
            index + 1 == sections.size() && !m_toName.isEmpty() ? m_toName : stopName(section.to(), i18nc("@item a place without a name", "the destination"));

        QVariantMap leg{{u"duration"_s, duration}, {u"distance"_s, QString()}, {u"steps"_s, QVariantList()}};
        switch (section.mode()) {
        case JourneySection::PublicTransport: {
            const Line line = section.route().line();
            const QString name = line.name().isEmpty() ? line.modeString() : line.name();
            const QString direction = section.route().direction();
            leg[u"title"_s] = direction.isEmpty() ? name : i18nc("@title a line and where it is headed, as on the vehicle", "%1 to %2", name, direction);
            leg[u"detail"_s] = i18nc("@info departure time and stop, arrival time and stop",
                                     "%1 %2 – %3 %4",
                                     clock(departure) + delayNote(section.departureDelay()),
                                     from,
                                     clock(arrival) + delayNote(section.arrivalDelay()),
                                     to);
            leg[u"icon"_s] = section.iconName();

            QVariantList steps;
            const QString platform = section.hasExpectedDeparturePlatform() ? section.expectedDeparturePlatform() : section.scheduledDeparturePlatform();
            steps.append(step(platform.isEmpty() ? i18nc("@item boarding a train or bus", "Board at %1", from)
                                                 : i18nc("@item boarding a train or bus; %2 is a platform", "Board at %1, platform %2", from, platform),
                              clock(departure),
                              stopIcon,
                              section.from()));
            for (const Stopover &stop : section.intermediateStops()) {
                const QDateTime time = stop.hasExpectedArrivalTime() ? stop.expectedArrivalTime()
                    : stop.scheduledArrivalTime().isValid()          ? stop.scheduledArrivalTime()
                                                                     : stop.scheduledDepartureTime();
                steps.append(step(stop.stopPoint().name(), time.isValid() ? clock(time) : QString(), stopIcon, stop.stopPoint()));
            }
            steps.append(step(i18nc("@item leaving a train or bus", "Get off at %1", to), clock(arrival), u"flag-symbolic"_s, section.to()));
            leg[u"steps"_s] = steps;
            break;
        }
        case JourneySection::Walking:
            leg[u"title"_s] = i18nc("@title part of a journey", "Walk to %1", to);
            leg[u"distance"_s] = section.distance() > 0 ? RouteController::formatDistance(section.distance()) : QString();
            leg[u"icon"_s] = u"travel-walk-symbolic"_s;
            break;
        case JourneySection::Transfer:
            leg[u"title"_s] = i18nc("@title part of a journey", "Change at %1", to);
            leg[u"distance"_s] = section.distance() > 0 ? RouteController::formatDistance(section.distance()) : QString();
            leg[u"icon"_s] = u"travel-walk-symbolic"_s;
            break;
        case JourneySection::Waiting:
            leg[u"title"_s] = i18nc("@title part of a journey", "Wait at %1", from);
            leg[u"icon"_s] = u"clock-symbolic"_s;
            break;
        default:
            leg[u"title"_s] = i18nc("@title part of a journey", "Continue to %1", to);
            leg[u"icon"_s] = section.iconName();
        }
        details.legs.append(leg);
    }

    if (!details.path.isEmpty()) {
        details.bounds = QGeoRectangle(QGeoCoordinate(north, west), QGeoCoordinate(south, east));
    }
    const QDateTime departure = journey.hasExpectedDepartureTime() ? journey.expectedDepartureTime() : journey.scheduledDepartureTime();
    const QDateTime arrival = journey.hasExpectedArrivalTime() ? journey.expectedArrivalTime() : journey.scheduledArrivalTime();
    details.duration = RouteController::formatDuration(double(departure.secsTo(arrival)));
    details.distance = i18nc("@info departure and arrival time of a journey", "%1 – %2", clock(departure), clock(arrival));
    return details;
}
