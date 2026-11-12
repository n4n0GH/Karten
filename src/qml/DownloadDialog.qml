// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.coreaddons as Core
import org.kde.kirigami as Kirigami

import io.github.n4n0gh.karten

/// Offers what can be stored for the part of the map that is on screen.
Kirigami.Dialog {
    id: root

    property real south
    property real west
    property real north
    property real east

    /// The extract picked in the list: see OfflineManager.regionChoices.
    readonly property var region: regionBox.currentIndex >= 0 ? OfflineManager.regionChoices[regionBox.currentIndex] ?? null : null
    readonly property bool regionAvailable: region !== null && !region.downloading && !region.tooLarge
    /// Vector maps have one level of detail, which the map source defines.
    readonly property int maxZoom: TileServer.vector ? 22 : detail.currentValue
    /// Counted again when the map source changes: maxPackTiles follows it.
    readonly property int tiles: TileServer.maxPackTiles, OfflineManager.tileCount(south, west, north, east, maxZoom)
    readonly property real size: TileServer.maxPackTiles, OfflineManager.estimatedSize(south, west, north, east, maxZoom)
    readonly property bool tilesTooMany: tiles > TileServer.maxPackTiles || size > TileServer.maxPackBytes

    signal mapSourceRequested

    title: i18nc("@title:window", "Download Offline Map")
    preferredWidth: Kirigami.Units.gridUnit * 26
    padding: Kirigami.Units.largeSpacing
    standardButtons: Kirigami.Dialog.Cancel
    customFooterActions: Kirigami.Action {
        text: i18nc("@action:button", "Download")
        icon.name: "download-symbolic"
        enabled: (routingCheck.checked && root.regionAvailable) || (tilesCheck.checked && tilesCheck.enabled)
        onTriggered: {
            if (routingCheck.checked && root.regionAvailable) {
                OfflineManager.downloadRegion(root.region.id);
            }
            if (tilesCheck.checked && tilesCheck.enabled) {
                OfflineManager.downloadTiles(nameField.text, root.south, root.west, root.north, root.east, root.maxZoom);
            }
            root.close();
        }
    }

    onOpened: {
        regionBox.chosenId = "";
        OfflineManager.lookUpRegion(south, west, north, east);
        nameField.text = "";
    }
    // Do not pre-select re-downloading something that is already there.
    onRegionChanged: routingCheck.checked = regionAvailable && !region.installed

    ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        QQC2.CheckBox {
            id: routingCheck
            text: i18nc("@option:check", "Routes and place search")
            enabled: root.regionAvailable
        }
        QQC2.ComboBox {
            id: regionBox
            Layout.fillWidth: true
            Layout.leftMargin: routingCheck.indicator.width + routingCheck.spacing
            visible: count > 0
            Accessible.name: i18nc("@label:listbox", "Region")
            textRole: "label"
            // Updating the sizes replaces the list, so keep the selection by hand.
            property string chosenId
            model: OfflineManager.regionChoices.map(choice => ({
                id: choice.id,
                label: choice.size > 0
                    ? i18nc("@item:inlistbox %1 is a region such as Bavaria, %2 its download size", "%1 (%2)", choice.name, Core.Format.formatByteSize(choice.size))
                    : choice.name
            }))
            onModelChanged: currentIndex = Math.max(0, model.findIndex(choice => choice.id === chosenId))
            onActivated: chosenId = model[currentIndex].id
        }
        QQC2.Label {
            Layout.fillWidth: true
            Layout.leftMargin: routingCheck.indicator.width + routingCheck.spacing
            wrapMode: Text.Wrap
            color: Kirigami.Theme.disabledTextColor
            text: {
                switch (OfflineManager.lookupState) {
                case "loading":
                    return i18nc("@info:status", "Looking up the regions that cover this part of the map…");
                case "error":
                    return i18nc("@info", "The list of regions could not be loaded. Check your internet connection.");
                case "none":
                    return i18nc("@info", "There is no routing data for this part of the map.");
                }
                const region = root.region;
                if (!region) {
                    return "";
                }
                if (region.downloading) {
                    return i18nc("@info %1 is a region such as Bavaria", "%1 is already being downloaded.", region.name);
                }
                if (region.tooLarge) {
                    return i18nc("@info %1 is a region such as Europe", "%1 is too large to prepare on this system. Choose a smaller region.", region.name);
                }
                return region.installed
                    ? i18nc("@info %1 is a region", "%1 is already downloaded. Select to update it.", region.name)
                    : i18nc("@info %1 is a region", "Plan routes and find places anywhere in %1 without an internet connection.", region.name);
            }
        }

        QQC2.CheckBox {
            id: tilesCheck
            Layout.topMargin: Kirigami.Units.largeSpacing
            text: i18nc("@option:check", "Map of the visible area")
            enabled: TileServer.offlineAllowed && !root.tilesTooMany && root.tiles > 0
            checked: TileServer.offlineAllowed
        }
        QQC2.Label {
            Layout.fillWidth: true
            Layout.leftMargin: routingCheck.indicator.width + routingCheck.spacing
            wrapMode: Text.Wrap
            color: Kirigami.Theme.disabledTextColor
            text: {
                if (!TileServer.offlineAllowed) {
                    return i18nc("@info", "The current map source does not permit downloading its map for offline use. Parts of the map you have looked at stay available.");
                }
                if (root.tilesTooMany) {
                    return TileServer.vector
                        ? i18nc("@info", "This area is larger than the map source permits in one download. Zoom in to download it in parts.")
                        : i18nc("@info", "This area is too large at this level of detail. Zoom in, or choose less detail.");
                }
                const size = Core.Format.formatByteSize(root.size);
                return TileServer.vector
                    ? i18nc("@info %1 is a size", "About %1, with all streets, buildings and paths", size)
                    : i18nc("@info %1 is a size", "About %1", size);
            }
        }
        Kirigami.LinkButton {
            Layout.leftMargin: routingCheck.indicator.width + routingCheck.spacing
            visible: !TileServer.offlineAllowed
            text: i18nc("@action:button", "Choose Another Map in Settings…")
            onClicked: {
                root.close();
                root.mapSourceRequested();
            }
        }

        Kirigami.FormLayout {
            Layout.fillWidth: true
            Layout.leftMargin: routingCheck.indicator.width + routingCheck.spacing
            visible: TileServer.offlineAllowed
            enabled: tilesCheck.checked

            QQC2.ComboBox {
                id: detail
                Kirigami.FormData.label: i18nc("@label:listbox", "Detail:")
                visible: !TileServer.vector
                textRole: "text"
                valueRole: "zoom"
                currentIndex: 1
                model: [
                    {
                        text: i18nc("@item:inlistbox level of map detail", "Towns and main roads"),
                        zoom: 12
                    },
                    {
                        text: i18nc("@item:inlistbox level of map detail", "All streets"),
                        zoom: 15
                    },
                    {
                        text: i18nc("@item:inlistbox level of map detail", "Buildings and paths"),
                        zoom: 17
                    }
                ]
            }
            QQC2.TextField {
                id: nameField
                Kirigami.FormData.label: i18nc("@label:textbox", "Name:")
                placeholderText: i18nc("default name of a downloaded map area", "Map area")
            }
        }
    }
}
