// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

/// The zoom level as a bar that floats over the map: a slider between the
/// zoom out and zoom in buttons, and the level as a percentage, from the
/// whole world at 0 % to the closest view at 100 %.
QQC2.Control {
    id: root

    /// What the map shows right now, which moves while a zoom is animated.
    required property real zoomLevel
    /// Where the map is heading; the same as zoomLevel when it is at rest.
    required property real targetZoomLevel
    required property real minimumZoomLevel
    required property real maximumZoomLevel
    /// How far one step of the spin box goes, in percent.
    required property int stepPercent
    required property Kirigami.Action zoomIn
    required property Kirigami.Action zoomOut

    /// The slider is being dragged: follow it directly.
    signal zoomMoved(real level)
    /// A level was entered: go there smoothly.
    signal zoomRequested(real level)

    readonly property real zoomRange: Math.max(maximumZoomLevel - minimumZoomLevel, 0.001)

    padding: Kirigami.Units.smallSpacing

    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false

    background: CardBackground {}

    contentItem: RowLayout {
        spacing: Kirigami.Units.smallSpacing

        QQC2.ToolButton {
            action: root.zoomOut
            display: QQC2.AbstractButton.IconOnly
            QQC2.ToolTip.text: text
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
        }
        QQC2.Slider {
            id: slider
            Layout.preferredWidth: Kirigami.Units.gridUnit * 8
            Layout.fillWidth: true
            from: root.minimumZoomLevel
            to: root.maximumZoomLevel
            Accessible.name: i18nc("@label:slider", "Zoom")
            onMoved: root.zoomMoved(value)

            // Moving the slider replaces a plain binding, so restore it by hand.
            Binding on value {
                value: root.zoomLevel
                when: !slider.pressed
            }
        }
        QQC2.ToolButton {
            action: root.zoomIn
            display: QQC2.AbstractButton.IconOnly
            QQC2.ToolTip.text: text
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
        }
        QQC2.SpinBox {
            id: spinBox
            visible: Preferences.showZoomLevel
            from: 0
            to: 100
            stepSize: root.stepPercent
            editable: true
            textFromValue: value => i18nc("@item:valuesuffix zoom level in percent", "%1 %", value)
            valueFromText: text => parseInt(text) || 0
            Accessible.name: i18nc("@label:spinbox", "Zoom")
            QQC2.ToolTip.text: Accessible.name
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            onValueModified: root.zoomRequested(root.minimumZoomLevel + value / 100 * root.zoomRange)

            // The target rather than the level on screen: that would count
            // through every step of the animation.
            Binding on value {
                value: Math.round((root.targetZoomLevel - root.minimumZoomLevel) / root.zoomRange * 100)
            }
        }
    }
}
