// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "router.h"

#include <QElapsedTimer>
#include <QGeoCoordinate>
#include <QGeoRectangle>
#include <QObject>
#include <QQmlEngine>
#include <QVariantList>

#include <atomic>
#include <functional>
#include <memory>

class OfflineManager;

/// Plans the route from a start to a destination, by way of further stops if
/// there are any. Downloaded routing data is used when it covers all of them;
/// otherwise the route comes from an online service.
class RouteController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(QGeoCoordinate from READ from NOTIFY endpointsChanged)
    Q_PROPERTY(QGeoCoordinate to READ to NOTIFY endpointsChanged)
    Q_PROPERTY(QString fromName READ fromName NOTIFY endpointsChanged)
    Q_PROPERTY(QString toName READ toName NOTIFY endpointsChanged)
    /// Start, stops on the way and destination, in the order of travel: maps
    /// with the keys name, latitude, longitude and valid. There are always at
    /// least two; one that has not been chosen yet is not valid.
    Q_PROPERTY(QVariantList stops READ stops NOTIFY endpointsChanged)
    /// Places the route is made to pass on the way from one stop to the
    /// next, without stopping there: maps with the keys latitude, longitude,
    /// stop (the one before it) and index (among those after that stop).
    Q_PROPERTY(QVariantList detours READ detours NOTIFY endpointsChanged)
    Q_PROPERTY(TravelMode mode READ mode WRITE setMode NOTIFY modeChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool hasRoute READ hasRoute NOTIFY routeChanged)
    /// The route line as a list of coordinates.
    Q_PROPERTY(QVariantList path READ path NOTIFY routeChanged)
    /// The route line as it would be with the detour that is being tried
    /// out; empty when none is, or while it is still being worked out.
    Q_PROPERTY(QVariantList previewPath READ previewPath NOTIFY previewChanged)
    Q_PROPERTY(QGeoRectangle bounds READ bounds NOTIFY routeChanged)
    /// The route from each stop to the next: maps with the keys title,
    /// distance, duration and steps. The steps are maps with the keys text,
    /// distance, icon, latitude and longitude.
    Q_PROPERTY(QVariantList legs READ legs NOTIFY routeChanged)
    Q_PROPERTY(QString distanceText READ distanceText NOTIFY routeChanged)
    Q_PROPERTY(QString durationText READ durationText NOTIFY routeChanged)
    /// Whether the route was computed from downloaded data.
    Q_PROPERTY(bool offline READ offline NOTIFY routeChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY routeChanged)

public:
    enum TravelMode {
        Drive,
        Cycle,
        Walk,
    };
    Q_ENUM(TravelMode)

    explicit RouteController(OfflineManager *offline, QObject *parent = nullptr);
    ~RouteController() override;
    static RouteController *create(QQmlEngine *, QJSEngine *);

    QGeoCoordinate from() const;
    QGeoCoordinate to() const;
    QString fromName() const;
    QString toName() const;
    TravelMode mode() const;
    void setMode(TravelMode mode);
    bool busy() const;
    bool hasRoute() const;
    QVariantList path() const;
    QVariantList previewPath() const;
    QGeoRectangle bounds() const;
    QVariantList stops() const;
    QVariantList detours() const;
    QVariantList legs() const;
    QString distanceText() const;
    QString durationText() const;
    bool offline() const;
    QString errorText() const;

    Q_INVOKABLE void setFrom(const QGeoCoordinate &coordinate, const QString &name);
    Q_INVOKABLE void setTo(const QGeoCoordinate &coordinate, const QString &name);
    Q_INVOKABLE void setStop(int index, const QGeoCoordinate &coordinate, const QString &name);
    /// Adds a further destination after the current one, still to be chosen.
    Q_INVOKABLE void addStop();
    /// Removes a stop; of the last two, it only forgets the place.
    Q_INVOKABLE void removeStop(int index);
    /// Replaces the placeholder name of an end point at exactly this position.
    Q_INVOKABLE void nameEndpoint(double latitude, double longitude, const QString &name);
    /// Puts the stops into another order; @p order lists their present
    /// numbers in the order wanted. Detours belong to the old order and go.
    Q_INVOKABLE void reorderStops(const QList<int> &order);
    /// Bends the route: what passes @p onRoute now is to pass @p through.
    Q_INVOKABLE void addDetour(const QGeoCoordinate &onRoute, const QGeoCoordinate &through);
    /// Works out what addDetour() or moveDetour() would lead to, without
    /// changing the route: see previewPath. Meant to be called over and over
    /// while a detour is dragged about; only the latest position counts.
    Q_INVOKABLE void previewDetour(const QGeoCoordinate &onRoute, const QGeoCoordinate &through);
    Q_INVOKABLE void previewMovedDetour(int stop, int index, const QGeoCoordinate &through);
    Q_INVOKABLE void clearPreview();
    Q_INVOKABLE void moveDetour(int stop, int index, const QGeoCoordinate &through);
    Q_INVOKABLE void removeDetour(int stop, int index);
    /// How far a position is from the route line, in metres.
    Q_INVOKABLE double distanceToRoute(const QGeoCoordinate &position) const;
    /// Reverses the order of travel.
    Q_INVOKABLE void swap();
    Q_INVOKABLE void clear();

    static QString formatDistance(double metres);
    static QString formatDuration(double seconds);

