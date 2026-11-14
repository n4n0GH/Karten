<!--
SPDX-FileCopyrightText: 2026 Karten contributors
SPDX-License-Identifier: CC0-1.0
-->

# Printing

Files: `src/roadbook.*`, `src/roadbookdocument.*`, `src/printmap.*`, the
shapes in `src/core/vectortile.*`, `autotests/printtest.cpp`.

## What is printed

A route by road is printed as a **roadbook**, the kind a co-driver reads on a
rally: something to navigate by when there is no screen to look at.

```
┌──────────────────────────────────────────────────────────────┐
│ Start → Destination                                          │
│ 51,1 km · 55 min · by car                                    │
│ A: Start · B: A stop · C: Destination                        │
│ ┌──────────────────────────────────────────────────────────┐ │
│ │ map of the whole route, north up, stops as letters       │ │
│ └──────────────────────────────────────────────────────────┘ │
│ what the figures and the maps mean                           │
│ ── To A stop ───────────────────────────── 30,6 km · 35 min ─ │
│ 1          ┌────────┐                                        │
│ 0 m        │junction│  Head north-east on Rotteckring        │
│            │  map   │  Continue for 210 m                    │
│            └────────┘                                        │
│ 2          ┌────────┐                                        │
│ 210 m      │        │  Turn right onto Friedrichring         │
│ + 210 m    │        │  Continue for 320 m                    │
│            └────────┘                                        │
│ map credits                                    … · Page 1 of 6 │
└──────────────────────────────────────────────────────────────┘
```

Each instruction is a row:

- its number; in large figures the **distance from the start**, to compare
  with the odometer, and below it the distance from the instruction before.
  From 100 km on the total keeps its tenths, which `formatDistance` drops;
- a **map of the junction**, 40 mm square. It is turned so that the direction
  of travel points up, like a tulip diagram: the route comes in from below at
  a dot and leaves by an arrow (or ends at a target). A compass needle shows
  north, a bar the scale. It shows 400 m across by car, 280 m by bicycle and
  200 m on foot;
- the instruction in words and how far to go on after it.

A route with stops on the way gets a heading for each part. Paper of 250 mm
width or more (A4 on its side) takes two columns of rows.

Public transport connections are not printed: `Roadbook.available` is false
for them and the action is disabled.

## The steps

`Roadbook::print(window)` is what the *Print Route…* action calls.

1. **What to print** (`prepare`): `RouteController::itinerary()` hands over
   the route with numbers where the interface has text: the line, and per
   part its steps with distance, street and position on the line. From it
   come the texts of the `RoadbookDocument` and a list of maps to draw
   (`Job`: view, style, overlay): the overview first, then one per step.
2. **Tiles** (`fetch`): the tiles all those maps lie on are asked of
   `TileServer::fetchTile`, two at a time, so they come from disk when they
   are there and are stored when they are not. No more are asked for than
   the map source permits in one download (`TileServer::maxPackTiles`). A
   tile that cannot be had leaves its maps without ground; the user is told
   once (`notice`).
3. **Drawing** (`draw`): on a worker thread every map is drawn into a
   `QPicture`, a recording of painter commands that plays back sharp at any
   resolution.
4. **Preview**: a `QPrintPreviewDialog` shows the pages. From there the
   roadbook is printed, or saved as a PDF file through the print dialog. For
   every sheet it shows or prints, `RoadbookDocument::paint` only arranges
   text and plays pictures, which is quick; changing the paper in the
   preview lays everything out anew.

The dialog is a widget, where the rest of the interface is Qt Quick: Qt has
print dialogs and a preview for widgets only, and the application object is
a `QApplication` already. It is tied to the main window as a window
(`setTransientParent`).

## The map on paper

The map on the screen is drawn by MapLibre with OpenGL into a window, in the
colours of the desktop. Paper wants something else, so `PrintMap` draws its
own from the same vector tiles with a `QPainter`: white ground, roads as grey
outlines, pale tints for water, woods and the bigger roads, and the route in
black on top. It reads in black and white, and it is vector graphics.

- **View** (`PrintMap::View`): centre, metres per millimetre, the bearing
  that points up, size, and the zoom level of the tiles. `worldToPaper()` is
  the one transform from Web Mercator coordinates to millimetres on the
  paper; the scale is true at the centre. `zoomFor()` picks the tile zoom
  for a scale: a tile is meant to be about 135 mm wide.
- **Tiles**: `addTile` keeps them as they came; `tile()` takes one apart on
  first use into `Shape`s (kind, road class, bridge or tunnel, name, parts)
  and keeps the last 24 that way. Only the layers and properties that are
  drawn are decoded (`VectorTile::features` with a list of keys).
- **Style**: `Junction` has buildings, footpaths and street names;
  `Overview` has built-up areas, the names of places, and fewer and thinner
  roads the smaller the scale (`roadStyle`). Roads are drawn tunnels first
  and bridges last, and within a level all outlines before all insides,
  which is what joins them at junctions.
- **Only what is in sight**: a tile merges everything alike into few
  features of many parts, each as large as the tile. Parts are therefore
  tested one by one, lines and areas are cut to the map (`clipLine`,
  `clipRing`), and points closer together than 0.08 mm are dropped
  (`thinned`). Without that a page of six maps was several megabytes.
- **Names**: text is turned into outlines (`makeLabel`) so that it can be
  given a white halo and turned. A street's pieces are joined (`joinLines`),
  cut to the map, and the name is set straight along the longest stretch
  where it fits: on the street, or beside it where the route runs along it,
  as a name must not cut the route in two. Streets named in the instruction
  go first and in bold. Places on the overview are set by importance.
  `taken` holds the room that is spoken for.
- **Overlay**: the route (or the stretch of it that concerns a step), how it
  starts and ends, stop markers, streets to prefer, the scale bar.

What it needs is vector tiles in the **OpenMapTiles schema** (layer and class
names in `printmap.cpp`). With a map source of image tiles, or another
schema, the maps show the route on white.

## The pages

`RoadbookDocument` holds texts and pictures and has one function, `paint`,
for any `QPagedPaintDevice`: the printer of the preview, a `QPdfWriter` in
the tests. It measures the head of the first page, places the rows down the
columns (`Placement`), and draws the pages the device asks for
(`pageRanges()`).

Two things to know:

- Everything is measured in millimetres and converted at the last moment
  (`Sheet`). The painter is not simply scaled, because text keeps its
  spacing only when the font is set in the size it is drawn in.
- Playing a `QPicture` back scales it by the resolution of the device over
  its own, taking its units for pixels. `Sheet::map` undoes that, as the
  maps are drawn with a millimetre as the unit.

## Trying it

`printtest` draws a map from a tile made by hand and checks pixels, and
writes roadbooks to PDF to count pages. None of the printing code needs
OpenGL, so unlike the map view it also runs with
`QT_QPA_PLATFORM=offscreen`.

In a Flatpak, printing goes through the CUPS socket the manifest grants.

## Limitations

- Routes by road only.
- The junction map shows what the map data has; where a tile cannot be
  fetched and is not stored, there is the route alone.
- Names are set in one font and straight; a name that does not fit its
  street in the map is left out. The instruction beside the map has it.
- Printing fetches the tiles around every instruction, which for a long
  route is a few hundred. Beyond what the map source permits in one download
  (3,000 for OpenFreeMap, far more than a route of ten stops comes to) the
  later maps are left without ground.
