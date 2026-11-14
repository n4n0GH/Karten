// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QList>
#include <QPicture>
#include <QSizeF>
#include <QString>

class QPagedPaintDevice;

/// A route as a roadbook on paper, the way a co-driver reads one: the
/// instructions one below the other, each with how far it is from the start
/// and from the instruction before, and a small map of the junction that
/// points the way the car does. A map of the whole route goes before them.
///
/// This class only arranges and draws what it is given; Roadbook fills it.
/// The maps are pictures in millimetres, as PrintMap draws them.
class RoadbookDocument
{
public:
    /// The side of the map of a junction, in millimetres.
    static constexpr double MapSize = 40;

    /// One instruction.
    struct Row {
        QString total; ///< the distance from the start
        QString interval; ///< the distance from the instruction before
        QString text;
        QString follow; ///< what comes after it, such as how far to go on
        QPicture map; ///< MapSize in both directions
    };
    /// The way from one stop to the next.
    struct Section {
        QString title;
        QString detail;
        QList<Row> rows;
    };

    QString title;
    QString subtitle;
    /// The stops with the letters they have on the overview map.
    QString stops;
    QPicture overview;
    QSizeF overviewSize; ///< millimetres
    /// What the figures and the maps mean.
    QString legend;
    /// Whom the maps are owed to.
    QString attribution;
    QList<Section> sections;

    /// Draws the pages the device asks for (all, unless it names page
    /// ranges) and returns how many the roadbook has on its paper.
    int paint(QPagedPaintDevice *device) const;
};
