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
