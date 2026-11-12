// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "offlinemanager.h"

#include "network.h"
#include "tileserver.h"
#include "tilestore.h"

#include <KFormat>
#include <KLocalizedString>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QtConcurrentRun>

#include <cmath>
#include <numbers>

using namespace Qt::StringLiterals;

namespace
{
OfflineManager *s_instance = nullptr;
const QString IndexUrl = u"https://download.geofabrik.de/index-v1.json"_s;
constexpr int ParallelTileDownloads = 2;
/// Building routing data needs several times the size of an extract in
/// memory, so very large extracts are not offered.
constexpr qint64 MaxExtractBytes = 1024LL * 1024 * 1024;
constexpr int MaxRegionChoices = 6;

QString routingKey(const QString &id)
{
    return u"routing:"_s + id;
}

QString packKey(qint64 pack)
{
    return u"tiles:"_s + QString::number(pack);
}

int tileX(double lon, int z)
{
    const int n = 1 << z;
    return std::clamp(int(std::floor((lon + 180.0) / 360.0 * n)), 0, n - 1);
}

int tileY(double lat, int z)
{
    const int n = 1 << z;
    const double rad = std::clamp(lat, -85.0511, 85.0511) * std::numbers::pi / 180.0;
    return std::clamp(int(std::floor((1.0 - std::asinh(std::tan(rad)) / std::numbers::pi) / 2.0 * n)), 0, n - 1);
}

qint64 directorySize(const QString &path)
{
    qint64 total = 0;
    const QFileInfoList files = QDir(path).entryInfoList(QDir::Files);
    for (const QFileInfo &file : files) {
        total += file.size();
    }
    return total;
}
}

OfflineManager::OfflineManager(TileStore *store, TileServer *server, QObject *parent)
    : QAbstractListModel(parent)
    , m_store(store)
    , m_server(server)
{
    s_instance = this;
    // Leftovers from a download that was interrupted by quitting.
    QDir dir(routingDirectory());
    const QStringList stale = dir.entryList({u"*.part"_s, u"*.new"_s}, QDir::Files | QDir::Dirs);
    for (const QString &name : stale) {
        if (!dir.remove(name)) {
            QDir(dir.filePath(name)).removeRecursively();
        }
    }
    scanRoutingRegions();
    rebuild();
}

OfflineManager::~OfflineManager()
{
    for (auto &[id, job] : m_routingJobs) {
        if (job.reply) {
            // Detach first: the reply's handlers would otherwise modify m_routingJobs.
            job.reply->disconnect(this);
            job.reply->abort();
            job.file->remove();
        }
        if (job.watcher) {
            job.cancel->store(true);
            job.watcher->disconnect(this);
            job.watcher->waitForFinished();
        }
    }
    s_instance = nullptr;
}

OfflineManager *OfflineManager::create(QQmlEngine *, QJSEngine *)
{
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

OfflineManager *OfflineManager::instance()
{
    return s_instance;
}

int OfflineManager::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_items.size());
}

QVariant OfflineManager::data(const QModelIndex &index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid)) {
        return {};
    }
    const Item &item = m_items[index.row()];
    switch (role) {
    case KeyRole:
        return item.key;
    case SectionRole:
        return item.section;
    case NameRole:
        return item.name;
    case DetailRole:
        return item.detail;
    case ProgressRole:
        return item.progress;
    case ActiveRole:
        return item.active;
    }
    return {};
}

QHash<int, QByteArray> OfflineManager::roleNames() const
{
    return {
        {KeyRole, "key"},
        {SectionRole, "section"},
        {NameRole, "name"},
        {DetailRole, "detail"},
        {ProgressRole, "progress"},
        {ActiveRole, "active"},
    };
}

int OfflineManager::count() const
{
    return int(m_items.size());
}

QString OfflineManager::lookupState() const
{
    return m_lookupState;
}

