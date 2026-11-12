// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "vectortile.h"

#include <QtEndian>

#include <cstring>
#include <zlib.h>

namespace
{
/// Cursor over a protobuf-encoded message.
struct Pb {
    const uchar *p;
    const uchar *end;
    bool ok = true;

    explicit Pb(QByteArrayView view)
        : p(reinterpret_cast<const uchar *>(view.data()))
        , end(p + view.size())
    {
    }

    bool atEnd() const
    {
        return !ok || p >= end;
    }

    quint64 varint()
    {
        quint64 result = 0;
        for (int shift = 0; p < end && shift <= 63; shift += 7) {
            const uchar byte = *p++;
            result |= quint64(byte & 0x7f) << shift;
            if (!(byte & 0x80)) {
                return result;
            }
        }
        ok = false;
        return 0;
    }

    QByteArrayView bytes(quint64 size)
    {
        if (size > quint64(end - p)) {
            ok = false;
            return {};
        }
        const QByteArrayView view(reinterpret_cast<const char *>(p), qsizetype(size));
        p += size;
        return view;
    }

    /// Reads the next field's number and wire type.
    int field(int &wireType)
    {
        const quint64 key = varint();
        wireType = int(key & 7);
        return int(key >> 3);
    }

    /// The content of a length-delimited field.
    QByteArrayView message()
    {
        return bytes(varint());
    }

    void skip(int wireType)
    {
        switch (wireType) {
        case 0:
            varint();
            break;
        case 1:
            bytes(8);
            break;
        case 2:
            bytes(varint());
            break;
        case 5:
            bytes(4);
            break;
        default:
            ok = false;
        }
    }
};

qint64 zigzag(quint64 value)
{
    return qint64(value >> 1) ^ -qint64(value & 1);
}

QVariant readValue(QByteArrayView data)
{
    Pb pb(data);
    QVariant value;
    while (!pb.atEnd()) {
        int wireType = 0;
        switch (pb.field(wireType)) {
        case 1:
            value = QString::fromUtf8(pb.message());
            break;
        case 2: {
            const QByteArrayView raw = pb.bytes(4);
            if (raw.size() == 4) {
                float number;
                const quint32 bits = qFromLittleEndian<quint32>(raw.data());
                std::memcpy(&number, &bits, 4);
                value = double(number);
            }
            break;
        }
        case 3: {
            const QByteArrayView raw = pb.bytes(8);
            if (raw.size() == 8) {
                double number;
                const quint64 bits = qFromLittleEndian<quint64>(raw.data());
                std::memcpy(&number, &bits, 8);
                value = number;
            }
            break;
        }
        case 4:
            value = qint64(pb.varint());
            break;
        case 5:
            value = pb.varint();
            break;
        case 6:
            value = zigzag(pb.varint());
            break;
        case 7:
            value = pb.varint() != 0;
            break;
        default:
            pb.skip(wireType);
        }
    }
    return value;
}

void readLayer(QByteArrayView data, const QStringList &wanted, QList<VectorTile::PointFeature> &result)
{
    QString name;
    QList<QByteArrayView> features;
    QStringList keys;
    QList<QByteArrayView> values;
    double extent = 4096;

    Pb pb(data);
    while (!pb.atEnd()) {
        int wireType = 0;
        switch (pb.field(wireType)) {
        case 1:
            name = QString::fromUtf8(pb.message());
            break;
        case 2:
            features.append(pb.message());
            break;
        case 3:
            keys.append(QString::fromUtf8(pb.message()));
            break;
        case 4:
            values.append(pb.message());
            break;
        case 5:
            extent = double(pb.varint());
            break;
        default:
            pb.skip(wireType);
        }
    }
    if (!pb.ok || !wanted.contains(name) || extent <= 0) {
        return;
    }

    constexpr int Point = 1;
    constexpr int MoveTo = 1;
    for (const QByteArrayView &featureData : std::as_const(features)) {
        QByteArrayView tags;
        QByteArrayView geometry;
        int type = 0;
        Pb feature(featureData);
        while (!feature.atEnd()) {
            int wireType = 0;
            switch (feature.field(wireType)) {
            case 2:
                tags = feature.message();
                break;
            case 3:
                type = int(feature.varint());
                break;
            case 4:
                geometry = feature.message();
                break;
            default:
                feature.skip(wireType);
            }
        }
        if (!feature.ok || type != Point) {
            continue;
        }

        // A point is "move to" with one position.
        Pb commands(geometry);
        const quint64 command = commands.varint();
        const qint64 x = zigzag(commands.varint());
        const qint64 y = zigzag(commands.varint());
        if (!commands.ok || (command & 7) != MoveTo || (command >> 3) < 1) {
            continue;
        }

        VectorTile::PointFeature point;
        point.layer = name;
        point.x = double(x) / extent;
        point.y = double(y) / extent;
        Pb pairs(tags);
        while (!pairs.atEnd()) {
            const quint64 key = pairs.varint();
            const quint64 value = pairs.varint();
            if (pairs.ok && key < quint64(keys.size()) && value < quint64(values.size())) {
                point.properties.insert(keys[qsizetype(key)], readValue(values[qsizetype(value)]));
            }
        }
        result.append(point);
    }
}
}

QList<VectorTile::PointFeature> VectorTile::pointFeatures(const QByteArray &tile, const QStringList &layers)
{
    QList<PointFeature> result;
    Pb pb(tile);
    while (!pb.atEnd()) {
        int wireType = 0;
        if (pb.field(wireType) == 3 && wireType == 2) {
            readLayer(pb.message(), layers, result);
        } else {
            pb.skip(wireType);
        }
    }
    return result;
}

QByteArray VectorTile::gunzip(const QByteArray &data)
{
    if (!data.startsWith("\x1f\x8b")) {
        return data;
    }
    z_stream stream{};
    // 16 + MAX_WBITS selects the gzip format.
    if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) {
        return {};
    }
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.data()));
    stream.avail_in = uInt(data.size());

    QByteArray result;
    char buffer[65536];
    int status = Z_OK;
    constexpr qsizetype Limit = 64 * 1024 * 1024; // far beyond any tile
    while (status == Z_OK && result.size() < Limit) {
        stream.next_out = reinterpret_cast<Bytef *>(buffer);
        stream.avail_out = sizeof(buffer);
        status = inflate(&stream, Z_NO_FLUSH);
        result.append(buffer, qsizetype(sizeof(buffer) - stream.avail_out));
    }
    inflateEnd(&stream);
    return status == Z_STREAM_END ? result : QByteArray();
}
