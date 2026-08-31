# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.28)
project(ckmux VERSION 0.1.3 LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(CkmuxSettings)
include(GNUInstallDirs)

# The ckVision release this ckmux is developed and released against. The
# co-development checkout below tracks HEAD by design; every other way of
# getting the library -- an installed package, or the FetchContent clone an
# adopter's build makes -- must satisfy exactly this version. Bumping it is a
# deliberate act that follows a ckVision release, never a side effect.
set(CKMUX_CKVISION_VERSION 0.1.4)

# Said once, plainly: ckmux builds on POSIX only. The server forks, opens
# PTYs, polls file descriptors and speaks over a unix-domain socket, and
# src/platform is written against those calls; the architecture spec puts Windows/ConPTY
# out of scope and names the seams that would have to grow first. Guarding
# the executable with if(UNIX) while the libraries built unconditionally
# advertised a portability that does not exist — the libraries are the part
# that cannot compile elsewhere. One honest check, here, instead.
if(NOT UNIX)
    message(FATAL_ERROR
        "ckmux builds on POSIX systems only (macOS, Linux). Windows/ConPTY is "
        "out of scope: the server forks, opens PTYs, polls file descriptors "
        "and speaks over a unix-domain socket, and src/platform is the seam "
        "that would have to grow first.")
endif()

# ckVision comes either from a sibling source checkout or from an installed
# package. Both are first-class, but they are not equal in rank and the rank
# is now stated rather than inherited from the order of two commands:
#
#   * A sibling checkout means co-development (the engineering standard: "Fix ckVision, never
#     work around it"). A library edit has to be visible in the very next
#     ckmux build, so when ../ckvision exists it wins even if a ckvision
#     package happens to be installed. The old order preferred the package,
#     which silently built ckmux against yesterday's installed library while
#     the developer read and edited today's source — the one failure mode
#     co-development cannot tolerate.
#   * An adopter has no sibling checkout, so this default computes to OFF for
#     them and find_package supplies the library, unchanged.
#
# Either preference can be stated outright with -DCKMUX_PREFER_CKVISION_SOURCE.
set(CKMUX_CKVISION_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ckvision"
    CACHE PATH "ckVision source checkout used for co-development builds")
if(EXISTS "${CKMUX_CKVISION_SOURCE_DIR}/CMakeLists.txt")
    set(ckmux_ckvision_source_available TRUE)
else()
    set(ckmux_ckvision_source_available FALSE)
endif()
option(CKMUX_PREFER_CKVISION_SOURCE
    "Build ckVision from CKMUX_CKVISION_SOURCE_DIR even if an installed package exists"
    ${ckmux_ckvision_source_available})

if(CKMUX_PREFER_CKVISION_SOURCE AND NOT ckmux_ckvision_source_available)
    message(FATAL_ERROR
        "CKMUX_PREFER_CKVISION_SOURCE is ON but no ckVision checkout is at "
        "${CKMUX_CKVISION_SOURCE_DIR} (its CMakeLists.txt was expected there). "
        "Point -DCKMUX_CKVISION_SOURCE_DIR at one, or set "
        "-DCKMUX_PREFER_CKVISION_SOURCE=OFF to use an installed ckVision "
        "package or the pinned v${CKMUX_CKVISION_VERSION} release fetch.")
endif()

# A sanitizer setting that reaches only half of a statically linked binary
# is worse than none: ASan's container annotations disagree across an
# instrumented/uninstrumented boundary and report container-overflow on
# correct code, while the uninstrumented half's real bugs stay invisible —
# so the reader learns to distrust the tool. CKMUX_SANITIZE is the knob
# The engineering standard documents, so it decides for both projects; the FORCE is
# required because ckVision's own `set(CKVISION_SANITIZE "" CACHE ...)`
# runs inside add_subdirectory below and a plain cache set there would not
# overwrite an existing entry, nor this one overwrite the user's.
# CKMUX_FORWARDED_SANITIZE remembers what we forwarded last time, so the
# warning fires on a real disagreement and not on our own value coming
# back around on the next configure.
if(CKMUX_SANITIZE)
    if(DEFINED CACHE{CKVISION_SANITIZE}
       AND NOT "${CKVISION_SANITIZE}" STREQUAL "${CKMUX_SANITIZE}"
       AND NOT "${CKVISION_SANITIZE}" STREQUAL "${CKMUX_FORWARDED_SANITIZE}")
        message(WARNING
            "CKVISION_SANITIZE='${CKVISION_SANITIZE}' disagrees with "
            "CKMUX_SANITIZE='${CKMUX_SANITIZE}'; using CKMUX_SANITIZE for both. "
            "A half-instrumented binary reports findings that are not bugs and "
            "misses bugs that are.")
    endif()
    set(CKVISION_SANITIZE "${CKMUX_SANITIZE}" CACHE STRING
        "Sanitizers for the ckVision subproject (forwarded from CKMUX_SANITIZE)" FORCE)
    set(CKMUX_FORWARDED_SANITIZE "${CKMUX_SANITIZE}" CACHE INTERNAL
        "Last CKVISION_SANITIZE value forwarded from CKMUX_SANITIZE")
elseif(CKVISION_SANITIZE)
    message(WARNING
        "CKVISION_SANITIZE='${CKVISION_SANITIZE}' is set while CKMUX_SANITIZE is "
        "empty: only the library half of the binary would be instrumented. Use "
        "-DCKMUX_SANITIZE='${CKVISION_SANITIZE}' to instrument both, or clear both.")
endif()

if(CKMUX_PREFER_CKVISION_SOURCE)
    message(STATUS "ckmux: building ckVision from ${CKMUX_CKVISION_SOURCE_DIR}")
    # EXCLUDE_FROM_ALL is here for the install rules and nothing else. CMake's
    # directory property of that name has three documented effects: the
    # subdirectory's targets leave the parent's ALL target, they leave IDE
    # project files, and "any install rules defined in the subdirectory or
    # below will be ignored when installing the parent directory". The third
    # is the point. Without it, `cmake --install` copied ckVision's entire SDK
    # — the library, 119 headers, the export set and the package config —
    # into ckmux's prefix, so installing an application installed a second
    # copy of a library the adopter may already have from its own package.
    #
    # The first effect costs nothing, because the same documentation adds that
    # inter-target dependencies supersede the exclusion: every ckmux target
    # links ckvision::cvision (an ALIAS for this subdirectory's `cvision`
    # target), so `cvision` is pulled into the build system to satisfy those
    # dependencies and a plain `cmake --build` still builds it. What stops
    # being built by default is only what nothing here depends on — ckVision's
    # diagnostic tools. Ask for one by name when you want it:
    #   cmake --build build --target ckvision_terminal_probe
    # ckVision's tests and examples were already off from here: both default
    # to PROJECT_IS_TOP_LEVEL, which is false in this configuration.
    add_subdirectory("${CKMUX_CKVISION_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/ckvision"
        EXCLUDE_FROM_ALL)
else()
    # An adopter's build: no co-development checkout. One declaration states
    # the whole policy (CMake >= 3.24): try find_package first, so a ckVision
    # already installed at the pinned version is used rather than downloaded;
    # otherwise clone exactly the v${CKMUX_CKVISION_VERSION} release tag and
    # build it as part of this build -- same compiler, same flags, same
    # sanitizers. EXCLUDE_FROM_ALL keeps ckVision's install rules out of
    # ckmux's prefix, for the same reason as the add_subdirectory above.
    #
    # CI supplies the sources locally through the standard
    # FETCHCONTENT_SOURCE_DIR_CKVISION override, checked out at the same tag;
    # this declaration stays the single statement of the pinned version.
    include(FetchContent)
    FetchContent_Declare(ckvision
        GIT_REPOSITORY https://github.com/cklukas/ckVision.git
        GIT_TAG        v${CKMUX_CKVISION_VERSION}
        GIT_SHALLOW    TRUE
        EXCLUDE_FROM_ALL
        FIND_PACKAGE_ARGS ${CKMUX_CKVISION_VERSION} CONFIG)
    FetchContent_MakeAvailable(ckvision)
endif()

# The OS seam as a real target. The engineering standard and the architecture spec both list `platform`
# as one of four modules; compiling its sources into ckmux_common made that a
# claim about a folder rather than a structure. As its own library the seam is
# something a Linux port can swap or select as a unit, and its dependency
# direction becomes checkable — tools/check_layers.py rejects an include of
# any other ckmux module from here, which is what keeps the seam a seam
# instead of a place where common code accumulated. The split costs nothing
# downstream: the include path is unchanged, and ckmux_common carries this
# library PUBLIC, so everything that used to find these symbols in
# ckmux_common's archive still finds them by linking ckmux_common.
add_library(ckmux_platform STATIC
    src/platform/clipboard.cpp
    src/platform/paths.cpp
    src/platform/poller.cpp
    src/platform/process.cpp
    src/platform/process_stats.cpp
    src/platform/socket.cpp)
target_include_directories(ckmux_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# The ONE mention of ckvision::cvision on any link line. PUBLIC here, it
# propagates to every downstream target through the PUBLIC chain
# (platform -> common -> client/server -> ckmux, tests), and naming it
# again downstream is what doubled the archive on the two final links
# (`ld: warning: ignoring duplicate libraries`).
target_link_libraries(ckmux_platform PUBLIC ckvision::cvision)
ckmux_strict(ckmux_platform)

# What both ends are made of: the wire format, the configuration, and the diff
# algebra, on top of the platform seam. Its own library because the client and
# the server are separate libraries that both need it, and a shared object
# linked into both would be the same symbols twice in the one binary that
# links both.
#
# Nothing here may touch ckVision's `ui`, `widgets` or `scene`, for the same
# reason the server may not: this is what the server is built out of.
# The version, stamped from project() above into a generated header so the
# binary can never report a number the package disagrees with. Generated into
# the build tree; the `generated` dir rides ckmux_common's PUBLIC includes.
configure_file(src/common/version.hpp.in
    "${CMAKE_BINARY_DIR}/generated/common/version.hpp" @ONLY)

add_library(ckmux_common STATIC
    src/common/config.cpp
    src/common/grid_delta.cpp
    src/common/keymap.cpp
    src/common/proto.cpp
    src/common/shell.cpp)
target_include_directories(ckmux_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(ckmux_common PUBLIC ckmux_platform)
ckmux_strict(ckmux_common)
# GCC 13 raises -Wmaybe-uninitialized from inside libstdc++'s vector reallocation
# (vector.tcc, stl_vector.h, basic_string.h) when grid_delta.cpp's
# `ops.push_back(CursorOp{...})` / `push_back(ModesOp{...})` are inlined under
# -O3 — 15 reports, every one against a fully brace-initialised aggregate where
# no member is left uninitialised. It is the same post-inlining false positive
# ckVision documents and suppresses for its own files; GCC 14 no longer emits
# it, and Debug, the sanitiser lanes and Clang are all clean. Narrowed to this
# one file and to GNU rather than dropped into ckmux_strict, so the warning
# keeps catching real defects everywhere else.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties(src/common/grid_delta.cpp
        PROPERTIES COMPILE_OPTIONS -Wno-maybe-uninitialized)
endif()

# The client object graph is a plain library so every part of it is reachable
# from headless tests; only the thin terminal host needs a real backend. This
# mirrors how ckVision's own examples are structured, for the same reason.
add_library(ckmux_client STATIC
    src/client/cli.cpp
    src/client/client_app.cpp
    src/client/commands.cpp
    src/client/key_reference.cpp
    src/client/copy_mode.cpp
    src/client/capability_grace.cpp
    src/client/printer_button.cpp
    src/client/printer_output.cpp
    src/client/mirror.cpp
    src/client/remote_terminal.cpp
    src/client/prefix_overlay.cpp
    src/client/server_connection.cpp
    src/client/run_client.cpp
    src/client/server_session.cpp
    src/client/stats_format.cpp)
target_include_directories(ckmux_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ckmux_client PUBLIC ckmux_common)
ckmux_strict(ckmux_client)

# The server is its own library and reaches ckVision through `core` and `term`
# only — the engineering standard's layer rule. What holds that rule is tools/check_layers.py,
# registered below as the CTest cases `layer_check` and
# `layer_checker_self_test`: it fails when common, platform or server include
# cvision/ui, cvision/widgets or cvision/scene. The link line cannot say it,
# and an earlier comment here claimed it could: ckvision::cvision is a single
# archive containing every layer, so linking it proves nothing about which
# headers a translation unit used. The rule is what lets the server run with
# no host terminal at all, which is the whole point of the split, so it is
# enforced by something that runs rather than by something a reader remembers.
add_library(ckmux_server STATIC
    src/server/diff_engine.cpp
    src/server/server.cpp
    src/server/terminals.cpp)
target_include_directories(ckmux_server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ckmux_server PUBLIC ckmux_common)
ckmux_strict(ckmux_server)

# The executable, followed by documentation generated from that exact build.
# Everything the program needs is linked statically; the additional installed
# files are references for the reader rather than runtime dependencies.
add_executable(ckmux src/main.cpp)
target_link_libraries(ckmux PRIVATE ckmux_client ckmux_server)
ckmux_strict(ckmux)
install(TARGETS ckmux RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

set(CKMUX_GENERATED_DOC_DIR "${CMAKE_BINARY_DIR}/docs")
set(CKMUX_GENERATED_KEYS "${CKMUX_GENERATED_DOC_DIR}/keys.md")
set(CKMUX_GENERATED_MAN "${CKMUX_GENERATED_DOC_DIR}/ckmux.1")
add_custom_command(
    OUTPUT "${CKMUX_GENERATED_KEYS}" "${CKMUX_GENERATED_MAN}"
    COMMAND ${CMAKE_COMMAND}
        "-DCKMUX_BINARY=$<TARGET_FILE:ckmux>"
        "-DCKMUX_MARKDOWN_TEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/doc/keys.md.in"
        "-DCKMUX_MAN_TEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/doc/ckmux.1.in"
        "-DCKMUX_OUTPUT_DIR=${CKMUX_GENERATED_DOC_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateDocs.cmake"
    DEPENDS ckmux
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateDocs.cmake"
            "${CMAKE_CURRENT_SOURCE_DIR}/doc/keys.md.in"
            "${CMAKE_CURRENT_SOURCE_DIR}/doc/ckmux.1.in"
    COMMENT "Generating ckmux man page and key reference"
    VERBATIM)
add_custom_target(ckmux_docs ALL DEPENDS "${CKMUX_GENERATED_KEYS}" "${CKMUX_GENERATED_MAN}")

# Explicitly requested because it writes the Pages source tree. The ordinary
# build writes only below CMAKE_BINARY_DIR; test_docs compares that output to
# the checked-in page and fails when somebody changes the registry without
# regenerating it.
add_custom_target(ckmux_update_key_docs
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CKMUX_GENERATED_KEYS}" "${CMAKE_CURRENT_SOURCE_DIR}/docs/keys.md"
    DEPENDS ckmux_docs
    COMMENT "Updating docs/keys.md from the compiled key registry"
    VERBATIM)

install(FILES "${CKMUX_GENERATED_MAN}"
    DESTINATION "${CMAKE_INSTALL_MANDIR}/man1")
install(FILES
    README.md
    LICENSE
    docs/index.md
    docs/getting-started.md
    docs/cli.md
    docs/configuration.md
    "${CKMUX_GENERATED_KEYS}"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}")

# Python is build tooling, never a dependency of ckmux (the engineering standard: no external
# dependencies beyond ckvision::cvision). The convenience target is offered
# when an interpreter is present; the CTest gate below demands one.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    add_custom_target(ckmux_check_layers
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_layers.py
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Checking ckmux module and ckVision layer includes"
        VERBATIM)
endif()

option(CKMUX_BUILD_TESTING "Build ckmux tests" ${PROJECT_IS_TOP_LEVEL})
if(CKMUX_BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)

    # The architecture rule fails like a test because it is one. REQUIRED, so
    # the gate can never vanish silently on a host without an interpreter —
    # the same reasoning, and the same wiring, as ckVision's own checker.
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME layer_check
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_layers.py)
    set_tests_properties(layer_check PROPERTIES LABELS "script;architecture" TIMEOUT 30)
    # And the checker is itself tested against a tree containing every
    # forbidden edge: a checker that has quietly stopped checking reads
    # exactly like a clean repository.
    add_test(NAME layer_checker_self_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_layers.py
                --self-test)
    set_tests_properties(layer_checker_self_test PROPERTIES
        LABELS "script;architecture" TIMEOUT 30)
    add_test(NAME homebrew_formula_generator_test
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/packaging/homebrew/generate_formula.py
                --self-test)
    set_tests_properties(homebrew_formula_generator_test PROPERTIES
        LABELS "script;packaging" TIMEOUT 30)
    # A seam nobody wires is a feature a reader cannot reach. The printer
    # shipped that way -- nine `std::function`s declared, assigned nowhere,
    # every test green, because every test exercised the half that existed.
    # Same wiring and same reasoning as the layer checker above, including the
    # self-test: a checker that has quietly stopped checking reads exactly like
    # a clean repository.
    add_test(NAME seam_check
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_seams.py)
    set_tests_properties(seam_check PROPERTIES LABELS "script;architecture" TIMEOUT 30)
    add_test(NAME seam_checker_self_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_seams.py
                --self-test)
    set_tests_properties(seam_checker_self_test PROPERTIES
        LABELS "script;architecture" TIMEOUT 30)
    # A function nobody reaches is a feature nobody can use, and the compiler
    # says nothing: `expand_user_path` sat defined and uncalled in HEAD for an
    # hour after its definition landed without its call site. The linker's
    # dead-strip pass answers the question a source scan only approximates --
    # is this reachable from `main` in the production binary. It re-links its
    # own probe, so an ordinary build is unchanged and pays for no map.
    add_test(NAME dead_code_check
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_dead_code.py
                --build ${CMAKE_BINARY_DIR})
    set_tests_properties(dead_code_check PROPERTIES LABELS "script;architecture" TIMEOUT 120)
    add_test(NAME dead_code_checker_self_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_dead_code.py
                --build ${CMAKE_BINARY_DIR} --self-test)
    set_tests_properties(dead_code_checker_self_test PROPERTIES
        LABELS "script;architecture" TIMEOUT 180)