QVariantList OfflineManager::regionChoices() const
{
    QVariantList choices;
    for (const ExtractInfo &info : m_candidates) {
        bool installed = false;
        for (const RoutingRegion &region : m_regions) {
            installed = installed || region.id == info.id;
        }
        const qint64 size = m_downloadSizes.value(info.id, -1);
        choices.append(QVariantMap{
            {u"id"_s, info.id},
            {u"name"_s, info.name},
            {u"size"_s, double(size)},
            {u"installed"_s, installed},
            {u"downloading"_s, m_routingJobs.contains(info.id)},
            {u"tooLarge"_s, size > MaxExtractBytes},
        });
    }
    return choices;
}

QList<OfflineManager::RoutingRegion> OfflineManager::routingRegions() const
{
    return m_regions;
}

QString OfflineManager::routingDirectory() const
{
    const QString path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + u"/routing"_s;
    QDir().mkpath(path);
    return path;
}

void OfflineManager::scanRoutingRegions()
{
    m_regions.clear();
    const QDir dir(routingDirectory());
    const QStringList entries = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &entry : entries) {
        const QString path = dir.filePath(entry);
        QFile meta(path + u"/meta.json"_s);
        if (!meta.open(QIODevice::ReadOnly) || !QFile::exists(ExtractImporter::graphPath(path))) {
            continue;
        }
        const QJsonObject object = QJsonDocument::fromJson(meta.readAll()).object();
        RoutingRegion region;
        region.id = entry;
        region.name = object["name"_L1].toString(entry);
        region.directory = path;
        region.south = object["south"_L1].toDouble();
        region.west = object["west"_L1].toDouble();
        region.north = object["north"_L1].toDouble();
        region.east = object["east"_L1].toDouble();
        region.updated = QDateTime::fromSecsSinceEpoch(qint64(object["updated"_L1].toDouble()));
        region.bytes = directorySize(path);
        m_regions.append(region);
    }
}

void OfflineManager::rebuild()
{
    const KFormat format;
    const QString routingSection = i18nc("@title:group", "Routing and Search");
    const QString tilesSection = i18nc("@title:group", "Map Areas");
    QList<Item> items;

    for (const RoutingRegion &region : std::as_const(m_regions)) {
        if (m_routingJobs.contains(region.id)) {
            continue; // shown as a running download below
        }
        Item item;
        item.key = routingKey(region.id);
        item.section = routingSection;
        item.name = region.name;
        item.detail =
            i18nc("size, date", "%1, updated %2", format.formatByteSize(double(region.bytes)), QLocale().toString(region.updated.date(), QLocale::ShortFormat));
        items.append(item);
    }
    for (const auto &[id, job] : m_routingJobs) {
        Item item;
        item.key = routingKey(id);
        item.section = routingSection;
        item.name = job.name;
        item.detail = i18nc("@info:status", "Starting download…");
        item.active = true;
        // Keep the progress that is already on screen.
        for (const Item &old : std::as_const(m_items)) {
            if (old.key == item.key && old.active) {
                item.detail = old.detail;
                item.progress = old.progress;
            }
        }
        items.append(item);
    }

    const QList<TileStore::Pack> packs = m_store->packs();
    for (const TileStore::Pack &pack : packs) {
        Item item;
        item.key = packKey(pack.id);
        item.section = tilesSection;
        item.name = pack.name;
        const auto job = m_packJobs.find(item.key);
        if (job != m_packJobs.end()) {
            item.active = true;
            item.progress = double(job->second.stored + job->second.missing) / std::max(job->second.total, 1);
            item.detail = i18nc("@info:status", "Downloading the map…");
        } else {
            item.detail = i18ncp("tile count, size", "%1 map tile, %2", "%1 map tiles, %2", pack.tiles, format.formatByteSize(double(pack.bytes)));
        }
        items.append(item);
    }

    beginResetModel();
    m_items = items;
    endResetModel();
    Q_EMIT countChanged();
}

