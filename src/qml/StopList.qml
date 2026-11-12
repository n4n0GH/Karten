// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// No "pragma ComponentBehavior: Bound" here: Kirigami's drag handle reads the
// row's "index" from the context, which that pragma would take away.

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

/// The places of a route as a list of text fields: the start, stops on the
/// way and the destination. With more than two, they can be dragged into
/// another order. The model is a ListModel with the role "name"; its rows
/// are moved while one is being dragged.
ListView {
    id: root

    property bool myLocationAvailable: false
    // As wide as the widest of the labels in front of the fields.
    readonly property real labelWidth: Math.max(fromMetrics.width, viaMetrics.width, toMetrics.width)
    readonly property bool reorderable: count > 2

    signal textEdited(int stop, string text)
    signal accepted(int stop, string text)
    signal myLocationRequested
    signal removeRequested(int stop)
    /// A row was dragged somewhere and let go; the model has the new order.
    signal dropped

    function focusStop(stop: int): void {
        const row = itemAtIndex(stop);
        if (row) {
            row.focusField();
        }
    }

    implicitHeight: contentHeight
    interactive: false
    spacing: Kirigami.Units.smallSpacing

    TextMetrics {
        id: fromMetrics
        text: i18nc("@label:textbox start of a route", "From:")
    }
    TextMetrics {
        id: viaMetrics
        text: i18nc("@label:textbox stop on the way of a route", "Via:")
    }
    TextMetrics {
        id: toMetrics
        text: i18nc("@label:textbox destination of a route", "To:")
    }

    moveDisplaced: Transition {
        NumberAnimation {
            property: "y"
            duration: Kirigami.Units.shortDuration
            easing.type: Easing.InOutQuad
        }
    }

    // The row is a child of the delegate, not the delegate itself: the drag
    // handle lifts it out of the list while it is dragged.
    delegate: Item {
        function focusField(): void {
            field.forceActiveFocus();
        }

        width: root.width
        implicitHeight: stopRow.height

        Item {
            id: stopRow

            readonly property bool isStart: index === 0
            readonly property bool isDestination: index === root.count - 1
            // The drag handle looks at this to show itself more clearly.
            readonly property bool hovered: rowHover.hovered

            width: root.width
            height: rowLayout.implicitHeight

            HoverHandler {
                id: rowHover
            }

            RowLayout {
                id: rowLayout
                anchors.fill: parent
                spacing: Kirigami.Units.smallSpacing

                Kirigami.ListItemDragHandle {
                    visible: root.reorderable
                    listItem: stopRow
                    listView: root
                    onMoveRequested: (oldIndex, newIndex) => root.model.move(oldIndex, newIndex, 1)
                    onDropped: root.dropped()
                }
                QQC2.Label {
                    Layout.preferredWidth: root.labelWidth
                    horizontalAlignment: Text.AlignRight
                    text: stopRow.isStart ? fromMetrics.text : stopRow.isDestination ? toMetrics.text : viaMetrics.text
                }
                QQC2.TextField {
                    id: field
                    Layout.fillWidth: true
                    text: model.name
                    placeholderText: i18nc("@info:placeholder", "Search, or right-click the map")
                    Accessible.name: stopRow.isStart ? i18nc("@label:textbox", "Start")
                        : stopRow.isDestination ? i18nc("@label:textbox", "Destination")
                        : i18nc("@label:textbox; %1 is a number", "Stop %1", index)
                    onTextEdited: root.textEdited(index, text)
                    onAccepted: root.accepted(index, text)
                    onActiveFocusChanged: {
                        if (activeFocus) {
                            selectAll();
                        }
                    }
                }
                QQC2.ToolButton {
                    text: stopRow.isStart ? i18nc("@action:button", "Start from My Location") : i18nc("@action:button", "Remove This Stop")
                    icon.name: stopRow.isStart ? "gps-symbolic" : "list-remove-symbolic"
                    display: QQC2.AbstractButton.IconOnly
                    // The start and the destination stay; with more stops, any other can go.
                    enabled: stopRow.isStart ? root.myLocationAvailable : root.reorderable
                    opacity: stopRow.isStart || root.reorderable ? 1 : 0
                    onClicked: {
                        if (stopRow.isStart) {
                            root.myLocationRequested();
                        } else {
                            root.removeRequested(index);
                        }
                    }
                    QQC2.ToolTip.text: text
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                }
            }
        }
    }
}
