# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
if(CPACK_GENERATOR STREQUAL "WIX" AND CMAKE_VERSION VERSION_LESS 4.3)
    message(FATAL_ERROR "The per-user ckmux MSI requires CMake 4.3 or newer")
endif()
