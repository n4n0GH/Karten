// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "geocoder.h"

#include "extractimporter.h"
#include "graphbuilder.h"
#include "network.h"
#include "offlinemanager.h"
#include "tileserver.h"
#include "vectortile.h"

#include <KConfigGroup>
#include <KCountry>
#include <KLocalizedString>
#include <KOpeningHours/Display>
#include <KOpeningHours/Interval>
#include <KOpeningHours/OpeningHours>
#include <KSharedConfig>
#include <KTimeZone>

#include <QGeoCoordinate>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTimeZone>
#include <QTimer>
#include <QUrlQuery>

#include <cmath>

using namespace Qt::StringLiterals;

namespace
{
Geocoder *s_instance = nullptr;
constexpr int MaxResults = 10;

QString kindLabel(int kind, const QString &type)
{
    switch (kind) {
    case Place::City:
        return i18nc("kind of place", "City");
    case Place::Town:
        return i18nc("kind of place", "Town");
    case Place::Village:
        return i18nc("kind of place", "Village");
    case Place::Suburb:
        return i18nc("kind of place", "Neighborhood");
    case Place::Locality:
        return i18nc("kind of place", "Locality");
    case Place::Street:
        return i18nc("kind of place", "Street");
    default: {
        // OpenStreetMap values such as "fast_food" are not translated.
        QString label = type;
        label.replace(u'_', u' ');
        if (!label.isEmpty()) {
            label[0] = label[0].toUpper();
        }
        return label;
    }
    }
}

QVariantMap result(const QString &title, const QString &subtitle, double latitude, double longitude)
{
    return {{u"title"_s, title}, {u"subtitle"_s, subtitle}, {u"latitude"_s, latitude}, {u"longitude"_s, longitude}};
}

/// "fast_food" as "Fast food". OpenStreetMap values are not translated.
QString readable(QString value)
{
    value.replace(u'_', u' ');
    if (!value.isEmpty()) {
        value[0] = value[0].toUpper();
    }
    return value;
}

QVariantMap detail(const QString &icon, const QString &text, const QString &link = {})
{
    return {{u"icon"_s, icon}, {u"text"_s, text}, {u"link"_s, link}, {u"state"_s, QString()}, {u"note"_s, QString()}};
}

/// Opening hours in OpenStreetMap's notation, such as "Mo-Fr 09:00-18:00",
/// as whether the place is open now and when it is open over the next week.
QVariantMap openingHoursDetail(const QString &expression, double latitude, double longitude)
{
    // As written, for when the notation cannot be evaluated.
    QVariantMap line = detail(u"clock-symbolic"_s, QString(expression).replace(u"; "_s, u"\n"_s));

    KOpeningHours::OpeningHours hours(expression.toUtf8());
    // Where the place is decides its time zone, public holidays and sunset.
    hours.setLocation(float(latitude), float(longitude));
    QTimeZone zone(KTimeZone::fromLocation(float(latitude), float(longitude)));
    if (!zone.isValid()) {
        zone = QTimeZone::systemTimeZone();
    }
    hours.setTimeZone(zone);
    const KCountry country = KCountry::fromLocation(float(latitude), float(longitude));
    if (country.isValid()) {
        hours.setRegion(country.alpha2());
    }
    if (hours.error() != KOpeningHours::OpeningHours::NoError) {
        return line;
    }

    const QDateTime now = QDateTime::currentDateTime().toTimeZone(zone);
    const KOpeningHours::Interval current = hours.interval(now);
    if (current.state() == KOpeningHours::Interval::Open) {
        line[u"state"_s] = u"open"_s;
    } else if (current.state() == KOpeningHours::Interval::Closed) {
        line[u"state"_s] = u"closed"_s;
    }
    const QString state = KOpeningHours::Display::currentState(hours);
    if (state.isEmpty()) {
        return line;
    }
    line[u"text"_s] = state;

    // The coming seven days, one entry each.
    const QLocale locale;
    QStringList days;
    QStringList times;
    KOpeningHours::Interval interval = hours.interval(QDateTime(now.date(), QTime(0, 0), zone));
    for (int offset = 0; offset < 7; ++offset) {
        const QDate date = now.date().addDays(offset);
        const QDateTime dayStart(date, QTime(0, 0), zone);
        const QDateTime dayEnd = dayStart.addDays(1);
        QStringList spans;
        bool unknown = false;
        // The intervals follow one another without gaps; bounded for safety.
        for (int guard = 0; guard < 64 && interval.isValid(); ++guard) {
            const QDateTime begin = interval.hasOpenBegin() ? dayStart : std::max(interval.begin(), dayStart);
            const QDateTime end = interval.hasOpenEnd() ? dayEnd : std::min(interval.end(), dayEnd);
            if (begin < end) {
                if (interval.state() == KOpeningHours::Interval::Open) {
                    spans.append(begin == dayStart && end == dayEnd ? i18nc("opening hours of a day", "24 hours")
                                                                    : i18nc("opening hours: from time to time",
                                                                            "%1 – %2",
                                                                            locale.toString(begin.time(), QLocale::ShortFormat),
                                                                            locale.toString(end.time(), QLocale::ShortFormat)));
                } else if (interval.state() != KOpeningHours::Interval::Closed) {
                    unknown = true;
                }
            }
            if (interval.hasOpenEnd() || interval.end() > dayEnd) {
                break; // reaches into the next day
            }
            interval = hours.nextInterval(interval);
        }
        if (unknown && spans.isEmpty()) {
            return line; // depends on something that cannot be known, so do not guess
        }
        days.append(locale.dayName(date.dayOfWeek(), QLocale::ShortFormat));
        times.append(spans.isEmpty() ? i18nc("opening hours of a day", "Closed") : spans.join(i18nc("separates list entries", ", ")));
    }

    // Shown as a week, from its first day, rather than from today.
    const int shift = (int(locale.firstDayOfWeek()) - now.date().dayOfWeek() + 7) % 7;
    std::rotate(days.begin(), days.begin() + shift, days.end());
    std::rotate(times.begin(), times.begin() + shift, times.end());

    // "Mon – Fri" for neighbouring days with the same hours.
    QStringList schedule;
    for (qsizetype first = 0; first < days.size();) {
        qsizetype last = first;
        while (last + 1 < days.size() && times[last + 1] == times[first]) {
            ++last;
        }
        const QString range = last == first ? days[first] : i18nc("opening hours: from weekday to weekday", "%1 – %2", days[first], days[last]);
        schedule.append(i18nc("opening hours: weekdays, times", "%1: %2", range, times[first]));
        first = last + 1;
    }
    line[u"note"_s] = schedule.join(u'\n');
    return line;
}

/// The lines of the place panel for a set of OSM tags. @p exact says whether
/// the tags are known to belong to the place itself, not just to its vicinity.
QVariantList detailsFromTags(const QJsonObject &tags, bool exact, double latitude, double longitude)
{
    QVariantList details;
    const auto tag = [&tags](QLatin1StringView key) {
        return tags.value(key).toString().trimmed();
    };
    const auto either = [&tag](QLatin1StringView key, QLatin1StringView alternative) {
        const QString value = tag(key);
        return value.isEmpty() ? tag(alternative) : value;
    };

    QString street = tag("addr:street"_L1);
    if (exact && !street.isEmpty() && !tag("addr:housenumber"_L1).isEmpty()) {
        street = i18nc("street, house number", "%1 %2", street, tag("addr:housenumber"_L1));
    }
    QString town = tag("addr:city"_L1);
    if (!town.isEmpty() && !tag("addr:postcode"_L1).isEmpty()) {
        town = i18nc("postcode, town", "%1 %2", tag("addr:postcode"_L1), town);
    }
    QStringList address{street, town};
    address.removeAll(QString());
    if (!address.isEmpty()) {
        details.append(detail(u"mark-location-symbolic"_s, address.join(i18nc("separates street and town", ", "))));
    }
    if (!exact) {
        return details;
    }

    if (!tag("opening_hours"_L1).isEmpty()) {
        details.append(openingHoursDetail(tag("opening_hours"_L1), latitude, longitude));
    }
    const QString phone = either("phone"_L1, "contact:phone"_L1);
    if (!phone.isEmpty()) {
        details.append(detail(u"call-start-symbolic"_s, phone, u"tel:"_s + QString(phone).remove(u' ')));
    }
    QString address2 = either("website"_L1, "contact:website"_L1);
    if (!address2.isEmpty() && !address2.contains("://"_L1)) {
        address2.prepend(u"https://"_s);
    }
    const QUrl website(address2);
    if (website.isValid() && !website.host().isEmpty() && (website.scheme() == "https"_L1 || website.scheme() == "http"_L1)) {
        details.append(detail(u"globe-symbolic"_s, website.host(), website.toString()));
    }
    const QString email = either("email"_L1, "contact:email"_L1);
    if (email.contains(u'@') && !email.contains(u' ')) {
        details.append(detail(u"mail-message-symbolic"_s, email, u"mailto:"_s + email));
    }

    if (!tag("cuisine"_L1).isEmpty()) {
        QStringList cuisines;
        const QStringList values = tag("cuisine"_L1).split(u';', Qt::SkipEmptyParts);
        for (const QString &value : values) {
            cuisines.append(readable(value.trimmed()));
        }
        details.append(detail(u"food-symbolic"_s, cuisines.join(i18nc("separates list entries", ", "))));
    }

    QStringList features;
    const QString wheelchair = tag("wheelchair"_L1);
    if (wheelchair == "yes"_L1 || wheelchair == "designated"_L1) {
        features.append(i18nc("@info feature of a place", "Wheelchair accessible"));
    } else if (wheelchair == "limited"_L1) {
        features.append(i18nc("@info feature of a place", "Partly wheelchair accessible"));
    } else if (wheelchair == "no"_L1) {
        features.append(i18nc("@info feature of a place", "Not wheelchair accessible"));
    }
    if (tag("diet:vegan"_L1) == "yes"_L1 || tag("diet:vegan"_L1) == "only"_L1) {
        features.append(i18nc("@info feature of a place", "Vegan options"));
    } else if (tag("diet:vegetarian"_L1) == "yes"_L1 || tag("diet:vegetarian"_L1) == "only"_L1) {
        features.append(i18nc("@info feature of a place", "Vegetarian options"));
    }
    if (tag("outdoor_seating"_L1) == "yes"_L1) {
        features.append(i18nc("@info feature of a place", "Outdoor seating"));
    }
    if (tag("takeaway"_L1) == "yes"_L1 || tag("takeaway"_L1) == "only"_L1) {
        features.append(i18nc("@info feature of a place", "Takeaway"));
    }
    if (tag("internet_access"_L1) == "wlan"_L1 || tag("internet_access"_L1) == "yes"_L1) {
        features.append(i18nc("@info feature of a place", "Wi-Fi"));
    }
    if (tag("fee"_L1) == "no"_L1) {
        features.append(i18nc("@info feature of a place", "Free of charge"));
    }
    if (!features.isEmpty()) {
        details.append(detail(u"checkmark-symbolic"_s, features.join(i18nc("separates list entries", ", "))));
    }

    const QString operatedBy = either("operator"_L1, "brand"_L1);
    if (!operatedBy.isEmpty()) {
        details.append(detail(u"group-symbolic"_s, i18nc("@info who runs a place", "Run by %1", operatedBy)));
    }
    return details;
}

QString languages()
{
    QStringList list = QLocale().uiLanguages();
    list.append(u"en"_s);
    return list.join(u',');
}
}

