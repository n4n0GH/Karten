// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "graphbuilder.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <limits>
#include <numeric>

namespace
{
constexpr qint32 NoCoordinate = std::numeric_limits<qint32>::min();
/// Road networks smaller than this that are cut off from everything else are
/// dropped per mode, so that a start point never snaps onto a dead island.
constexpr quint32 MinComponentSize = 150;

struct ClassEntry {
    const char *tag;
    HighwayClass cls;
    quint8 speed; ///< default km/h when there is no usable maxspeed tag
    quint8 access; ///< default mode bits
};

constexpr ClassEntry Classes[] = {
    {"motorway", HighwayClass::Motorway, 110, Car},
    {"motorway_link", HighwayClass::MotorwayLink, 60, Car},
    {"trunk", HighwayClass::Trunk, 90, Car | Bike | Foot},
    {"trunk_link", HighwayClass::TrunkLink, 50, Car | Bike | Foot},
    {"primary", HighwayClass::Primary, 70, Car | Bike | Foot},
    {"primary_link", HighwayClass::PrimaryLink, 45, Car | Bike | Foot},
    {"secondary", HighwayClass::Secondary, 60, Car | Bike | Foot},
    {"secondary_link", HighwayClass::SecondaryLink, 40, Car | Bike | Foot},
    {"tertiary", HighwayClass::Tertiary, 50, Car | Bike | Foot},
    {"tertiary_link", HighwayClass::TertiaryLink, 35, Car | Bike | Foot},
    {"unclassified", HighwayClass::Unclassified, 45, Car | Bike | Foot},
    {"road", HighwayClass::Unclassified, 40, Car | Bike | Foot},
    {"residential", HighwayClass::Residential, 30, Car | Bike | Foot},
    {"living_street", HighwayClass::LivingStreet, 10, Car | Bike | Foot},
    {"service", HighwayClass::Service, 15, Car | Bike | Foot},
    {"track", HighwayClass::Track, 15, Bike | Foot},
    {"pedestrian", HighwayClass::Pedestrian, 5, Foot},
    {"footway", HighwayClass::Footway, 5, Foot},
    {"cycleway", HighwayClass::Cycleway, 15, Bike | Foot},
    {"path", HighwayClass::Path, 5, Bike | Foot},
    {"bridleway", HighwayClass::Path, 5, Foot},
    {"steps", HighwayClass::Steps, 3, Foot},
};

bool isAny(QByteArrayView value, std::initializer_list<const char *> options)
{
    for (const char *option : options) {
        if (value == option) {
            return true;
        }
    }
    return false;
}

/// Applies an access tag value to @p allowed. Unknown values leave it alone.
void applyAccess(QByteArrayView value, bool &allowed)
{
    if (value.isEmpty()) {
        return;
    }
    if (isAny(value, {"no", "private", "agricultural", "forestry", "military", "use_sidepath", "dismount", "discouraged"})) {
        allowed = false;
    } else if (isAny(value, {"yes", "permissive", "designated", "destination", "customers", "official", "delivery"})) {
        allowed = true;
    }
}

quint8 parseMaxSpeed(QByteArrayView value)
{
    if (value.isEmpty()) {
        return 0;
    }
    if (value == "none") {
        return 130;
    }
    if (value == "walk") {
        return 7;
    }
    if (value.endsWith(":urban")) {
        return 50;
    }
    if (value.endsWith(":rural")) {
        return 90;
    }
    if (value.endsWith(":motorway")) {
        return 130;
    }
    if (value.endsWith(":living_street")) {
        return 10;
    }
    int digits = 0;
    while (digits < value.size() && value[digits] >= '0' && value[digits] <= '9') {
        ++digits;
    }
    if (digits == 0) {
        return 0;
    }
    double speed = value.first(digits).toInt();
    if (value.sliced(digits).trimmed() == "mph") {
        speed *= 1.609344;
    }
    return quint8(std::clamp(speed, 5.0, 130.0));
}

struct UnionFind {
    std::vector<quint32> parent;
    std::vector<quint32> size;

