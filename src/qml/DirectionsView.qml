// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtPositioning
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

import io.github.n4n0gh.karten

/// Start, destination and stops on the way, the travel mode, and the
/// resulting route with its steps.
ColumnLayout {
    id: root

    required property geoCoordinate mapCenter
    required property geoCoordinate myLocation
    /// Which stop the search results belong to.
    property int editingStop: 0
    /// The parts of the route whose steps are shown, by number.
    property var expandedLegs: ({})
    readonly property bool searching: results.count > 0 || Geocoder.finished || Geocoder.busy

    signal closed
    signal positionChosen(real latitude, real longitude)

    spacing: Kirigami.Units.largeSpacing

    /// Puts the cursor where the next place is to be entered.
    function focusFirstEmptyStop(): void {
        const index = RouteController.stops.findIndex(stop => !stop.valid);
        if (index >= 0) {
            stopList.focusStop(index);
        }
    }

    onVisibleChanged: if (visible) {
        focusFirstEmptyStop();
    }

    Connections {
        target: RouteController
        function onRouteChanged(): void {
            root.expandedLegs = {};
        }
        function onEndpointsChanged(): void {
            root.syncStops();
        }
    }

    // A copy of the stops that can be rearranged while one is being dragged;
    // the route only learns of the new order when the stop is dropped.
    ListModel {
        id: stopModel
    }
    function syncStops(): void {
        stopModel.clear();
        RouteController.stops.forEach((stop, index) => stopModel.append({
            name: stop.name,
            origin: index
        }));
    }
    Component.onCompleted: syncStops()

    RowLayout {
        spacing: Kirigami.Units.smallSpacing

        QQC2.ToolButton {
            text: i18nc("@action:button", "Close Directions")
            icon.name: "go-previous-symbolic"
            display: QQC2.AbstractButton.IconOnly
            onClicked: root.closed()
            QQC2.ToolTip.text: text
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
        }
        Kirigami.Heading {
            Layout.fillWidth: true
            level: 2
            text: i18nc("@title", "Directions")
            elide: Text.ElideRight
        }
    }

    // Travel mode.
    RowLayout {
        Layout.fillWidth: true
        spacing: 0

        QQC2.ButtonGroup {
            id: modeGroup
        }
        Repeater {
            model: [
                {
                    mode: RouteController.Drive,
                    icon: "travel-drive-symbolic",
                    label: i18nc("@option:radio travel by car", "Drive")
                },
                {
                    mode: RouteController.Cycle,
                    icon: "travel-cycle-symbolic",
                    label: i18nc("@option:radio travel by bicycle", "Cycle")
                },
                {
                    mode: RouteController.Walk,
                    icon: "travel-walk-symbolic",
                    label: i18nc("@option:radio travel on foot", "Walk")
                }
            ]
            QQC2.Button {
                required property var modelData
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                text: modelData.label
                icon.name: modelData.icon
                checkable: true
                checked: RouteController.mode === modelData.mode
                QQC2.ButtonGroup.group: modeGroup
                onClicked: RouteController.mode = modelData.mode
            }
        }
    }

    // Start, stops on the way and destination.
    StopList {
        id: stopList
        Layout.fillWidth: true
        model: stopModel
        myLocationAvailable: root.myLocation.isValid
        onTextEdited: (stop, text) => {
            root.editingStop = stop;
            Geocoder.suggest(text, root.mapCenter.latitude, root.mapCenter.longitude);
        }
        onAccepted: (stop, text) => {
            root.editingStop = stop;
            Geocoder.search(text, root.mapCenter.latitude, root.mapCenter.longitude);
        }
        onMyLocationRequested: RouteController.setFrom(root.myLocation, i18nc("@item start of a route", "My Location"))
        onRemoveRequested: stop => RouteController.removeStop(stop)
        onDropped: {
            const order = [];
            for (let i = 0; i < stopModel.count; ++i) {
                order.push(stopModel.get(i).origin);
            }
            RouteController.reorderStops(order);
            // Also when nothing changed, to get the numbering back.
            root.syncStops();
        }
    }

    RowLayout {
        spacing: Kirigami.Units.smallSpacing

        QQC2.Button {
            text: i18nc("@action:button", "Add Destination")
            icon.name: "list-add-symbolic"
            // A new one is added once the last has been chosen.
            enabled: RouteController.to.isValid && RouteController.stops.length < 10
            onClicked: {
                RouteController.addStop();
                root.focusFirstEmptyStop();
            }
        }
        Item {
            Layout.fillWidth: true
        }
        QQC2.ToolButton {
            text: i18nc("@action:button", "Reverse Route")
            icon.name: "exchange-positions-symbolic"
            display: QQC2.AbstractButton.IconOnly
            enabled: RouteController.stops.some(stop => stop.valid)
            onClicked: RouteController.swap()
            QQC2.ToolTip.text: text
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
        }
    }

    SearchResults {
        id: results
        Layout.fillWidth: true
        Layout.fillHeight: true
        visible: count > 0 || Geocoder.finished
        onChosen: chosen => {
            const coordinate = QtPositioning.coordinate(chosen.latitude, chosen.longitude);
            Geocoder.clear();
            RouteController.setStop(root.editingStop, coordinate, chosen.title);
        }
    }

    // --- The route ---
    RowLayout {
        visible: RouteController.busy && !root.searching
        spacing: Kirigami.Units.largeSpacing
        QQC2.BusyIndicator {
            implicitWidth: Kirigami.Units.iconSizes.medium
            implicitHeight: Kirigami.Units.iconSizes.medium
        }
        QQC2.Label {
            Layout.fillWidth: true
            text: i18nc("@info:progress", "Planning route…")
        }
    }

    Kirigami.InlineMessage {
        Layout.fillWidth: true
        visible: RouteController.errorText.length > 0 && !RouteController.busy && !root.searching
        type: Kirigami.MessageType.Warning
        text: RouteController.errorText
    }

    ColumnLayout {
        visible: RouteController.hasRoute && !RouteController.busy && !root.searching
        spacing: 0

        Kirigami.Heading {
            Layout.fillWidth: true
            level: 1
            text: RouteController.durationText
        }
        QQC2.Label {
            Layout.fillWidth: true
            text: RouteController.offline
                ? i18nc("@info distance of a route", "%1, planned offline", RouteController.distanceText)
                : RouteController.distanceText
            color: Kirigami.Theme.disabledTextColor
        }
    }

    // One entry per part of the route; its steps appear when it is clicked.
    QQC2.ScrollView {
        id: legsScroll
        Layout.fillWidth: true
        Layout.fillHeight: true
        visible: RouteController.hasRoute && !RouteController.busy && !root.searching
        contentWidth: availableWidth
        QQC2.ScrollBar.horizontal.policy: QQC2.ScrollBar.AlwaysOff

        ColumnLayout {
            width: legsScroll.availableWidth
            spacing: 0

            Repeater {
                model: RouteController.legs

                ColumnLayout {
                    id: leg
                    required property int index
                    required property var modelData
                    readonly property bool expanded: root.expandedLegs[index] === true
                    Layout.fillWidth: true
                    spacing: 0

                    QQC2.ItemDelegate {
                        id: legHeader
                        Layout.fillWidth: true
                        text: leg.modelData.title
                        Accessible.description: leg.expanded ? i18nc("@info accessible", "Steps shown") : i18nc("@info accessible", "Steps hidden")
                        onClicked: {
                            const expanded = Object.assign({}, root.expandedLegs);
                            expanded[leg.index] = !leg.expanded;
                            root.expandedLegs = expanded;
                        }

                        contentItem: RowLayout {
                            spacing: Kirigami.Units.largeSpacing
                            Kirigami.Icon {
                                source: leg.expanded ? "arrow-down-symbolic" : (Application.layoutDirection === Qt.RightToLeft ? "arrow-left-symbolic" : "arrow-right-symbolic")
                                implicitWidth: Kirigami.Units.iconSizes.small
                                implicitHeight: Kirigami.Units.iconSizes.small
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0
                                QQC2.Label {
                                    Layout.fillWidth: true
                                    text: leg.modelData.title
                                    elide: Text.ElideRight
                                }
                                QQC2.Label {
                                    Layout.fillWidth: true
                                    text: i18nc("@info duration and distance of a part of a route", "%1 · %2", leg.modelData.duration, leg.modelData.distance)
                                    font: Kirigami.Theme.smallFont
                                    color: Kirigami.Theme.disabledTextColor
                                    elide: Text.ElideRight
                                }
                            }
                        }
                    }

                    Repeater {
                        // Only built while they are shown.
                        model: leg.expanded ? leg.modelData.steps : []

                        QQC2.ItemDelegate {
                            id: step
                            required property var modelData
                            Layout.fillWidth: true
                            Layout.leftMargin: Kirigami.Units.iconSizes.small + Kirigami.Units.largeSpacing
                            text: modelData.text
                            onClicked: root.positionChosen(modelData.latitude, modelData.longitude)

                            contentItem: RowLayout {
                                spacing: Kirigami.Units.largeSpacing
                                Kirigami.Icon {
                                    source: step.modelData.icon
                                    implicitWidth: Kirigami.Units.iconSizes.smallMedium
                                    implicitHeight: Kirigami.Units.iconSizes.smallMedium
                                }
                                QQC2.Label {
                                    Layout.fillWidth: true
                                    text: step.modelData.text
                                    wrapMode: Text.Wrap
                                }
                                QQC2.Label {
                                    text: step.modelData.distance
                                    color: Kirigami.Theme.disabledTextColor
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    QQC2.Label {
        Layout.fillWidth: true
        visible: !RouteController.hasRoute && !RouteController.busy && RouteController.errorText.length === 0 && !root.searching
        text: i18nc("@info", "Choose a start and a destination to plan a route.")
        wrapMode: Text.Wrap
        color: Kirigami.Theme.disabledTextColor
    }
}
