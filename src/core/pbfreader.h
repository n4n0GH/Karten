// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArrayView>
#include <QString>

#include <functional>
#include <vector>

class QIODevice;

namespace Osm
{
struct Tag {
    QByteArrayView key;
    QByteArrayView value;
};
using Tags = std::vector<Tag>;

/// Returns the value for @p key, or an empty view.
QByteArrayView tagValue(const Tags &tags, QByteArrayView key);

/// Receives OSM primitives. Tag views are only valid during the call.
class Handler
{
public:
    virtual ~Handler();
    virtual void node(qint64 id, double lat, double lon, const Tags &tags);
    virtual void way(qint64 id, const std::vector<qint64> &refs, const Tags &tags);
};

/// Minimal streaming reader for the OSM PBF format (zlib or uncompressed blobs).
class PbfReader
{
public:
    enum Want {
        Nodes = 1,
        Ways = 2,
    };

    /// Reads @p device to the end. @p progress receives the byte position and
    /// returns false to cancel.
    bool read(QIODevice *device, Handler *handler, int want, const std::function<bool(qint64)> &progress = {});
    QString errorString() const;

private:
    bool parseBlock(QByteArrayView data, Handler *handler, int want);
    QString m_error;
    Tags m_tags;
    std::vector<qint64> m_refs;
    std::vector<QByteArrayView> m_strings;
};
}