    explicit UnionFind(size_t n)
        : parent(n)
        , size(n, 1)
    {
        std::iota(parent.begin(), parent.end(), 0);
    }
    quint32 find(quint32 x)
    {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    }
    void unite(quint32 a, quint32 b)
    {
        a = find(a);
        b = find(b);
        if (a == b) {
            return;
        }
        if (size[a] < size[b]) {
            std::swap(a, b);
        }
        parent[b] = a;
        size[a] += size[b];
    }
};
}

quint32 GraphBuilder::nameIndex(QByteArrayView name)
{
    if (name.isEmpty()) {
        return 0;
    }
    const QByteArray key = name.toByteArray();
    const auto it = m_nameIndex.constFind(key);
    if (it != m_nameIndex.constEnd()) {
        return it.value();
    }
    const quint32 index = quint32(m_nameOffsets.size() - 1);
    m_nameData.append(key);
    m_nameOffsets.push_back(quint32(m_nameData.size()));
    m_nameIndex.insert(key, index);
    return index;
}

void GraphBuilder::way(qint64, const std::vector<qint64> &refs, const Osm::Tags &tags)
{
    const QByteArrayView highway = Osm::tagValue(tags, "highway");
    if (highway.isEmpty() || refs.size() < 2) {
        // Not a road, but possibly the outline of a point of interest.
        PlaceWay outline;
        if (refs.size() >= 2 && describePlace(tags, &outline.place) && outline.place.kind == Place::Poi) {
            outline.refs[0] = refs.front();
            outline.refs[1] = refs[refs.size() / 2];
            const quint32 index = quint32(m_placeWays.size());
            m_placeRefs.push_back({outline.refs[0], index, 0});
            m_placeRefs.push_back({outline.refs[1], index, 1});
            m_placeWays.push_back(std::move(outline));
        }
        return;
    }
    const ClassEntry *entry = nullptr;
    for (const ClassEntry &candidate : Classes) {
        if (highway == candidate.tag) {
            entry = &candidate;
            break;
        }
    }
    if (!entry) {
        return; // construction, proposed, platforms, …
    }

    bool car = entry->access & Car;
    bool bike = entry->access & Bike;
    bool foot = entry->access & Foot;

    if (Osm::tagValue(tags, "motorroad") == "yes") {
        bike = false;
        foot = false;
    }
    // More specific keys override more general ones, so apply general first.
    const QByteArrayView access = Osm::tagValue(tags, "access");
    applyAccess(access, car);
    applyAccess(access, bike);
    applyAccess(access, foot);
    const QByteArrayView vehicle = Osm::tagValue(tags, "vehicle");
    applyAccess(vehicle, car);
    applyAccess(vehicle, bike);
    applyAccess(Osm::tagValue(tags, "motor_vehicle"), car);
    applyAccess(Osm::tagValue(tags, "motorcar"), car);
    applyAccess(Osm::tagValue(tags, "bicycle"), bike);
    applyAccess(Osm::tagValue(tags, "foot"), foot);
    if (entry->cls == HighwayClass::Steps) {
        bike = false;
        car = false;
    }
    if (!car && !bike && !foot) {
        return;
    }

    const QByteArrayView junction = Osm::tagValue(tags, "junction");
    const bool roundabout = junction == "roundabout" || junction == "circular";
    const QByteArrayView oneway = Osm::tagValue(tags, "oneway");
    bool forwardOnly = isAny(oneway, {"yes", "true", "1"});
    const bool backwardOnly = isAny(oneway, {"-1", "reverse"});
    if (oneway.isEmpty() && (roundabout || entry->cls == HighwayClass::Motorway || entry->cls == HighwayClass::MotorwayLink)) {
        forwardOnly = true;
    }
    const QByteArrayView cycleway = Osm::tagValue(tags, "cycleway");
    const bool bikeContraflow = Osm::tagValue(tags, "oneway:bicycle") == "no" || cycleway.startsWith("opposite")
        || Osm::tagValue(tags, "cycleway:left").startsWith("opposite") || Osm::tagValue(tags, "cycleway:right").startsWith("opposite")
        || Osm::tagValue(tags, "cycleway:both").startsWith("opposite");

    quint8 forward = 0;
    quint8 backward = 0;
    if (car) {
        forward |= backwardOnly ? 0 : Car;
        backward |= forwardOnly ? 0 : Car;
    }
    if (bike) {
        forward |= (backwardOnly && !bikeContraflow) ? 0 : Bike;
        backward |= (forwardOnly && !bikeContraflow) ? 0 : Bike;
    }
    if (foot) {
        forward |= Foot;
        backward |= Foot;
    }

    WayInfo info;
    QByteArrayView name = Osm::tagValue(tags, "name");
    if (name.isEmpty()) {
        name = Osm::tagValue(tags, "ref");
    }
    info.name = nameIndex(name);
    info.cls = entry->cls;
    const quint8 maxSpeed = parseMaxSpeed(Osm::tagValue(tags, "maxspeed"));
    info.speed = maxSpeed ? maxSpeed : entry->speed;
    if (roundabout) {
        info.flags |= WayInfo::Roundabout;
    }
    if (entry->cls == HighwayClass::Cycleway || Osm::tagValue(tags, "bicycle") == "designated" || isAny(cycleway, {"lane", "track", "shared_lane"})
        || !Osm::tagValue(tags, "cycleway:right").isEmpty() || !Osm::tagValue(tags, "cycleway:both").isEmpty()) {
        info.flags |= WayInfo::CycleFriendly;
    }
    const QByteArrayView surface = Osm::tagValue(tags, "surface");
    const QByteArrayView trackType = Osm::tagValue(tags, "tracktype");
    if (isAny(surface, {"unpaved", "gravel", "fine_gravel", "dirt", "ground", "earth", "grass", "sand", "mud", "pebblestone", "cobblestone", "sett"})
        || isAny(trackType, {"grade3", "grade4", "grade5"}) || (surface.isEmpty() && trackType.isEmpty() && entry->cls == HighwayClass::Track)) {
        info.flags |= WayInfo::Unpaved;
    }

    m_ways.push_back({m_refs.size(), quint32(refs.size()), forward, backward});
    m_wayInfo.push_back(info);
    m_refs.insert(m_refs.end(), refs.begin(), refs.end());
}

