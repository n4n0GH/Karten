// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tilestore.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

using namespace Qt::StringLiterals;

namespace
{
const QString ConnectionName = u"tiles"_s;

QSqlDatabase db()
{
    return QSqlDatabase::database(ConnectionName);
}

/// Prepares and binds in one go. Values are bound positionally.
QSqlQuery run(const QString &sql, const QVariantList &values = {})
{
    QSqlQuery query(db());
    query.prepare(sql);
    for (const QVariant &value : values) {
        query.addBindValue(value);
    }
    if (!query.exec()) {
        qWarning("Tile store: %s (%s)", qPrintable(query.lastError().text()), qPrintable(sql));
    }
    return query;
}
}

TileStore::~TileStore()
{
    if (QSqlDatabase::contains(ConnectionName)) {
        db().close();
        QSqlDatabase::removeDatabase(ConnectionName);
    }
}

QString TileStore::errorString() const
{
    return m_error;
}

bool TileStore::open(const QString &path)
{
    QSqlDatabase database = QSqlDatabase::addDatabase(u"QSQLITE"_s, ConnectionName);
    database.setDatabaseName(path);
    if (!database.open()) {
        m_error = database.lastError().text();
        return false;
    }
    run(u"PRAGMA journal_mode = WAL"_s);
    run(u"PRAGMA synchronous = NORMAL"_s);
    run(u"CREATE TABLE IF NOT EXISTS sources (id INTEGER PRIMARY KEY, url TEXT UNIQUE NOT NULL)"_s);
    run(u"CREATE TABLE IF NOT EXISTS tiles ("
        "source INTEGER NOT NULL, z INTEGER NOT NULL, x INTEGER NOT NULL, y INTEGER NOT NULL, "
        "data BLOB NOT NULL, etag BLOB, expires INTEGER NOT NULL, accessed INTEGER NOT NULL, "
        "PRIMARY KEY (source, z, x, y)) WITHOUT ROWID"_s);
    run(u"CREATE INDEX IF NOT EXISTS tiles_accessed ON tiles (accessed)"_s);
    run(u"CREATE TABLE IF NOT EXISTS packs ("
        "id INTEGER PRIMARY KEY, source INTEGER NOT NULL, name TEXT NOT NULL, "
        "south REAL, west REAL, north REAL, east REAL, max_zoom INTEGER, "
        "tiles INTEGER DEFAULT 0, bytes INTEGER DEFAULT 0, created INTEGER)"_s);
    run(u"CREATE TABLE IF NOT EXISTS pack_tiles ("
        "source INTEGER NOT NULL, z INTEGER NOT NULL, x INTEGER NOT NULL, y INTEGER NOT NULL, pack INTEGER NOT NULL, "
        "PRIMARY KEY (source, z, x, y, pack)) WITHOUT ROWID"_s);
    run(u"CREATE TABLE IF NOT EXISTS resources (url TEXT PRIMARY KEY, data BLOB NOT NULL, etag BLOB, expires INTEGER NOT NULL) WITHOUT ROWID"_s);
    return true;
}

int TileStore::sourceId(const QString &name, const QString &formerName)
{
    if (!formerName.isEmpty()) {
        QSqlQuery known = run(u"SELECT 1 FROM sources WHERE url = ?"_s, {name});
        if (!known.next()) {
            known.finish();
            run(u"UPDATE sources SET url = ? WHERE url = ?"_s, {name, formerName});
        }
    }
    run(u"INSERT OR IGNORE INTO sources (url) VALUES (?)"_s, {name});
    QSqlQuery query = run(u"SELECT id FROM sources WHERE url = ?"_s, {name});
    return query.next() ? query.value(0).toInt() : 0;
}

TileStore::Resource TileStore::resource(const QString &url)
{
    Resource resource;
    QSqlQuery query = run(u"SELECT data, etag, expires FROM resources WHERE url = ?"_s, {url});
    if (query.next()) {
        resource.found = true;
        resource.data = query.value(0).toByteArray();
        resource.etag = query.value(1).toByteArray();
        resource.expires = query.value(2).toLongLong();
    }
    return resource;
}

void TileStore::putResource(const QString &url, const QByteArray &data, const QByteArray &etag, qint64 expires)
{
    run(u"INSERT OR REPLACE INTO resources (url, data, etag, expires) VALUES (?, ?, ?, ?)"_s, {url, data, etag, expires});
}

void TileStore::setResourceExpiry(const QString &url, qint64 expires)
{
    run(u"UPDATE resources SET expires = ? WHERE url = ?"_s, {expires, url});
}

