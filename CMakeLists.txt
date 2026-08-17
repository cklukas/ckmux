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

# What both ends are made of: the wire format, the configuration, the diff
# algebra, and the platform calls. Its own library because the client and the
# server are separate libraries that both need it, and a shared object linked
# into both would be the same symbols twice in the one binary that links both.
#
# Nothing here may touch ckVision's `ui`, `widgets` or `scene`, for the same
# reason the server may not: this is what the server is built out of.
add_library(ckmux_common STATIC
    src/common/config.cpp
    src/common/grid_delta.cpp
    src/common/keymap.cpp
    src/common/proto.cpp
    src/common/shell.cpp
    src/platform/clipboard.cpp
    src/platform/paths.cpp
    src/platform/poller.cpp
    src/platform/process.cpp
    src/platform/socket.cpp)
target_include_directories(ckmux_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ckmux_common PUBLIC ckvision::cvision)
ckmux_strict(ckmux_common)

# The client object graph is a plain library so every part of it is reachable
# from headless tests; only the thin terminal host needs a real backend. This
# mirrors how ckVision's own examples are structured, for the same reason.
add_library(ckmux_client STATIC
    src/client/client_app.cpp
    src/client/commands.cpp
    src/client/copy_mode.cpp
    src/client/mirror.cpp
    src/client/remote_terminal.cpp
    src/client/prefix_overlay.cpp
    src/client/server_connection.cpp
    src/client/server_session.cpp)
target_include_directories(ckmux_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ckmux_client PUBLIC ckmux_common ckvision::cvision)
ckmux_strict(ckmux_client)

# The server is its own library and links only ckVision's core and term through
# ckmux_common, so AGENTS.md's layer rule — server code may include core and
# term, never ui, widgets or scene — is a fact the build enforces rather than a
# convention a reader has to remember. It is also what lets the server run with
# no host terminal at all, which is the whole point of the split.
add_library(ckmux_server STATIC
    src/server/diff_engine.cpp
    src/server/server.cpp
    src/server/terminals.cpp)
target_include_directories(ckmux_server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ckmux_server PUBLIC ckmux_common ckvision::cvision)
ckmux_strict(ckmux_server)

if(UNIX)
    add_executable(ckmux src/main.cpp)
    target_link_libraries(ckmux PRIVATE ckmux_client ckmux_server ckvision::cvision)
    ckmux_strict(ckmux)
    install(TARGETS ckmux RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

option(CKMUX_BUILD_TESTING "Build ckmux tests" ${PROJECT_IS_TOP_LEVEL})
if(CKMUX_BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
