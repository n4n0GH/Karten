// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami
import org.kde.kirigami.delegates as KD

import io.github.n4n0gh.karten

/// The places found by the Geocoder, or a note that there are none.
QQC2.ScrollView {
    id: root

    readonly property alias count: list.count
    signal chosen(var chosen)

    implicitHeight: list.count > 0 ? list.contentHeight : placeholder.implicitHeight + Kirigami.Units.gridUnit * 2

    ListView {
        id: list
        clip: true
        model: Geocoder.results

        delegate: KD.SubtitleDelegate {
            required property var modelData
            width: ListView.view.width
            text: modelData.title
            subtitle: modelData.subtitle
            icon.name: "mark-location-symbolic"
            onClicked: root.chosen(modelData)
        }

        Kirigami.PlaceholderMessage {
            id: placeholder
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 2
            visible: list.count === 0 && Geocoder.finished
            icon.name: "search-symbolic"
            text: i18nc("@info:placeholder", "No places found")
            explanation: Geocoder.onlineFailed
                ? i18nc("@info", "The online search could not be reached, so only downloaded regions were searched.")
                : i18nc("@info", "Check the spelling, or try a nearby town or street.")
        }
    }
}