void OfflineManager::setItemState(const QString &key, const QString &detail, double progress)
{
    for (int row = 0; row < m_items.size(); ++row) {
        if (m_items[row].key == key) {
            m_items[row].detail = detail;
            m_items[row].progress = progress;
            const QModelIndex changed = index(row);
            Q_EMIT dataChanged(changed, changed, {DetailRole, ProgressRole});
            return;
        }
    }
}

// --- Geofabrik extract index ---

bool OfflineManager::parseIndex(const QByteArray &json)
{
    const QJsonArray features = QJsonDocument::fromJson(json).object()["features"_L1].toArray();
    QList<ExtractInfo> index;
    static const QRegularExpression validId(u"^[a-z0-9_-]+$"_s);
    for (const QJsonValue &feature : features) {
        const QJsonObject properties = feature["properties"_L1].toObject();
        ExtractInfo info;
        // Ids such as "us/california" become directory names.
        info.id = properties["id"_L1].toString().replace(u'/', u'_');
        info.name = properties["name"_L1].toString();
        info.url = properties["urls"_L1]["pbf"_L1].toString();
        if (!validId.match(info.id).hasMatch() || !info.url.startsWith("https://download.geofabrik.de/"_L1)) {
            continue;
        }
        const QJsonObject geometry = feature["geometry"_L1].toObject();
        QJsonArray polygons = geometry["coordinates"_L1].toArray();
        if (geometry["type"_L1].toString() == "Polygon"_L1) {
            polygons = QJsonArray{polygons};
        }
        for (const QJsonValue &polygon : std::as_const(polygons)) {
            // The first ring is the outline; holes do not matter here.
            const QJsonArray ring = polygon[0].toArray();
            QPolygonF outline;
            outline.reserve(ring.size());
            for (const QJsonValue &point : ring) {
                outline.append(QPointF(point[0].toDouble(), point[1].toDouble()));
            }
            if (outline.size() >= 3) {
                info.bounds = info.bounds.united(outline.boundingRect());
                info.outlines.append(outline);
            }
        }
        if (!info.outlines.isEmpty()) {
            index.append(info);
        }
    }
    if (index.isEmpty()) {
        return false;
    }
    m_index = index;
    return true;
}

void OfflineManager::loadIndex(const std::function<void(bool)> &done)
{
    if (!m_index.isEmpty()) {
        done(true);
        return;
    }
    const QString cachePath = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + u"/geofabrik-index-v1.json"_s;
    const QFileInfo cached(cachePath);
    const auto loadCached = [this, cachePath] {
        QFile file(cachePath);
        return file.open(QIODevice::ReadOnly) && parseIndex(file.readAll());
    };
    if (cached.exists() && cached.lastModified().daysTo(QDateTime::currentDateTime()) < 30 && loadCached()) {
        done(true);
        return;
    }
    QNetworkReply *reply = Network::manager()->get(Network::request(QUrl(IndexUrl)));
    connect(reply, &QNetworkReply::finished, this, [=, this] {
        reply->deleteLater();
        const QByteArray json = reply->readAll();
        if (reply->error() == QNetworkReply::NoError && parseIndex(json)) {
            QDir().mkpath(QFileInfo(cachePath).path());
            QSaveFile file(cachePath);
            if (file.open(QIODevice::WriteOnly)) {
                file.write(json);
                file.commit();
            }
            done(true);
        } else {
            done(loadCached()); // an outdated list is still useful
        }
    });
}

void OfflineManager::setLookupState(const QString &state)
{
    m_lookupState = state;
    Q_EMIT regionChoicesChanged();
}

void OfflineManager::updateChoices()
{
    Q_EMIT regionChoicesChanged();
}

