// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import org.kde.kirigami as Kirigami

/// The background of everything that floats over the map.
Kirigami.ShadowedRectangle {
    color: Kirigami.Theme.backgroundColor
    radius: Kirigami.Units.cornerRadius
    border.width: 1
    border.color: Kirigami.ColorUtils.linearInterpolation(Kirigami.Theme.backgroundColor, Kirigami.Theme.textColor, Kirigami.Theme.frameContrast)
    shadow.size: Kirigami.Units.gridUnit
    shadow.yOffset: Kirigami.Units.smallSpacing
    shadow.color: Qt.rgba(0, 0, 0, 0.2)

    // Keep clicks and scrolling on the card from reaching the map below.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: wheel => wheel.accepted = true
    }
}