Q_SIGNALS:
    void endpointsChanged();
    void modeChanged();
    void busyChanged();
    void routeChanged();
    void previewChanged();

private:
    struct Stop {
        QGeoCoordinate coordinate;
        QString name;
        /// Places to pass on the way to the next stop.
        QList<QGeoCoordinate> detours;
    };
    /// The way from one stop to the next. Its maneuvers refer to the path of
    /// the whole journey.
    struct Leg {
        double distance = 0;
        double duration = 0;
        std::vector<Maneuver> maneuvers;
    };
    struct Journey {
        std::vector<GeoPoint> path;
        std::vector<Leg> legs;
        bool isValid() const
        {
            return path.size() >= 2 && !legs.empty();
        }
    };
    static Journey parseOsrm(const QByteArray &json);
    bool complete() const;
    void setRoute(const Journey &journey, bool offline);
    /// How a planning attempt ended.
    struct Outcome {
        Journey journey; ///< not valid when there is no route
        bool offline = false; ///< planned with downloaded data
        bool hadOfflineData = false;
        bool serviceAnswered = false; ///< the online service was reached
    };
    using Places = std::vector<GeoPoint>;
    using Cancel = std::shared_ptr<std::atomic_bool>;
    static Places placesOf(const QList<Stop> &stops);
    /// Puts a detour into the stretch of the route nearest to @p onRoute.
    bool insertDetour(QList<Stop> &stops, const QGeoCoordinate &onRoute, const QGeoCoordinate &through) const;
    /// Plans a route through the places, with downloaded data if possible
    /// and online otherwise. Nothing is reported once @p cancel is set.
    void plan(const Places &places, const Cancel &cancel, const std::function<void(const Outcome &)> &done);
    void planOnline(const Places &places, const Cancel &cancel, bool hadOfflineData, const std::function<void(const Outcome &)> &done);
    void recalculate();
    void startPreview(const QList<Stop> &stops);
    void runPreview();
    void setError(const QString &error);
    void setBusy(bool busy);

    OfflineManager *const m_offline;
    QList<Stop> m_stops{2};
    TravelMode m_mode = Drive;
    bool m_busy = false;
    bool m_isOffline = false;
    QVariantList m_path;
    QGeoRectangle m_bounds;
    QVariantList m_legs;
    std::vector<GeoPoint> m_routePoints; ///< the route line
    /// Where on the route line the stops and detours lie, in travel order
    /// and without the destination.
    std::vector<quint32> m_waypointPositions;
    QString m_distanceText;
    QString m_durationText;
    QString m_errorText;
    Cancel m_cancel;

    // Trying out a detour.
    QVariantList m_previewPath;
    Places m_previewWanted; ///< what to work out next; empty when nothing
    bool m_previewRunning = false;
    Cancel m_previewCancel;
    /// The last preview that was worked out, to be used as it is if the
    /// detour ends up there.
    Places m_previewPlaces;
    Outcome m_previewOutcome;
    QElapsedTimer m_lastOnlineRequest;
};
