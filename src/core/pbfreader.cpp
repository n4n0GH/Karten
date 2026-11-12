// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pbfreader.h"

#include <QByteArray>
#include <QIODevice>
#include <QtEndian>

using namespace Qt::StringLiterals;

namespace Osm
{
namespace
{
/// Cursor over a protobuf-encoded message.
struct Pb {
    const uchar *p;
    const uchar *end;
    bool ok = true;

    explicit Pb(QByteArrayView v)
        : p(reinterpret_cast<const uchar *>(v.data()))
        , end(p + v.size())
    {
    }

    bool atEnd() const
    {
        return !ok || p >= end;
    }

    quint64 varint()
    {
        quint64 result = 0;
        int shift = 0;
        while (p < end && shift <= 63) {
            const uchar b = *p++;
            result |= quint64(b & 0x7f) << shift;
            if (!(b & 0x80)) {
                return result;
            }
            shift += 7;
        }
        ok = false;
        return 0;
    }

    qint64 svarint()
    {
        const quint64 v = varint();
        return qint64(v >> 1) ^ -qint64(v & 1);
    }

    bool key(quint32 &field, quint32 &wire)
    {
        if (atEnd()) {
            return false;
        }
        const quint64 k = varint();
        field = quint32(k >> 3);
        wire = quint32(k & 7);
        return ok;
    }

    QByteArrayView bytes()
    {
        const quint64 n = varint();
        if (!ok || n > quint64(end - p)) {
            ok = false;
            return {};
        }
        const QByteArrayView v(reinterpret_cast<const char *>(p), qsizetype(n));
        p += n;
        return v;
    }

    void advance(quint64 n)
    {
        if (n > quint64(end - p)) {
            ok = false;
        } else {
            p += n;
        }
    }

