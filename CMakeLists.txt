# Copyright (c) 2026 C. Klukas. All rights reserved.
cmake_minimum_required(VERSION 3.25)
project(ckmux VERSION 0.0.1 LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(CkmuxSettings)
include(GNUInstallDirs)

# ckVision is consumed as an installed package when one is available, and
# otherwise from a sibling source checkout. Both paths are first-class: the
# package path is what an ordinary adopter uses, and the source path is what
# co-development uses, because ckmux and ckVision are developed together and a
# library change must be buildable from ckmux in the same edit (plans/07).
find_package(ckvision CONFIG QUIET)
if(NOT ckvision_FOUND)
    set(CKMUX_CKVISION_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ckvision"
        CACHE PATH "ckVision source checkout used when no installed package is found")
    if(NOT EXISTS "${CKMUX_CKVISION_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "ckVision not found. Install it, or point CKMUX_CKVISION_SOURCE_DIR at a "
            "checkout (looked in ${CKMUX_CKVISION_SOURCE_DIR}).")
    endif()
    message(STATUS "ckmux: building ckVision from ${CKMUX_CKVISION_SOURCE_DIR}")
    # ckVision defaults its examples/tests to PROJECT_IS_TOP_LEVEL, so both
    # stay off here without being forced.
    add_subdirectory("${CKMUX_CKVISION_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/ckvision")
endif()

# The client object graph is a plain library so every part of it is reachable
# from headless tests; only the thin terminal host needs a real backend. This
# mirrors how ckVision's own examples are structured, for the same reason.
add_library(ckmux_client STATIC
    src/client/client_app.cpp
    src/client/commands.cpp
    src/client/prefix_overlay.cpp
    src/common/config.cpp
    src/common/shell.cpp
    src/platform/paths.cpp)
target_include_directories(ckmux_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ckmux_client PUBLIC ckvision::cvision)
ckmux_strict(ckmux_client)

if(UNIX)
    add_executable(ckmux src/main.cpp)
    target_link_libraries(ckmux PRIVATE ckmux_client ckvision::cvision)
    ckmux_strict(ckmux)
    install(TARGETS ckmux RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

option(CKMUX_BUILD_TESTING "Build ckmux tests" ${PROJECT_IS_TOP_LEVEL})
if(CKMUX_BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