void GraphBuilder::prepareNodes()
{
    m_nodeIds = m_refs;
    std::sort(m_nodeIds.begin(), m_nodeIds.end());
    m_nodeIds.erase(std::unique(m_nodeIds.begin(), m_nodeIds.end()), m_nodeIds.end());
    m_nodeIds.shrink_to_fit();
    m_lat.assign(m_nodeIds.size(), NoCoordinate);
    m_lon.assign(m_nodeIds.size(), NoCoordinate);
    std::sort(m_placeRefs.begin(), m_placeRefs.end(), [](const PlaceRef &a, const PlaceRef &b) {
        return a.node < b.node;
    });
}

void GraphBuilder::node(qint64 id, double lat, double lon, const Osm::Tags &tags)
{
    const auto it = std::lower_bound(m_nodeIds.begin(), m_nodeIds.end(), id);
    if (it != m_nodeIds.end() && *it == id) {
        const size_t index = size_t(it - m_nodeIds.begin());
        m_lat[index] = qint32(std::lround(lat * 1e7));
        m_lon[index] = qint32(std::lround(lon * 1e7));
    }

    // Corners of points of interest that are drawn as outlines.
    for (auto ref = std::lower_bound(m_placeRefs.begin(),
                                     m_placeRefs.end(),
                                     id,
                                     [](const PlaceRef &entry, qint64 node) {
                                         return entry.node < node;
                                     });
         ref != m_placeRefs.end() && ref->node == id;
         ++ref) {
        PlaceWay &way = m_placeWays[ref->way];
        way.lat[ref->corner] = lat;
        way.lon[ref->corner] = lon;
        way.found |= quint8(1 << ref->corner);
    }

    Place place;
    if (tags.empty() || !describePlace(tags, &place)) {
        return;
    }
    place.lat = lat;
    place.lon = lon;
    m_places.push_back(std::move(place));
}

