// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "desktopcolors.h"

#include <QTest>

using namespace Qt::StringLiterals;

/// The colour scheme file made of what the settings portal has.
class DesktopColorsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void aSchemeHasTheColorGroupsOnly()
    {
        DesktopColors::Groups groups;
        groups[u"Colors:View"_s] = {{u"BackgroundNormal"_s, u"27,30,32"_s}};
        groups[u"WM"_s] = {{u"activeBackground"_s, u"39,44,49"_s}};
        groups[u"KFileDialog Settings"_s] = {{u"Recent Files"_s, u"a"_s}};
        groups[u"ColorEffects:Inactive"_s] = {};
        QCOMPARE(DesktopColors::schemeText(groups), u"[Colors:View]\nBackgroundNormal=27,30,32\n\n[WM]\nactiveBackground=39,44,49\n\n"_s);
    }

    void theInactiveTitlebarIsPutBackFromTheWmGroup()
    {
        DesktopColors::Groups groups;
        groups[u"Colors:Header"_s] = {{u"BackgroundNormal"_s, u"1,1,1"_s}, {u"ForegroundNormal"_s, u"2,2,2"_s}, {u"ForegroundLink"_s, u"3,3,3"_s}};
        groups[u"WM"_s] = {{u"inactiveBackground"_s, u"4,4,4"_s}, {u"inactiveForeground"_s, u"5,5,5"_s}};
        const QString text = DesktopColors::schemeText(groups);
        QVERIFY2(
            text.endsWith(u"[Colors:Header][Inactive]\nBackgroundAlternate=1,1,1\nBackgroundNormal=4,4,4\nForegroundLink=3,3,3\nForegroundNormal=5,5,5\n\n"_s),
            qPrintable(text));
    }

    void aLineBreakInAValueIsWrittenAsKConfigDoes()
    {
        DesktopColors::Groups groups;
        groups[u"General"_s] = {{u"Name"_s, u"a\\b\nc"_s}};
        QCOMPARE(DesktopColors::schemeText(groups), u"[General]\nName=a\\\\b\\nc\n\n"_s);
    }
};

QTEST_GUILESS_MAIN(DesktopColorsTest)

#include "desktopcolorstest.moc"
