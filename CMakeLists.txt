# SPDX-FileCopyrightText: 2026 Karten contributors
# SPDX-License-Identifier: BSD-2-Clause

cmake_minimum_required(VERSION 3.20)

project(karten VERSION 0.1.1 LANGUAGES CXX)

set(QT_MIN_VERSION "6.6.0")
set(KF_MIN_VERSION "6.8.0")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ECM ${KF_MIN_VERSION} REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(ECMAddTests)
include(ECMFindQmlModule)
include(ECMQmlModule)
include(ECMSetupVersion)
include(FeatureSummary)
include(KDEClangFormat)
include(KDEGitCommitHooks)

ecm_setup_version(${PROJECT_VERSION}
    VARIABLE_PREFIX KARTEN
    VERSION_HEADER ${CMAKE_CURRENT_BINARY_DIR}/karten-version.h
)

find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS
    Concurrent
    Core
    Gui
    Network
    Positioning
    Qml
    Quick
    QuickControls2
    Sql
    Widgets
)
find_package(KF6 ${KF_MIN_VERSION} REQUIRED COMPONENTS
    ColorScheme
    Config
    CoreAddons
    I18n
    Kirigami
    Wallet
)

# QML modules that are only needed at run time.
find_package(ZLIB REQUIRED)
# Understands OpenStreetMap's notation for opening hours.
find_package(KOpeningHours REQUIRED)

# Draws the map. Only its Qt Location plugin is used, at run time; see
# scripts/build-maplibre.sh where the distribution does not package it.
find_package(QMapLibre 3.0 REQUIRED COMPONENTS Location)
get_target_property(_maplibre_plugin QMapLibre::PluginGeoServices LOCATION)
get_filename_component(_maplibre_plugin_dir "${_maplibre_plugin}" DIRECTORY)
get_filename_component(KARTEN_MAPLIBRE_PLUGIN_PATH "${_maplibre_plugin_dir}" DIRECTORY)

ecm_find_qmlmodule(QtLocation REQUIRED)
ecm_find_qmlmodule(QtPositioning REQUIRED)
ecm_find_qmlmodule(org.kde.kirigami REQUIRED)
ecm_find_qmlmodule(org.kde.coreaddons REQUIRED)

add_subdirectory(src)

if(BUILD_TESTING)
    find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Test)
    add_subdirectory(autotests)
endif()

install(PROGRAMS io.github.n4n0gh.karten.desktop DESTINATION ${KDE_INSTALL_APPDIR})
install(FILES io.github.n4n0gh.karten.metainfo.xml DESTINATION ${KDE_INSTALL_METAINFODIR})
install(FILES icons/io.github.n4n0gh.karten.svg DESTINATION ${KDE_INSTALL_FULL_ICONDIR}/hicolor/scalable/apps)

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/po)
    ki18n_install(po)
endif()

feature_summary(WHAT ALL INCLUDE_QUIET_PACKAGES FATAL_ON_MISSING_REQUIRED_PACKAGES)

file(GLOB_RECURSE ALL_CLANG_FORMAT_SOURCE_FILES src/*.cpp src/*.h autotests/*.cpp)
kde_clang_format(${ALL_CLANG_FORMAT_SOURCE_FILES})
kde_configure_git_pre_commit_hook(CHECKS CLANG_FORMAT)
