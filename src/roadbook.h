// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "printmap.h"
#include "roadbookdocument.h"
#include "routecontroller.h"

#include <QObject>
#include <QQmlEngine>
#include <QWindow>

#include <functional>
#include <memory>

class TileServer;

/// Prints the planned route as a roadbook: see RoadbookDocument for what
/// that looks like.
///
/// Printing takes three steps. The tiles the maps are drawn from are asked
/// of the TileServer, which has them on disk or fetches them. The maps are
/// then drawn on a worker thread, as pictures that can be played back at
/// any size. Last, a print preview opens, from which the roadbook is
/// printed or saved as a PDF file; for each sheet of paper it shows, the
/// pictures only have to be arranged.
class Roadbook : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    /// Whether there is a route to print: one by road.
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    /// Whether a roadbook is being prepared.
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

public:
    Roadbook(RouteController *routes, TileServer *server, QObject *parent = nullptr);
    static Roadbook *create(QQmlEngine *, QJSEngine *);

    bool available() const;
    bool busy() const;

    /// Prepares the roadbook of the current route and shows its print
    /// preview, as a dialog of @p window.
    Q_INVOKABLE void print(QWindow *window);

    /// Prepares the roadbook of the current route and hands it over; an
    /// empty one when there is no route. @p mapsMissing says whether some
    /// of the map could not be had.
    void prepare(const std::function<void(const RoadbookDocument &, bool mapsMissing)> &done);

Q_SIGNALS:
    void availableChanged();
    void busyChanged();
    /// Something for the user to know, such as that the maps are incomplete.
    void notice(const QString &message);

private:
    /// One map to draw.
    struct Job {
        PrintMap::View view;
        PrintMap::Style style = PrintMap::Junction;
        PrintMap::Overlay overlay;
    };
    struct Preparation;
    /// Asks for the tiles that are still to be had, a few at a time, and
    /// goes on to draw() once all have been answered.
    void fetch(const std::shared_ptr<Preparation> &preparation);
    void draw(const std::shared_ptr<Preparation> &preparation);
    void setBusy(bool busy);

    RouteController *const m_routes;
    TileServer *const m_server;
    bool m_busy = false;
};
