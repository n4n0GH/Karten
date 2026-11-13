// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "appearance.h"
#include "desktopcolors.h"
#include "geocoder.h"
#include "karten-version.h"
#include "network.h"
#include "offlinemanager.h"
#include "routecontroller.h"
#include "tileserver.h"
#include "tilestore.h"

#include <KAboutData>
#include <KColorSchemeManager>
#include <KLocalizedQmlContext>
#include <KLocalizedString>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlNetworkAccessManagerFactory>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>

using namespace Qt::StringLiterals;

/// In a Flatpak, the settings of Plasma's global theme (the icon theme, say)
/// as on the host: they are in "kdedefaults" in the configuration folder,
/// which Plasma lists in XDG_CONFIG_DIRS. Flatpak sets that variable anew, so
/// KDE's libraries see only "kdeglobals", and with no icon theme named there
/// they draw Breeze's. (The manifest lets the folder be read.)
static void plasmaDefaultsInFlatpak()
{
    if (!QFile::exists(u"/.flatpak-info"_s)) {
        return;
    }
    // Flatpak says where the host has its configuration; the sandbox's own is elsewhere.
    QString config = qEnvironmentVariable("HOST_XDG_CONFIG_HOME");
    if (config.isEmpty()) {
        config = QDir::homePath() + u"/.config"_s;
    }
    const QString defaults = config + u"/kdedefaults"_s;
    if (!QDir(defaults).exists()) {
        return;
    }
    QStringList dirs = qEnvironmentVariable("XDG_CONFIG_DIRS").split(u':', Qt::SkipEmptyParts);
    if (dirs.isEmpty()) {
        dirs.append(u"/etc/xdg"_s);
    }
    dirs.removeAll(defaults);
    dirs.prepend(defaults);
    qputenv("XDG_CONFIG_DIRS", dirs.join(u':').toLocal8Bit());
}

int main(int argc, char **argv)
{
    plasmaDefaultsInFlatpak();

    // MapLibre draws with OpenGL, and its plugin asks for rendering on the
    // main thread rather than on Qt's separate render thread.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    if (qEnvironmentVariableIsEmpty("QSG_RENDER_LOOP")) {
        qputenv("QSG_RENDER_LOOP", "basic");
    }

    // QApplication rather than QGuiApplication: the desktop style draws with widgets.
    QApplication app(argc, argv);
    // Where MapLibre's map plugin was found when Karten was built. This only
    // matters when it is not installed alongside Qt's own plugins.
    if (QDir(QStringLiteral(KARTEN_MAPLIBRE_PLUGIN_PATH "/geoservices")).exists()) {
        QCoreApplication::addLibraryPath(QStringLiteral(KARTEN_MAPLIBRE_PLUGIN_PATH));
    }
    KLocalizedString::setApplicationDomain("karten");

    KAboutData about(u"karten"_s,
                     i18nc("@title", "Karten"),
                     QStringLiteral(KARTEN_VERSION_STRING),
                     i18nc("@info", "Maps and route planning that also work offline"),
                     KAboutLicense::GPL_V3,
                     i18nc("@info:credit", "© 2026 Karten contributors"));
    about.setLicense(KAboutLicense::GPL_V3, KAboutLicense::OrLaterVersions);
    about.setDesktopFileName(u"io.github.n4n0gh.karten"_s);
    about.setHomepage(u"https://github.com/n4n0gh/karten"_s);
    about.setBugAddress("https://github.com/n4n0gh/karten/issues");
    about.setOtherText(i18nc("@info",
                             "Map data © OpenStreetMap contributors, available under the Open Database License. "
                             "The map is provided by OpenFreeMap and drawn by MapLibre. "
                             "Offline routing data is built from extracts provided by Geofabrik."));
    KAboutData::setApplicationData(about);
    QGuiApplication::setWindowIcon(QIcon::fromTheme(u"io.github.n4n0gh.karten"_s, QIcon::fromTheme(u"map-globe"_s)));

    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) {
        QQuickStyle::setStyle(u"org.kde.desktop"_s);
    }

    // Applies the light or dark colours chosen in the settings, if any.
    KColorSchemeManager::instance();
    // In a Flatpak, the colours of the desktop as they change.
    DesktopColors desktopColors(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
    desktopColors.setFollowed(Appearance().scheme() == Appearance::FollowSystem);

    const QString dataDirectory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDirectory);
    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));

    TileStore store;
    if (!store.open(dataDirectory + u"/tiles.sqlite"_s)) {
        qCritical("Cannot open the tile database: %s", qPrintable(store.errorString()));
        return 1;
    }
    store.pruneCache(10000); // roughly 400 MB of map that was only looked at

    TileServer server(&store);
    if (!server.listen()) {
        qCritical("Cannot start the local tile server");
        return 1;
    }
    OfflineManager offline(&store, &server);
    Geocoder geocoder(&offline);
    RouteController routes(&offline);

    QQmlApplicationEngine engine;
    const std::unique_ptr<QQmlNetworkAccessManagerFactory> networkFactory(Network::qmlFactory());
    engine.setNetworkAccessManagerFactory(networkFactory.get());
    KLocalization::setupLocalizedContext(&engine);
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        [] {
            QCoreApplication::exit(1);
        },
        Qt::QueuedConnection);
    engine.loadFromModule("io.github.n4n0gh.karten", "Main");

    return app.exec();
}
