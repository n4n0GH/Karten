// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

import io.github.n4n0gh.karten

/// Everything that is stored for offline use, with its downloads in progress.
Kirigami.ScrollablePage {
    id: page

    signal downloadRequested

    title: i18nc("@title:window", "Offline Maps")

    actions: Kirigami.Action {
        id: downloadAction
        text: i18nc("@action:button", "Download Visible Area…")
        icon.name: "download-symbolic"
        onTriggered: page.downloadRequested()
    }

    ListView {
        id: list
        model: OfflineManager

        section.property: "section"
        section.delegate: Kirigami.ListSectionHeader {
            required property string section
            width: ListView.view.width
            text: section
        }

        delegate: QQC2.ItemDelegate {
            id: entry
            required property string key
            required property string name
            required property string detail
            required property real progress
            required property bool active

            width: ListView.view.width
            text: name
            // The row itself does nothing; its button does.
            hoverEnabled: false
            down: false

            contentItem: RowLayout {
                spacing: Kirigami.Units.largeSpacing

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing
                    QQC2.Label {
                        Layout.fillWidth: true
                        text: entry.name
                        elide: Text.ElideRight
                    }
                    QQC2.Label {
                        Layout.fillWidth: true
                        text: entry.detail
                        font: Kirigami.Theme.smallFont
                        color: Kirigami.Theme.disabledTextColor
                        elide: Text.ElideRight
                    }
                    QQC2.ProgressBar {
                        Layout.fillWidth: true
                        visible: entry.active
                        indeterminate: entry.progress < 0
                        value: Math.max(0, entry.progress)
                    }
                }
                QQC2.ToolButton {
                    text: entry.active ? i18nc("@action:button", "Cancel Download") : i18nc("@action:button", "Delete")
                    icon.name: entry.active ? "dialog-cancel-symbolic" : "edit-delete-symbolic"
                    display: QQC2.AbstractButton.IconOnly
                    onClicked: {
                        if (entry.active) {
                            OfflineManager.remove(entry.key);
                        } else {
                            deleteDialog.key = entry.key;
                            deleteDialog.name = entry.name;
                            deleteDialog.open();
                        }
                    }
                    QQC2.ToolTip.text: text
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                }
            }
        }

        Kirigami.PlaceholderMessage {
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 4
            visible: list.count === 0
            icon.name: "offline-symbolic"
            text: i18nc("@info:placeholder", "No offline maps yet")
            explanation: i18nc("@info", "Move the map to the area you need, then download it to plan routes and find places without an internet connection.")
            helpfulAction: downloadAction
        }
    }

    Kirigami.PromptDialog {
        id: deleteDialog
        property string key
        property string name

        title: i18nc("@title:window", "Delete Offline Map")
        subtitle: i18nc("@info %1 is the name of a downloaded region", "Delete %1 from this system? To use it offline again, you will have to download it again.", name)
        dialogType: Kirigami.PromptDialog.Warning
        standardButtons: Kirigami.Dialog.Cancel
        customFooterActions: Kirigami.Action {
            text: i18nc("@action:button", "Delete")
            icon.name: "edit-delete-symbolic"
            onTriggered: {
                OfflineManager.remove(deleteDialog.key);
                deleteDialog.close();
            }
        }
    }
}
