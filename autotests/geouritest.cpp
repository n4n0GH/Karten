// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "geouri.h"

#include <QTest>

using namespace Qt::StringLiterals;

class GeoUriTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void readsAPosition()
    {
        const auto uri = GeoUri::parse(u"geo:52.5163,13.3777"_s);
        QVERIFY(uri);
        QCOMPARE(uri->position.lat, 52.5163);
        QCOMPARE(uri->position.lon, 13.3777);
        QVERIFY(uri->hasPosition());
        QCOMPARE(uri->zoom, 0.0);
        QVERIFY(uri->label.isEmpty());
        QVERIFY(uri->query.isEmpty());
    }

    void readsWhatTheRfcAllows()
    {
        // An altitude, parameters, and capitals where they do not matter.
        const auto uri = GeoUri::parse(u" GEO:-33.8568,151.2153,12;CRS=WGS84;u=35 "_s);
        QVERIFY(uri);
        QCOMPARE(uri->position.lat, -33.8568);
        QCOMPARE(uri->position.lon, 151.2153);
    }

    void readsTheZoomLevel()
    {
        const auto uri = GeoUri::parse(u"geo:48.137,11.575?z=12"_s);
        QVERIFY(uri);
        QCOMPARE(uri->zoom, 12.0);
        // One no map has is none.
        QCOMPARE(GeoUri::parse(u"geo:48.137,11.575?z=99"_s)->zoom, 0.0);
        QCOMPARE(GeoUri::parse(u"geo:48.137,11.575?z=near"_s)->zoom, 0.0);
    }

    void readsANamedPosition()
    {
        // The position in the query is the one that counts.
        const auto uri = GeoUri::parse(u"geo:0,0?q=47.9959,7.8522(Freiburger+M%C3%BCnster)&z=17"_s);
        QVERIFY(uri);
        QCOMPARE(uri->position.lat, 47.9959);
        QCOMPARE(uri->position.lon, 7.8522);
        QCOMPARE(uri->label, u"Freiburger Münster"_s);
        QCOMPARE(uri->zoom, 17.0);
        QVERIFY(uri->query.isEmpty());

        const auto unnamed = GeoUri::parse(u"geo:0,0?q=47.9959,%207.8522"_s);
        QVERIFY(unnamed);
        QCOMPARE(unnamed->position.lon, 7.8522);
        QVERIFY(unnamed->label.isEmpty());
        QVERIFY(unnamed->query.isEmpty());
    }

    void readsASearch()
    {
        const auto uri = GeoUri::parse(u"geo:0,0?q=Unter%20den%20Linden+1,+Berlin"_s);
        QVERIFY(uri);
        QCOMPARE(uri->query, u"Unter den Linden 1, Berlin"_s);
        QVERIFY(!uri->hasPosition());

        // Around a position.
        const auto near = GeoUri::parse(u"geo:53.55,9.99?q=Bäckerei"_s);
        QVERIFY(near);
        QCOMPARE(near->query, u"Bäckerei"_s);
        QVERIFY(near->hasPosition());
        QCOMPARE(near->position.lat, 53.55);

        // Numbers that are no position are words like others.
        QCOMPARE(GeoUri::parse(u"geo:0,0?q=120,500"_s)->query, u"120,500"_s);
    }

    void refusesWhatIsNoPlace_data()
    {
        QTest::addColumn<QString>("uri");
        QTest::newRow("empty") << QString();
        QTest::newRow("another scheme") << u"https://example.org/52.5,13.4"_s;
        QTest::newRow("no scheme") << u"52.5,13.4"_s;
        QTest::newRow("nothing") << u"geo:"_s;
        QTest::newRow("one number") << u"geo:52.5"_s;
        QTest::newRow("four numbers") << u"geo:52.5,13.4,30,7"_s;
        QTest::newRow("words") << u"geo:north,east"_s;
        QTest::newRow("not a number") << u"geo:nan,13.4"_s;
        QTest::newRow("beyond the pole") << u"geo:90.5,13.4"_s;
        QTest::newRow("beyond the date line") << u"geo:52.5,180.5"_s;
        QTest::newRow("on the Moon") << u"geo:0.67,23.47;crs=moon-2011"_s;
    }
    void refusesWhatIsNoPlace()
    {
        QFETCH(QString, uri);
        QVERIFY(!GeoUri::parse(uri));
    }
};

QTEST_GUILESS_MAIN(GeoUriTest)

#include "geouritest.moc"