bool GraphBuilder::describePlace(const Osm::Tags &tags, Place *place)
{
    const QByteArrayView name = Osm::tagValue(tags, "name");
    if (name.isEmpty()) {
        return false;
    }
    const QByteArrayView placeTag = Osm::tagValue(tags, "place");
    if (!placeTag.isEmpty()) {
        if (placeTag == "city") {
            place->kind = Place::City;
        } else if (placeTag == "town") {
            place->kind = Place::Town;
        } else if (placeTag == "village") {
            place->kind = Place::Village;
        } else if (isAny(placeTag, {"suburb", "borough", "quarter", "neighbourhood"})) {
            place->kind = Place::Suburb;
        } else if (isAny(placeTag, {"hamlet", "isolated_dwelling", "locality", "island", "islet", "square"})) {
            place->kind = Place::Locality;
        } else {
            return false;
        }
    } else {
        QByteArrayView type;
        for (const char *key : {"amenity",
                                "shop",
                                "tourism",
                                "leisure",
                                "historic",
                                "railway",
                                "aeroway",
                                "office",
                                "craft",
                                "healthcare",
                                "public_transport",
                                "sport",
                                "man_made"}) {
            type = Osm::tagValue(tags, key);
            if (!type.isEmpty()) {
                break;
            }
        }
        if (type.isEmpty()) {
            // Only some values of these keys are places.
            const QByteArrayView highway = Osm::tagValue(tags, "highway");
            const QByteArrayView natural = Osm::tagValue(tags, "natural");
            if (highway == "bus_stop") {
                type = highway;
            } else if (isAny(natural, {"peak", "volcano", "saddle", "spring", "cave_entrance", "beach", "waterfall"})) {
                type = natural;
            }
        }
        if (type.isEmpty() || type == "yes" || type == "no") {
            return false;
        }
        place->kind = Place::Poi;
        place->type = QString::fromUtf8(type);

        // What a person would want to know before going there.
        static const char *const Details[] = {
            "addr:street",
            "addr:housenumber",
            "addr:postcode",
            "addr:city",
            "opening_hours",
            "phone",
            "contact:phone",
            "website",
            "contact:website",
            "email",
            "contact:email",
            "cuisine",
            "wheelchair",
            "operator",
            "brand",
            "internet_access",
            "outdoor_seating",
            "takeaway",
            "diet:vegetarian",
            "diet:vegan",
            "fee",
            "stars",
            "ele",
            "wikidata",
            "wikipedia",
        };
        QJsonObject details;
        for (const char *key : Details) {
            const QByteArrayView value = Osm::tagValue(tags, key);
            if (!value.isEmpty()) {
                details.insert(QLatin1StringView(key), QString::fromUtf8(value));
            }
        }
        if (!details.isEmpty()) {
            place->tags = QJsonDocument(details).toJson(QJsonDocument::Compact);
        }
    }
    place->name = QString::fromUtf8(name);
    return true;
}

