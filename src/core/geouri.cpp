// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "geouri.h"

#include <QList>
#include <QRegularExpression>
#include <QUrl>

using namespace Qt::StringLiterals;

namespace
{
/// A latitude and a longitude as two numbers with a comma between; nothing
/// when they are not numbers or not on the globe.
std::optional<GeoPoint> readPosition(QStringView latitude, QStringView longitude)
{
    bool latitudeOk = false;
    bool longitudeOk = false;
    const GeoPoint point{latitude.trimmed().toDouble(&latitudeOk), longitude.trimmed().toDouble(&longitudeOk)};
    if (!latitudeOk || !longitudeOk || !(std::abs(point.lat) <= 90) || !(std::abs(point.lon) <= 180)) {
        return std::nullopt;
    }
    return point;
}

/// The value of a query item as text: in a query a plus sign stands for a
/// space, and the rest is percent-encoded.
QString decoded(QStringView value)
{
    return QUrl::fromPercentEncoding(value.toString().replace(u'+', u' ').toUtf8());
}
}

std::optional<GeoUri> GeoUri::parse(const QString &uri)
{
    static constexpr QLatin1StringView Scheme("geo:");
    const QStringView all = QStringView(uri).trimmed();
    if (!all.startsWith(Scheme, Qt::CaseInsensitive)) {
        return std::nullopt;
    }
    QStringView rest = all.sliced(Scheme.size());
    if (const qsizetype fragment = rest.indexOf(u'#'); fragment >= 0) {
        rest = rest.first(fragment);
    }
    const qsizetype queryStart = rest.indexOf(u'?');
    const QStringView path = queryStart < 0 ? rest : rest.first(queryStart);
    const QStringView query = queryStart < 0 ? QStringView() : rest.sliced(queryStart + 1);

    // "latitude,longitude[,altitude]", then parameters, each after a semicolon.
    const QList<QStringView> pathParts = path.split(u';');
    const QList<QStringView> numbers = pathParts.first().split(u',');
    if (numbers.size() < 2 || numbers.size() > 3) {
        return std::nullopt;
    }
    const std::optional<GeoPoint> position = readPosition(numbers[0], numbers[1]);
    if (!position) {
        return std::nullopt;
    }
    for (const QStringView parameter : pathParts.sliced(1)) {
        // Coordinates of anything but the Earth (the RFC foresees others) are of no use here.
        static constexpr QLatin1StringView Crs("crs=");
        if (parameter.startsWith(Crs, Qt::CaseInsensitive) && parameter.sliced(Crs.size()).compare("wgs84"_L1, Qt::CaseInsensitive) != 0) {
            return std::nullopt;
        }
    }
    GeoUri result;
    result.position = *position;

    const QList<QStringView> items = query.split(u'&', Qt::SkipEmptyParts);
    for (const QStringView item : items) {
        const qsizetype equals = item.indexOf(u'=');
        if (equals < 0) {
            continue;
        }
        const QStringView key = item.first(equals);
        const QString value = decoded(item.sliced(equals + 1)).trimmed();
        if (key == u'z') {
            bool ok = false;
            const double zoom = value.toDouble(&ok);
            if (ok && zoom > 0 && zoom <= 24) {
                result.zoom = zoom;
            }
        } else if (key == u'q' && !value.isEmpty()) {
            // A position, perhaps with its name in brackets: or else words to search for.
            static const QRegularExpression positioned(uR"(^(-?[0-9.]+)\s*,\s*(-?[0-9.]+)\s*(?:\((.*)\))?$)"_s);
            const QRegularExpressionMatch match = positioned.match(value);
            const std::optional<GeoPoint> named = match.hasMatch() ? readPosition(match.capturedView(1), match.capturedView(2)) : std::nullopt;
            if (named) {
                result.position = *named;
                result.label = match.captured(3).trimmed();
                result.query.clear();
            } else {
                result.query = value;
            }
        }
    }
    return result;
}
