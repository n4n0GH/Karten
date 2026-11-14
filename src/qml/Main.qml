// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import QtCore
import QtQuick
import org.kde.coreaddons as Core
import org.kde.kirigami as Kirigami

import io.github.n4n0gh.karten

Kirigami.ApplicationWindow {
    id: root

    width: Kirigami.Units.gridUnit * 64
    height: Kirigami.Units.gridUnit * 40
    minimumWidth: Kirigami.Units.gridUnit * 20
    minimumHeight: Kirigami.Units.gridUnit * 22

    // The map is the whole window; its controls float on top of it.
    pageStack.globalToolBar.style: Kirigami.ApplicationHeaderStyle.None
    pageStack.columnView.columnResizeMode: Kirigami.ColumnView.SingleColumn
    pageStack.initialPage: MapPage {
        id: mapPage
        onOfflineMapsRequested: root.pageStack.pushDialogLayer(offlinePage, {}, {
            title: i18nc("@title:window", "Offline Maps"),
            width: Kirigami.Units.gridUnit * 30,
            height: Kirigami.Units.gridUnit * 28
        })
        onSettingsRequested: root.pageStack.pushDialogLayer(settingsPage, {}, {
            title: i18nc("@title:window", "Settings"),
            width: Kirigami.Units.gridUnit * 34,
            height: Kirigami.Units.gridUnit * 34
        })
        onAboutRequested: root.pageStack.pushDialogLayer(aboutPage, {}, {
            title: i18nc("@title:window", "About Karten"),
            width: Kirigami.Units.gridUnit * 30,
            height: Kirigami.Units.gridUnit * 34
        })
    }

    Settings {
        category: "Window"
        property alias width: root.width
        property alias height: root.height
    }

    Component {
        id: offlinePage
        OfflinePage {
            id: offline
            onDownloadRequested: {
                offline.Kirigami.PageStack.closeDialog();
                mapPage.openDownloadDialog();
            }
        }
    }

    Component {
        id: settingsPage
        SettingsPage {}
    }

    Component {
        id: aboutPage
        Kirigami.AboutPage {
            aboutData: Core.AboutData
        }
    }

    Connections {
        target: Roadbook
        function onNotice(message: string): void {
            root.showPassiveNotification(message, "long");
        }
    }

    Connections {
        target: OfflineManager
        function onCompleted(message: string): void {
            root.showPassiveNotification(message);
        }
        function onFailed(message: string): void {
            root.showPassiveNotification(message, "long");
        }
    }
}