Geocoder::Geocoder(OfflineManager *offline, QObject *parent)
    : QObject(parent)
    , m_offline(offline)
{
    s_instance = this;
    // Region files may be replaced or deleted, so let go of them.
    connect(offline, &OfflineManager::routingRegionsChanged, this, &Geocoder::closeDatabases);
}

Geocoder::~Geocoder()
{
    closeDatabases();
}

Geocoder *Geocoder::create(QQmlEngine *, QJSEngine *)
{
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

QVariantList Geocoder::results() const
{
    return m_results;
}

bool Geocoder::busy() const
{
    return m_busy;
}

bool Geocoder::finished() const
{
    return m_finished;
}

bool Geocoder::onlineFailed() const
{
    return m_onlineFailed;
}

QString Geocoder::baseUrl() const
{
    // Configurable so that the service can be switched without an update.
    return KSharedConfig::openConfig()->group(u"Services"_s).readEntry("NominatimUrl", u"https://nominatim.openstreetmap.org"_s);
}

void Geocoder::closeDatabases()
{
    for (const QString &connection : std::as_const(m_connections)) {
        QSqlDatabase::database(connection).close();
        QSqlDatabase::removeDatabase(connection);
    }
    m_connections.clear();
}

void Geocoder::setResults(const QVariantList &results, bool finished, bool onlineFailed)
{
    m_results = results;
    m_finished = finished;
    m_onlineFailed = onlineFailed;
    Q_EMIT resultsChanged();
}

void Geocoder::setBusy(bool busy)
{
    if (m_busy != busy) {
        m_busy = busy;
        Q_EMIT busyChanged();
    }
}

void Geocoder::throttled(const std::function<void()> &send)
{
    const qint64 wait = m_lastRequest.isValid() ? std::max<qint64>(0, 1100 - m_lastRequest.elapsed()) : 0;
    m_lastRequest.start(); // reserves the slot, so queued requests stay a second apart
    if (wait == 0) {
        send();
    } else {
        QTimer::singleShot(wait, this, send);
    }
}

QSqlDatabase Geocoder::regionDatabase(const QString &id, const QString &directory) const
{
    const QString connection = u"places-"_s + id;
    if (!m_connections.contains(connection)) {
        QSqlDatabase db = QSqlDatabase::addDatabase(u"QSQLITE"_s, connection);
        db.setDatabaseName(ExtractImporter::placesPath(directory));
        db.setConnectOptions(u"QSQLITE_OPEN_READONLY"_s);
        db.open();
        m_connections.append(connection);
    }
    return QSqlDatabase::database(connection);
}

QVariantList Geocoder::offlineSearch(const QString &text, double latitude, double longitude, bool substring) const
{
    struct Hit {
        QVariantMap map;
        int kind;
        double distance;
    };
    QList<Hit> hits;
    const QString fold = text.trimmed().toCaseFolded();
    if (fold.size() < 2) {
        return {};
    }

    const QList<OfflineManager::RoutingRegion> regions = m_offline->routingRegions();
    for (const OfflineManager::RoutingRegion &region : regions) {
        QSqlQuery query(regionDatabase(region.id, region.directory));
        const QString order = u" ORDER BY kind, (lat - ?) * (lat - ?) + (lon - ?) * (lon - ?) LIMIT ?"_s;
        if (substring) {
            QString pattern = fold;
            pattern.replace(u'\\', u"\\\\"_s).replace(u'%', u"\\%"_s).replace(u'_', u"\\_"_s);
            query.prepare(u"SELECT name, kind, type, lat, lon FROM places WHERE fold LIKE ? ESCAPE '\\'"_s + order);
            query.addBindValue(QString(u'%' + pattern + u'%'));
        } else {
            // A range scan on the index finds everything that starts with the text.
            query.prepare(u"SELECT name, kind, type, lat, lon FROM places WHERE fold >= ? AND fold < ?"_s + order);
            query.addBindValue(fold);
            query.addBindValue(QString(fold + QChar(0xffff)));
        }
        query.addBindValue(latitude);
        query.addBindValue(latitude);
        query.addBindValue(longitude);
        query.addBindValue(longitude);
        query.addBindValue(MaxResults);
        query.exec();
        while (query.next()) {
            const int kind = query.value(1).toInt();
            const double lat = query.value(3).toDouble();
            const double lon = query.value(4).toDouble();
            const QString subtitle = i18nc("kind of place, region it is in", "%1 in %2", kindLabel(kind, query.value(2).toString()), region.name);
            hits.append(
                {result(query.value(0).toString(), subtitle, lat, lon), kind, (lat - latitude) * (lat - latitude) + (lon - longitude) * (lon - longitude)});
        }
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) {
        return a.kind != b.kind ? a.kind < b.kind : a.distance < b.distance;
    });
    QVariantList list;
    for (const Hit &hit : std::as_const(hits)) {
        if (list.size() == MaxResults) {
            break;
        }
        list.append(hit.map);
    }
    return list;
}

