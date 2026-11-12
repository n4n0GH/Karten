// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "appearance.h"

#include <KColorSchemeManager>

#include <QModelIndex>

using namespace Qt::StringLiterals;

namespace
{
// The colour schemes that every Plasma installation has.
const QString LightId = u"BreezeLight"_s;
const QString DarkId = u"BreezeDark"_s;
}

Appearance::Appearance(QObject *parent)
    : QObject(parent)
{
}

Appearance::Scheme Appearance::scheme() const
{
    const QString active = KColorSchemeManager::instance()->activeSchemeId();
    return active == LightId ? Light : active == DarkId ? Dark : FollowSystem;
}

void Appearance::setScheme(Scheme scheme)
{
    if (scheme == this->scheme()) {
        return;
    }
    KColorSchemeManager *manager = KColorSchemeManager::instance();
    // An invalid index stands for the desktop's own colours.
    manager->activateScheme(scheme == FollowSystem ? QModelIndex() : manager->indexForSchemeId(scheme == Light ? LightId : DarkId));
    Q_EMIT schemeChanged();
}
