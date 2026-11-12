// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "roadgraph.h"

#include <QFile>
#include <QSaveFile>

#include <algorithm>
#include <limits>

using namespace Qt::StringLiterals;

namespace
{
// The file is a header followed by the raw arrays in host byte order. It is a
// local cache that is rebuilt from the OSM extract, not an interchange format.
constexpr char Magic[8] = {'K', 'R', 'T', 'G', 'R', 'P', 'H', '2'};

struct Header {
    char magic[8];
    quint32 byteOrderMark;
    quint32 nodes;
    quint32 edges;
    quint32 ways;
    quint32 names;
    quint32 nameBytes;
};

template<typename T>
bool writeArray(QIODevice &out, const std::vector<T> &v)
{
    const qint64 bytes = qint64(v.size() * sizeof(T));
    return out.write(reinterpret_cast<const char *>(v.data()), bytes) == bytes;
}

template<typename T>
bool readArray(QIODevice &in, std::vector<T> &v, size_t count)
{
    v.resize(count);
    const qint64 bytes = qint64(count * sizeof(T));
    return in.read(reinterpret_cast<char *>(v.data()), bytes) == bytes;
}
}

QString RoadGraph::wayName(quint32 way) const
{
    const quint32 name = ways[way].name;
    if (name + 1 >= nameOffsets.size()) {
        return {};
    }
    const quint32 begin = nameOffsets[name];
    const quint32 end = nameOffsets[name + 1];
    return QString::fromUtf8(nameData.constData() + begin, end - begin);
}

bool RoadGraph::save(const QString &path, QString *error) const
{
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = out.errorString();
        }
        return false;
    }
    Header header{};
    memcpy(header.magic, Magic, sizeof(Magic));
    header.byteOrderMark = 0x01020304;
    header.nodes = nodeCount();
    header.edges = quint32(edges.size());
    header.ways = quint32(ways.size());
    header.names = quint32(nameOffsets.size());
    header.nameBytes = quint32(nameData.size());

    bool ok = out.write(reinterpret_cast<const char *>(&header), sizeof(header)) == sizeof(header);
    ok = ok && writeArray(out, lat) && writeArray(out, lon) && writeArray(out, first) && writeArray(out, edges) && writeArray(out, ways)
        && writeArray(out, nameOffsets);
    ok = ok && out.write(nameData) == nameData.size();
    if (!ok || !out.commit()) {
        if (error) {
            *error = out.errorString();
        }
        return false;
    }
    return true;
}

bool RoadGraph::load(const QString &path, QString *error)
{
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = in.errorString();
        }
        return false;
    }
    Header header{};
    bool ok = in.read(reinterpret_cast<char *>(&header), sizeof(header)) == sizeof(header);
    ok = ok && memcmp(header.magic, Magic, sizeof(Magic)) == 0 && header.byteOrderMark == 0x01020304;
    // Guard the allocations below against a damaged header.
    const quint64 expected = sizeof(header) + quint64(header.nodes) * 8 + (quint64(header.nodes) + 1) * 4 + quint64(header.edges) * sizeof(Edge)
        + quint64(header.ways) * sizeof(WayInfo) + quint64(header.names) * 4 + header.nameBytes;
    ok = ok && quint64(in.size()) == expected;
    ok = ok && readArray(in, lat, header.nodes) && readArray(in, lon, header.nodes) && readArray(in, first, size_t(header.nodes) + 1)
        && readArray(in, edges, header.edges) && readArray(in, ways, header.ways) && readArray(in, nameOffsets, header.names);
    if (ok) {
        nameData = in.read(header.nameBytes);
        ok = nameData.size() == qsizetype(header.nameBytes);
    }
    // Validate the indices so that a damaged file cannot cause out-of-bounds access.
    ok = ok && !first.empty() && first.back() == edges.size() && std::is_sorted(first.begin(), first.end());
    if (ok) {
        for (const Edge &e : edges) {
            if (e.target >= header.nodes || e.way() >= header.ways) {
                ok = false;
                break;
            }
        }
    }
    ok = ok && std::is_sorted(nameOffsets.begin(), nameOffsets.end()) && (nameOffsets.empty() || nameOffsets.back() <= header.nameBytes);
    if (!ok) {
        if (error) {
            *error = u"The routing data file is damaged"_s;
        }
        *this = RoadGraph();
        return false;
    }
    buildIndex();
    return true;
}

