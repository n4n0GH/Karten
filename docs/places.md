<!--
SPDX-FileCopyrightText: 2026 Karten contributors
SPDX-License-Identifier: CC0-1.0
-->

# Places

Files: `src/geocoder.*`, `src/core/vectortile.*`, the place part of
`src/core/graphbuilder.*`, `src/keystore.*`, `src/qml/PlacePanel.qml`.

A *place* in the interface is a JavaScript object with `title`, `subtitle`,
`latitude` and `longitude` (and `type` when it came from the map). It is
`MapPage.place`; the panel under the search box shows it.

## Place data of a region

When an extract is imported, `GraphBuilder::describePlace` decides whether a
named OSM object is worth finding:

- `place=*` becomes a city, town, village, suburb or locality;
- otherwise the first of `amenity`, `shop`, `tourism`, `leisure`,
  `historic`, `railway`, `aeroway`, `office`, `craft`, `healthcare`,
  `public_transport`, `sport`, `man_made` gives the type of a point of
  interest; bus stops and some `natural` features count as well;
- a fixed list of tags worth showing (address, opening hours, phone, website,
  cuisine, wheelchair, operator, Wikidata and Wikipedia links, …) is kept as a
  JSON object.

Points of interest that are mapped as **outlines** (buildings) are placed
halfway between two of their corners, the first and the middle node of the
way. Places mapped as relations are not read.

Each road name also becomes a place, once per cell of about 2 km, so that
streets can be searched.

`ExtractImporter` writes them to `places.sqlite`:

```
places(name, fold, kind, type, lat, lon, tags)     index on fold
```

`fold` is the case-folded name; `kind` is `Place::Kind` (city … street, point
of interest); `type` the raw OSM value such as `cafe`; `tags` the JSON
object or NULL.

## Search

- `suggest(text)` runs on every keystroke and searches downloaded regions
  only, by prefix (a range scan on the `fold` index).
- `search(text)` runs when the query is submitted: a substring search in the
  regions, then Nominatim. Results are cached in memory. Text that looks like
  `48.137, 11.575` is taken as a position.
- `reverse(lat, lon)` asks Nominatim for a name, for dropped pins and route
  end points.

Nominatim's policy forbids search-as-you-type, which is why only `search`
goes online.

## Clicking the map

MapLibre's Qt binding cannot say what is drawn at a point, so
`Geocoder::placeAt(lat, lon, zoom)` works it out from the data: it takes the
vector tile under the click from the store (`TileServer::storedTile`, which
never goes online), unpacks it (`VectorTile::gunzip`) and reads the point
features of the layers `poi`, `mountain_peak` and `aerodrome_label`
(`VectorTile::pointFeatures`). Those are the layer names of the OpenMapTiles
schema, which OpenFreeMap uses.

The nearest named feature within reach of the click wins: 24 pixels to the
sides, 14 above and 28 below, because the icon sits on the position and the
name below it. Features are only considered when the map shows them at the
current zoom, following the rank rules of the OpenMapTiles styles; stops are
always shown.

`VectorTile::pointFeatures` is a small decoder for the Mapbox Vector Tile
format: layers, their key and value tables, and for point features the first
position. It tolerates damaged input by leaving things out.

## Details of a place

`lookUpDetails(lat, lon, name, type)` fills the panel in up to three steps,
each reported by a signal that carries the position so that late answers for
another place are ignored.

1. **Downloaded region** (`offlineTags`): a place of the same name *and* type
   within about 200 m. The type matters because a signpost may carry the name
   of what it points to.
2. **Nominatim** when the region has nothing: a reverse lookup restricted to
   points of interest. Its answer is the nearest object, not necessarily the
   one clicked; only if the name matches are its tags trusted, otherwise just
   street and town are shown.
3. **Wikipedia** (`lookUpArticle`) when the tags link to Wikidata or
   Wikipedia: Wikidata gives the article in the user's language, else English,
   else the linked one; the MediaWiki Action API gives three sentences, a
   picture and the address. Reported by `articleFinished`.

Steps 1 and 2 end in `detailsFromTags`, which turns OSM tags into the lines of
the panel: address, opening hours, phone, website, e-mail, cuisine, features
and operator. Each line is a map with `icon`, `text`, `link`, `note` and
`state`.

### Opening hours

`openingHoursDetail` evaluates OpenStreetMap's notation with KOpeningHours.
The place's own time zone and country (`KTimeZone::fromLocation`,
`KCountry::fromLocation`, both offline) are set first, so that "open now" and
public-holiday rules are right for places elsewhere. The line's `text` is the
library's description of the current state, `state` is `open` or `closed`
for the colour, and `note` lists the coming week with neighbouring days of
equal hours joined. When the notation cannot be evaluated the raw text is
shown instead.

## Google ratings and reviews

Off by default. Google's terms do not permit showing Places content next to a
map that is not Google's; the settings page says so and requires a checkbox
before a key can be entered (see the README).

- `Preferences.googleEnabled` is true when the box is ticked and a key is
  available.
- `KeyStore` keeps the key in KWallet (folder `Karten`). The wallet is opened
  only once the box is ticked. A key found in the settings file of an older
  version is moved into the wallet and removed from the file.
- `Geocoder::lookUpReviews` makes two requests to the Places API (New): a
  text search for the name near the position that asks for ids only, then the
  details of that place. A result further than 300 m away is discarded.
  `reviewsFinished` carries rating, count, the place's address on Google Maps
  and the reviews. Nothing is stored.

The "Google Maps" button in the panel is independent of all this: it opens the
web browser.
