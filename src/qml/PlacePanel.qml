// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

/// What is known about the selected place: a point of interest that was
/// clicked on the map, a search result or a dropped pin.
QQC2.Control {
    id: root

    /// A map with the keys title, subtitle, latitude and longitude.
    required property var place
    /// Further lines to show: see Geocoder.detailsFinished().
    required property list<var> details
    /// An encyclopedia article about the place, or null: a map with the keys
    /// text, url, image and imagePage.
    required property var article
    /// Google's rating and reviews of the place, or null: see
    /// Geocoder.reviewsFinished().
    required property var reviews

    /// The place on Google Maps: Google's own address for it when known,
    /// or else a search for its name at its position.
    readonly property string googleMapsUrl: {
        if (reviews?.url) {
            return reviews.url;
        }
        if (!place) {
            return "";
        }
        const position = place.latitude.toFixed(6) + "," + place.longitude.toFixed(6);
        return "https://www.google.com/maps/search/" + encodeURIComponent(place.title) + "/@" + position + ",17z";
    }
    function stars(rating: real): string {
        const full = Math.max(0, Math.min(5, Math.round(rating)));
        return "★".repeat(full) + "☆".repeat(5 - full);
    }
    property real maximumHeight: Infinity

    signal directionsRequested
    signal closed

    height: Math.min(implicitHeight, maximumHeight)
    padding: Kirigami.Units.largeSpacing

    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false

    background: CardBackground {}

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        RowLayout {
            spacing: Kirigami.Units.smallSpacing

            Kirigami.Heading {
                Layout.fillWidth: true
                level: 3
                text: root.place?.title ?? ""
                wrapMode: Text.Wrap
            }
            QQC2.ToolButton {
                Layout.alignment: Qt.AlignTop
                text: i18nc("@action:button", "Close")
                icon.name: "window-close-symbolic"
                display: QQC2.AbstractButton.IconOnly
                onClicked: root.closed()
                QQC2.ToolTip.text: text
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            }
        }
        QQC2.Label {
            Layout.fillWidth: true
            text: root.place?.subtitle ?? ""
            wrapMode: Text.Wrap
            color: Kirigami.Theme.disabledTextColor
            visible: text.length > 0
        }
        RowLayout {
            Layout.topMargin: Kirigami.Units.smallSpacing
            Layout.bottomMargin: Kirigami.Units.smallSpacing
            spacing: Kirigami.Units.smallSpacing

            QQC2.Button {
                text: i18nc("@action:button", "Directions")
                icon.name: "routeplanning-symbolic"
                onClicked: root.directionsRequested()
            }
            QQC2.Button {
                text: i18nc("@action:button opens the place in the web browser", "Google Maps")
                icon.name: "internet-services-symbolic"
                onClicked: Qt.openUrlExternally(root.googleMapsUrl)
                QQC2.ToolTip.text: i18nc("@info:tooltip", "Look this place up on Google Maps in the web browser")
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            }
        }

        // Everything that was looked up; it scrolls when the window is small.
        QQC2.ScrollView {
            id: scroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.details.length > 0 || root.article !== null || root.reviews !== null
            contentWidth: availableWidth
            QQC2.ScrollBar.horizontal.policy: QQC2.ScrollBar.AlwaysOff

            ColumnLayout {
                width: scroll.availableWidth
                spacing: Kirigami.Units.smallSpacing

                // --- Article ---
                Image {
                    id: picture
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.round(width * 9 / 16)
                    visible: source.toString().length > 0 && status === Image.Ready
                    source: root.article?.image ?? ""
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    // No larger than it can be shown.
                    sourceSize.width: 640
                    Accessible.name: i18nc("@info accessible name of a picture; %1 is a place", "Picture of %1", root.place?.title ?? "")

                    TapHandler {
                        cursorShape: Qt.PointingHandCursor
                        onTapped: Qt.openUrlExternally(root.article?.imagePage ?? "")
                    }
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    visible: root.article !== null
                    text: root.article?.text ?? ""
                    wrapMode: Text.Wrap
                }
                // The text is under a licence that asks for its source and
                // the licence to be named; the picture's page names its own.
                QQC2.Label {
                    Layout.fillWidth: true
                    visible: root.article !== null
                    textFormat: Text.StyledText
                    wrapMode: Text.Wrap
                    font: Kirigami.Theme.smallFont
                    color: Kirigami.Theme.disabledTextColor
                    text: {
                        const link = (url, label) => "<a href=\"" + url + "\">" + label + "</a>";
                        const source = i18nc("@info credit; %1 is a link titled Wikipedia, %2 one titled CC BY-SA", "From %1, %2",
                            link(root.article?.url ?? "", i18nc("@info link title", "Wikipedia")),
                            link("https://creativecommons.org/licenses/by-sa/4.0/", i18nc("@info link title, name of a licence", "CC BY-SA")));
                        if (!picture.visible) {
                            return source;
                        }
                        return i18nc("@info credit; %1 is the credit for the text, %2 a link titled Picture credits", "%1 · %2", source,
                            link(root.article?.imagePage ?? "", i18nc("@info link title", "Picture credits")));
                    }
                    onLinkActivated: link => Qt.openUrlExternally(link)
                    HoverHandler {
                        cursorShape: parent.hoveredLink ? Qt.PointingHandCursor : Qt.ArrowCursor
                    }
                }

                // --- Details ---
                Repeater {
                    model: root.details
                    RowLayout {
                        id: line
                        required property var modelData
                        Layout.topMargin: Kirigami.Units.smallSpacing
                        spacing: Kirigami.Units.largeSpacing

                        Kirigami.Icon {
                            Layout.alignment: Qt.AlignTop
                            implicitWidth: Kirigami.Units.iconSizes.small
                            implicitHeight: Kirigami.Units.iconSizes.small
                            source: line.modelData.icon
                        }
                        Kirigami.UrlButton {
                            Layout.fillWidth: true
                            visible: line.modelData.link.length > 0
                            text: line.modelData.text
                            url: line.modelData.link
                            horizontalAlignment: Text.AlignLeft
                            wrapMode: Text.Wrap
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            visible: line.modelData.link.length === 0
                            spacing: 0

                            Kirigami.SelectableLabel {
                                Layout.fillWidth: true
                                text: line.modelData.text
                                wrapMode: Text.Wrap
                                color: line.modelData.state === "open" ? Kirigami.Theme.positiveTextColor
                                    : line.modelData.state === "closed" ? Kirigami.Theme.negativeTextColor
                                    : Kirigami.Theme.textColor
                            }
                            QQC2.Label {
                                Layout.fillWidth: true
                                visible: text.length > 0
                                text: line.modelData.note
                                wrapMode: Text.Wrap
                                color: Kirigami.Theme.disabledTextColor
                            }
                        }
                    }
                }

                // --- Rating and reviews from Google ---
                // Laid out as Google's terms for this content ask: its name
                // with the content, and each review with its author's
                // picture, name and a way to the review itself.
                Kirigami.Separator {
                    Layout.fillWidth: true
                    Layout.topMargin: Kirigami.Units.smallSpacing
                    visible: root.reviews !== null && (root.details.length > 0 || root.article !== null)
                }
                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: (root.reviews?.error ?? "").length > 0
                    type: Kirigami.MessageType.Warning
                    text: i18nc("@info %1 is an error message from Google", "Google did not provide reviews: %1", root.reviews?.error ?? "")
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: root.reviews !== null && root.reviews.error.length === 0
                    spacing: Kirigami.Units.smallSpacing

                    Kirigami.Heading {
                        level: 4
                        text: Number(root.reviews?.rating ?? 0).toLocaleString(Qt.locale(), "f", 1)
                    }
                    QQC2.Label {
                        text: root.stars(root.reviews?.rating ?? 0)
                        color: Kirigami.Theme.neutralTextColor
                        Accessible.ignored: true
                    }
                    QQC2.Label {
                        Layout.fillWidth: true
                        text: i18ncp("@info number of ratings", "%1 rating", "%1 ratings", root.reviews?.count ?? 0)
                        color: Kirigami.Theme.disabledTextColor
                        elide: Text.ElideRight
                    }
                    // The name of the source, as it is, in every language.
                    QQC2.Label {
                        text: "Google Maps"
                        font: Kirigami.Theme.smallFont
                    }
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    visible: (root.reviews?.reviews ?? []).length > 0
                    wrapMode: Text.Wrap
                    font: Kirigami.Theme.smallFont
                    color: Kirigami.Theme.disabledTextColor
                    text: i18nc("@info", "The reviews Google considers most relevant. Google does not verify reviews.")
                }
                Repeater {
                    model: root.reviews?.reviews ?? []

                    ColumnLayout {
                        id: review
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.topMargin: Kirigami.Units.largeSpacing
                        spacing: Kirigami.Units.smallSpacing

                        RowLayout {
                            spacing: Kirigami.Units.largeSpacing

                            Kirigami.ShadowedImage {
                                implicitWidth: Kirigami.Units.iconSizes.medium
                                implicitHeight: Kirigami.Units.iconSizes.medium
                                radius: width / 2
                                source: review.modelData.photo
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                                sourceSize.width: 96
                                sourceSize.height: 96
                                color: Kirigami.Theme.alternateBackgroundColor
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0

                                Kirigami.UrlButton {
                                    Layout.fillWidth: true
                                    visible: review.modelData.authorUrl.length > 0
                                    text: review.modelData.author
                                    url: review.modelData.authorUrl
                                    horizontalAlignment: Text.AlignLeft
                                    elide: Text.ElideRight
                                }
                                QQC2.Label {
                                    Layout.fillWidth: true
                                    visible: review.modelData.authorUrl.length === 0
                                    text: review.modelData.author
                                    elide: Text.ElideRight
                                }
                                RowLayout {
                                    spacing: Kirigami.Units.smallSpacing
                                    QQC2.Label {
                                        text: root.stars(review.modelData.rating)
                                        color: Kirigami.Theme.neutralTextColor
                                        font: Kirigami.Theme.smallFont
                                        Accessible.name: i18ncp("@info accessible", "%1 star", "%1 stars", Math.round(review.modelData.rating))
                                    }
                                    QQC2.Label {
                                        Layout.fillWidth: true
                                        text: review.modelData.time
                                        font: Kirigami.Theme.smallFont
                                        color: Kirigami.Theme.disabledTextColor
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                        }
                        QQC2.Label {
                            Layout.fillWidth: true
                            visible: text.length > 0
                            text: review.modelData.text
                            wrapMode: Text.Wrap
                            // Long ones are read in full where they came from.
                            maximumLineCount: 8
                            elide: Text.ElideRight
                        }
                        Kirigami.UrlButton {
                            visible: review.modelData.url.length > 0
                            text: i18nc("@action:button", "Read on Google Maps")
                            url: review.modelData.url
                            font: Kirigami.Theme.smallFont
                        }
                    }
                }
            }
        }
    }
}
