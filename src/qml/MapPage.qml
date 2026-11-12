// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtCore
import QtLocation
import QtPositioning
import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami

import io.github.n4n0gh.karten

Kirigami.Page {
    id: page

    signal offlineMapsRequested
    signal aboutRequested
    signal settingsRequested

    /// The map itself; null for a moment while the map source changes.
    readonly property Map map: mapLoader.item ? (mapLoader.item as MapView).map : null
    /// The place shown with a pin: an object with title, subtitle, latitude, longitude.
    property var place: null
    /// More about the place, once it has been looked up: see PlacePanel.
    property list<var> placeDetails: []
    /// An encyclopedia article about the place, or null: see PlacePanel.
    property var placeArticle: null
    /// Google's rating and reviews of the place, or null: see PlacePanel.
    property var placeReviews: null
    onPlaceChanged: if (place === null) {
        placeDetails = [];
        placeArticle = null;
        placeReviews = null;
    }
    property bool directionsMode: false
    /// Set while waiting for the first position fix after "Show My Location".
    property bool centreOnPosition: false

    title: i18nc("@title:window", "Map")
    padding: 0
    globalToolBarStyle: Kirigami.ApplicationHeaderStyle.None

    function coordinateLabel(coordinate: geoCoordinate): string {
        return coordinate.latitude.toFixed(5) + ", " + coordinate.longitude.toFixed(5);
    }

    function showPlace(newPlace: var): void {
        placeDetails = [];
        placeArticle = null;
        placeReviews = null;
        place = newPlace;
        if (map) {
            map.center = QtPositioning.coordinate(newPlace.latitude, newPlace.longitude);
            map.zoomLevel = Math.max(map.zoomLevel, 15);
        }
    }

    /// Selects what the map labels at a position, if anything.
    function selectPlaceAt(coordinate: geoCoordinate): void {
        const found = Geocoder.placeAt(coordinate.latitude, coordinate.longitude, map.zoomLevel);
        if (found.title === undefined) {
            return;
        }
        directionsMode = false;
        placeDetails = [];
        placeArticle = null;
        placeReviews = null;
        place = found;
        Geocoder.lookUpDetails(found.latitude, found.longitude, found.title, found.type);
        // Only for those who asked for it and entered their own key.
        if (Preferences.googleEnabled) {
            Geocoder.lookUpReviews(found.latitude, found.longitude, found.title, Preferences.googleKey);
        }
    }

    function dropPin(coordinate: geoCoordinate): void {
        placeDetails = [];
        placeArticle = null;
        placeReviews = null;
        place = {
            title: i18nc("@title a pin placed on the map", "Dropped Pin"),
            subtitle: coordinateLabel(coordinate),
            latitude: coordinate.latitude,
            longitude: coordinate.longitude
        };
        Geocoder.reverse(coordinate.latitude, coordinate.longitude);
    }

    /// Uses a point on the map as start or destination, and finds a name for it.
    function routeEndpoint(coordinate: geoCoordinate, isStart: bool): void {
        directionsMode = true;
        if (isStart) {
            RouteController.setFrom(coordinate, coordinateLabel(coordinate));
        } else {
            RouteController.setTo(coordinate, coordinateLabel(coordinate));
        }
        Geocoder.reverse(coordinate.latitude, coordinate.longitude);
    }

    function directionsTo(target: var): void {
        directionsMode = true;
        RouteController.setTo(QtPositioning.coordinate(target.latitude, target.longitude), target.title);
        if (!RouteController.from.isValid && myLocation.isValid) {
            RouteController.setFrom(myLocation, i18nc("@item start of a route", "My Location"));
        }
        place = null;
    }

    function openDownloadDialog(): void {
        if (!map) {
            return;
        }
        const region = map.visibleRegion.boundingGeoRectangle();
        downloadDialog.south = region.bottomRight.latitude;
        downloadDialog.west = region.topLeft.longitude;
        downloadDialog.north = region.topLeft.latitude;
        downloadDialog.east = region.bottomRight.longitude;
        downloadDialog.open();
    }

    // --- Zooming ---
    // Changes of the zoom level by wheel, button or number glide to their
    // target instead of jumping there.

    /// Where a running zoom animation ends.
    property real zoomTarget: 0
    /// Driven by the animation and passed on to the map.
    property real animatedZoom: 0
    /// The zoom level the map shows once it has come to rest.
    readonly property real pendingZoom: zoomAnimation.running ? zoomTarget : (map ? map.zoomLevel : 0)
    /// The spot that stays put while zooming, such as under the mouse pointer.
    property bool zoomAnchored: false
    property point zoomAnchor
    property geoCoordinate zoomAnchorCoordinate

    /// One step of the zoom buttons, the wheel and the spin box, as a
    /// percentage of the way from the whole world to the closest view.
    readonly property int zoomStepPercent: 5
    readonly property real zoomStep: map ? (map.maximumZoomLevel - map.minimumZoomLevel) * zoomStepPercent / 100 : 1

    /// Zooms by whole steps, landing on the steps the percentage counts in.
    function zoomBySteps(steps: int): void {
        const current = Math.round((pendingZoom - map.minimumZoomLevel) / zoomStep);
        zoomTo(map.minimumZoomLevel + (current + steps) * zoomStep);
    }

    function zoomTo(level: real): void {
        zoomAnchored = false;
        startZoom(level);
    }

    function zoomAround(level: real, anchor: point): void {
        zoomAnchored = true;
        zoomAnchor = anchor;
        zoomAnchorCoordinate = map.toCoordinate(anchor, false);
        startZoom(level);
    }

    function startZoom(level: real): void {
        zoomAnimation.stop();
        zoomTarget = Math.max(map.minimumZoomLevel, Math.min(map.maximumZoomLevel, level));
        zoomAnimation.from = map.zoomLevel;
        zoomAnimation.to = zoomTarget;
        zoomAnimation.start();
    }

    function applyZoom(level: real): void {
        map.zoomLevel = level;
        if (zoomAnchored) {
            map.alignCoordinateToPoint(zoomAnchorCoordinate, zoomAnchor);
        }
    }

    onAnimatedZoomChanged: if (zoomAnimation.running && map) {
        applyZoom(animatedZoom);
    }

    NumberAnimation {
        id: zoomAnimation
        target: page
        property: "animatedZoom"
        duration: Kirigami.Units.longDuration
        easing.type: Easing.OutCubic
        onFinished: if (page.map) {
            page.applyZoom(page.zoomTarget);
        }
    }

    function showRoute(): void {
        if (!map || !RouteController.hasRoute) {
            return;
        }
        map.fitViewportToGeoShape(RouteController.bounds, Kirigami.Units.gridUnit * 4);
        if (panel.width < page.width / 2) {
            // Keep the route clear of the panel that covers the left of the map.
            map.zoomLevel -= 0.5;
            map.pan(-(panel.width + Kirigami.Units.largeSpacing) / 2, 0);
        }
    }

    Settings {
        id: viewSettings
        category: "Map"
        property real latitude: 50.0
        property real longitude: 10.0
        property real zoomLevel: 4.5
    }

    /// Where the system says it is, while showing that is switched on;
    /// otherwise not a valid coordinate.
    readonly property geoCoordinate myLocation: locateAction.checked ? positionSource.position.coordinate : QtPositioning.coordinate()

    PositionSource {
        id: positionSource
        // Only asks for the location while the user wants it shown.
        active: locateAction.checked
        updateInterval: 2000
        onPositionChanged: {
            if (page.centreOnPosition && position.coordinate.isValid && page.map) {
                page.centreOnPosition = false;
                page.map.center = position.coordinate;
                page.map.zoomLevel = Math.max(page.map.zoomLevel, 15);
            }
        }
    }

    Kirigami.Action {
        id: openDirectionsAction
        text: i18nc("@action:button", "Directions")
        icon.name: "routeplanning-symbolic"
        shortcut: "Ctrl+D"
        onTriggered: page.directionsMode = true
    }
    Kirigami.Action {
        id: downloadAction
        text: i18nc("@action:inmenu", "Download Visible Area…")
        icon.name: "download-symbolic"
        shortcut: "Ctrl+Shift+D"
        onTriggered: page.openDownloadDialog()
    }
    Kirigami.Action {
        id: offlineMapsAction
        text: i18nc("@action:inmenu", "Offline Maps")
        icon.name: "offline-symbolic"
        shortcut: "Ctrl+Shift+O"
        onTriggered: page.offlineMapsRequested()
    }
    Kirigami.Action {
        id: reportAction
        text: i18nc("@action:inmenu", "Report a Map Problem")
        icon.name: "tools-report-bug-symbolic"
        onTriggered: {
            const c = page.map.center;
            Qt.openUrlExternally("https://www.openstreetmap.org/fixthemap?lat=" + c.latitude.toFixed(5) + "&lon=" + c.longitude.toFixed(5) + "&zoom="
                                 + Math.round(page.map.zoomLevel));
        }
    }
    Kirigami.Action {
        id: settingsAction
        text: i18nc("@action:inmenu", "Settings")
        icon.name: "configure-symbolic"
        shortcut: StandardKey.Preferences
        onTriggered: page.settingsRequested()
    }
    Kirigami.Action {
        id: aboutAction
        text: i18nc("@action:inmenu", "About Karten")
        icon.name: "help-about-symbolic"
        onTriggered: page.aboutRequested()
    }
    Kirigami.Action {
        id: zoomInAction
        text: i18nc("@action:button", "Zoom In")
        icon.name: "zoom-in-symbolic"
        shortcut: StandardKey.ZoomIn
        enabled: page.map && page.map.zoomLevel < page.map.maximumZoomLevel
        onTriggered: page.zoomBySteps(1)
    }
    Kirigami.Action {
        id: zoomOutAction
        text: i18nc("@action:button", "Zoom Out")
        icon.name: "zoom-out-symbolic"
        shortcut: StandardKey.ZoomOut
        enabled: page.map && page.map.zoomLevel > page.map.minimumZoomLevel
        onTriggered: page.zoomBySteps(-1)
    }
    Kirigami.Action {
        id: northAction
        text: i18nc("@action:button", "Point North Up")
        icon.name: "compass-symbolic"
        visible: page.map && (page.map.bearing !== 0 || page.map.tilt !== 0)
        onTriggered: {
            page.map.bearing = 0;
            page.map.tilt = 0;
        }
    }
    // A switch: while it is on, the location is followed and shown on the
    // map; switching it off stops asking for it and takes it off the map.
    Kirigami.Action {
        id: locateAction
        text: i18nc("@action:button", "Show My Location")
        icon.name: "gps-symbolic"
        shortcut: "Ctrl+L"
        visible: positionSource.valid
        checkable: true
        onCheckedChanged: on => {
            if (!on) {
                page.centreOnPosition = false;
            } else if (positionSource.position.coordinate.isValid) {
                // Known from last time; go there while waiting for news.
                page.map.center = positionSource.position.coordinate;
                page.map.zoomLevel = Math.max(page.map.zoomLevel, 15);
            } else {
                page.centreOnPosition = true;
            }
        }
    }
    Shortcut {
        sequence: "Escape"
        enabled: page.directionsMode && !downloadDialog.visible
        onActivated: page.directionsMode = false
    }

    Connections {
        target: TileServer
        // A different map source needs a new map view.
        // Assigned, not bound: it must keep the old value until the handler ran.
        property string shownStyle
        Component.onCompleted: shownStyle = TileServer.styleUrl
        function onSourceChanged(): void {
            // The choice may have changed without the map changing.
            if (TileServer.styleUrl === shownStyle) {
                return;
            }
            shownStyle = TileServer.styleUrl;
            page.saveView();
            mapLoader.active = false;
            mapLoader.active = true;
        }
    }
    Connections {
        target: Geocoder
        function onReviewsFinished(latitude: real, longitude: real, reviews: var): void {
            if (page.place && page.place.latitude === latitude && page.place.longitude === longitude) {
                page.placeReviews = reviews;
            }
        }
        function onArticleFinished(latitude: real, longitude: real, article: var): void {
            if (page.place && page.place.latitude === latitude && page.place.longitude === longitude) {
                page.placeArticle = article;
            }
        }
        function onDetailsFinished(latitude: real, longitude: real, details: list<var>): void {
            if (page.place && page.place.latitude === latitude && page.place.longitude === longitude) {
                page.placeDetails = details;
            }
        }
        function onReverseFinished(latitude: real, longitude: real, name: string): void {
            RouteController.nameEndpoint(latitude, longitude, name);
            if (page.place && page.place.latitude === latitude && page.place.longitude === longitude) {
                page.place = {
                    title: name,
                    subtitle: page.place.subtitle,
                    latitude: latitude,
                    longitude: longitude
                };
            }
        }
    }
    Connections {
        target: RouteController
        /// The stops of the route the map was last fitted to.
        property string fittedStops
        function onRouteChanged(): void {
            if (!RouteController.hasRoute) {
                return;
            }
            // Only a route between other places is brought into view. Bending
            // it or changing how to travel keeps the view, so that the change
            // can be seen where it was made.
            const stops = RouteController.stops.map(stop => stop.latitude + "," + stop.longitude).join(";");
            if (stops !== fittedStops) {
                fittedStops = stops;
                page.showRoute();
            }
        }
    }

    function saveView(): void {
        if (map) {
            viewSettings.latitude = map.center.latitude;
            viewSettings.longitude = map.center.longitude;
            viewSettings.zoomLevel = map.zoomLevel;
        }
    }
    function restoreView(): void {
        if (map) {
            map.center = QtPositioning.coordinate(viewSettings.latitude, viewSettings.longitude);
            map.zoomLevel = viewSettings.zoomLevel;
        }
    }
    // The settings are only read once the page is complete, which is after
    // the map view inside it has been created.
    Component.onCompleted: restoreView()
    Component.onDestruction: saveView()

    Loader {
        id: mapLoader
        anchors.fill: parent
        sourceComponent: mapComponent
        onLoaded: page.restoreView()
    }

    Component {
        id: mapComponent

        MapView {
            id: view

            component Marker: MapQuickItem {
                id: marker
                property bool filled: true
                anchorPoint.x: dot.width / 2
                anchorPoint.y: dot.height / 2
                sourceItem: Rectangle {
                    id: dot
                    width: Kirigami.Units.gridUnit
                    height: width
                    radius: width / 2
                    color: marker.filled ? Kirigami.Theme.highlightColor : Kirigami.Theme.backgroundColor
                    border.width: marker.filled ? Math.round(Kirigami.Units.smallSpacing / 2) : Kirigami.Units.smallSpacing
                    border.color: marker.filled ? Kirigami.Theme.backgroundColor : Kirigami.Theme.highlightColor
                }
            }

            /// Smaller than a stop: a place the route only passes.
            component DetourPin: MapQuickItem {
                anchorPoint.x: pin.width / 2
                anchorPoint.y: pin.height / 2
                sourceItem: Rectangle {
                    id: pin
                    width: Math.round(Kirigami.Units.gridUnit * 0.7)
                    height: width
                    radius: width / 2
                    color: Kirigami.Theme.backgroundColor
                    border.width: Math.round(Kirigami.Units.smallSpacing * 0.75)
                    border.color: Kirigami.Theme.highlightColor
                }
            }

            Kirigami.Theme.colorSet: Kirigami.Theme.View
            Kirigami.Theme.inherit: false

            map.plugin: Plugin {
                name: "maplibre"
                // The whole map comes from Karten's own server on this machine,
                // which answers from disk when it can and from the map source otherwise.
                PluginParameter {
                    name: "maplibre.map.styles"
                    value: TileServer.styleUrl
                }
                // Karten keeps the map on disk itself; a second copy would only go stale.
                PluginParameter {
                    name: "maplibre.cache.memory"
                    value: true
                }
            }
            map.copyrightsVisible: false // shown by our own label, see below
            map.minimumZoomLevel: 2
            map.onCenterChanged: saveTimer.restart()
            map.onZoomLevelChanged: saveTimer.restart()

            Component.onCompleted: {
                for (const item of [previewLine, routeCasing, routeLine, placeMarker, positionMarker]) {
                    map.addMapItem(item);
                }
                map.addMapItemView(stopMarkers);
                map.addMapItemView(detourMarkers);
                map.addMapItem(dragMarker);
            }

            Timer {
                id: saveTimer
                interval: Kirigami.Units.humanMoment
                onTriggered: page.saveView()
            }

            MapPolyline {
                id: routeCasing
                line.width: Kirigami.Units.smallSpacing * 2
                line.color: Kirigami.Theme.backgroundColor
                path: RouteController.path
                visible: RouteController.hasRoute && page.directionsMode
            }
            // The route as it would be with the detour that is being dragged.
            MapPolyline {
                id: previewLine
                line.width: routeLine.line.width
                // A paler shade rather than a see-through line, which would
                // turn solid wherever the route doubles back on itself. It
                // lies under the route and shows where the two differ.
                line.color: Kirigami.ColorUtils.linearInterpolation(Kirigami.Theme.highlightColor, Kirigami.Theme.backgroundColor, 0.55)
                path: RouteController.previewPath
                visible: routeCasing.visible && RouteController.previewPath.length > 1
            }
            MapPolyline {
                id: routeLine
                line.width: Kirigami.Units.smallSpacing + Math.round(Kirigami.Units.smallSpacing / 4)
                line.color: Kirigami.Theme.highlightColor
                path: RouteController.path
                visible: routeCasing.visible
            }
            // The start is drawn hollow, the stops and the destination filled.
            MapItemView {
                id: stopMarkers
                model: page.directionsMode ? RouteController.stops : []
                delegate: Marker {
                    required property int index
                    required property var modelData
                    filled: index > 0
                    coordinate: QtPositioning.coordinate(modelData.latitude, modelData.longitude)
                    visible: modelData.valid
                }
            }
            Marker {
                id: placeMarker
                coordinate: page.place ? QtPositioning.coordinate(page.place.latitude, page.place.longitude) : QtPositioning.coordinate()
                visible: page.place !== null
            }
            MapQuickItem {
                id: positionMarker
                coordinate: page.myLocation
                visible: page.myLocation.isValid
                anchorPoint.x: halo.width / 2
                anchorPoint.y: halo.height / 2
                sourceItem: Rectangle {
                    id: halo
                    width: Kirigami.Units.gridUnit * 2
                    height: width
                    radius: width / 2
                    color: Qt.alpha(Kirigami.Theme.highlightColor, 0.25)
                    Rectangle {
                        anchors.centerIn: parent
                        width: Kirigami.Units.gridUnit * 0.75
                        height: width
                        radius: width / 2
                        color: Kirigami.Theme.highlightColor
                        border.width: Math.round(Kirigami.Units.smallSpacing / 2)
                        border.color: Kirigami.Theme.backgroundColor
                    }
                }
            }

            // Places the route was bent through by dragging it.
            MapItemView {
                id: detourMarkers
                model: page.directionsMode ? RouteController.detours : []
                delegate: DetourPin {
                    required property var modelData
                    coordinate: QtPositioning.coordinate(modelData.latitude, modelData.longitude)
                    // Hidden while it is the one being dragged.
                    visible: !(routeDrag.dragging && routeDrag.detour !== null && routeDrag.detour.stop === modelData.stop && routeDrag.detour.index === modelData.index)
                }
            }
            // Follows the pointer while the route or a detour is dragged.
            DetourPin {
                id: dragMarker
                visible: routeDrag.dragging
            }

            // Sits on top of the map to get the wheel before the map view's
            // own handling, which jumps from one zoom level to the next.
            Item {
                anchors.fill: parent

                // Bending the route: drag the line to somewhere else to make
                // it pass there, drag a detour to move it, click it to remove
                // it. Presses elsewhere are left to the map underneath.
                MouseArea {
                    id: routeDrag

                    /// How near the pointer has to be, in pixels.
                    readonly property real reach: Kirigami.Units.gridUnit * 0.6
                    /// The detour that was pressed, or null for the route line.
                    property var detour: null
                    property point pressedAt
                    property geoCoordinate grabbed
                    property bool dragging: false
                    property bool overRoute: false

                    function detourAt(x: real, y: real): var {
                        for (const candidate of RouteController.detours) {
                            const at = view.map.fromCoordinate(QtPositioning.coordinate(candidate.latitude, candidate.longitude), false);
                            if (Math.hypot(at.x - x, at.y - y) <= reach) {
                                return candidate;
                            }
                        }
                        return null;
                    }
                    function nearRoute(x: real, y: real): bool {
                        const coordinate = view.map.toCoordinate(Qt.point(x, y), false);
                        // What the reach comes to on the ground at this spot.
                        const metres = coordinate.distanceTo(view.map.toCoordinate(Qt.point(x + reach, y), false));
                        return RouteController.distanceToRoute(coordinate) <= metres;
                    }

                    anchors.fill: parent
                    enabled: page.directionsMode && RouteController.hasRoute && !RouteController.busy
                    acceptedButtons: Qt.LeftButton
                    // Once the route is taken hold of, the map must not pan.
                    preventStealing: true
                    hoverEnabled: enabled
                    cursorShape: dragging ? Qt.ClosedHandCursor : overRoute ? Qt.OpenHandCursor : Qt.ArrowCursor

                    onPositionChanged: mouse => {
                        if (!pressed) {
                            overRoute = detourAt(mouse.x, mouse.y) !== null || nearRoute(mouse.x, mouse.y);
                            return;
                        }
                        if (!dragging && Math.hypot(mouse.x - pressedAt.x, mouse.y - pressedAt.y) > Application.styleHints.startDragDistance) {
                            dragging = true;
                        }
                        if (dragging) {
                            const target = view.map.toCoordinate(Qt.point(mouse.x, mouse.y), false);
                            dragMarker.coordinate = target;
                            // Shows where the route would go if let go here.
                            if (detour !== null) {
                                RouteController.previewMovedDetour(detour.stop, detour.index, target);
                            } else {
                                RouteController.previewDetour(grabbed, target);
                            }
                        }
                    }
                    onPressed: mouse => {
                        detour = detourAt(mouse.x, mouse.y);
                        if (detour === null && !nearRoute(mouse.x, mouse.y)) {
                            mouse.accepted = false; // the map's to pan
                            return;
                        }
                        pressedAt = Qt.point(mouse.x, mouse.y);
                        grabbed = view.map.toCoordinate(pressedAt, false);
                    }
                    onReleased: mouse => {
                        const target = view.map.toCoordinate(Qt.point(mouse.x, mouse.y), false);
                        if (dragging && detour !== null) {
                            RouteController.moveDetour(detour.stop, detour.index, target);
                        } else if (dragging) {
                            RouteController.addDetour(grabbed, target);
                        } else if (detour !== null) {
                            RouteController.removeDetour(detour.stop, detour.index);
                        }
                        // After the change, which takes over what was previewed.
                        RouteController.clearPreview();
                        dragging = false;
                        detour = null;
                    }
                    onCanceled: {
                        RouteController.clearPreview();
                        dragging = false;
                        detour = null;
                    }
                    onExited: overRoute = false
                }

                WheelHandler {
                    id: wheelHandler
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    onWheel: event => {
                        const map = view.map;
                        const position = wheelHandler.point.position;
                        if (event.modifiers === Qt.NoModifier) {
                            page.zoomAround(page.pendingZoom + event.angleDelta.y / 120 * page.zoomStep, position);
                            return;
                        }
                        // As the map view does it: turn with Shift, tilt with Ctrl.
                        const coordinate = map.toCoordinate(position, false);
                        if (event.modifiers === Qt.ShiftModifier) {
                            map.bearing += event.angleDelta.y / 15;
                        } else if (event.modifiers === Qt.ControlModifier) {
                            map.tilt += event.angleDelta.y / 15;
                        }
                        map.alignCoordinateToPoint(coordinate, position);
                    }
                }
            }

            TapHandler {
                acceptedButtons: Qt.LeftButton
                onTapped: eventPoint => page.selectPlaceAt(view.map.toCoordinate(eventPoint.position))
            }
            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: eventPoint => contextMenu.openAt(eventPoint.position)
            }
            TapHandler {
                acceptedDevices: PointerDevice.TouchScreen
                onLongPressed: contextMenu.openAt(point.position)
            }

            QQC2.Menu {
                id: contextMenu
                property geoCoordinate coordinate

                function openAt(position: point): void {
                    coordinate = view.map.toCoordinate(position);
                    popup(position.x, position.y);
                }

                QQC2.MenuItem {
                    text: i18nc("@action:inmenu", "Directions from Here")
                    icon.name: "arrow-up-symbolic"
                    onTriggered: page.routeEndpoint(contextMenu.coordinate, true)
                }
                QQC2.MenuItem {
                    text: i18nc("@action:inmenu", "Directions to Here")
                    icon.name: "flag-symbolic"
                    onTriggered: page.routeEndpoint(contextMenu.coordinate, false)
                }
                QQC2.MenuSeparator {}
                QQC2.MenuItem {
                    text: i18nc("@action:inmenu", "Drop Pin")
                    icon.name: "mark-location-symbolic"
                    onTriggered: page.dropPin(contextMenu.coordinate)
                }
            }
        }
    }

    SidePanel {
        id: panel
        x: Kirigami.Units.largeSpacing
        y: Kirigami.Units.largeSpacing
        // Next to the menu, also in a narrow window.
        width: Math.min(Kirigami.Units.gridUnit * 20, page.width - menuCard.width - Kirigami.Units.largeSpacing * 3)
        maximumHeight: page.height - Kirigami.Units.largeSpacing * 2

        mapCenter: page.map ? page.map.center : QtPositioning.coordinate(0, 0)
        place: page.place
        directionsMode: page.directionsMode
        myLocation: page.myLocation
        directionsAction: openDirectionsAction

        onPlaceChosen: chosen => page.showPlace(chosen)
        onPlaceDismissed: page.place = null
        onDirectionsClosed: page.directionsMode = false
        onPositionChosen: (latitude, longitude) => {
            page.map.center = QtPositioning.coordinate(latitude, longitude);
            page.map.zoomLevel = Math.max(page.map.zoomLevel, 16);
        }
    }

    PlacePanel {
        id: placePanel
        x: panel.x
        y: panel.y + panel.height + Kirigami.Units.largeSpacing
        width: panel.width
        visible: page.place !== null && !page.directionsMode
        place: page.place
        details: page.placeDetails
        article: page.placeArticle
        reviews: page.placeReviews
        // The panel keeps its place under the search card; in a small window
        // it ends above the bottom edge and scrolls. Likewise above the zoom
        // bar when the two are above each other.
        maximumHeight: page.height - y - Kirigami.Units.largeSpacing - (x + width > zoomControl.x ? page.height - zoomControl.y : 0)
        onDirectionsRequested: page.directionsTo(page.place)
        onClosed: page.place = null
    }

    // The application menu, in a card of its own.
    QQC2.Control {
        id: menuCard
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Kirigami.Units.largeSpacing
        // The same padding as the search card, to line up with it.
        padding: Kirigami.Units.largeSpacing

        Kirigami.Theme.colorSet: Kirigami.Theme.View
        Kirigami.Theme.inherit: false

        background: CardBackground {}

        contentItem: QQC2.ToolButton {
            id: menuButton
            text: i18nc("@action:button", "Menu")
            icon.name: "application-menu-symbolic"
            display: QQC2.AbstractButton.IconOnly
            down: pressed || menu.visible
            onClicked: menu.popup(menuButton, 0, menuButton.height)
            QQC2.ToolTip.text: text
            QQC2.ToolTip.visible: hovered && !menu.visible
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay

            QQC2.Menu {
                id: menu
                Repeater {
                    model: [downloadAction, offlineMapsAction, settingsAction, reportAction, aboutAction]
                    QQC2.MenuItem {
                        required property Kirigami.Action modelData
                        action: modelData
                    }
                }
            }
        }
    }

    MapControls {
        anchors.right: parent.right
        anchors.bottom: zoomControl.top
        anchors.margins: Kirigami.Units.largeSpacing
        actions: [northAction, locateAction]
    }

    ZoomControl {
        id: zoomControl
        anchors.right: parent.right
        anchors.bottom: attribution.top
        anchors.margins: Kirigami.Units.largeSpacing
        visible: page.map !== null
        zoomLevel: page.map ? page.map.zoomLevel : 0
        targetZoomLevel: page.pendingZoom
        minimumZoomLevel: page.map ? page.map.minimumZoomLevel : 0
        maximumZoomLevel: page.map ? page.map.maximumZoomLevel : 1
        stepPercent: page.zoomStepPercent
        zoomIn: zoomInAction
        zoomOut: zoomOutAction
        onZoomMoved: level => {
            zoomAnimation.stop();
            page.map.zoomLevel = level;
        }
        onZoomRequested: level => page.zoomTo(level)
    }

    // OpenStreetMap's licence requires the attribution to stay visible.
    QQC2.Label {
        id: attribution
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        padding: Kirigami.Units.smallSpacing
        // Map sources name their own credits; without any, credit the data.
        text: TileServer.attribution.length > 0 ? TileServer.attribution
            : i18nc("@info map attribution; %1 is a link titled OpenStreetMap", "© %1 contributors",
                    "<a href=\"https://www.openstreetmap.org/copyright\">OpenStreetMap</a>")
        textFormat: Text.StyledText
        font: Kirigami.Theme.smallFont
        onLinkActivated: link => Qt.openUrlExternally(link)

        Kirigami.Theme.colorSet: Kirigami.Theme.View
        Kirigami.Theme.inherit: false
        background: Rectangle {
            color: Qt.alpha(Kirigami.Theme.backgroundColor, 0.8)
            topLeftRadius: Kirigami.Units.cornerRadius
        }
        HoverHandler {
            cursorShape: attribution.hoveredLink ? Qt.PointingHandCursor : Qt.ArrowCursor
        }
    }

    DownloadDialog {
        id: downloadDialog
        onMapSourceRequested: page.settingsRequested()
    }

}
