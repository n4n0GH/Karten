// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QQmlEngine>

/// Whether the application follows the colours of the desktop or is always
/// light or dark. The choice is remembered by KColorSchemeManager.
class Appearance : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(Scheme scheme READ scheme WRITE setScheme NOTIFY schemeChanged)

public:
    enum Scheme {
        FollowSystem,
        Light,
        Dark,
    };
    Q_ENUM(Scheme)

    explicit Appearance(QObject *parent = nullptr);

    Scheme scheme() const;
    void setScheme(Scheme scheme);

Q_SIGNALS:
    void schemeChanged();
};
