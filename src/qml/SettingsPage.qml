// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

import io.github.n4n0gh.karten

/// The application's settings. Changes apply right away.
Kirigami.ScrollablePage {
    id: root

    /// The last entry of the list stands for an address typed in by hand.
    readonly property bool customSource: sourceBox.currentIndex === sourceBox.count - 1
    readonly property string customAddress: addressField.text.trim()
    readonly property bool customValid: TileServer.isValidSource(customAddress)

    title: i18nc("@title:window", "Settings")

    function showCurrentSource(): void {
        const preset = TileServer.presets.findIndex(entry => entry.url === TileServer.source);
        sourceBox.currentIndex = preset >= 0 ? preset : sourceBox.count - 1;
        addressField.text = preset >= 0 ? "" : TileServer.source;
    }
    Component.onCompleted: showCurrentSource()

    Kirigami.FormLayout {
        // --- Appearance ---
        QQC2.ComboBox {
            Kirigami.FormData.label: i18nc("@label:listbox", "Colors:")
            textRole: "text"
            valueRole: "scheme"
            model: [
                {
                    text: i18nc("@item:inlistbox colors of the application", "Follow System"),
                    scheme: Appearance.FollowSystem
                },
                {
                    text: i18nc("@item:inlistbox colors of the application", "Light"),
                    scheme: Appearance.Light
                },
                {
                    text: i18nc("@item:inlistbox colors of the application", "Dark"),
                    scheme: Appearance.Dark
                }
            ]
            Component.onCompleted: currentIndex = indexOfValue(Appearance.scheme)
            onActivated: Appearance.scheme = currentValue
        }

        // --- Map ---
        Item {
            Kirigami.FormData.isSection: true
        }
        QQC2.ComboBox {
            id: sourceBox
            Kirigami.FormData.label: i18nc("@label:listbox", "Map:")
            model: TileServer.presets.map(entry => entry.name).concat([i18nc("@item:inlistbox a map source entered by hand", "Other…")])
            onActivated: {
                if (root.customSource) {
                    addressField.forceActiveFocus();
                } else {
                    TileServer.source = TileServer.presets[currentIndex].url;
                }
            }
        }
        RowLayout {
            Kirigami.FormData.label: i18nc("@label:textbox", "Address:")
            Layout.fillWidth: true
            visible: root.customSource
            spacing: Kirigami.Units.smallSpacing

            QQC2.TextField {
                id: addressField
                Layout.fillWidth: true
                placeholderText: "https://"
                inputMethodHints: Qt.ImhUrlCharactersOnly
                onAccepted: applyButton.clicked()
            }
            // An address is only used once it is complete, not while typing.
            QQC2.Button {
                id: applyButton
                text: i18nc("@action:button", "Use This Map")
                icon.name: "dialog-ok-apply-symbolic"
                enabled: root.customValid && root.customAddress !== TileServer.source
                onClicked: if (enabled) {
                    TileServer.source = root.customAddress;
                }
            }
        }
        QQC2.Label {
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 22
            wrapMode: Text.Wrap
            readonly property bool invalid: root.customSource && root.customAddress.length > 0 && !root.customValid
            color: invalid ? Kirigami.Theme.negativeTextColor : Kirigami.Theme.disabledTextColor
            text: {
                if (!root.customSource) {
                    return i18nc("@info", "These maps are provided by OpenFreeMap and can be downloaded for offline use, one area at a time.");
                }
                if (invalid) {
                    return i18nc("@info", "The address must start with https://.");
                }
                return i18nc("@info", "The address of a MapLibre style, or of map image tiles with {z}, {x} and {y} standing for the zoom level and tile position. Check the terms of a map source before using it.");
            }
        }

        // --- Zoom bar ---
        Item {
            Kirigami.FormData.isSection: true
        }
        QQC2.CheckBox {
            Kirigami.FormData.label: i18nc("@title:group", "Zoom bar:")
            text: i18nc("@option:check", "Show the zoom level as a number")
            checked: Preferences.showZoomLevel
            onToggled: Preferences.showZoomLevel = checked
        }

        // --- Reviews from Google ---
        Kirigami.Separator {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: i18nc("@title:group", "Ratings and Reviews from Google")
        }
        QQC2.Label {
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 24
            wrapMode: Text.Wrap
            text: i18nc("@info", "Karten can show Google's rating and reviews for a place you click, using your own key for Google's Places API. Each click on a named place tells Google which place it was and counts towards the usage Google bills to your key.")
        }
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 24
            visible: true
            type: Kirigami.MessageType.Warning
            text: i18nc("@info", "Google's terms for its Maps Platform do not permit showing its place information on or near a map that is not Google's, as Karten's is. Google may suspend a key or account that is used this way.")
            actions: Kirigami.Action {
                text: i18nc("@action:button", "Read Google's Terms")
                icon.name: "internet-services-symbolic"
                onTriggered: Qt.openUrlExternally("https://cloud.google.com/maps-platform/terms")
            }
        }
        QQC2.CheckBox {
            id: googleAccept
            Layout.maximumWidth: Kirigami.Units.gridUnit * 24
            text: i18nc("@option:check", "I have read this and want to use my key anyway")
            checked: Preferences.googleAccepted
            onToggled: Preferences.googleAccepted = checked
        }
        Kirigami.PasswordField {
            Kirigami.FormData.label: i18nc("@label:textbox", "Places API key:")
            Layout.fillWidth: true
            // The key lives in the wallet, which has to be open.
            enabled: googleAccept.checked && KeyStore.ready
            text: Preferences.googleKey
            placeholderText: googleAccept.checked ? "" : i18nc("@info:placeholder", "Tick the box above first")
            onEditingFinished: KeyStore.googleKey = text
        }
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 24
            visible: googleAccept.checked && KeyStore.error.length > 0
            type: Kirigami.MessageType.Error
            text: KeyStore.error
            actions: Kirigami.Action {
                text: i18nc("@action:button", "Try Again")
                icon.name: "view-refresh-symbolic"
                onTriggered: KeyStore.open()
            }
        }
        QQC2.Label {
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 24
            wrapMode: Text.Wrap
            color: Kirigami.Theme.disabledTextColor
            text: Preferences.googleEnabled
                ? i18nc("@info", "Ratings and reviews from Google are switched on. Untick the box to switch them off; the key stays in your wallet.")
                : i18nc("@info", "Ratings and reviews from Google are switched off. The key is kept in your wallet (KWallet), not in a file.")
        }
    }
}
