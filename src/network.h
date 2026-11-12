// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QNetworkRequest>

class QNetworkAccessManager;
class QQmlNetworkAccessManagerFactory;

/// Shared network access for everything the application itself fetches.
namespace Network
{
QNetworkAccessManager *manager();

/// A request that identifies the application, as the usage policies of the
/// OpenStreetMap services require.
QNetworkRequest request(const QUrl &url);

/// Makes what the interface loads by itself, such as pictures, identify the
/// application in the same way. The caller owns the factory.
QQmlNetworkAccessManagerFactory *qmlFactory();
}
