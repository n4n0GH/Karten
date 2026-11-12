<!--
SPDX-FileCopyrightText: 2026 Karten contributors
SPDX-License-Identifier: CC0-1.0
-->

# Karten code documentation

How Karten is put together, for people who want to read or change the code.
The [README](../README.md) at the top of the repository says what Karten is,
how to build it and which rules of the online services it follows; these pages
go into the code.

| Page | What it covers |
| --- | --- |
| [Architecture](architecture.md) | The parts, how they talk to each other, threads, where data is kept |
| [The map](map.md) | Local map server, tile store, map styles, offline map areas |
| [Routing](routing.md) | Reading OpenStreetMap extracts, the road graph, the router, stops, detours and previews |
| [Places](places.md) | Search, clicking places on the map, details, opening hours, Wikipedia, Google |
| [Interface](interface.md) | The QML files, settings, colours, icons |
| [Development](development.md) | Building, tests, debugging, conventions, how to add things |
| [Releasing](releasing.md) | Making a release, what the workflow does with it, the Flatpak repository and its signing key |

The headers under `src/` carry the reference documentation of each class as
comments. These pages explain how the classes fit together and why they are
the way they are; they do not repeat every member.

## The code at a glance

About 10,500 lines: C++20 for the logic, QML (Kirigami) for the interface.

```
src/
  core/           No GUI, unit-tested: OSM reader, road graph, router, vector tiles
  *.cpp, *.h      The application's services, each a QML singleton
  qml/            The interface
  icons/          Pictograms the icon themes lack
  tools/          karten-route, a command line router for development
autotests/        Unit tests for src/core
scripts/          build-maplibre.sh
docs/             These pages
```
