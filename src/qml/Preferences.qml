// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma Singleton

import QtCore
import QtQuick

import io.github.n4n0gh.karten

/// What the user has chosen on the settings page. Kept in one place so that
/// a change takes effect everywhere at once.
QtObject {
    id: root

    /// Whether the zoom bar shows the zoom level as a number next to the slider.
    property bool showZoomLevel: true
    // Written at once, so that quitting right after a change does not lose it.
    onShowZoomLevelChanged: storage.sync()

    /// Whether the user has agreed to place information being fetched from
    /// Google, having been told what Google's terms say about that.
    property bool googleAccepted: false
    /// The user's own key for Google's Places API. It is kept in the
    /// wallet, which is only opened for those who agreed: see KeyStore.
    readonly property string googleKey: googleAccepted ? KeyStore.googleKey : ""
    readonly property bool googleEnabled: googleAccepted && googleKey.length > 0
    onGoogleAcceptedChanged: {
        googleStorage.sync();
        if (googleAccepted) {
            KeyStore.open();
        }
    }
    Component.onCompleted: if (googleAccepted) {
        KeyStore.open();
    }

    readonly property Settings googleStorage: Settings {
        category: "Google"
        property alias accepted: root.googleAccepted
    }

    readonly property Settings storage: Settings {
        category: "Interface"
        property alias showZoomLevel: root.showZoomLevel
    }
}
