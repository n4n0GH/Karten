# SPDX-FileCopyrightText: 2026 Karten contributors
# SPDX-License-Identifier: BSD-2-Clause

# Routing core: no GUI, so that it can be tested and reused on its own.
add_library(kartencore STATIC
    core/geo.h
    core/graphbuilder.cpp
    core/graphbuilder.h
    core/pbfreader.cpp
    core/pbfreader.h
    core/roadgraph.cpp
    core/roadgraph.h
    core/router.cpp
    core/router.h
    core/vectortile.cpp
    core/vectortile.h
)
target_include_directories(kartencore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core)
target_link_libraries(kartencore PUBLIC Qt6::Core PRIVATE ZLIB::ZLIB)

# Developer tool: build routing data from a .osm.pbf file and plan a route.
add_executable(karten-route tools/karten-route.cpp)
target_link_libraries(karten-route PRIVATE kartencore)

add_executable(karten
    main.cpp
    appearance.cpp
    appearance.h
    desktopcolors.cpp
    desktopcolors.h
    extractimporter.cpp
    extractimporter.h
    geocoder.cpp
    geocoder.h
    keystore.cpp
    keystore.h
    network.cpp
    network.h
    offlinemanager.cpp
    offlinemanager.h
    routecontroller.cpp
    routecontroller.h
    tileserver.cpp
    tileserver.h
    tilestore.cpp
    tilestore.h
    transitplanner.cpp
    transitplanner.h
)

ecm_add_qml_module(karten
    URI io.github.n4n0gh.karten
    GENERATE_PLUGIN_SOURCE
    DEPENDENCIES
        QtQuick
        QtPositioning
)
# Pictograms that the icon themes do not have. In this place they are found
# by name like any other icon, and take on the colours of the colour scheme.
qt_add_resources(karten "icons"
    PREFIX "/icons/hicolor/scalable/actions"
    BASE icons
    FILES
        icons/transit-stop-symbolic.svg
        icons/travel-cycle-symbolic.svg
        icons/travel-drive-symbolic.svg
        icons/travel-transit-symbolic.svg
        icons/travel-walk-symbolic.svg
        icons/turn-left-symbolic.svg
        icons/turn-right-symbolic.svg
        icons/turn-roundabout-symbolic.svg
        icons/turn-sharp-left-symbolic.svg
        icons/turn-sharp-right-symbolic.svg
        icons/turn-slight-left-symbolic.svg
        icons/turn-slight-right-symbolic.svg
        icons/turn-straight-symbolic.svg
        icons/turn-uturn-symbolic.svg
)

set_source_files_properties(qml/Preferences.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
ecm_target_qml_sources(karten SOURCES
    qml/Main.qml
    qml/MapPage.qml
    qml/MapControls.qml
    qml/CardBackground.qml
    qml/PlacePanel.qml
    qml/SidePanel.qml
    qml/StopList.qml
    qml/ZoomControl.qml
    qml/SearchResults.qml
    qml/DirectionsView.qml
    qml/DownloadDialog.qml
    qml/OfflinePage.qml
    qml/Preferences.qml
    qml/SettingsPage.qml
)

target_include_directories(karten PRIVATE ${CMAKE_BINARY_DIR})
target_compile_definitions(karten PRIVATE
    TRANSLATION_DOMAIN="karten"
    KARTEN_MAPLIBRE_PLUGIN_PATH="${KARTEN_MAPLIBRE_PLUGIN_PATH}"
)
target_link_libraries(karten PRIVATE
    kartencore
    Qt6::Concurrent
    Qt6::DBus
    Qt6::Gui
    Qt6::Network
    Qt6::Positioning
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Sql
    Qt6::Widgets
    KF6::ColorScheme
    KF6::ConfigCore
    KF6::CoreAddons
    KF6::I18n
    KF6::I18nLocaleData
    KF6::I18nQml
    KF6::Wallet
    KOpeningHours
    KPublicTransport
)

install(TARGETS karten ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