void OfflineManager::lookUpRegion(double south, double west, double north, double east)
{
    const int lookup = ++m_lookup;
    m_candidates.clear();
    setLookupState(u"loading"_s);
    loadIndex([=, this](bool ok) {
        if (lookup != m_lookup) {
            return;
        }
        if (!ok) {
            setLookupState(u"error"_s);
            return;
        }
        // Extracts overlap and nest (a district, its state, its country, the
        // continent), so offer every one that covers the centre of the view.
        const QPointF centre((west + east) / 2, (south + north) / 2);
        for (const ExtractInfo &info : std::as_const(m_index)) {
            if (!info.bounds.contains(centre)) {
                continue;
            }
            for (const QPolygonF &outline : info.outlines) {
                if (outline.containsPoint(centre, Qt::OddEvenFill)) {
                    m_candidates.append(info);
                    break;
                }
            }
        }
        std::sort(m_candidates.begin(), m_candidates.end(), [](const ExtractInfo &a, const ExtractInfo &b) {
            return a.bounds.width() * a.bounds.height() < b.bounds.width() * b.bounds.height();
        });
        if (m_candidates.size() > MaxRegionChoices) {
            m_candidates.resize(MaxRegionChoices);
        }
        setLookupState(m_candidates.isEmpty() ? u"none"_s : u"ready"_s);

        // Ask for the download sizes without fetching the files.
        for (const ExtractInfo &info : std::as_const(m_candidates)) {
            if (m_downloadSizes.contains(info.id)) {
                continue;
            }
            const QString id = info.id;
            QNetworkReply *reply = Network::manager()->head(Network::request(QUrl(info.url)));
            connect(reply, &QNetworkReply::finished, this, [this, reply, id] {
                reply->deleteLater();
                const qint64 size = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
                if (reply->error() == QNetworkReply::NoError && size > 0) {
                    m_downloadSizes.insert(id, size);
                    updateChoices();
                }
            });
        }
    });
}

// --- Routing regions ---

void OfflineManager::downloadRegion(const QString &id)
{
    for (const ExtractInfo &info : std::as_const(m_candidates)) {
        if (info.id == id && m_downloadSizes.value(id, 0) <= MaxExtractBytes) {
            startRoutingDownload(info);
            return;
        }
    }
}

