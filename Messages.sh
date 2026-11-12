#! /usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Karten contributors
# SPDX-License-Identifier: CC0-1.0
$XGETTEXT $(find src -name '*.cpp' -o -name '*.h' -o -name '*.qml') -o $podir/karten.pot
