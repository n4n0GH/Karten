<!--
SPDX-FileCopyrightText: 2026 Karten contributors
SPDX-License-Identifier: CC0-1.0
-->

# The map

Files: `src/tileserver.*`, `src/tilestore.*`, the map-area part of
`src/offlinemanager.*`, and the map view in `src/qml/MapPage.qml`.

## The idea

The map is drawn by [MapLibre Native](https://maplibre.org) through its Qt
Location plugin (`Plugin { name: "maplibre" }`). MapLibre is given one
address: a style served by Karten itself on `127.0.0.1`. In that style every
address — tiles, fonts, icons — points back at the same local server.

So the map view never talks to the internet. `TileServer` answers each request
from the SQLite store when it can and from the online map source otherwise,
storing what it fetched. That one indirection gives:

- a map that keeps working offline, for everything that was looked at or
  downloaded;
- one place where the rules of a map source are enforced;
- storage that Karten controls (MapLibre's own cache is kept in memory only,
  through the plugin parameter `maplibre.cache.memory`).

## Map sources

The configured source (`TileServer::source`, config key `Map/Source`) is one of:

- the address of a **MapLibre style** (JSON) — the default is OpenFreeMap's
  `liberty`;
- the address of **image tiles** with `{z}`, `{x}`, `{y}`, which is wrapped
  in a minimal one-layer style (`buildRasterStyle`);
- `auto`: OpenFreeMap's `liberty` when the application's colours are light
  and `dark` when they are dark. `TileServer` watches for
  `ApplicationPaletteChange` and switches.

`m_choice` holds what was chosen, `m_source` the address in use. Only https
addresses are accepted, plus http on localhost for development.

## Loading a style

`fetchStyle(address)` downloads the style and, for every source that refers to
a TileJSON document (`"url"`), that document too. `buildStyle` then produces a
`Style`:

- `json`: the style as handed to MapLibre, with sources rewritten to
  `tiles: [http://127.0.0.1:<port>/<token>/tiles/<id>/{z}/{x}/{y}]`, `glyphs`
  and `sprite` pointing at the local server, and zoom range, bounds and
  attribution copied in from the TileJSON;
- `tileSources`: for each source its store id, the real address template, zoom
  range, whether it is vector data and whether rows count from the south (TMS);
- `glyphs`, `sprites`, `fontStacks`: what to fetch for offline use;
- `attribution`: the credits, shown by the label in the corner of the map.

A pitfall worth knowing: optional fields are read with `QJsonObject::value()`,
never with `operator[]` on a non-const object. The latter inserts the key as
`null`, and MapLibre rejects a source with `"minzoom": null`.

### Source ids

Tiles are stored per *source id* (`TileStore::sourceId(name)`), not per
address, because the tile address of a source changes whenever the provider
publishes new data. The name is the TileJSON address when there is one, else
`<style address>#<source name>`. Two styles that draw from the same TileJSON
therefore share their tiles. That is why a map area downloaded with the light
map also works with the dark one.

## The local server

A `QTcpServer` on a random port of the loopback interface. Paths start with a
random token created at start-up, so that other local programs cannot use the
server as a proxy.

| Request | Answered by |
| --- | --- |
| `/<token>/style/<n>.json` | `loadStyle`; `n` counts source changes so that MapLibre sees a new address |
| `/<token>/tiles/<source>/<z>/<x>/<y>` | `fetchTile` |
| `/<token>/fonts/<font stack>/<range>.pbf` | `fetchGlyphs` |
| `/<token>/sprites/<index>/sprite[@2x].{json,png}` | `fetchSprite` |

The server only fetches addresses that derive from the loaded style; font
ranges and sprite suffixes are validated against patterns.

Status codes matter to MapLibre:

- `200` with the data. Tiles are stored the way the source sent them; data
  that starts with the gzip signature is served with `Content-Encoding: gzip`.
- `404` when the source says there is nothing there (open sea).
- `503` when the data is neither stored nor reachable, which makes MapLibre
  ask again later instead of treating the spot as empty.

## Freshness

`expiryOf()` follows `Cache-Control: max-age` (at least an hour) or `Expires`,
and falls back to a week.

- **Tiles, fonts, icons**: a stored copy is served at once even when it is out
  of date, and refreshed in the background (`StaleIsFine`). Waiting for the
  network first would hold the map up when there is none.
- **Style and TileJSON**: the source is asked first when the copy is out of
  date, with a five-second limit, because they say where the current tiles
  are (`PreferCurrent`). The stored copy is the fallback.

Unchanged data is revalidated with `If-None-Match`.

## The store

One SQLite file in WAL mode.

| Table | Content |
| --- | --- |
| `sources(id, url)` | Names of tile sources, see above |
| `tiles(source, z, x, y, data, etag, expires, accessed)` | The tiles |
| `resources(url, data, etag, expires)` | Styles, TileJSON, fonts, icons, by address. Small; never pruned |
| `packs(id, source, name, south, west, north, east, max_zoom, tiles, bytes, created)` | Offline map areas |
| `pack_tiles(source, z, x, y, pack)` | Which tiles an area holds on to |

Tiles that no area refers to are a cache of what was looked at.
`pruneCache(10000)` at start-up deletes the least recently used beyond that
number. `accessed` is updated at most once a day per tile to keep reads cheap.

## Offline map areas

`OfflineManager::downloadTiles` creates a pack and a queue of tiles for the
visible area: for every source in `TileServer::packSources()` every zoom level
of the source up to the requested maximum, coarse levels first. Vector sources
have one level of detail (the source's maximum, usually 14); the dialog only
offers a choice of detail for image tiles.

`pumpPack` keeps two requests in flight through `TileServer::fetchTile`, which
means tiles that are already stored cost nothing. `fetchStyleResources` stores
the icons and the fonts for Latin, Greek, Cyrillic and general punctuation.

With the `auto` source, `packSources()` and `fetchStyleResources` also cover
the counterpart style (`loadCounterpart`), so the area works in light and dark.

### What a source permits

`policyFor(host)` in `tileserver.cpp`:

| Host | Tiles per download | Bytes per download |
| --- | --- | --- |
| `tile.openstreetmap.org`, `tile.osm.org` | 0 (not permitted) | — |
| `openfreemap.org` | 3,000 | 50 MB |
| anything else | 30,000 | 1 GB |

The strictest host among a style's sources decides. The download dialog
refuses up front from `OfflineManager::tileCount` and `estimatedSize`; a
download that still exceeds the byte limit is aborted and its pack removed.

## MapLibre specifics

- **Zoom levels** differ by one: Qt counts in 256-pixel tiles, MapLibre in
  512-pixel tiles, so MapLibre's zoom is Qt's minus one. `Geocoder::placeAt`
  relies on this.
- **Map items**: `MapPolyline` items are turned into MapLibre style layers by
  the plugin, in the order they were added, below the labels. `MapQuickItem`
  markers are ordinary Qt Quick items drawn on top.
- **Loading needs frames**: MapLibre advances while the window is being drawn.
  A hidden window loads nothing.
- **Changing the source** recreates the map view (`mapLoader` in `MapPage`),
  keeping the camera.
- **Zooming** is Karten's own: an item on top of the map takes the wheel and
  animates the zoom level around the pointer (`zoomAround` in `MapPage`).
