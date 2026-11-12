// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtPositioning
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

import io.github.n4n0gh.karten

/// The card that floats over the map: search and its results, or directions.
QQC2.Control {
    id: root

    required property geoCoordinate mapCenter
    required property var place
    required property bool directionsMode
    required property geoCoordinate myLocation
    required property Kirigami.Action directionsAction
    property real maximumHeight: Infinity

    signal placeChosen(var chosen)
    signal placeDismissed
    signal directionsClosed
    signal positionChosen(real latitude, real longitude)

    height: Math.min(implicitHeight, maximumHeight)
    padding: Kirigami.Units.largeSpacing

    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false

    onDirectionsModeChanged: Geocoder.clear()

    background: CardBackground {}

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        // --- Search ---
        RowLayout {
            visible: !root.directionsMode
            spacing: Kirigami.Units.smallSpacing

            Kirigami.SearchField {
                id: searchField
                Layout.fillWidth: true
                // Submitting asks the online service, which must not be done while typing.
                autoAccept: false
                placeholderText: i18nc("@info:placeholder", "Search for a place or address…")
                onTextEdited: {
                    root.placeDismissed();
                    Geocoder.suggest(text, root.mapCenter.latitude, root.mapCenter.longitude);
                }
                onTextChanged: {
                    if (text.length === 0) {
                        Geocoder.clear();
                        root.placeDismissed();
                    }
                }
                onAccepted: Geocoder.search(text, root.mapCenter.latitude, root.mapCenter.longitude)
            }
            QQC2.ToolButton {
                action: root.directionsAction
                display: QQC2.AbstractButton.IconOnly
                QQC2.ToolTip.text: text
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            }
        }

        QQC2.ProgressBar {
            Layout.fillWidth: true
            visible: Geocoder.busy
            indeterminate: true
        }

        SearchResults {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !root.directionsMode && root.place === null && (count > 0 || Geocoder.finished)
            onChosen: chosen => {
                searchField.text = chosen.title;
                Geocoder.clear();
                root.placeChosen(chosen);
            }
        }

        // --- Directions ---
        DirectionsView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.directionsMode
            mapCenter: root.mapCenter
            myLocation: root.myLocation
            onClosed: root.directionsClosed()
            onPositionChosen: (latitude, longitude) => root.positionChosen(latitude, longitude)
        }
    }
}
