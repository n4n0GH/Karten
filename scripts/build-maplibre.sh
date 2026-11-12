#!/bin/sh
# SPDX-FileCopyrightText: 2026 Karten contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds MapLibre Native Qt, which draws Karten's map, for distributions that
# do not package it. Needs the Qt Location private headers
# (openSUSE: qt6-location-private-devel).
#
#   scripts/build-maplibre.sh [PREFIX]
#
# Afterwards configure Karten with -DCMAKE_PREFIX_PATH=PREFIX.

set -eu

version=v3.0.0
prefix=$(realpath -m "${1:-$PWD/../Karten-deps/prefix}")
work=$(dirname "$prefix")
source=$work/maplibre-native-qt
mkdir -p "$work"

if [ ! -d "$source" ]; then
    git clone --depth 1 --branch "$version" --recurse-submodules --shallow-submodules \
        https://github.com/maplibre/maplibre-native-qt.git "$source"
fi

# Since Qt 6.9 the private modules have to be asked for by name.
sed -i 's/COMPONENTS Location REQUIRED/COMPONENTS Location LocationPrivate REQUIRED/' "$source/CMakeLists.txt"

# - MapLibre treats warnings as errors, and current compilers have new ones.
# - Some of its files rely on <cstdint> being included by others.
# - The plugins must find the libraries next to them in the prefix.
cmake -S "$source" -B "$work/build-maplibre" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_INSTALL_RPATH='$ORIGIN/../../lib64;$ORIGIN/../../lib;$ORIGIN' \
    -DCMAKE_CXX_FLAGS="-include cstdint" \
    -DMLN_WITH_WERROR=OFF \
    -DMLN_QT_WITH_INTERNAL_ICU=ON
cmake --build "$work/build-maplibre"
cmake --install "$work/build-maplibre"

echo "MapLibre Native Qt $version is installed in $prefix"