void Geocoder::clear()
{
    ++m_generation;
    setBusy(false);
    setResults({}, false, false);
}

void Geocoder::suggest(const QString &text, double latitude, double longitude)
{
    ++m_generation;
    setBusy(false);
    setResults(offlineSearch(text, latitude, longitude, false), false, false);
}

void Geocoder::search(const QString &text, double latitude, double longitude)
{
    const int generation = ++m_generation;
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        clear();
        return;
    }

    // "48.137, 11.575" is a position, not a name.
    static const QRegularExpression coordinates(u"^(-?\\d+(?:\\.\\d+)?)\\s*[,; ]\\s*(-?\\d+(?:\\.\\d+)?)$"_s);
    const auto match = coordinates.match(trimmed);
    if (match.hasMatch()) {
        const double lat = match.captured(1).toDouble();
        const double lon = match.captured(2).toDouble();
        if (std::abs(lat) <= 90 && std::abs(lon) <= 180) {
            setBusy(false);
            setResults({result(trimmed, i18nc("@info", "Coordinates"), lat, lon)}, true, false);
            return;
        }
    }

    const QVariantList local = offlineSearch(trimmed, latitude, longitude, true);
    if (const QVariantList *cached = m_cache.object(trimmed)) {
        setBusy(false);
        setResults(*cached, true, false);
        return;
    }
    setResults(local, false, false);
    setBusy(true);

    throttled([=, this] {
        if (generation != m_generation) {
            return;
        }
        QUrl url(baseUrl() + u"/search"_s);
        url.setQuery(QUrlQuery{
            {u"q"_s, trimmed},
            {u"format"_s, u"jsonv2"_s},
            {u"limit"_s, QString::number(MaxResults)},
            // Prefer matches near what the map shows, without excluding others.
            {u"viewbox"_s, u"%1,%2,%3,%4"_s.arg(longitude - 1).arg(latitude + 1).arg(longitude + 1).arg(latitude - 1)},
        });
        QNetworkRequest request = Network::request(url);
        request.setRawHeader("Accept-Language", languages().toLatin1());
        QNetworkReply *reply = Network::manager()->get(request);
        connect(reply, &QNetworkReply::finished, this, [=, this] {
            reply->deleteLater();
            if (generation != m_generation) {
                return;
            }
            setBusy(false);
            if (reply->error() != QNetworkReply::NoError) {
                setResults(local, true, true);
                return;
            }
            QVariantList online;
            const QJsonArray array = QJsonDocument::fromJson(reply->readAll()).array();
            for (const QJsonValue &value : array) {
                const QString display = value["display_name"_L1].toString();
                QString title = value["name"_L1].toString();
                if (title.isEmpty()) {
                    title = display.section(u", "_s, 0, 0);
                }
                QString subtitle = display;
                if (subtitle.startsWith(title + u", "_s)) {
                    subtitle = subtitle.mid(title.size() + 2);
                }
                online.append(result(title, subtitle, value["lat"_L1].toString().toDouble(), value["lon"_L1].toString().toDouble()));
            }
            m_cache.insert(trimmed, new QVariantList(online));
            setResults(online.isEmpty() ? local : online, true, false);
        });
    });
}

