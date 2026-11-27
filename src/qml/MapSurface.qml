// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtLocation
import QtPositioning
import QtQuick
import org.kde.kirigami as Kirigami

/// A map that can be dragged, flung, pinched, turned and tilted.
///
/// Takes the place of Qt's MapView, which moves the map with Map.pan(). That
/// counts in whole pixels from a rounded-down middle of the view: with
/// fractional scaling part of every movement is lost, and in a view of odd
/// width or height the map creeps sideways by half a pixel per event. Here
/// the spot that was taken hold of stays under the pointer.
///
/// The wheel is left to whoever uses this.
Item {
    id: root

    property alias map: mapItem

    /// How far along a fling is, from 0 to 1.
    property real glide: 0

    /// Moves the map by a distance on screen, which need not be whole pixels.
    function panBy(dx: real, dy: real): void {
        const centre = mapItem.toCoordinate(Qt.point(mapItem.width / 2 - dx, mapItem.height / 2 - dy), false);
        if (centre.isValid) {
            mapItem.center = centre;
        }
    }

    onGlideChanged: if (fling.running) {
        const step = glide - fling.done;
        fling.done = glide;
        panBy(fling.reach.x * step, fling.reach.y * step);
    }

    // Lets the map run on for a moment after it was let go of in motion.
    NumberAnimation {
        id: fling

        /// How far it carries the map in all, in pixels.
        property vector2d reach
        /// How much of the way has been passed on to the map.
        property real done: 0

        function begin(velocity: vector2d): void {
            stop();
            // Slowing down evenly to a halt covers half of what the speed
            // at the start would, so the map sets off as fast as it was let go.
            reach = velocity.times(duration / 2000);
            if (reach.length() < 1) {
                return;
            }
            done = 0;
            root.glide = 0;
            start();
        }

        target: root
        property: "glide"
        from: 0
        to: 1
        duration: Kirigami.Units.veryLongDuration
        easing.type: Easing.OutQuad
    }

    Map {
        id: mapItem

        anchors.fill: parent
        // Zooming holds on to a spot of its own.
        onZoomLevelChanged: fling.stop()

        DragHandler {
            id: drag

            /// The spot on the map that was pressed.
            property geoCoordinate grabbed

            /// Puts it back under the pointer.
            function follow(): void {
                if (grabbed.isValid) {
                    mapItem.alignCoordinateToPoint(grabbed, centroid.position);
                }
            }

            target: null
            onActiveChanged: {
                if (active) {
                    fling.stop();
                    // The pointer has gone some way before it counts as
                    // dragging: the map catches up with it.
                    grabbed = mapItem.toCoordinate(centroid.pressPosition, false);
                    follow();
                } else {
                    fling.begin(centroid.velocity);
                }
            }
            onCentroidChanged: if (active) {
                follow();
            }
        }

        // Zooms and turns the map around the spot between two fingers.
        PinchHandler {
            id: pinch

            property geoCoordinate grabbed
            /// Where the fingers have turned the map to, before north catches it.
            property real turned: 0

            target: null
            grabPermissions: PointerHandler.TakeOverForbidden
            onActiveChanged: {
                if (active) {
                    fling.stop();
                    grabbed = mapItem.toCoordinate(centroid.position, false);
                    turned = mapItem.bearing;
                } else {
                    fling.begin(centroid.velocity);
                }
            }
            onScaleChanged: delta => {
                mapItem.zoomLevel += Math.log2(delta);
                mapItem.alignCoordinateToPoint(grabbed, centroid.position);
            }
            onRotationChanged: delta => {
                turned -= delta;
                // Within five degrees of north, north it is.
                const offNorth = ((turned % 360) + 540) % 360 - 180;
                mapItem.bearing = Math.abs(offNorth) < 5 ? 0 : turned;
                mapItem.alignCoordinateToPoint(grabbed, centroid.position);
            }
        }

        // Tilts the map when two fingers move up or down together.
        DragHandler {
            minimumPointCount: 2
            maximumPointCount: 2
            target: null
            xAxis.enabled: false
            grabPermissions: PointerHandler.TakeOverForbidden
            onActiveChanged: if (active) {
                fling.stop();
            }
            onTranslationChanged: delta => mapItem.tilt -= delta.y / 5
        }

        // A press brings a fling to a halt.
        TapHandler {
            acceptedButtons: Qt.AllButtons
            onPressedChanged: if (pressed) {
                fling.stop();
            }
        }
    }
}
