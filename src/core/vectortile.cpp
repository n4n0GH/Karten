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

/// Reads the features of one layer, if it is among the wanted ones. With
/// @p keys, only the properties of those names are read.
void readLayer(QByteArrayView data, const QStringList &wanted, const QStringList *keys, QList<VectorTile::Feature> &result)
{
    QString name;
    QList<QByteArrayView> features;
    QStringList keyNames;
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
            keyNames.append(QString::fromUtf8(pb.message()));
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
    const int layer = int(wanted.indexOf(name));
    if (!pb.ok || layer < 0 || extent <= 0) {
        return;
    }
    QList<bool> keyWanted(keyNames.size(), true);
    if (keys) {
        for (qsizetype i = 0; i < keyNames.size(); ++i) {
            keyWanted[i] = keys->contains(keyNames[i]);
        }
    }
    // Many features share a value; each is decoded once.
    QList<QVariant> decoded(values.size());
    QList<bool> isDecoded(values.size(), false);

    constexpr quint64 MoveTo = 1;
    constexpr quint64 LineTo = 2;
    constexpr quint64 ClosePath = 7;
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
        if (!feature.ok || type < VectorTile::Feature::Point || type > VectorTile::Feature::Polygon) {
            continue;
        }

        VectorTile::Feature entry;
        entry.type = VectorTile::Feature::Type(type);
        entry.layer = layer;
        // Commands with positions relative to the one before.
        qint64 x = 0;
        qint64 y = 0;
        Pb commands(geometry);
        while (!commands.atEnd()) {
            const quint64 command = commands.varint();
            const quint64 id = command & 7;
            quint64 count = command >> 3;
            if (id == ClosePath) {
                continue; // rings are closed by whoever draws them
            }
            // Every position takes two bytes at least.
            if ((id != MoveTo && id != LineTo) || count > quint64(commands.end - commands.p) / 2) {
                commands.ok = false;
                break;
            }
            for (; count > 0 && commands.ok; --count) {
                x += zigzag(commands.varint());
                y += zigzag(commands.varint());
                // Points are one part; lines and rings start with a move.
                if (entry.parts.isEmpty() || (id == MoveTo && type != VectorTile::Feature::Point)) {
                    entry.parts.append(QList<QPointF>());
                }
                entry.parts.last().append(QPointF(double(x) / extent, double(y) / extent));
            }
        }
        if (!commands.ok || entry.parts.isEmpty()) {
            continue;
        }

        Pb pairs(tags);
        while (!pairs.atEnd()) {
            const quint64 key = pairs.varint();
            const quint64 value = pairs.varint();
            if (!pairs.ok || key >= quint64(keyNames.size()) || value >= quint64(values.size()) || !keyWanted[qsizetype(key)]) {
                continue;
            }
            if (!isDecoded[qsizetype(value)]) {
                decoded[qsizetype(value)] = readValue(values[qsizetype(value)]);
                isDecoded[qsizetype(value)] = true;
            }
            entry.properties.insert(keyNames[qsizetype(key)], decoded[qsizetype(value)]);
        }
        result.append(std::move(entry));
    }
}

QList<VectorTile::Feature> readTile(const QByteArray &tile, const QStringList &layers, const QStringList *keys)
{
    QList<VectorTile::Feature> result;
    Pb pb(tile);
    while (!pb.atEnd()) {
        int wireType = 0;
        if (pb.field(wireType) == 3 && wireType == 2) {
            readLayer(pb.message(), layers, keys, result);
        } else {
            pb.skip(wireType);
        }
    }
    return result;
}
}

QList<VectorTile::Feature> VectorTile::features(const QByteArray &tile, const QStringList &layers, const QStringList &keys)
{
    return readTile(tile, layers, &keys);
}

QList<VectorTile::PointFeature> VectorTile::pointFeatures(const QByteArray &tile, const QStringList &layers)
{
    QList<PointFeature> result;
    const QList<Feature> all = readTile(tile, layers, nullptr);
    for (const Feature &feature : all) {
        if (feature.type != Feature::Point) {
            continue;
        }
        PointFeature point;
        point.layer = layers[feature.layer];
        point.x = feature.parts.first().first().x();
        point.y = feature.parts.first().first().y();
        point.properties = feature.properties;
        result.append(point);
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
