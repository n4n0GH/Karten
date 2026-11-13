// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "desktopcolors.h"

#include <QApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QPalette>
#include <QSaveFile>

using namespace Qt::StringLiterals;

namespace
{
const QString s_portal = u"org.freedesktop.portal.Desktop"_s;
const QString s_portalPath = u"/org/freedesktop/portal/desktop"_s;
const QString s_settings = u"org.freedesktop.portal.Settings"_s;
/// What Plasma's portal has "kdeglobals" under: a namespace for each group.
const QString s_namespace = u"org.kde.kdeglobals."_s;
const QString s_filePrefix = u"desktop-colors-"_s;
const QString s_fileSuffix = u".colors"_s;
DesktopColors *s_instance = nullptr;

/// The groups of "kdeglobals" a colour scheme is made of.
bool ofAScheme(const QString &group)
{
    return group.startsWith("Colors:"_L1) || group.startsWith("ColorEffects:"_L1) || group == "General"_L1 || group == "KDE"_L1 || group == "WM"_L1;
}

void groupText(QString &out, const QString &name, const QMap<QString, QString> &entries)
{
    out += u'[' + name + u"]\n"_s;
    for (auto entry = entries.cbegin(); entry != entries.cend(); ++entry) {
        // (As KConfig writes a line break in a value.)
        QString value = entry.value();
        value.replace(u'\\', u"\\\\"_s).replace(u'\n', u"\\n"_s);
        out += entry.key() + u'=' + value + u'\n';
    }
    out += u'\n';
}

/// KColorScheme, which Kirigami's style takes its colours from, reads the
/// scheme the application names in place of "kdeglobals". Plasma's Qt
/// integration has the new palette by then (from the settings portal), but
/// leaves the application's alone once a scheme is named: it is taken from
/// there, and the style is told to read its colours again.
void apply(const QString &path)
{
    qApp->setProperty("KDE_COLOR_SCHEME_PATH", path);
    const QPalette before = QApplication::palette();
    QApplication::setPalette(QPalette());
    if (QApplication::palette() == before) {
        QEvent change(QEvent::ApplicationPaletteChange);
        QCoreApplication::sendEvent(qApp, &change);
    }
}
}

DesktopColors::DesktopColors(const QString &directory, QObject *parent)
    : QObject(parent)
    , m_directory(directory)
{
    s_instance = this;
    if (!QFile::exists(u"/.flatpak-info"_s)) {
        return;
    }
    // What an earlier Karten left behind.
    QDir dir(m_directory);
    const QStringList old = dir.entryList({s_filePrefix + u'*' + s_fileSuffix}, QDir::Files);
    for (const QString &name : old) {
        dir.remove(name);
    }
    QDBusConnection::sessionBus().connect(s_portal, s_portalPath, s_settings, u"SettingChanged"_s, this, SLOT(settingChanged(QString, QString, QDBusVariant)));
}

DesktopColors::~DesktopColors()
{
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

DesktopColors *DesktopColors::instance()
{
    return s_instance;
}

void DesktopColors::setFollowed(bool followed)
{
    if (m_followed == followed) {
        return;
    }
    m_followed = followed;
    if (m_followed && !m_written.isEmpty()) {
        apply(m_written.constLast());
    }
}

QString DesktopColors::schemeText(const Groups &groups)
{
    QString out;
    for (auto group = groups.cbegin(); group != groups.cend(); ++group) {
        if (ofAScheme(group.key()) && !group.value().isEmpty()) {
            groupText(out, group.key(), group.value());
        }
    }
    const auto header = groups.constFind(u"Colors:Header"_s);
    const auto wm = groups.constFind(u"WM"_s);
    if (header != groups.cend() && wm != groups.cend() && wm->contains(u"inactiveBackground"_s) && wm->contains(u"inactiveForeground"_s)) {
        QMap<QString, QString> inactive = *header;
        if (header->contains(u"BackgroundNormal"_s)) {
            inactive.insert(u"BackgroundAlternate"_s, header->value(u"BackgroundNormal"_s));
        }
        inactive.insert(u"BackgroundNormal"_s, wm->value(u"inactiveBackground"_s));
        inactive.insert(u"ForegroundNormal"_s, wm->value(u"inactiveForeground"_s));
        groupText(out, u"Colors:Header][Inactive"_s, inactive);
    }
    return out;
}

void DesktopColors::settingChanged(const QString &group, const QString &key, const QDBusVariant &)
{
    // Plasma's portal says so for every change of the colours, whatever changed them.
    if (group != s_namespace + u"General"_s || key != "ColorScheme"_L1) {
        return;
    }
    QDBusMessage call = QDBusMessage::createMethodCall(s_portal, s_portalPath, s_settings, u"ReadAll"_s);
    call << QStringList{s_namespace + u'*'};
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *finished) {
        finished->deleteLater();
        const QDBusMessage reply = finished->reply();
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
            qWarning("The desktop's changed colours: %s", qPrintable(reply.errorMessage()));
            return;
        }
        QMap<QString, QVariantMap> all;
        reply.arguments().constFirst().value<QDBusArgument>() >> all;
        Groups groups;
        for (auto entries = all.cbegin(); entries != all.cend(); ++entries) {
            if (!entries.key().startsWith(s_namespace)) {
                continue;
            }
            QMap<QString, QString> &into = groups[entries.key().mid(s_namespace.size())];
            for (auto entry = entries->cbegin(); entry != entries->cend(); ++entry) {
                if (entry->typeId() == QMetaType::QString) {
                    into.insert(entry.key(), entry->toString());
                }
            }
        }
        write(groups);
    });
}

void DesktopColors::write(const Groups &groups)
{
    if (!groups.contains(u"Colors:View"_s)) {
        qWarning("The desktop's changed colours: the settings portal has none");
        return;
    }
    // Under a name not used before: KDE's libraries keep a file they have
    // read by its name.
    QDir().mkpath(m_directory);
    const QString path = u"%1/%2%3-%4%5"_s.arg(m_directory, s_filePrefix).arg(QCoreApplication::applicationPid()).arg(++m_serial).arg(s_fileSuffix);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(schemeText(groups).toUtf8()) < 0 || !file.commit()) {
        qWarning("The desktop's changed colours: %s", qPrintable(file.errorString()));
        return;
    }
    if (m_followed) {
        apply(path);
    }
    // The one before stays until the next change, for what still reads it.
    m_written.append(path);
    if (m_written.size() > 2) {
        QFile::remove(m_written.takeFirst());
    }
}