endif()

# After tests/, deliberately: the fuzz directory registers its bounded corpus
# runs as CTest cases when CKMUX_BUILD_TESTING is on, and enable_testing() has
# to have run before an add_test(). With testing off it still builds the
# drivers, which is what a fuzzing campaign wants and a test run does not.
if(CKMUX_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()

# Packaging. `cpack -G TGZ` (any platform) or `cpack -G "TGZ;DEB;RPM"`
# (Linux, with dpkg and rpmbuild installed) from a Release build directory
# produces the installable packages that .github/workflows/release.yml attaches
# to a GitHub release. The workflow runs exactly these commands, so any
# published package can be reproduced locally. Top-level only: nobody packages
# ckmux from inside another build.
if(PROJECT_IS_TOP_LEVEL)
    set(CPACK_PACKAGE_NAME "ckmux")
    set(CPACK_PACKAGE_VENDOR "Dr. Christian Klukas")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "Terminal multiplexer with a visible interface: menus, windows, mouse")
    set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/cklukas/ckmux")
    set(CPACK_PACKAGE_CONTACT "Dr. Christian Klukas <christian.klukas@gmail.com>")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
    set(CPACK_PACKAGE_CHECKSUM SHA256)
    set(CPACK_STRIP_FILES TRUE)
    # Name the architecture: the default file name says only "Darwin"/"Linux",
    # which cannot tell an arm64 mac package from an x86_64 one.
    set(CPACK_PACKAGE_FILE_NAME
        "${PROJECT_NAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
    set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_RPM_PACKAGE_RELEASE 1)
    set(CPACK_RPM_PACKAGE_LICENSE "MIT")
    set(CPACK_RPM_PACKAGE_GROUP "Applications/System")
    set(CPACK_RPM_PACKAGE_AUTOREQ ON)
    set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
    include(CPack)
endif()
