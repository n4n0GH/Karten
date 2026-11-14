// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QList>
#include <QPoint>

/// A minimal protobuf writer, enough to put a vector tile together.
namespace TileWriter
{
inline QByteArray varint(quint64 value)
{
    QByteArray out;
    do {
        const char byte = char(value & 0x7f);
        value >>= 7;
        out.append(value ? char(byte | 0x80) : byte);
    } while (value);
    return out;
}

inline QByteArray number(int field, quint64 value)
{
    return varint(quint64(field) << 3) + varint(value);
}

inline QByteArray bytes(int field, const QByteArray &content)
{
    return varint(quint64(field) << 3 | 2) + varint(quint64(content.size())) + content;
}

inline quint64 zigzag(qint64 value)
{
    return (quint64(value) << 1) ^ quint64(value >> 63);
}

inline QByteArray packed(std::initializer_list<quint64> values)
{
    QByteArray out;
    for (const quint64 value : values) {
        out += varint(value);
    }
    return out;
}

/// The commands that draw one line, or one ring when @p closed, through the
/// points. @p cursor is where the commands before it ended.
inline QByteArray path(const QList<QPoint> &points, bool closed, QPoint &cursor)
{
    constexpr quint64 MoveTo = 1;
    constexpr quint64 LineTo = 2;
    constexpr quint64 ClosePath = 7;
    QByteArray out;
    for (qsizetype i = 0; i < points.size(); ++i) {
        if (i == 0) {
            out += varint(MoveTo | 1 << 3);
        } else if (i == 1) {
            out += varint(LineTo | quint64(points.size() - 1) << 3);
        }
        out += varint(zigzag(points[i].x() - cursor.x())) + varint(zigzag(points[i].y() - cursor.y()));
        cursor = points[i];
    }
    if (closed) {
        out += varint(ClosePath | 1 << 3);
    }
    return out;
}

/// A feature of a geometry type, with indices into the keys and values of its layer.
inline QByteArray shape(int type, const QByteArray &geometry, std::initializer_list<quint64> tags)
{
    return bytes(2, packed(tags)) + number(3, quint64(type)) + bytes(4, geometry);
}

inline QByteArray text(const QByteArray &value)
{
    return bytes(1, value);
}

/// A layer; the values are encoded already, as text() does it.
inline QByteArray layer(const QByteArray &name, const QList<QByteArray> &features, const QList<QByteArray> &keys, const QList<QByteArray> &values)
{
    QByteArray out = bytes(1, name);
    for (const QByteArray &entry : features) {
        out += bytes(2, entry);
    }
    for (const QByteArray &key : keys) {
        out += bytes(3, key);
    }
    for (const QByteArray &value : values) {
        out += bytes(4, value);
    }
    out += number(5, 4096);
    return out;
}
}