void Geocoder::reverse(double latitude, double longitude)
{
    throttled([=, this] {
        QUrl url(baseUrl() + u"/reverse"_s);
        url.setQuery(QUrlQuery{
            {u"lat"_s, QString::number(latitude, 'f', 6)},
            {u"lon"_s, QString::number(longitude, 'f', 6)},
            {u"format"_s, u"jsonv2"_s},
            {u"zoom"_s, u"18"_s},
        });
        QNetworkRequest request = Network::request(url);
        request.setRawHeader("Accept-Language", languages().toLatin1());
        QNetworkReply *reply = Network::manager()->get(request);
        connect(reply, &QNetworkReply::finished, this, [=, this] {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                return; // the position keeps its coordinates as a name
            }
            const QJsonObject object = QJsonDocument::fromJson(reply->readAll()).object();
            QString name = object["name"_L1].toString();
            if (name.isEmpty()) {
                name = object["display_name"_L1].toString().section(u", "_s, 0, 1);
            }
            if (!name.isEmpty()) {
                Q_EMIT reverseFinished(latitude, longitude, name);
            }
        });
    });
}

QVariantMap Geocoder::placeAt(double latitude, double longitude, double zoomLevel) const
{
    const TileServer *server = TileServer::instance();
    if (!server || std::abs(latitude) > 85) {
        return {};
    }
    // MapLibre counts zoom levels in tiles of 512 pixels, Qt in tiles of 256.
    const double mapZoom = zoomLevel - 1;
    // The names follow the OpenMapTiles schema, which most vector maps use.
    static const QStringList layers{u"poi"_s, u"mountain_peak"_s, u"aerodrome_label"_s};
    // How far from its position a click still counts, in pixels: the icon
    // sits on the position and the name below it.
    constexpr double Sideways = 24;
    constexpr double Above = 14;
    constexpr double Below = 28;

    const double worldX = (longitude + 180) / 360;
    const double sinLatitude = std::sin(latitude * M_PI / 180);
    const double worldY = 0.5 - std::log((1 + sinLatitude) / (1 - sinLatitude)) / (4 * M_PI);

    QVariantMap best;
    double bestDistance = std::numeric_limits<double>::max();
    const QList<TileServer::TileSource> sources = server->tileSources();
    for (const TileServer::TileSource &source : sources) {
        if (!source.vector) {
            continue;
        }
        const int z = std::clamp(int(std::floor(mapZoom)), source.minZoom, source.maxZoom);
        const double tiles = std::pow(2, z);
        const double pixelsPerTile = 512 * std::pow(2, mapZoom - z);
        const double x = worldX * tiles;
        const double y = worldY * tiles;
        // A place near the edge of a tile may belong to the one next to it.
        const double reach = Below / pixelsPerTile;
        for (int tileX = int(std::floor(x - reach)); tileX <= int(std::floor(x + reach)); ++tileX) {
            for (int tileY = int(std::floor(y - reach)); tileY <= int(std::floor(y + reach)); ++tileY) {
                if (tileX < 0 || tileY < 0 || tileX >= tiles || tileY >= tiles) {
                    continue;
                }
                const int row = source.tms ? int(tiles) - 1 - tileY : tileY;
                const QByteArray tile = VectorTile::gunzip(server->storedTile(source.id, z, tileX, row));
                const QList<VectorTile::PointFeature> features = VectorTile::pointFeatures(tile, layers);
                for (const VectorTile::PointFeature &feature : features) {
                    // Outside of its own tile a feature is a copy for drawing across the edge.
                    if (feature.x < 0 || feature.x >= 1 || feature.y < 0 || feature.y >= 1) {
                        continue;
                    }
                    const double dx = (x - (tileX + feature.x)) * pixelsPerTile;
                    const double dy = (y - (tileY + feature.y)) * pixelsPerTile;
                    if (std::abs(dx) > Sideways || dy < -Above || dy > Below) {
                        continue;
                    }
                    const QVariantMap &tags = feature.properties;
                    // The local name, which is also what the map shows.
                    QString name = tags.value(u"name"_s).toString();
                    if (name.isEmpty()) {
                        name = tags.value(u"name:latin"_s).toString();
                    }
                    if (name.isEmpty()) {
                        continue;
                    }
                    QString kind;
                    QString type; // as OpenStreetMap calls it
                    if (feature.layer == "poi"_L1) {
                        // Less important places only appear when zoomed in,
                        // as in the OpenMapTiles styles; stops always do.
                        const int rank = tags.value(u"rank"_s).toInt();
                        const QString group = tags.value(u"class"_s).toString();
                        const bool transit = group == "bus"_L1 || group == "rail"_L1 || group == "airport"_L1;
                        if (!transit && !(mapZoom >= 17 || (mapZoom >= 16 && rank < 20) || (mapZoom >= 15 && rank < 7))) {
                            continue;
                        }
                        const QString subclass = tags.value(u"subclass"_s).toString();
                        type = subclass.isEmpty() ? group : subclass;
                        kind = readable(type);
                    } else if (feature.layer == "mountain_peak"_L1) {
                        type = u"peak"_s;
                        const int elevation = tags.value(u"ele"_s).toInt();
                        kind = elevation > 0 ? i18nc("kind of place, elevation in metres", "Peak, %1 m", elevation) : i18nc("kind of place", "Peak");
                    } else {
                        type = u"aerodrome"_s;
                        const QString code = tags.value(u"iata"_s).toString();
                        kind = code.isEmpty() ? i18nc("kind of place", "Airport") : i18nc("kind of place, airport code", "Airport (%1)", code);
                    }
                    const double distance = std::hypot(dx, dy);
                    if (distance < bestDistance) {
                        bestDistance = distance;
                        const double featureX = (tileX + feature.x) / tiles;
                        const double featureY = (tileY + feature.y) / tiles;
                        const double featureLatitude = std::atan(std::sinh(M_PI * (1 - 2 * featureY))) * 180 / M_PI;
                        best = result(name, kind, featureLatitude, featureX * 360 - 180);
                        best.insert(u"type"_s, type);
                    }
                }
            }
        }
    }
    return best;
}

