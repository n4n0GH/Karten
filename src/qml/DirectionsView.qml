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
                },
                {
                    mode: RouteController.Transit,
                    icon: "travel-transit-symbolic",
                    label: i18nc("@option:radio travel by public transport", "Transit")
                }
            ]
            QQC2.Button {
                required property var modelData
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                // Four of them only fit as pictures; the name is in the tooltip.
                text: modelData.label
                icon.name: modelData.icon
                display: QQC2.AbstractButton.IconOnly
                QQC2.ToolTip.text: text
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
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

    // --- Public transport: when to travel ---
    readonly property bool transit: RouteController.mode === RouteController.Transit

    /// Passes the chosen time on. "Leave now" is a time that is not valid.
    function applyTransitTime(): void {
        if (whenBox.currentIndex === 0) {
            RouteController.setTransitTime(new Date(NaN), false);
            return;
        }
        const parts = timeField.text.split(":");
        const hours = parseInt(parts[0]);
        const minutes = parseInt(parts[1]);
        if (parts.length !== 2 || !(hours >= 0 && hours < 24) || !(minutes >= 0 && minutes < 60)) {
            return;
        }
        const when = new Date();
        when.setDate(when.getDate() + dayBox.currentIndex);
        when.setHours(hours, minutes, 0, 0);
        RouteController.setTransitTime(when, whenBox.currentIndex === 2);
    }

    RowLayout {
        visible: root.transit
        spacing: Kirigami.Units.smallSpacing

        QQC2.ComboBox {
            id: whenBox
            Layout.fillWidth: true
            Accessible.name: i18nc("@label:listbox", "When to travel")
            model: [
                i18nc("@item:inlistbox when to travel", "Leave now"),
                i18nc("@item:inlistbox when to travel", "Leave at"),
                i18nc("@item:inlistbox when to travel", "Arrive by")
            ]
            onActivated: {
                if (currentIndex > 0 && timeField.text.length === 0) {
                    timeField.text = Qt.formatTime(new Date(), "HH:mm");
                }
                root.applyTransitTime();
            }
        }
        QQC2.ComboBox {
            id: dayBox
            visible: whenBox.currentIndex > 0
            Accessible.name: i18nc("@label:listbox", "Day")
            // Today, tomorrow, then the days of the week to come.
            model: Array.from({length: 7}, (unused, offset) => {
                if (offset === 0) {
                    return i18nc("@item:inlistbox day to travel", "Today");
                }
                if (offset === 1) {
                    return i18nc("@item:inlistbox day to travel", "Tomorrow");
                }
                const day = new Date();
                day.setDate(day.getDate() + offset);
                return day.toLocaleDateString(Qt.locale(), "dddd");
            })
            onActivated: root.applyTransitTime()
        }
        QQC2.TextField {
            id: timeField
            visible: whenBox.currentIndex > 0
            Layout.preferredWidth: Kirigami.Units.gridUnit * 4
            horizontalAlignment: Text.AlignHCenter
            placeholderText: "12:00"
            inputMethodHints: Qt.ImhTime
            validator: RegularExpressionValidator {
                regularExpression: /([01]?[0-9]|2[0-3]):[0-5][0-9]/
            }
            Accessible.name: i18nc("@label:textbox", "Time")
            onEditingFinished: root.applyTransitTime()
        }
    }
    QQC2.Label {
        Layout.fillWidth: true
        visible: root.transit && RouteController.stops.length > 2
        wrapMode: Text.Wrap
        font: Kirigami.Theme.smallFont
        color: Kirigami.Theme.disabledTextColor
        text: i18nc("@info", "Public transport is looked up from the start to the destination. The stops in between are left out.")
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
            // By public transport this is the time of departure and arrival.
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

            // By public transport: the connections to choose from.
            Repeater {
                model: RouteController.connections

                QQC2.ItemDelegate {
                    id: connection
                    required property int index
                    required property var modelData
                    Layout.fillWidth: true
                    highlighted: index === RouteController.connection
                    text: i18nc("@item departure and arrival time of a connection", "%1 – %2", modelData.departure, modelData.arrival)
                    onClicked: RouteController.connection = index

                    contentItem: RowLayout {
                        spacing: Kirigami.Units.largeSpacing
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            QQC2.Label {
                                Layout.fillWidth: true
                                text: connection.text
                                font.bold: connection.highlighted
                                color: connection.highlighted ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.textColor
                                elide: Text.ElideRight
                            }
                            QQC2.Label {
                                Layout.fillWidth: true
                                text: connection.modelData.lines.join(i18nc("separates the lines of a connection", " › "))
                                font: Kirigami.Theme.smallFont
                                color: connection.highlighted ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.disabledTextColor
                                elide: Text.ElideRight
                            }
                        }
                        ColumnLayout {
                            spacing: 0
                            QQC2.Label {
                                Layout.alignment: Qt.AlignRight
                                text: connection.modelData.duration
                                color: connection.highlighted ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.textColor
                            }
                            QQC2.Label {
                                Layout.alignment: Qt.AlignRight
                                text: connection.modelData.changes === 0
                                    ? i18nc("@info a connection without changing trains", "Direct")
                                    : i18ncp("@info number of times to change trains", "%1 change", "%1 changes", connection.modelData.changes)
                                font: Kirigami.Theme.smallFont
                                color: connection.highlighted ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.disabledTextColor
                            }
                        }
                    }
                }
            }
            Kirigami.Separator {
                Layout.fillWidth: true
                Layout.topMargin: Kirigami.Units.smallSpacing
                Layout.bottomMargin: Kirigami.Units.smallSpacing
                visible: RouteController.connections.length > 0
            }

            Repeater {
                model: RouteController.legs

                ColumnLayout {
                    id: leg
                    // A walk between two rides has nothing to unfold.
                    readonly property bool expandable: modelData.steps.length > 0
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
                            if (!leg.expandable) {
                                return;
                            }
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
                                opacity: leg.expandable ? 1 : 0
                            }
                            // What kind of travel this part is, by public transport.
                            Kirigami.Icon {
                                visible: (leg.modelData.icon ?? "").length > 0
                                source: leg.modelData.icon ?? ""
                                implicitWidth: Kirigami.Units.iconSizes.smallMedium
                                implicitHeight: Kirigami.Units.iconSizes.smallMedium
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
                                    text: leg.modelData.detail
                                        ?? (leg.modelData.distance.length > 0
                                            ? i18nc("@info duration and distance of a part of a route", "%1 · %2", leg.modelData.duration, leg.modelData.distance)
                                            : leg.modelData.duration)
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

    // The service asks for a visible link to where its timetables come from.
    QQC2.Label {
        Layout.fillWidth: true
        visible: root.transit && RouteController.connections.length > 0 && !RouteController.busy && !root.searching
        textFormat: Text.StyledText
        wrapMode: Text.Wrap
        font: Kirigami.Theme.smallFont
        color: Kirigami.Theme.disabledTextColor
        text: i18nc("@info credit; %1 is a link titled Transitous, %2 one titled their sources", "Connections by %1, from %2",
            "<a href=\"https://transitous.org\">Transitous</a>",
            "<a href=\"https://transitous.org/sources/\">" + i18nc("@info link title", "these sources") + "</a>")
        onLinkActivated: link => Qt.openUrlExternally(link)
        HoverHandler {
            cursorShape: parent.hoveredLink ? Qt.PointingHandCursor : Qt.ArrowCursor
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
