// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>

/// On-disk map storage in one SQLite database.
///
/// Tiles are keyed by map source, so changing the source never mixes styles.
/// Everything else a map style consists of (the style itself, fonts, icons)
/// is small, stored by address and kept for good.
/// A tile that belongs to an offline pack is kept until the pack is deleted;
/// all other tiles form a cache of what was looked at and are pruned by age.
class TileStore
{
public:
    struct Tile {
        bool found = false;
        QByteArray data;
        QByteArray etag;
        qint64 expires = 0; ///< seconds since the epoch
    };

    struct Resource {
        bool found = false;
        QByteArray data;
        QByteArray etag;
        qint64 expires = 0; ///< seconds since the epoch
    };

    struct Pack {
        qint64 id = 0;
        int source = 0; ///< the map style the pack was made for
        QString name;
        double south = 0;
        double west = 0;
        double north = 0;
        double east = 0;
        int maxZoom = 0;
        qint64 tiles = 0;
        qint64 bytes = 0;
        QDateTime created;
    };

    ~TileStore();
    bool open(const QString &path);
    QString errorString() const;

    /// Returns the id for a map source, creating it if needed. The name is
    /// whatever identifies the source for good, such as its address.
    /// A source stored under @p formerName, by an earlier version, is taken
    /// over under the new name together with its tiles.
    int sourceId(const QString &name, const QString &formerName = {});

    Tile tile(int source, int z, int x, int y);
    void putTile(int source, int z, int x, int y, const QByteArray &data, const QByteArray &etag, qint64 expires);
    void setExpiry(int source, int z, int x, int y, qint64 expires);

    Resource resource(const QString &url);
    void putResource(const QString &url, const QByteArray &data, const QByteArray &etag, qint64 expires);
    void setResourceExpiry(const QString &url, qint64 expires);

    qint64 createPack(const Pack &pack);
    void addPackTile(qint64 pack, int source, int z, int x, int y);
    /// Stores the final tile count and size of a pack.
    void finishPack(qint64 pack);
    void removePack(qint64 pack);
    QList<Pack> packs();

    void beginTransaction();
    void commitTransaction();

    /// Deletes the least recently used cached tiles beyond @p keep.
    void pruneCache(qint64 keep);

private:
    QString m_error;
};