bool Geocoder::offlineTags(double latitude, double longitude, const QString &name, const QString &type, QJsonObject *tags) const
{
    // The map and the region data come from different snapshots of
    // OpenStreetMap, and put outlines at slightly different spots.
    constexpr double Reach = 0.002; // about 200 m
    bool found = false;
    double nearest = std::numeric_limits<double>::max();
    const QList<OfflineManager::RoutingRegion> regions = m_offline->routingRegions();
    for (const OfflineManager::RoutingRegion &region : regions) {
        if (latitude < region.south || latitude > region.north || longitude < region.west || longitude > region.east) {
            continue;
        }
        QSqlQuery query(regionDatabase(region.id, region.directory));
        query.prepare(u"SELECT lat, lon, tags FROM places WHERE fold = ? AND type = ? AND lat BETWEEN ? AND ? AND lon BETWEEN ? AND ?"_s);
        query.addBindValue(name.toCaseFolded());
        // The kind as well: a signpost may carry the name of what it points to.
        query.addBindValue(type);
        query.addBindValue(latitude - Reach);
        query.addBindValue(latitude + Reach);
        query.addBindValue(longitude - Reach);
        query.addBindValue(longitude + Reach);
        // Fails for regions downloaded before details were kept: then ask online.
        if (!query.exec()) {
            continue;
        }
        while (query.next()) {
            const double distance = std::hypot(query.value(0).toDouble() - latitude, query.value(1).toDouble() - longitude);
            if (distance < nearest) {
                nearest = distance;
                found = true;
                *tags = QJsonDocument::fromJson(query.value(2).toString().toUtf8()).object();
            }
        }
    }
    return found;
}