void GraphBuilder::finish(RoadGraph *graph, std::vector<Place> *places)
{
    struct Segment {
        quint32 a;
        quint32 b;
        quint32 way;
        quint8 forward;
        quint8 backward;
    };
    std::vector<Segment> segments;
    segments.reserve(m_refs.size());

    const auto indexOf = [this](qint64 id) -> qint64 {
        const auto it = std::lower_bound(m_nodeIds.begin(), m_nodeIds.end(), id);
        const size_t index = size_t(it - m_nodeIds.begin());
        // Nodes outside the extract are referenced by ways but have no coordinates.
        return m_lat[index] == NoCoordinate ? -1 : qint64(index);
    };

    for (size_t w = 0; w < m_ways.size(); ++w) {
        const RawWay &way = m_ways[w];
        qint64 previous = indexOf(m_refs[way.firstRef]);
        for (quint32 i = 1; i < way.refCount; ++i) {
            const qint64 current = indexOf(m_refs[way.firstRef + i]);
            if (previous >= 0 && current >= 0 && previous != current) {
                segments.push_back({quint32(previous), quint32(current), quint32(w), way.forward, way.backward});
            }
            previous = current;
        }
    }
    m_refs = {};

    // Remove small disconnected networks, separately for each mode.
    for (const quint8 mode : {Car, Bike, Foot}) {
        UnionFind components(m_nodeIds.size());
        for (const Segment &s : segments) {
            if ((s.forward | s.backward) & mode) {
                components.unite(s.a, s.b);
            }
        }
        for (Segment &s : segments) {
            if (((s.forward | s.backward) & mode) && components.size[components.find(s.a)] < MinComponentSize) {
                s.forward &= ~mode;
                s.backward &= ~mode;
            }
        }
    }
    std::erase_if(segments, [](const Segment &s) {
        return (s.forward | s.backward) == 0;
    });

    // Renumber the nodes that are still in use.
    constexpr quint32 Unused = std::numeric_limits<quint32>::max();
    std::vector<quint32> newIndex(m_nodeIds.size(), Unused);
    for (const Segment &s : segments) {
        newIndex[s.a] = 0;
        newIndex[s.b] = 0;
    }
    quint32 count = 0;
    for (quint32 &index : newIndex) {
        if (index != Unused) {
            index = count++;
        }
    }

    RoadGraph &g = *graph;
    g = RoadGraph();
    g.lat.resize(count);
    g.lon.resize(count);
    for (size_t i = 0; i < newIndex.size(); ++i) {
        if (newIndex[i] != Unused) {
            g.lat[newIndex[i]] = m_lat[i];
            g.lon[newIndex[i]] = m_lon[i];
        }
    }

    g.first.assign(size_t(count) + 1, 0);
    for (const Segment &s : segments) {
        ++g.first[newIndex[s.a] + 1];
        ++g.first[newIndex[s.b] + 1];
    }
    for (size_t i = 1; i < g.first.size(); ++i) {
        g.first[i] += g.first[i - 1];
    }
    g.edges.resize(segments.size() * 2);
    std::vector<quint32> cursor(g.first.begin(), g.first.end() - 1);
    for (const Segment &s : segments) {
        const quint32 a = newIndex[s.a];
        const quint32 b = newIndex[s.b];
        const float length = float(Geo::distance(g.point(a), g.point(b)));
        g.edges[cursor[a]++] = {b, s.way << 6 | quint32(s.backward) << 3 | s.forward, length};
        g.edges[cursor[b]++] = {a, s.way << 6 | quint32(s.forward) << 3 | s.backward, length};
    }

    g.ways = std::move(m_wayInfo);
    g.nameOffsets = std::move(m_nameOffsets);
    g.nameData = std::move(m_nameData);
    g.buildIndex();

    // One search entry per street name and neighbourhood, at the middle of a way.
    QSet<QByteArray> seenStreets;
    std::vector<quint32> wayMiddle(m_ways.size(), Unused);
    std::vector<quint32> waySegments(m_ways.size(), 0);
    for (const Segment &s : segments) {
        ++waySegments[s.way];
    }
    std::vector<quint32> wayProgress(m_ways.size(), 0);
    for (const Segment &s : segments) {
        if (wayProgress[s.way]++ == waySegments[s.way] / 2) {
            wayMiddle[s.way] = newIndex[s.a];
        }
    }
    for (size_t w = 0; w < m_ways.size(); ++w) {
        if (wayMiddle[w] == Unused || g.ways[w].name == 0) {
            continue;
        }
        const GeoPoint p = g.point(wayMiddle[w]);
        const QByteArray key = QByteArray::number(g.ways[w].name) + ':' + QByteArray::number(int(p.lat / 0.02)) + ':' + QByteArray::number(int(p.lon / 0.02));
        if (seenStreets.contains(key)) {
            continue;
        }
        seenStreets.insert(key);
        Place place;
        place.name = g.wayName(quint32(w));
        place.kind = Place::Street;
        place.lat = p.lat;
        place.lon = p.lon;
        m_places.push_back(std::move(place));
    }

    for (PlaceWay &outline : m_placeWays) {
        if (outline.found == 3) {
            outline.place.lat = (outline.lat[0] + outline.lat[1]) / 2;
            outline.place.lon = (outline.lon[0] + outline.lon[1]) / 2;
            m_places.push_back(std::move(outline.place));
        }
    }

    *places = std::move(m_places);
    *this = GraphBuilder();
}