TileStore::Tile TileStore::tile(int source, int z, int x, int y)
{
    Tile tile;
    QSqlQuery query = run(u"SELECT data, etag, expires, accessed FROM tiles WHERE source = ? AND z = ? AND x = ? AND y = ?"_s, {source, z, x, y});
    if (!query.next()) {
        return tile;
    }
    tile.found = true;
    tile.data = query.value(0).toByteArray();
    tile.etag = query.value(1).toByteArray();
    tile.expires = query.value(2).toLongLong();
    // Record use for cache pruning, but at most once a day per tile to keep reads cheap.
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (now - query.value(3).toLongLong() > 86400) {
        query.finish();
        run(u"UPDATE tiles SET accessed = ? WHERE source = ? AND z = ? AND x = ? AND y = ?"_s, {now, source, z, x, y});
    }
    return tile;
}

void TileStore::putTile(int source, int z, int x, int y, const QByteArray &data, const QByteArray &etag, qint64 expires)
{
    run(u"INSERT OR REPLACE INTO tiles (source, z, x, y, data, etag, expires, accessed) VALUES (?, ?, ?, ?, ?, ?, ?, ?)"_s,
        {source, z, x, y, data, etag, expires, QDateTime::currentSecsSinceEpoch()});
}

void TileStore::setExpiry(int source, int z, int x, int y, qint64 expires)
{
    run(u"UPDATE tiles SET expires = ? WHERE source = ? AND z = ? AND x = ? AND y = ?"_s, {expires, source, z, x, y});
}

qint64 TileStore::createPack(const Pack &pack)
{
    QSqlQuery query = run(u"INSERT INTO packs (source, name, south, west, north, east, max_zoom, created) VALUES (?, ?, ?, ?, ?, ?, ?, ?)"_s,
                          {pack.source, pack.name, pack.south, pack.west, pack.north, pack.east, pack.maxZoom, QDateTime::currentSecsSinceEpoch()});
    return query.lastInsertId().toLongLong();
}

void TileStore::addPackTile(qint64 pack, int source, int z, int x, int y)
{
    run(u"INSERT OR IGNORE INTO pack_tiles (source, z, x, y, pack) VALUES (?, ?, ?, ?, ?)"_s, {source, z, x, y, pack});
}

void TileStore::finishPack(qint64 pack)
{
    QSqlQuery query =
        run(u"SELECT COUNT(*), COALESCE(SUM(LENGTH(t.data)), 0) FROM pack_tiles p "
            "JOIN tiles t ON t.source = p.source AND t.z = p.z AND t.x = p.x AND t.y = p.y WHERE p.pack = ?"_s,
            {pack});
    if (query.next()) {
        const QVariant tiles = query.value(0);
        const QVariant bytes = query.value(1);
        query.finish();
        run(u"UPDATE packs SET tiles = ?, bytes = ? WHERE id = ?"_s, {tiles, bytes, pack});
    }
}

void TileStore::removePack(qint64 pack)
{
    // The tiles themselves stay behind as ordinary cache entries.
    run(u"DELETE FROM pack_tiles WHERE pack = ?"_s, {pack});
    run(u"DELETE FROM packs WHERE id = ?"_s, {pack});
}

QList<TileStore::Pack> TileStore::packs()
{
    QList<Pack> result;
    QSqlQuery query = run(u"SELECT id, source, name, south, west, north, east, max_zoom, tiles, bytes, created FROM packs ORDER BY name"_s);
    while (query.next()) {
        Pack pack;
        pack.id = query.value(0).toLongLong();
        pack.source = query.value(1).toInt();
        pack.name = query.value(2).toString();
        pack.south = query.value(3).toDouble();
        pack.west = query.value(4).toDouble();
        pack.north = query.value(5).toDouble();
        pack.east = query.value(6).toDouble();
        pack.maxZoom = query.value(7).toInt();
        pack.tiles = query.value(8).toLongLong();
        pack.bytes = query.value(9).toLongLong();
        pack.created = QDateTime::fromSecsSinceEpoch(query.value(10).toLongLong());
        result.append(pack);
    }
    return result;
}

void TileStore::beginTransaction()
{
    db().transaction();
}

void TileStore::commitTransaction()
{
    db().commit();
}

void TileStore::pruneCache(qint64 keep)
{
    const QString unpinned = u"NOT EXISTS (SELECT 1 FROM pack_tiles p WHERE p.source = tiles.source AND p.z = tiles.z AND p.x = tiles.x AND p.y = tiles.y)"_s;
    QSqlQuery query = run(u"SELECT accessed FROM tiles WHERE "_s + unpinned + u" ORDER BY accessed DESC LIMIT 1 OFFSET ?"_s, {keep});
    if (query.next()) {
        const QVariant threshold = query.value(0);
        query.finish();
        run(u"DELETE FROM tiles WHERE accessed <= ? AND "_s + unpinned, {threshold});
    }
}
