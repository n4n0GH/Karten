// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "geo.h"

#include <QString>

#include <optional>

/// A "geo:" link, by which one program asks another to show a place: an
/// address book the home of a contact, say.
///
/// RFC 5870 defines it as "geo:latitude,longitude", optionally followed by an
/// altitude and parameters. Two additions that Android made are in wide use
/// and understood too: "?z=" with a zoom level, and "?q=" with either a
/// position and its name, "52.5,13.4(Name)", or words to search for.
struct GeoUri {
    /// Where the place is; with a query, where to look for it. A link that
    /// only searches has 0, 0 here by convention.
    GeoPoint position;
    /// The zoom level asked for, of maps in Web Mercator; 0 for none.
    double zoom = 0;
    /// What the place is called, if the link says.
    QString label;
    /// What to search for. Empty when the link names a position.
    QString query;

    /// Whether there is a position to go by, rather than the 0, 0 of a link
    /// that only searches.
    bool hasPosition() const
    {
        return position.lat != 0 || position.lon != 0;
    }

    /// Reads a link; nothing when it is not a geo: link, or one of a place
    /// that is not on this Earth.
    static std::optional<GeoUri> parse(const QString &uri);
};
