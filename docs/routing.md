<!--
SPDX-FileCopyrightText: 2026 Karten contributors
SPDX-License-Identifier: CC0-1.0
-->

# Routing

Files: `src/core/{pbfreader,graphbuilder,roadgraph,router}.*`,
`src/extractimporter.*`, `src/routecontroller.*`, `src/tools/karten-route.cpp`.

## From extract to graph

A routing region starts as an `.osm.pbf` extract from Geofabrik.
`ExtractImporter::run` reads it **twice**, because the file lists nodes before
ways but only the ways tell which nodes matter:

1. **Ways** (`PbfReader::Ways`): `GraphBuilder::way` keeps every way with a
   known `highway` value: its node ids, per-direction access bits and a
   `WayInfo`. Other named ways that describe a place are remembered as
   outlines of points of interest.
2. `prepareNodes()` sorts the node ids that roads use.
3. **Nodes** (`PbfReader::Nodes`): `GraphBuilder::node` records the position
   of road nodes (binary search in the sorted ids) and collects named places.
4. `finish()` builds the graph and the place list.

`PbfReader` is a minimal streaming reader: it walks the blobs of the file,
inflates them and decodes dense nodes and ways with its own protobuf cursor.
Relations are not read.

### Access and speed

`graphbuilder.cpp` has a table of `highway` values with class, default speed
and default modes. On top of that it evaluates `access`, `vehicle`,
`motor_vehicle`, `motorcar`, `bicycle`, `foot`, `motorroad`, `oneway`
(including `-1`), `junction` (roundabouts), `oneway:bicycle` and the
`cycleway` keys (contraflow and cycle lanes), `maxspeed`, `surface` and
`tracktype`. The result per way is two sets of mode bits, forward and
backward, and flags for roundabouts, cycle-friendly ways and unpaved surfaces.

Not evaluated: turn restrictions, barriers, conditional access, ferries.

### The graph

`RoadGraph` is a compressed-sparse-row graph in which **every OSM node on a
road is a vertex**. That keeps the geometry in the graph (no separate shape
storage) at the price of more vertices.

- `lat`, `lon`: positions in 1e-7 degrees.
- `first[n]..first[n+1]`: the edges leaving vertex `n`.
- `Edge`: target vertex, length in metres and `wayAccess`, which packs the way
  index with the mode bits of this direction and of the opposite one. Every
  segment is stored in both directions even when one is closed, so that the
  shape of a junction stays known for turn instructions.
- `ways`: one `WayInfo` per OSM way; `nameOffsets`/`nameData`: a string table.

`finish()` also removes, per mode, connected components of fewer than 150
vertices. Otherwise a start point could snap onto a car park that is cut off
from the rest for that mode.

A grid of 0.005° cells (`buildIndex`) answers `nearestNode(point, mode,
radius)`: the nearest vertex that has an edge usable by the mode.

### File format

`graph.bin` is a header followed by the arrays as they are in memory:

```
magic "KRTGRPH2" | byte order mark 0x01020304 | counts: nodes, edges, ways, names, name bytes
lat[] | lon[] | first[] | edges[] | ways[] | nameOffsets[] | nameData
```

`load()` checks the magic, the byte order, that the counts match the file size
and that edge targets and way indices are in range. The format is tied to the
machine's byte order; a file from elsewhere is rejected and the region has to
be downloaded again. Change the magic when the layout changes.

## The router

`Router::route(from, to, mode)` snaps both ends to the nearest usable vertex
within 1 km and runs **A\*** over the graph.

- **Cost** (`edgeCost`): travel time, multiplied by a factor that makes
  unpleasant roads look longer. Cars drive at 90 % of the speed limit and
  avoid service roads; bicycles ride at 18 km/h, are slowed on footways, paths
  and unpaved surfaces, avoid big roads and prefer cycle-friendly ways;
  pedestrians walk at 5 km/h and avoid big roads. The route's duration is the
  real time, without the factors.
- **Heuristic**: straight-line distance times the lowest possible cost per
  metre for the mode (`minWeightPerMetre`), which keeps the search optimal.
- **Cancelling**: an `atomic_bool` is checked every 4,096 settled vertices.

`buildRoute` turns the edge path into a line and **maneuvers**. A new one
starts at a junction where the direction changes by 20° or more, where the
road changes its name with a bend of more than 35°, or where it changes to
another named road at a junction. Turn angles map to
slight (under 55°), normal (under 130°), sharp (under 170°) and U-turn.
Roundabouts become one maneuver with the number of the exit, counted from the
exits passed.

## Trying it without the application

```sh
./build/bin/karten-route extract.osm.pbf 52.5163,13.3777 52.5200,13.4050 bike
```

builds the graph in memory, plans one route and prints the maneuvers with
timings.

## RouteController

Holds what the user asked for and what came out of it.

### Stops and detours

`m_stops` is a list of at least two `Stop`s: coordinate, name and `detours`,
the places to pass on the way to the *next* stop. A stop without a coordinate
has not been chosen yet; a route is planned once all are chosen.

- **Stops** are destinations. They appear in the list, start a new leg, and
  can be reordered (`reorderStops`), reversed (`swap`), added and removed.
- **Detours** only bend the route. They are created by dragging the route line
  on the map (`addDetour`), have a pin, and do not split the leg list.
  Changing, removing or reordering stops drops the detours of the stretches
  involved; reversing keeps them.

`addDetour(onRoute, through)` has to find which stretch was dragged. After
each planning `m_waypointPositions` records where on the route line every stop
and detour lies; the stretch is the last of those at or before the point of
the line nearest to `onRoute`.

### Planning

`plan(places, cancel, done)` is the one path for the real route and for
previews:

1. Downloaded regions whose bounding box covers **all** places are candidates.
   On a worker thread each candidate's graph is loaded and the legs are routed
   one after the other and joined.
2. If there is no candidate or no route, `planOnline` asks the OSRM server
   with all places in one request and converts the answer (`parseOsrm`).

The result is a `Journey`: the line and one `Leg` per pair of consecutive
places. `setRoute` joins the legs between two stops into one (dropping the
arrival at a detour and turning the following departure into "continue") and
fills the properties the interface reads: `path`, `bounds`, `legs` with their
`steps`, and the totals.

### Previews

While a detour is dragged, `previewDetour` / `previewMovedDetour` plan the
route that would result, without touching the stops, and publish it as
`previewPath`. Only one preview runs at a time and always for the latest
position (`m_previewWanted`), since the pointer moves faster than routes can
be planned — online at least, where the rate limit allows one per second.

When the detour is dropped where the last preview was computed for,
`recalculate` uses that result instead of planning again.
