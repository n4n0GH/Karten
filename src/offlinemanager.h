// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "extractimporter.h"

#include <QAbstractListModel>
#include <QDateTime>
#include <QFutureWatcher>
#include <QPointer>
#include <QPolygonF>
#include <QQmlEngine>

#include <atomic>
#include <functional>
#include <map>
#include <memory>

class QFile;
class QNetworkReply;
class TileServer;
class TileStore;

/// Everything that is stored for offline use, as one list:
///
/// - routing regions: road graph and place names built from an OpenStreetMap
///   extract provided by Geofabrik, and
/// - map areas: the map itself for a part of the world, when the map source
///   permits downloading it.
class OfflineManager : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    /// Progress of lookUpRegion(): "idle", "loading", "ready", "none" or "error".
    Q_PROPERTY(QString lookupState READ lookupState NOTIFY regionChoicesChanged)
    /// The extracts that cover the centre of the area passed to lookUpRegion(),
    /// smallest first. Maps with the keys id, name, size (bytes, -1 while
    /// unknown), installed, downloading and tooLarge.
    Q_PROPERTY(QVariantList regionChoices READ regionChoices NOTIFY regionChoicesChanged)

public:
    enum Roles {
        KeyRole = Qt::UserRole + 1,
        SectionRole,
        NameRole,
        DetailRole,
        ProgressRole, ///< 0 to 1, or -1 when not measurable
        ActiveRole, ///< a download or import is running
    };

    struct RoutingRegion {
        QString id;
        QString name;
        QString directory;
        double south = 0;
        double west = 0;
        double north = 0;
        double east = 0;
        qint64 bytes = 0;
        QDateTime updated;
    };

    OfflineManager(TileStore *store, TileServer *server, QObject *parent = nullptr);
    ~OfflineManager() override;
    static OfflineManager *create(QQmlEngine *, QJSEngine *);
    static OfflineManager *instance();

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const;
    QString lookupState() const;
    QVariantList regionChoices() const;
    QList<RoutingRegion> routingRegions() const;

    Q_INVOKABLE void lookUpRegion(double south, double west, double north, double east);
    Q_INVOKABLE void downloadRegion(const QString &id);
    /// How many tiles the map of an area consists of. @p maxZoom limits the
    /// detail of maps made of images; vector maps have a single level.
    Q_INVOKABLE int tileCount(double south, double west, double north, double east, int maxZoom) const;
    /// Roughly how many bytes those tiles add up to.
    Q_INVOKABLE double estimatedSize(double south, double west, double north, double east, int maxZoom) const;
    Q_INVOKABLE void downloadTiles(const QString &name, double south, double west, double north, double east, int maxZoom);
    /// Deletes an entry, or cancels it when it is still being downloaded.
    Q_INVOKABLE void remove(const QString &key);

Q_SIGNALS:
    void countChanged();
    void regionChoicesChanged();
    void routingRegionsChanged();
    void completed(const QString &message);
    void failed(const QString &message);

private:
    struct Item {
        QString key;
        QString section;
        QString name;
        QString detail;
        double progress = -1;
        bool active = false;
    };
    struct ExtractInfo {
        QString id;
        QString name;
        QString url;
        QList<QPolygonF> outlines; ///< x is longitude, y is latitude
        QRectF bounds;
    };
    struct RoutingJob {
        QString id;
        QString name;
        QPointer<QNetworkReply> reply;
        std::unique_ptr<QFile> file;
        QFutureWatcher<ExtractImporter::Result> *watcher = nullptr;
        std::shared_ptr<std::atomic_bool> cancel;
    };
    struct TileId {
        int source;
        int z;
        int x;
        int y;
    };
    struct PackJob {
        qint64 pack = 0;
        int source = 0;
        QString name;
        QList<TileId> queue;
        int total = 0;
        int stored = 0;
        int missing = 0;
        int inFlight = 0;
        qint64 bytes = 0;
        bool tooLarge = false;
        bool resourcesPending = false;
        bool resourcesFailed = false;
    };

    void rebuild();
    void setItemState(const QString &key, const QString &detail, double progress);
    QString routingDirectory() const;
    void scanRoutingRegions();

    void loadIndex(const std::function<void(bool)> &done);
    bool parseIndex(const QByteArray &json);
    void setLookupState(const QString &state);
    /// Rebuilds regionChoices from the candidates and what is on disk.
    void updateChoices();

    void startRoutingDownload(const ExtractInfo &extract);
    void importExtract(const QString &id);
    /// Ends a job and cleans up. @p quiet suppresses the messages, for cancelling.
    void finishRoutingJob(const QString &id, const QString &error, bool quiet = false);

    QList<TileId> tilesFor(double south, double west, double north, double east, int maxZoom, qint64 limit) const;
    void pumpPack(const QString &key);

    TileStore *const m_store;
    TileServer *const m_server;
    QList<Item> m_items;
    QList<RoutingRegion> m_regions;
    QList<ExtractInfo> m_index;
    QString m_lookupState = QStringLiteral("idle");
    QList<ExtractInfo> m_candidates;
    QHash<QString, qint64> m_downloadSizes;
    int m_lookup = 0;
    std::map<QString, RoutingJob> m_routingJobs;
    std::map<QString, PackJob> m_packJobs;
};