void Geocoder::lookUpDetails(double latitude, double longitude, const QString &name, const QString &type)
{
    QJsonObject stored;
    if (offlineTags(latitude, longitude, name, type, &stored)) {
        const QVariantList details = detailsFromTags(stored, true, latitude, longitude);
        if (!details.isEmpty()) {
            Q_EMIT detailsFinished(latitude, longitude, details);
        }
        lookUpArticle(latitude, longitude, stored);
        return;
    }

    throttled([=, this] {
        QUrl url(baseUrl() + u"/reverse"_s);
        url.setQuery(QUrlQuery{
            {u"lat"_s, QString::number(latitude, 'f', 6)},
            {u"lon"_s, QString::number(longitude, 'f', 6)},
            {u"format"_s, u"jsonv2"_s},
            {u"zoom"_s, u"18"_s},
            {u"layer"_s, u"poi"_s},
            {u"addressdetails"_s, u"1"_s},
            {u"extratags"_s, u"1"_s},
        });
        QNetworkRequest request = Network::request(url);
        request.setRawHeader("Accept-Language", languages().toLatin1());
        QNetworkReply *reply = Network::manager()->get(request);
        connect(reply, &QNetworkReply::finished, this, [=, this] {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                return; // the place keeps what the map says about it
            }
            const QJsonObject object = QJsonDocument::fromJson(reply->readAll()).object();
            const QJsonObject address = object["address"_L1].toObject();
            // The answer is the nearest place the service knows, which is
            // not necessarily the one that was asked about.
            const bool same = object["name"_L1].toString().compare(name, Qt::CaseInsensitive) == 0;

            // Bring the answer into the form of OSM tags.
            QJsonObject tags = object["extratags"_L1].toObject();
            for (const auto &[from, to] :
                 {std::pair{"road"_L1, "addr:street"_L1}, {"house_number"_L1, "addr:housenumber"_L1}, {"postcode"_L1, "addr:postcode"_L1}}) {
                if (address.contains(from)) {
                    tags.insert(to, address[from]);
                }
            }
            for (const QLatin1StringView key : {"city"_L1, "town"_L1, "village"_L1, "municipality"_L1}) {
                if (address.contains(key)) {
                    tags.insert("addr:city"_L1, address[key]);
                    break;
                }
            }
            const QVariantList details = detailsFromTags(tags, same, latitude, longitude);
            if (!details.isEmpty()) {
                Q_EMIT detailsFinished(latitude, longitude, details);
            }
            if (same) {
                lookUpArticle(latitude, longitude, tags);
            }
        });
    });
}

// --- Encyclopedia articles ---

