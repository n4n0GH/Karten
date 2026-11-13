// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDBusVariant>
#include <QMap>
#include <QObject>
#include <QStringList>

/// The desktop's colours inside a Flatpak, when they change while Karten
/// runs (a switch between a light and a dark theme, say).
///
/// The sandbox has "kdeglobals" as a single file, which stays the file it
/// was at the start: Plasma writes a new one in its place, and the sandbox
/// keeps the old. Plasma's Qt integration knows, and takes a changed palette
/// from the settings portal; Kirigami's style reads its colours from the file
/// (by KColorScheme), and so stayed with the old ones.
///
/// So on a change, the colours the portal has are written as a colour scheme
/// file, and the application is given it: KColorScheme reads that one in
/// place of "kdeglobals". Outside a Flatpak nothing of this is needed, and
/// nothing is done.
///
/// The setting for light or dark colours (Appearance) names a scheme in the
/// same way. While one of those is chosen the desktop's colours are only
/// kept, and given to the application once it follows the desktop again:
/// KColorSchemeManager would go back to the "kdeglobals" of the start.
class DesktopColors : public QObject
{
    Q_OBJECT

public:
    /// The groups of "kdeglobals" with their entries, as the portal has them.
    using Groups = QMap<QString, QMap<QString, QString>>;

    /// Watches the colours from now on, in a Flatpak. @p directory is where
    /// the scheme files go.
    explicit DesktopColors(const QString &directory, QObject *parent = nullptr);
    ~DesktopColors() override;

    /// The one there is, if any.
    static DesktopColors *instance();

    /// Whether the application has the desktop's colours, rather than a
    /// scheme of its own. To be called after KColorSchemeManager was told.
    void setFollowed(bool followed);

    /// The text of a colour scheme file with the groups the portal gave.
    ///
    /// The portal has no groups within groups, so not the titlebar's colours
    /// in a window that isn't active ("[Colors:Header][Inactive]"): those are
    /// put back from the "WM" group, which has the two that tell such a
    /// titlebar apart.
    static QString schemeText(const Groups &groups);

private Q_SLOTS:
    void settingChanged(const QString &group, const QString &key, const QDBusVariant &value);

private:
    void write(const Groups &groups);

    const QString m_directory;
    bool m_followed = true;
    int m_serial = 0;
    QStringList m_written;
};
