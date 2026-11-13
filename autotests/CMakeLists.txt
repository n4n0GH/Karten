# SPDX-FileCopyrightText: 2026 Karten contributors
# SPDX-License-Identifier: BSD-2-Clause

ecm_add_test(vectortiletest.cpp
    TEST_NAME vectortiletest
    LINK_LIBRARIES kartencore Qt6::Test
)

ecm_add_test(routertest.cpp
    TEST_NAME routertest
    LINK_LIBRARIES kartencore Qt6::Test
)

# The colours of the desktop in a Flatpak: the scheme file made of the portal's.
ecm_add_test(desktopcolorstest.cpp
        ${CMAKE_SOURCE_DIR}/src/desktopcolors.cpp
    TEST_NAME desktopcolorstest
    LINK_LIBRARIES Qt6::Test Qt6::DBus Qt6::Widgets
)
target_include_directories(desktopcolorstest PRIVATE ${CMAKE_SOURCE_DIR}/src)
