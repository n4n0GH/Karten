<!--
SPDX-FileCopyrightText: 2026 Karten contributors
SPDX-License-Identifier: CC0-1.0
-->

# Development

## Building

What is needed is listed in the [README](../README.md). On openSUSE
Tumbleweed the development packages beyond the usual Qt 6 and KDE Frameworks
ones are `qt6-location-private-devel`, `kopeninghours-devel` and
`kf6-kwallet-devel`.

MapLibre Native Qt is not packaged there. `scripts/build-maplibre.sh [PREFIX]`
clones version 3.0.0 with its submodules and builds it into a prefix next to
the source tree. It does three things a plain build would trip over:

- adds `LocationPrivate` to the Qt components it looks for (needed since
  Qt 6.9);
- turns off warnings-as-errors and force-includes `<cstdint>`, for current
  compilers;
- sets a relative run-time library path so that the plugins find the
  libraries in the prefix.

```sh
scripts/build-maplibre.sh ../Karten-deps/prefix
cmake -B build -G Ninja -DCMAKE_PREFIX_PATH=$PWD/../Karten-deps/prefix
cmake --build build
```

The directory of MapLibre's Qt Location plugin is compiled into Karten
(`KARTEN_MAPLIBRE_PLUGIN_PATH`) and added to Qt's plugin search path at
start-up if it exists. With MapLibre installed alongside Qt that does nothing.

CMake prints `error: No such remote 'origin'` while the repository has no
remote. It comes from the setup of the clang-format git hook and is harmless.

## Flatpak

`flatpak/io.github.n4n0gh.karten.yml` builds Karten on the KDE runtime. Two
things the runtime lacks are built first: KOpeningHours, and MapLibre Native
Qt with the same adjustments as `scripts/build-maplibre.sh`
(`flatpak/maplibre-qt-location-private.patch` is the one-line change).

```sh
flatpak-builder --user --install --force-clean build-flatpak flatpak/io.github.n4n0gh.karten.yml
flatpak run io.github.n4n0gh.karten
```

The sandbox is given the network, the display, the graphics device, KWallet
(for the optional Google key) and GeoClue (for "Show My Location"), and no
access to files. Data ends up under `~/.var/app/io.github.n4n0gh.karten/`.

How a version gets from there to users is described in
[Releasing](releasing.md).

## Tests

```sh
ctest --test-dir build --output-on-failure
```

- `autotests/routertest.cpp`: builds a 15×15 grid of streets through
  `GraphBuilder` and checks the graph, routes, turn directions, one-way and
  access handling, saving and loading, places and their details, and that
  garbage input is rejected.
- `autotests/vectortiletest.cpp`: decodes a tile put together by hand, feeds
  the decoder garbage, and unpacks gzip.
- `appstreamtest`: validates the metainfo file.

Only `src/core` has unit tests. The services and the interface are tested by
running the application.

## Running and debugging

```sh
./build/bin/karten
QT_LOGGING_RULES="karten.tileserver.debug=true" ./build/bin/karten   # what the map asks for
QT_FORCE_STDERR_LOGGING=1 ./build/bin/karten                          # log to the terminal, not the journal
```

To try things without touching your own data, point the XDG directories
somewhere else:

```sh
P=/tmp/karten-profile
XDG_DATA_HOME=$P/data XDG_CONFIG_HOME=$P/config XDG_CACHE_HOME=$P/cache ./build/bin/karten
```

Put `[Icons]` / `Theme=breeze` into `$P/config/kdeglobals`, or icons will be
missing and Karten's own will not take the theme's colour.

To run without internet while keeping the display and the local map server:

```sh
unshare -rn sh -c "ip link set lo up; ./build/bin/karten"
```

Things to know when testing:

- The map needs OpenGL; the `offscreen` platform and the software renderer
  do not draw it.
- The map only loads while its window is being drawn. A covered or hidden
  window stalls after the style request.
- KWallet is a service of the session. A test profile still opens the real
  wallet.
- Two warnings appear whenever a dialog page opens ("Created graphical object
  was not placed in the graphics scene" and a `flickable` TypeError in
  Kirigami's `ScrollablePage`). They also appear for Kirigami's own About
  page and have no visible effect.

## Conventions

- **C++**: C++20 with KDE's compiler settings, which are strict: string
  literals need `u"…"_s` or `"…"_L1`, signals are emitted with `Q_EMIT`,
  `Q_SIGNALS`/`Q_SLOTS` instead of the keywords. Formatting is KDE's
  clang-format style (`clang-format -i` on the files you touch).
- **Comments** say why, or what is not obvious; class comments in headers are
  the reference documentation.
- **Strings** for the user go through `i18nc` with a context. `Messages.sh`
  extracts them; there are no translations yet. Values taken from
  OpenStreetMap (`fast_food`) are shown as they are, made readable.
- **Licensing** follows REUSE: every file starts with SPDX lines. Code is
  GPL-3.0-or-later, build files BSD-2-Clause, documentation and icons CC0-1.0;
  the texts are in `LICENSES/`.
- **Network**: every request is built with `Network::request()`. A service
  with a rate limit gets a limiter like `Geocoder::throttled`. Nothing is sent
  to a service the user has not asked for by an action.
- **QML**: see the two pitfalls in [Interface](interface.md).

## How to …

**… add a setting.** Add a property to `Preferences.qml` with an alias in its
`Settings` object and a `sync()` on change, a control to `SettingsPage.qml`,
and read `Preferences.<name>` where it matters. If it needs logic or secrets,
make a C++ singleton as `Appearance` and `KeyStore` are.

**… add a line to the place panel.** If the information is an OSM tag: add the
key to the `Details` list in `GraphBuilder::describePlace` (regions have to be
downloaded again to have it) and a line in `detailsFromTags` in
`geocoder.cpp`. Nominatim's answers go through the same function.

**… add a built-in map.** Add an entry to `TileServer::presets()` and, if its
host has rules about offline use, to `policyFor()`.

**… make another kind of place clickable.** Add its layer to the list in
`Geocoder::placeAt` and a branch that names its kind and type.

**… change what the road graph stores.** Change `RoadGraph`, its `save` and
`load`, and the magic string in `roadgraph.cpp`, so that files of the old
layout are rejected instead of misread.

**… change routing behaviour.** Speeds and preferences are in `edgeCost` in
`router.cpp`; which roads a mode may use is decided in `GraphBuilder::way`.
When a cost factor can drop below the present minimum, lower
`minWeightPerMetre` too, or the search stops being optimal.

**… add a turn or travel icon.** Put a 16×16 SVG into `src/icons/` with the
`ColorScheme-Text` style sheet the others have, list it in
`src/CMakeLists.txt`, and return its name from `iconOf` in
`routecontroller.cpp` or use it by name in QML.

## Known limitations

They are listed in the [README](../README.md#known-limitations) and repeated
where they belong in these pages. The ones that shape the code most: the
router has no turn restrictions, a route is planned offline only when one
region covers all of its places, relations are not read from extracts, and
clicking places relies on the OpenMapTiles schema.