void Geocoder::lookUpArticle(double latitude, double longitude, const QJsonObject &tags)
{
    // OpenStreetMap links places to Wikidata ("Q694782") and to one Wikipedia
    // article ("de:Schloss Vaduz"). Wikidata knows the article in other languages.
    static const QRegularExpression validItem(u"^Q[1-9][0-9]{0,11}$"_s);
    static const QRegularExpression validArticle(u"^([a-z]{2,3}(?:-[a-z]{2,12})?):(.+)$"_s);
    const QString item = tags.value("wikidata"_L1).toString().trimmed();
    const auto tagged = validArticle.match(tags.value("wikipedia"_L1).toString().trimmed());
    const QString taggedLanguage = tagged.captured(1);
    const QString taggedTitle = tagged.captured(2);

    QStringList wanted{QLocale().name().left(2).toLower()};
    for (const QString &language : {u"en"_s, taggedLanguage}) {
        if (!language.isEmpty() && !wanted.contains(language)) {
            wanted.append(language);
        }
    }
    if (tagged.hasMatch() && taggedLanguage == wanted.first()) {
        fetchArticle(latitude, longitude, taggedLanguage, taggedTitle);
        return;
    }
    if (!validItem.match(item).hasMatch()) {
        if (tagged.hasMatch()) {
            fetchArticle(latitude, longitude, taggedLanguage, taggedTitle);
        }
        return;
    }

    QStringList sites;
    for (const QString &language : std::as_const(wanted)) {
        sites.append(QString(language).replace(u'-', u'_') + u"wiki"_s);
    }
    QUrl url(u"https://www.wikidata.org/w/api.php"_s);
    url.setQuery(QUrlQuery{
        {u"action"_s, u"wbgetentities"_s},
        {u"format"_s, u"json"_s},
        {u"ids"_s, item},
        {u"props"_s, u"sitelinks"_s},
        {u"sitefilter"_s, sites.join(u'|')},
    });
    QNetworkReply *reply = Network::manager()->get(Network::request(url));
    connect(reply, &QNetworkReply::finished, this, [=, this] {
        reply->deleteLater();
        const QJsonObject links = QJsonDocument::fromJson(reply->readAll())["entities"_L1][item]["sitelinks"_L1].toObject();
        for (qsizetype i = 0; i < wanted.size(); ++i) {
            const QString title = links[sites[i]]["title"_L1].toString();
            if (!title.isEmpty()) {
                fetchArticle(latitude, longitude, wanted[i], title);
                return;
            }
        }
        if (tagged.hasMatch()) {
            fetchArticle(latitude, longitude, taggedLanguage, taggedTitle);
        }
    });
}

void Geocoder::fetchArticle(double latitude, double longitude, const QString &language, const QString &title)
{
    const QString key = language + u':' + title;
    if (const QVariantMap *cached = m_articles.object(key)) {
        Q_EMIT articleFinished(latitude, longitude, *cached);
        return;
    }
    QUrl url(u"https://%1.wikipedia.org/w/api.php"_s.arg(language));
    url.setQuery(QUrlQuery{
        {u"action"_s, u"query"_s},
        {u"format"_s, u"json"_s},
        {u"formatversion"_s, u"2"_s},
        {u"redirects"_s, u"1"_s},
        {u"prop"_s, u"extracts|pageimages|info"_s},
        {u"exintro"_s, u"1"_s},
        {u"explaintext"_s, u"1"_s},
        {u"exsentences"_s, u"3"_s},
        {u"piprop"_s, u"thumbnail|name"_s},
        {u"pithumbsize"_s, u"640"_s},
        {u"inprop"_s, u"url"_s},
        {u"titles"_s, title},
    });
    QNetworkReply *reply = Network::manager()->get(Network::request(url));
    connect(reply, &QNetworkReply::finished, this, [=, this] {
        reply->deleteLater();
        const QJsonValue page = QJsonDocument::fromJson(reply->readAll())["query"_L1]["pages"_L1][0];
        const QString text = page["extract"_L1].toString().trimmed();
        const QUrl address(page["fullurl"_L1].toString());
        if (reply->error() != QNetworkReply::NoError || text.isEmpty() || address.scheme() != "https"_L1) {
            return;
        }
        QVariantMap article{{u"text"_s, text}, {u"url"_s, address.toString()}, {u"image"_s, QString()}, {u"imagePage"_s, QString()}};
        const QUrl image(page["thumbnail"_L1]["source"_L1].toString());
        const QString file = page["pageimage"_L1].toString();
        if (image.scheme() == "https"_L1 && image.host().endsWith(".wikimedia.org"_L1) && !file.isEmpty()) {
            article[u"image"_s] = image.toString();
            // Where the picture's author and licence are stated.
            QUrl filePage(u"https://%1.wikipedia.org/wiki/"_s.arg(language));
            filePage.setPath(u"/wiki/File:"_s + file);
            article[u"imagePage"_s] = filePage.toString();
        }
        m_articles.insert(key, new QVariantMap(article));
        Q_EMIT articleFinished(latitude, longitude, article);
    });
}

