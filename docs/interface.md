<!--
SPDX-FileCopyrightText: 2026 Karten contributors
SPDX-License-Identifier: CC0-1.0
-->

# Interface

Files: `src/qml/`, `src/appearance.*`, `src/icons/`.

The interface follows the Kirigami design guide the project uses
(<https://n4n0gh.github.io/Doku/>): colours from `Kirigami.Theme` only,
spacing from `Kirigami.Units`, icon names from the freedesktop naming
specification, verbs as dialog titles, placeholders for empty views.

## Layout

The map fills the window; everything else floats on it as cards with the same
background (`CardBackground.qml`), which also keeps clicks and scrolling from
reaching the map.

```
┌──────────────────────────────────────────────────────────────┐
│ ┌ SidePanel ──────┐                                  ┌ menu ┐ │
│ │ search / directions                                └──────┘ │
│ └─────────────────┘                                           │
│ ┌ PlacePanel ─────┐                                           │
│ │ selected place  │               map                         │
│ └─────────────────┘                                           │
│                                                  MapControls  │
│                                             ┌ ZoomControl ──┐ │
│                                             └───────────────┘ │
│                                                   attribution │
└──────────────────────────────────────────────────────────────┘
```

## Files

| File | What it is |
| --- | --- |
| `Main.qml` | The window. Opens Offline Maps, Settings and About as dialog layers; shows notifications from `OfflineManager`. |
| `MapPage.qml` | The page: actions and shortcuts, the map view and its items, zooming, clicks and drags on the map, and the floating cards. The largest file. |
| `SidePanel.qml` | The card at the top left: search field and results, or `DirectionsView`. |
| `SearchResults.qml` | The result list, shared by search and directions. |
| `DirectionsView.qml` | Travel mode, stops, totals and the legs with their steps. |
| `StopList.qml` | The stops as text fields that can be dragged into another order. |
| `PlacePanel.qml` | The selected place: title, buttons, article, details, reviews. Scrolls when the window is short. |
| `ZoomControl.qml` | Zoom out, slider, zoom in, percentage. |
| `MapControls.qml` | Round buttons for a list of actions (north up, my location). |
| `DownloadDialog.qml` | What to store for the visible area. |
| `OfflinePage.qml` | What is stored, with progress and delete. |
| `SettingsPage.qml` | Colours, map source, zoom bar, Google. |
| `Preferences.qml` | Singleton with the options chosen in the settings. |
| `CardBackground.qml` | Background of the floating cards. |

## State in MapPage

- `place`, `placeDetails`, `placeArticle`, `placeReviews`: the selected place
  and what was looked up about it. Answers are matched by position.
- `directionsMode`: whether the side panel shows directions.
- Zooming: `zoomTo`, `zoomAround`, `zoomBySteps` animate `map.zoomLevel`
  through the proxy property `animatedZoom`. `pendingZoom` is where a running
  animation will end. One step is `zoomStepPercent` (5 %) of the range between
  the map's minimum and maximum zoom, which is also what the percentage in the
  zoom bar counts.
- The map is fitted to a route only when the stops change, not for detours or
  another travel mode (`fittedStops` in the `RouteController` connection).

## Pointer handling on the map

Order matters, because several things want the same events. An `Item` on top
of the map holds:

- a `WheelHandler` that replaces the map view's stepwise zoom with the
  animated one (and keeps Shift for rotating, Ctrl for tilting);
- a `MouseArea` (`routeDrag`) that is only interested in presses near the
  route line or on a detour pin. It rejects every other press, which then
  reaches the map view's own pan and pinch handling. Once it has a press it
  keeps it (`preventStealing`).

Below that, on the map view: a left tap selects the place under it
(`selectPlaceAt`), a right tap or long press opens the context menu.

Whether a press is "near the route" is asked of `RouteController.distanceToRoute`
and compared with a reach in pixels converted to metres at that spot.

## Two QML conventions that bite

- **`pragma ComponentBehavior: Bound`** is used in most files. It removes the
  context properties `index` and `model` from delegates, which
  `Kirigami.ListItemDragHandle` reads. `StopList.qml` therefore does without
  the pragma, and wraps each row in a delegate item because the handle lifts
  the row out of the list while dragging.
- **A property that should remember an old value must be assigned, not
  bound.** `shownStyle` in `MapPage` is set in `Component.onCompleted`; as a
  binding it would already have the new value when the change handler runs.

## Settings

`Preferences.qml` is a QML singleton backed by `Settings`; it calls `sync()` on
every change so that quitting right afterwards does not lose it. Options with
their own logic live in C++:

- `Appearance.scheme`: follow the system, light or dark. Implemented with
  `KColorSchemeManager`, which activates Breeze Light or Breeze Dark for this
  application and remembers the choice itself.
- `TileServer.source`: the map, see [The map](map.md).
- `KeyStore`: see [Places](places.md).

## Icons

Icons come from the icon theme by name. For pictograms no theme has, Karten
ships its own in `src/icons/`: `travel-{drive,cycle,walk}-symbolic` and
`turn-*-symbolic`. They are compiled in under
`:/icons/hicolor/scalable/actions/`, where the icon loader finds them by name
like any other, and they carry Breeze's `ColorScheme-Text` style sheet so that
they take on the colour of the colour scheme.

The left-turn icons are the right-turn ones mirrored by a transform in the
SVG; draw a new turn icon for a right turn, stem at the bottom.
