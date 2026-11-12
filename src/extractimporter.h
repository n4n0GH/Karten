// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

#include <functional>

/// Converts a downloaded OpenStreetMap extract into the files Karten uses
/// offline: a road graph for routing and a small database of place names.
namespace ExtractImporter
{
struct Result {
    QString error; ///< empty on success
    double south = 0;
    double west = 0;
    double north = 0;
    double east = 0;
};

QString graphPath(const QString &directory);
QString placesPath(const QString &directory);

/// Blocking; meant to run on a worker thread. @p progress receives a
/// percentage and returns false to cancel.
Result run(const QString &pbfPath, const QString &directory, const std::function<bool(int)> &progress);
}
