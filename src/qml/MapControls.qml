// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami

/// Round buttons that float over the map, one per action.
Column {
    id: root

    required property list<Kirigami.Action> actions

    spacing: Kirigami.Units.smallSpacing

    Repeater {
        model: root.actions
        QQC2.RoundButton {
            required property Kirigami.Action modelData
            action: modelData
            visible: modelData.visible
            display: QQC2.AbstractButton.IconOnly
            implicitWidth: Kirigami.Units.gridUnit * 2
            implicitHeight: Kirigami.Units.gridUnit * 2
            QQC2.ToolTip.text: text
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
        }
    }
}