void RoadGraph::buildIndex()
{
    const quint32 n = nodeCount();
    if (n == 0) {
        m_cols = m_rows = 0;
        return;
    }
    const auto [latLo, latHi] = std::minmax_element(lat.begin(), lat.end());
    const auto [lonLo, lonHi] = std::minmax_element(lon.begin(), lon.end());
    minLat = *latLo * 1e-7;
    maxLat = *latHi * 1e-7;
    minLon = *lonLo * 1e-7;
    maxLon = *lonHi * 1e-7;

    m_cell = 0.005;
    while ((maxLat - minLat) / m_cell * ((maxLon - minLon) / m_cell) > 4e6) {
        m_cell *= 2;
    }
    m_rows = int((maxLat - minLat) / m_cell) + 1;
    m_cols = int((maxLon - minLon) / m_cell) + 1;

    const auto cellOf = [this](quint32 node) {
        const int row = int((lat[node] * 1e-7 - minLat) / m_cell);
        const int col = int((lon[node] * 1e-7 - minLon) / m_cell);
        return size_t(row) * size_t(m_cols) + size_t(col);
    };
    // Counting sort of the nodes by cell.
    m_cellFirst.assign(size_t(m_rows) * size_t(m_cols) + 1, 0);
    for (quint32 i = 0; i < n; ++i) {
        ++m_cellFirst[cellOf(i) + 1];
    }
    for (size_t c = 1; c < m_cellFirst.size(); ++c) {
        m_cellFirst[c] += m_cellFirst[c - 1];
    }
    m_cellNodes.resize(n);
    std::vector<quint32> cursor(m_cellFirst.begin(), m_cellFirst.end() - 1);
    for (quint32 i = 0; i < n; ++i) {
        m_cellNodes[cursor[cellOf(i)]++] = i;
    }
}

qint64 RoadGraph::nearestNode(GeoPoint p, quint8 mode, double maxMetres) const
{
    if (m_cols == 0) {
        return -1;
    }
    const double metresPerLat = Geo::EarthRadius * Geo::DegToRad;
    const double metresPerLon = metresPerLat * std::cos(p.lat * Geo::DegToRad);
    const double cellMetres = m_cell * std::min(metresPerLat, metresPerLon);

    const int row0 = int(std::floor((p.lat - minLat) / m_cell));
    const int col0 = int(std::floor((p.lon - minLon) / m_cell));
    const int maxRing = int(maxMetres / cellMetres) + 1;

    double best = std::numeric_limits<double>::max();
    qint64 bestNode = -1;
    const auto visit = [&](int row, int col) {
        if (row < 0 || col < 0 || row >= m_rows || col >= m_cols) {
            return;
        }
        const size_t cell = size_t(row) * size_t(m_cols) + size_t(col);
        for (quint32 i = m_cellFirst[cell]; i < m_cellFirst[cell + 1]; ++i) {
            const quint32 node = m_cellNodes[i];
            const double dy = (lat[node] * 1e-7 - p.lat) * metresPerLat;
            const double dx = (lon[node] * 1e-7 - p.lon) * metresPerLon;
            const double d = dx * dx + dy * dy;
            if (d >= best) {
                continue;
            }
            for (quint32 e = first[node]; e < first[node + 1]; ++e) {
                if (edges[e].touches(mode)) {
                    best = d;
                    bestNode = node;
                    break;
                }
            }
        }
    };

    for (int ring = 0; ring <= maxRing; ++ring) {
        // Everything outside the rings searched so far is at least this far away.
        const double ringMetres = (ring - 1) * cellMetres;
        if (bestNode >= 0 && ringMetres > 0 && ringMetres * ringMetres > best) {
            break;
        }
        if (ring == 0) {
            visit(row0, col0);
            continue;
        }
        for (int c = col0 - ring; c <= col0 + ring; ++c) {
            visit(row0 - ring, c);
            visit(row0 + ring, c);
        }
        for (int r = row0 - ring + 1; r <= row0 + ring - 1; ++r) {
            visit(r, col0 - ring);
            visit(r, col0 + ring);
        }
    }
    if (bestNode < 0 || best > maxMetres * maxMetres) {
        return -1;
    }
    return bestNode;
}