void OfflineManager::startRoutingDownload(const ExtractInfo &extract)
{
    if (m_routingJobs.contains(extract.id)) {
        return;
    }
    const QString id = extract.id;
    RoutingJob &job = m_routingJobs[id];
    job.id = id;
    job.name = extract.name;
    job.cancel = std::make_shared<std::atomic_bool>(false);
    job.file = std::make_unique<QFile>(routingDirectory() + u'/' + id + u".osm.pbf.part"_s);
    if (!job.file->open(QIODevice::WriteOnly)) {
        const QString error = job.file->errorString();
        finishRoutingJob(id, error);
        return;
    }

    QNetworkRequest request = Network::request(QUrl(extract.url));
    request.setTransferTimeout(60000);
    QNetworkReply *reply = Network::manager()->get(request);
    job.reply = reply;
    rebuild();
    updateChoices();

    connect(reply, &QNetworkReply::readyRead, this, [this, id, reply] {
        const auto it = m_routingJobs.find(id);
        if (it != m_routingJobs.end() && it->second.file) {
            it->second.file->write(reply->readAll());
        }
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [this, id](qint64 received, qint64 total) {
        const KFormat format;
        if (total > 0) {
            setItemState(routingKey(id),
                         i18nc("@info:status downloaded size of total size",
                               "Downloading: %1 of %2",
                               format.formatByteSize(double(received)),
                               format.formatByteSize(double(total))),
                         double(received) / double(total));
        } else {
            setItemState(routingKey(id), i18nc("@info:status", "Downloading: %1", format.formatByteSize(double(received))), -1);
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, id, reply] {
        reply->deleteLater();
        const auto it = m_routingJobs.find(id);
        if (it == m_routingJobs.end()) {
            return; // cancelled
        }
        it->second.file->write(reply->readAll());
        it->second.file->close();
        if (reply->error() != QNetworkReply::NoError) {
            finishRoutingJob(id, reply->errorString());
        } else {
            importExtract(id);
        }
    });
}

void OfflineManager::importExtract(const QString &id)
{
    RoutingJob &job = m_routingJobs[id];
    const QString pbfPath = job.file->fileName();
    const QString directory = routingDirectory() + u'/' + id + u".new"_s;
    setItemState(routingKey(id), i18nc("@info:status", "Preparing routing data…"), 0);

    job.watcher = new QFutureWatcher<ExtractImporter::Result>(this);
    connect(job.watcher, &QFutureWatcherBase::finished, this, [this, id] {
        const auto it = m_routingJobs.find(id);
        if (it == m_routingJobs.end()) {
            return;
        }
        const ExtractImporter::Result result = it->second.watcher->result();
        it->second.watcher->deleteLater();
        it->second.watcher = nullptr;
        if (it->second.cancel->load()) {
            finishRoutingJob(id, {}, true);
            return;
        }
        if (!result.error.isEmpty()) {
            finishRoutingJob(id, result.error);
            return;
        }
        const QString base = routingDirectory() + u'/' + id;
        QFile meta(base + u".new/meta.json"_s);
        if (meta.open(QIODevice::WriteOnly)) {
            meta.write(QJsonDocument(QJsonObject{
                                         {"name"_L1, it->second.name},
                                         {"south"_L1, result.south},
                                         {"west"_L1, result.west},
                                         {"north"_L1, result.north},
                                         {"east"_L1, result.east},
                                         {"updated"_L1, double(QDateTime::currentSecsSinceEpoch())},
                                     })
                           .toJson());
            meta.close();
        }
        // Replace an older copy of the same region.
        QDir(base).removeRecursively();
        if (!QDir().rename(base + u".new"_s, base)) {
            finishRoutingJob(id, i18n("Could not store the routing data."));
            return;
        }
        finishRoutingJob(id, {});
    });

    const std::shared_ptr<std::atomic_bool> cancel = job.cancel;
    QPointer<OfflineManager> self(this);
    job.watcher->setFuture(QtConcurrent::run([=] {
        return ExtractImporter::run(pbfPath, directory, [&](int percent) {
            if (cancel->load()) {
                return false;
            }
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [self, id, percent] {
                    if (self && self->m_routingJobs.contains(id)) {
                        self->setItemState(routingKey(id), i18nc("@info:status", "Preparing routing data…"), percent / 100.0);
                    }
                },
                Qt::QueuedConnection);
            return true;
        });
    }));
}

void OfflineManager::finishRoutingJob(const QString &id, const QString &error, bool quiet)
{
    const auto it = m_routingJobs.find(id);
    if (it == m_routingJobs.end()) {
        return;
    }
    const QString name = it->second.name;
    if (it->second.file) {
        it->second.file->remove();
    }
    m_routingJobs.erase(it);
    QDir(routingDirectory() + u'/' + id + u".new"_s).removeRecursively();

    scanRoutingRegions();
    rebuild();
    updateChoices();
    if (quiet) {
        return;
    }
    if (error.isEmpty()) {
        Q_EMIT routingRegionsChanged();
        Q_EMIT completed(i18nc("@info", "%1 is ready for offline routing and search", name));
    } else {
        Q_EMIT failed(i18nc("@info region name, error message", "Could not download %1: %2", name, error));
    }
}

// --- Map areas ---

QList<OfflineManager::TileId> OfflineManager::tilesFor(double south, double west, double north, double east, int maxZoom, qint64 limit) const
{
    QList<TileId> tiles;
    qint64 total = 0;
    const QList<TileServer::TileSource> sources = m_server->packSources();
    // Coarse levels first: they cover the most with the fewest tiles.
    for (int z = 0; z <= maxZoom; ++z) {
        for (const TileServer::TileSource &source : sources) {
            if (z < source.minZoom || z > source.maxZoom) {
                continue;
            }
            const int left = tileX(west, z);
            const int right = tileX(east, z);
            const int top = tileY(north, z);
            const int bottom = tileY(south, z);
            total += qint64(right - left + 1) * qint64(bottom - top + 1);
            if (total > limit) {
                return tiles; // only the count matters from here on
            }
            for (int x = left; x <= right; ++x) {
                for (int y = top; y <= bottom; ++y) {
                    tiles.append({source.id, z, x, source.tms ? (1 << z) - 1 - y : y});
                }
            }
        }
    }
    return tiles;
}

int OfflineManager::tileCount(double south, double west, double north, double east, int maxZoom) const
{
    qint64 total = 0;
    const QList<TileServer::TileSource> sources = m_server->packSources();
    for (const TileServer::TileSource &source : sources) {
        for (int z = source.minZoom; z <= std::min(maxZoom, source.maxZoom); ++z) {
            total += qint64(tileX(east, z) - tileX(west, z) + 1) * qint64(tileY(south, z) - tileY(north, z) + 1);
        }
    }
    return int(std::min<qint64>(total, std::numeric_limits<int>::max()));
}

double OfflineManager::estimatedSize(double south, double west, double north, double east, int maxZoom) const
{
    // Typical sizes as measured with OpenFreeMap: a vector tile of a whole
    // country holds far more than one of a few streets.
    const auto typicalSize = [](const TileServer::TileSource &source, int z) {
        if (!source.vector) {
            return z <= 6 ? 250'000 : 20'000;
        }
        return z <= 9 ? 300'000 : z == 10 ? 125'000 : z == 11 ? 70'000 : z == 12 ? 40'000 : 25'000;
    };
    double total = 0;
    const QList<TileServer::TileSource> sources = m_server->packSources();
    for (const TileServer::TileSource &source : sources) {
        for (int z = source.minZoom; z <= std::min(maxZoom, source.maxZoom); ++z) {
            total += double(tileX(east, z) - tileX(west, z) + 1) * double(tileY(south, z) - tileY(north, z) + 1) * typicalSize(source, z);
        }
    }
    return total;
}

void OfflineManager::downloadTiles(const QString &name, double south, double west, double north, double east, int maxZoom)
{
    if (!m_server->offlineAllowed()) {
        Q_EMIT failed(i18nc("@info", "The current map source does not permit downloading its map for offline use."));
        return;
    }
    const int count = tileCount(south, west, north, east, maxZoom);
    if (west > east || south > north || count == 0 || count > m_server->maxPackTiles()) {
        Q_EMIT failed(i18nc("@info", "This area is too large to download. Zoom in and try again."));
        return;
    }
    TileStore::Pack pack;
    pack.source = m_server->styleId();
    pack.name = name.trimmed().isEmpty() ? i18nc("default name of a downloaded map area", "Map area") : name.trimmed();
    pack.south = south;
    pack.west = west;
    pack.north = north;
    pack.east = east;
    pack.maxZoom = maxZoom;

    PackJob job;
    job.pack = m_store->createPack(pack);
    job.source = pack.source;
    job.name = pack.name;
    job.queue = tilesFor(south, west, north, east, maxZoom, count);
    job.total = int(job.queue.size());
    job.resourcesPending = true;
    const QString key = packKey(job.pack);
    m_packJobs[key] = job;
    rebuild();

    // The fonts and icons the map is drawn with.
    m_server->fetchStyleResources([this, key](bool ok) {
        const auto found = m_packJobs.find(key);
        if (found != m_packJobs.end()) {
            found->second.resourcesPending = false;
            found->second.resourcesFailed = !ok;
            QTimer::singleShot(0, this, [this, key] {
                pumpPack(key);
            });
        }
    });
    pumpPack(key);
}

void OfflineManager::pumpPack(const QString &key)
{
    const auto it = m_packJobs.find(key);
    if (it == m_packJobs.end()) {
        return;
    }
    PackJob &job = it->second;

    if (job.source != m_server->styleId()) {
        job.queue.clear(); // the map source changed underneath us
    }
    if (job.queue.isEmpty() && job.inFlight == 0 && !job.resourcesPending) {
        const qint64 pack = job.pack;
        const QString name = job.name;
        const bool complete = job.stored + job.missing == job.total && !job.resourcesFailed;
        const bool empty = job.stored == 0;
        const bool tooLarge = job.tooLarge;
        m_packJobs.erase(it);
        if (tooLarge) {
            m_store->removePack(pack);
            Q_EMIT failed(i18nc("@info", "%1 holds more map data than the map source permits in one download. Zoom in and try again.", name));
        } else if (empty) {
            m_store->removePack(pack);
            Q_EMIT failed(i18nc("@info", "Could not download the map of %1.", name));
        } else {
            m_store->finishPack(pack);
            if (complete) {
                Q_EMIT completed(i18nc("@info", "%1 is ready for offline use", name));
            } else {
                Q_EMIT failed(i18nc("@info", "Parts of the map of %1 could not be downloaded.", name));
            }
        }
        rebuild();
        return;
    }

    const qint64 maxBytes = m_server->maxPackBytes();
    while (job.inFlight < ParallelTileDownloads && !job.queue.isEmpty()) {
        const TileId tile = job.queue.takeFirst();
        ++job.inFlight;
        m_server->fetchTile(tile.source, tile.z, tile.x, tile.y, [this, key, tile, maxBytes](const QByteArray &data, TileServer::Status status) {
            const auto found = m_packJobs.find(key);
            if (found == m_packJobs.end()) {
                return; // cancelled
            }
            PackJob &running = found->second;
            --running.inFlight;
            if (status == TileServer::Ok) {
                ++running.stored;
                running.bytes += data.size();
                m_store->addPackTile(running.pack, tile.source, tile.z, tile.x, tile.y);
            } else if (status == TileServer::Missing) {
                ++running.missing; // nothing to draw there, which is complete as well
            }
            if (running.bytes > maxBytes) {
                running.tooLarge = true;
                running.queue.clear();
            }
            const int done = running.total - int(running.queue.size()) - running.inFlight;
            if (done % 10 == 0 || done == running.total) {
                setItemState(key,
                             i18nc("@info:status", "Downloading the map: %1%", done * 100 / std::max(1, running.total)),
                             double(done) / std::max(1, running.total));
            }
            // Tiles that are already stored answer immediately, so continue
            // from the event loop instead of recursing.
            QTimer::singleShot(0, this, [this, key] {
                pumpPack(key);
            });
        });
    }
}

void OfflineManager::remove(const QString &key)
{
    if (key.startsWith("routing:"_L1)) {
        const QString id = key.mid(8);
        const auto job = m_routingJobs.find(id);
        if (job != m_routingJobs.end()) {
            if (job->second.watcher) {
                // The import notices the flag and stops; its finished handler cleans up.
                job->second.cancel->store(true);
                return;
            }
            QPointer<QNetworkReply> reply = job->second.reply;
            job->second.file->remove();
            m_routingJobs.erase(job);
            if (reply) {
                reply->abort();
            }
            updateChoices();
            rebuild();
            return;
        }
        static const QRegularExpression validId(u"^[a-z0-9_-]+$"_s);
        if (validId.match(id).hasMatch()) {
            QDir(routingDirectory() + u'/' + id).removeRecursively();
        }
        scanRoutingRegions();
        rebuild();
        updateChoices();
        Q_EMIT routingRegionsChanged();
    } else if (key.startsWith("tiles:"_L1)) {
        m_packJobs.erase(key);
        m_store->removePack(key.mid(6).toLongLong());
        rebuild();
    }
}
