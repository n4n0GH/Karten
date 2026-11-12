// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDateTime>
#include <QGeoCoordinate>
#include <QGeoRectangle>
#include <QObject>
#include <QVariantList>

#include <KPublicTransport/Journey>

#include <vector>

namespace KPublicTransport
{
class JourneyReply;
class Manager;
}

/// Looks up public transport connections with KPublicTransport and turns
/// them into what the interface shows.
///
/// Only the Transitous service is asked: a community-run router for free
/// software whose terms Karten meets (see the README). KPublicTransport
/// knows many operators' own services as well; each has terms of its own.
class TransitPlanner : public QObject
{
    Q_OBJECT

public:
    /// One connection, ready to be shown.
    struct Details {
        QVariantList path; ///< coordinates of the line on the map
        QGeoRectangle bounds;
        QVariantList legs; ///< as RouteController::legs
        QString duration;
        QString distance;
    };

    explicit TransitPlanner(QObject *parent = nullptr);
    ~TransitPlanner() override;

    /// Asks for connections; finished() follows. An invalid @p when means now.
    void plan(const QGeoCoordinate &from, const QString &fromName, const QGeoCoordinate &to, const QString &toName, const QDateTime &when, bool arriveBy);
    /// Forgets the connections and any request under way.
    void clear();

    bool isBusy() const;
    /// For the user; empty when connections were found.
    QString errorText() const;
    /// The connections found, in order of departure: maps with the keys
    /// departure, arrival, duration, changes (a number), lines (names of the
    /// lines taken) and delayed.
    QVariantList connections() const;
    Details details(int connection) const;

Q_SIGNALS:
    void finished();

private:
    KPublicTransport::Manager *manager();

    KPublicTransport::Manager *m_manager = nullptr;
    KPublicTransport::JourneyReply *m_reply = nullptr;
    std::vector<KPublicTransport::Journey> m_journeys;
    QString m_errorText;
    QString m_fromName;
    QString m_toName;
};
