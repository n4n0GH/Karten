<!--
SPDX-FileCopyrightText: 2026 Karten contributors
SPDX-License-Identifier: CC0-1.0
-->

# Architecture

Karten is one process. The interface is QML; everything else is a handful of
C++ objects created in `src/main.cpp` and exposed to QML as singletons.

```
                         ┌──────────────────────── QML (src/qml) ───────────────────────┐
                         │ MapPage ─ SidePanel ─ DirectionsView ─ PlacePanel ─ Settings │
                         └───┬───────────┬──────────────┬──────────────┬───────────────┘
                             │           │              │              │
   MapLibre map view ──HTTP──▶ TileServer   Geocoder   RouteController   OfflineManager
   (Qt Location plugin)      │    │          │   │         │   │           │      │
                             ▼    ▼          ▼   ▼         ▼   ▼           ▼      ▼
                       TileStore  network  places  network graph network  TileStore network
                       (SQLite)            .sqlite        .bin            + importer
```

## The services

| Class | File | Role |
| --- | --- | --- |
| `TileStore` | `src/tilestore.*` | SQLite storage for map tiles, style resources and offline map areas. Not a QObject; owned by `main()`. |
| `TileServer` | `src/tileserver.*` | A web server on the loopback interface. The map view fetches everything from it; it answers from the store or the online map source. |
| `OfflineManager` | `src/offlinemanager.*` | List model of what is stored offline. Downloads Geofabrik extracts and map areas. |
| `ExtractImporter` | `src/extractimporter.*` | Turns a downloaded extract into `graph.bin` and `places.sqlite`. A namespace of functions, run on a worker thread. |
| `Geocoder` | `src/geocoder.*` | Search, reverse lookup, what is at a spot of the map, and details about a place. |
| `RouteController` | `src/routecontroller.*` | Stops, detours, the planned route and its preview. |
| `Appearance` | `src/appearance.*` | Light, dark or system colours, through `KColorSchemeManager`. |
| `KeyStore` | `src/keystore.*` | Secrets in KWallet. |
| `Network` | `src/network.*` | One `QNetworkAccessManager` and the User-Agent every request carries. |

`TileServer`, `OfflineManager`, `Geocoder` and `RouteController` are created
in `main()` in dependency order and handed to QML through a static
`create()` function that returns the existing instance (`QML_SINGLETON` with
C++ ownership). `Appearance` and `KeyStore` have no dependencies and are
created by the QML engine on first use.

## The core library

`src/core` builds as the static library `kartencore`. It depends on Qt Core
and zlib only, so it can be tested without a display and reused:

- `geo.h`: distance, bearing and turn angle on the sphere.
- `pbfreader.*`: streaming reader for `.osm.pbf` files.
- `graphbuilder.*`: OSM ways and nodes to a road graph and a list of places.
- `roadgraph.*`: the graph, its file format and a spatial index.
- `router.*`: A* search and turn instructions.
- `vectortile.*`: point features out of a Mapbox Vector Tile, and gzip.

## Threads

Almost everything runs on the main thread and is asynchronous through Qt's
event loop: network replies, the local server, SQLite access.

Two things run on worker threads through `QtConcurrent::run`:

- **Importing an extract** (`ExtractImporter::run`): reads the file twice and
  writes the results. Cancelled through an `atomic_bool` checked in the
  progress callback.
- **Offline routing** (`RouteController::plan`): loads the graph if needed and
  runs the router. Each request carries its own `atomic_bool`; a result whose
  flag is set is dropped.

The most recently used road graph is kept in memory in a mutex-guarded cache
(`GraphCache` in `routecontroller.cpp`), because loading takes much longer
than routing.

Rendering uses Qt's *basic* (single-threaded) render loop with OpenGL, set in
`main()`. The MapLibre plugin asks for that; see [The map](map.md).

## Where data is kept

All under the user's XDG directories, `karten` being the application name.

| What | Where |
| --- | --- |
| Map tiles, style resources, offline map areas | `~/.local/share/karten/tiles.sqlite` |
| Routing regions | `~/.local/share/karten/routing/<id>/{graph.bin,places.sqlite,meta.json}` |
| Geofabrik's list of extracts | `~/.cache/karten/geofabrik-index-v1.json` |
| Map source, service addresses, colour scheme | `~/.config/kartenrc` (KConfig) |
| Window size, map view, interface options, Google consent | `~/.config/kde.org/karten.conf` (QML `Settings`) |
| Google Places API key | KWallet, folder `Karten` |

Two settings files exist because the C++ side uses KConfig and the QML side
uses Qt's `Settings` type. Keys:

| File | Group | Key | Meaning |
| --- | --- | --- | --- |
| `kartenrc` | `Map` | `Source` | Style address, tile address with `{z}/{x}/{y}`, or `auto`. Absent means the default. (`TileUrl` is read as a fallback from older versions.) |
| `kartenrc` | `Services` | `NominatimUrl` | Base address of the search service |
| `kartenrc` | `Services` | `RoutingUrl` | Routing service; `%1` is replaced by `car`, `bike` or `foot` |
| `kartenrc` | `UiSettings` | `ColorScheme` | Written by `KColorSchemeManager` |
| `karten.conf` | `Map` | `latitude`, `longitude`, `zoomLevel` | Last view |
| `karten.conf` | `Window` | `width`, `height` | Window size |
| `karten.conf` | `Interface` | `showZoomLevel` | Zoom percentage in the zoom bar |
| `karten.conf` | `Google` | `accepted` | The consent checkbox |

## Online services

Every request goes through `Network::request()`, which sets the User-Agent
`Karten/<version> (KDE desktop maps application)`. What the interface loads by
itself (pictures) goes through `Network::qmlFactory()`, which sets the same.

| Service | Used by | When |
| --- | --- | --- |
| OpenFreeMap (or the chosen map source) | `TileServer` | Drawing the map, downloading a map area |
| Nominatim | `Geocoder` | A submitted search; naming a dropped pin; details of a place outside downloaded regions |
| routing.openstreetmap.de (OSRM) | `RouteController` | Routes that downloaded data does not cover |
| Geofabrik | `OfflineManager` | Listing and downloading extracts |
| Wikidata, Wikipedia | `Geocoder` | A clicked place that links to an article |
| Google Places API | `Geocoder` | A clicked place, only with consent and a key |

Nominatim and the routing service each get at most one request per 1.1
seconds, enforced with an `QElapsedTimer` that reserves the next slot.