    void skip(quint32 wire)
    {
        switch (wire) {
        case 0:
            varint();
            break;
        case 1:
            advance(8);
            break;
        case 2:
            bytes();
            break;
        case 5:
            advance(4);
            break;
        default:
            ok = false;
        }
    }
};
}

QByteArrayView tagValue(const Tags &tags, QByteArrayView key)
{
    for (const Tag &tag : tags) {
        if (tag.key == key) {
            return tag.value;
        }
    }
    return {};
}

Handler::~Handler() = default;

void Handler::node(qint64, double, double, const Tags &)
{
}

void Handler::way(qint64, const std::vector<qint64> &, const Tags &)
{
}

QString PbfReader::errorString() const
{
    return m_error;
}

bool PbfReader::read(QIODevice *device, Handler *handler, int want, const std::function<bool(qint64)> &progress)
{
    m_error.clear();
    qint64 pos = 0;
    while (true) {
        uchar lenBuf[4];
        const qint64 got = device->read(reinterpret_cast<char *>(lenBuf), 4);
        if (got == 0) {
            break; // clean end of file
        }
        if (got != 4) {
            m_error = u"Truncated file"_s;
            return false;
        }
        const quint32 headerLen = qFromBigEndian<quint32>(lenBuf);
        if (headerLen > 64 * 1024) {
            m_error = u"Not an OSM PBF file"_s;
            return false;
        }
        const QByteArray header = device->read(headerLen);
        if (header.size() != qsizetype(headerLen)) {
            m_error = u"Truncated file"_s;
            return false;
        }

        QByteArrayView type;
        qint64 dataSize = -1;
        Pb h{QByteArrayView(header)};
        quint32 field = 0;
        quint32 wire = 0;
        while (h.key(field, wire)) {
            if (field == 1 && wire == 2) {
                type = h.bytes();
            } else if (field == 3 && wire == 0) {
                dataSize = qint64(h.varint());
            } else {
                h.skip(wire);
            }
        }
        if (!h.ok || dataSize < 0 || dataSize > 64 * 1024 * 1024) {
            m_error = u"Not an OSM PBF file"_s;
            return false;
        }

        const QByteArray blob = device->read(dataSize);
        if (blob.size() != dataSize) {
            m_error = u"Truncated file"_s;
            return false;
        }
        pos += 4 + headerLen + dataSize;

        if (type == "OSMData") {
            QByteArrayView raw;
            QByteArrayView zlib;
            qint64 rawSize = 0;
            bool unsupported = false;
            Pb b{QByteArrayView(blob)};
            while (b.key(field, wire)) {
                if (field == 1 && wire == 2) {
                    raw = b.bytes();
                } else if (field == 2 && wire == 0) {
                    rawSize = qint64(b.varint());
                } else if (field == 3 && wire == 2) {
                    zlib = b.bytes();
                } else {
                    unsupported = unsupported || (wire == 2 && field >= 4 && field <= 7);
                    b.skip(wire);
                }
            }
            if (!b.ok) {
                m_error = u"Corrupt data block"_s;
                return false;
            }

            QByteArray inflated;
            if (!zlib.isEmpty()) {
                // qUncompress() expects the uncompressed size as a big-endian prefix.
                QByteArray prefixed(4 + zlib.size(), Qt::Uninitialized);
                qToBigEndian<quint32>(quint32(rawSize), prefixed.data());
                memcpy(prefixed.data() + 4, zlib.data(), size_t(zlib.size()));
                inflated = qUncompress(prefixed);
                if (inflated.isEmpty() && rawSize > 0) {
                    m_error = u"Could not decompress a data block"_s;
                    return false;
                }
                raw = inflated;
            } else if (raw.isEmpty() && unsupported) {
                m_error = u"This file uses a compression method that is not supported"_s;
                return false;
            }

            if (!parseBlock(raw, handler, want)) {
                m_error = u"Corrupt data block"_s;
                return false;
            }
        }

        if (progress && !progress(pos)) {
            m_error = u"Cancelled"_s;
            return false;
        }
    }
    return true;
}

bool PbfReader::parseBlock(QByteArrayView data, Handler *handler, int want)
{
    m_strings.clear();
    std::vector<QByteArrayView> groups;
    qint64 granularity = 100;
    qint64 latOffset = 0;
    qint64 lonOffset = 0;

    Pb block{data};
    quint32 field = 0;
    quint32 wire = 0;
    while (block.key(field, wire)) {
        if (field == 1 && wire == 2) {
            Pb table{block.bytes()};
            quint32 f = 0;
            quint32 w = 0;
            while (table.key(f, w)) {
                if (f == 1 && w == 2) {
                    m_strings.push_back(table.bytes());
                } else {
                    table.skip(w);
                }
            }
            if (!table.ok) {
                return false;
            }
        } else if (field == 2 && wire == 2) {
            groups.push_back(block.bytes());
        } else if (field == 17 && wire == 0) {
            granularity = qint64(block.varint());
        } else if (field == 19 && wire == 0) {
            latOffset = qint64(block.varint());
        } else if (field == 20 && wire == 0) {
            lonOffset = qint64(block.varint());
        } else {
            block.skip(wire);
        }
    }
    if (!block.ok) {
        return false;
    }

    const auto string = [this](quint64 index) {
        return index < m_strings.size() ? m_strings[index] : QByteArrayView();
    };
    const auto coordinate = [granularity](qint64 offset, qint64 value) {
        return 1e-9 * double(offset + granularity * value);
    };
    // Reads the packed key and value index arrays shared by nodes and ways.
    const auto readTags = [this, &string](QByteArrayView keys, QByteArrayView values) {
        m_tags.clear();
        Pb k{keys};
        Pb v{values};
        while (!k.atEnd() && !v.atEnd()) {
            const QByteArrayView key = string(k.varint());
            const QByteArrayView value = string(v.varint());
            m_tags.push_back({key, value});
        }
    };

    for (const QByteArrayView groupData : groups) {
        Pb group{groupData};
        while (group.key(field, wire)) {
            if (wire != 2) {
                group.skip(wire);
                continue;
            }
            const QByteArrayView message = group.bytes();
            if (!group.ok) {
                return false;
            }

            if (field == 1 && (want & Nodes)) {
                qint64 id = 0;
                qint64 lat = 0;
                qint64 lon = 0;
                QByteArrayView keys;
                QByteArrayView values;
                Pb n{message};
                quint32 f = 0;
                quint32 w = 0;
                while (n.key(f, w)) {
                    if (f == 1 && w == 0) {
                        id = n.svarint();
                    } else if (f == 2 && w == 2) {
                        keys = n.bytes();
                    } else if (f == 3 && w == 2) {
                        values = n.bytes();
                    } else if (f == 8 && w == 0) {
                        lat = n.svarint();
                    } else if (f == 9 && w == 0) {
                        lon = n.svarint();
                    } else {
                        n.skip(w);
                    }
                }
                if (!n.ok) {
                    return false;
                }
                readTags(keys, values);
                handler->node(id, coordinate(latOffset, lat), coordinate(lonOffset, lon), m_tags);
            } else if (field == 2 && (want & Nodes)) {
                QByteArrayView ids;
                QByteArrayView lats;
                QByteArrayView lons;
                QByteArrayView keysValues;
                Pb d{message};
                quint32 f = 0;
                quint32 w = 0;
                while (d.key(f, w)) {
                    if (w != 2) {
                        d.skip(w);
                        continue;
                    }
                    const QByteArrayView v = d.bytes();
                    if (f == 1) {
                        ids = v;
                    } else if (f == 8) {
                        lats = v;
                    } else if (f == 9) {
                        lons = v;
                    } else if (f == 10) {
                        keysValues = v;
                    }
                }
                if (!d.ok) {
                    return false;
                }
                Pb idCursor{ids};
                Pb latCursor{lats};
                Pb lonCursor{lons};
                Pb tagCursor{keysValues};
                qint64 id = 0;
                qint64 lat = 0;
                qint64 lon = 0;
                while (!idCursor.atEnd()) {
                    id += idCursor.svarint();
                    lat += latCursor.svarint();
                    lon += lonCursor.svarint();
                    m_tags.clear();
                    while (!tagCursor.atEnd()) {
                        const quint64 key = tagCursor.varint();
                        if (key == 0) {
                            break;
                        }
                        const quint64 value = tagCursor.varint();
                        m_tags.push_back({string(key), string(value)});
                    }
                    if (!idCursor.ok || !latCursor.ok || !lonCursor.ok || !tagCursor.ok) {
                        return false;
                    }
                    handler->node(id, coordinate(latOffset, lat), coordinate(lonOffset, lon), m_tags);
                }
            } else if (field == 3 && (want & Ways)) {
                qint64 id = 0;
                QByteArrayView keys;
                QByteArrayView values;
                QByteArrayView refs;
                Pb wy{message};
                quint32 f = 0;
                quint32 w = 0;
                while (wy.key(f, w)) {
                    if (f == 1 && w == 0) {
                        id = qint64(wy.varint());
                    } else if (f == 2 && w == 2) {
                        keys = wy.bytes();
                    } else if (f == 3 && w == 2) {
                        values = wy.bytes();
                    } else if (f == 8 && w == 2) {
                        refs = wy.bytes();
                    } else {
                        wy.skip(w);
                    }
                }
                if (!wy.ok) {
                    return false;
                }
                readTags(keys, values);
                m_refs.clear();
                Pb r{refs};
                qint64 ref = 0;
                while (!r.atEnd()) {
                    ref += r.svarint();
                    m_refs.push_back(ref);
                }
                if (!r.ok) {
                    return false;
                }
                handler->way(id, m_refs, m_tags);
            }
        }
        if (!group.ok) {
            return false;
        }
    }
    return true;
}
}