// --- Reviews from Google ---
//
// Optional, and only with a key the user has entered: see the settings page
// for what Google's terms say about it. Nothing of what comes back is stored.

namespace
{
QNetworkRequest googleRequest(const QUrl &url, const QString &key, const QByteArray &fields)
{
    QNetworkRequest request = Network::request(url);
    request.setRawHeader("X-Goog-Api-Key", key.toUtf8());
    request.setRawHeader("X-Goog-FieldMask", fields);
    request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    return request;
}

/// What went wrong, in Google's words where it gives any.
QVariantMap googleError(QNetworkReply *reply, const QByteArray &body)
{
    QString message = QJsonDocument::fromJson(body)["error"_L1]["message"_L1].toString();
    if (message.isEmpty()) {
        message = reply->errorString();
    }
    return {{u"error"_s, message}};
}
}

void Geocoder::lookUpReviews(double latitude, double longitude, const QString &name, const QString &key)
{
    if (key.trimmed().isEmpty() || name.trimmed().isEmpty()) {
        return;
    }
    // First find Google's own id for the place. Asking for nothing but ids
    // keeps this request in the cheapest class.
    const QJsonObject centre{{u"latitude"_s, latitude}, {u"longitude"_s, longitude}};
    const QJsonObject body{
        {u"textQuery"_s, name},
        {u"pageSize"_s, 1},
        {u"locationBias"_s, QJsonObject{{u"circle"_s, QJsonObject{{u"center"_s, centre}, {u"radius"_s, 300.0}}}}},
    };
    QNetworkReply *reply = Network::manager()->post(googleRequest(QUrl(u"https://places.googleapis.com/v1/places:searchText"_s), key.trimmed(), "places.id"),
                                                    QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [=, this] {
        reply->deleteLater();
        const QByteArray answer = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            Q_EMIT reviewsFinished(latitude, longitude, googleError(reply, answer));
            return;
        }
        const QString id = QJsonDocument::fromJson(answer)["places"_L1][0]["id"_L1].toString();
        static const QRegularExpression validId(u"^[A-Za-z0-9_-]{1,512}$"_s);
        if (validId.match(id).hasMatch()) {
            fetchReviews(latitude, longitude, id, key.trimmed());
        }
    });
}

void Geocoder::fetchReviews(double latitude, double longitude, const QString &placeId, const QString &key)
{
    QUrl url(u"https://places.googleapis.com/v1/places/"_s + placeId);
    url.setQuery(QUrlQuery{{u"languageCode"_s, QLocale().name().left(2).toLower()}});
    QNetworkReply *reply = Network::manager()->get(googleRequest(url, key, "id,location,rating,userRatingCount,googleMapsUri,reviews"));
    connect(reply, &QNetworkReply::finished, this, [=, this] {
        reply->deleteLater();
        const QByteArray answer = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            Q_EMIT reviewsFinished(latitude, longitude, googleError(reply, answer));
            return;
        }
        const QJsonObject place = QJsonDocument::fromJson(answer).object();
        // The search answers with its best guess, which may be another place
        // of that name further away.
        const QGeoCoordinate found(place["location"_L1]["latitude"_L1].toDouble(), place["location"_L1]["longitude"_L1].toDouble());
        if (!found.isValid() || found.distanceTo(QGeoCoordinate(latitude, longitude)) > 300) {
            return;
        }
        const auto webAddress = [](const QJsonValue &value) {
            const QUrl address(value.toString());
            return address.scheme() == "https"_L1 ? address.toString() : QString();
        };

        QVariantList reviews;
        const QJsonArray entries = place["reviews"_L1].toArray();
        for (const QJsonValue &entry : entries) {
            const QJsonObject author = entry["authorAttribution"_L1].toObject();
            QString photo = webAddress(author["photoUri"_L1]);
            if (photo.isEmpty() && author["photoUri"_L1].toString().startsWith("//"_L1)) {
                photo = u"https:"_s + author["photoUri"_L1].toString();
            }
            reviews.append(QVariantMap{
                {u"author"_s, author["displayName"_L1].toString()},
                {u"authorUrl"_s, webAddress(author["uri"_L1])},
                {u"photo"_s, photo},
                {u"rating"_s, entry["rating"_L1].toDouble()},
                {u"text"_s, entry["text"_L1]["text"_L1].toString()},
                {u"time"_s, entry["relativePublishTimeDescription"_L1].toString()},
                {u"url"_s, webAddress(entry["googleMapsUri"_L1])},
            });
        }
        if (!place.contains("rating"_L1) && reviews.isEmpty()) {
            return; // known to Google, but without any ratings
        }
        Q_EMIT reviewsFinished(latitude,
                               longitude,
                               QVariantMap{
                                   {u"error"_s, QString()},
                                   {u"rating"_s, place["rating"_L1].toDouble()},
                                   {u"count"_s, place["userRatingCount"_L1].toInt()},
                                   {u"url"_s, webAddress(place["googleMapsUri"_L1])},
                                   {u"reviews"_s, reviews},
                               });
    });
}
