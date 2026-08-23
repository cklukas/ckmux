# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
#
# Central strict build settings for ckmux. Deliberately identical in shape to
# ckVision's own CkVisionSettings.cmake: one place defines the flag set, and
# warnings are errors (the conventions).
#
# There is no MSVC arm. ckmux is POSIX-only — the root CMakeLists says so once
# and stops — so a /W4 branch here would be flag selection for a compiler that
# never reaches the second translation unit.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CKMUX_SANITIZE "" CACHE STRING
    "Comma-separated sanitizer list applied to all ckmux targets, e.g. address,undefined")

# Opt-in, and off by default because the default has to be true when it is
# claimed. src/ narrows at the wire boundary today — geometry into the
# protocol's fixed-width fields, sizes into ints — in places -Wconversion
# rightly objects to, and every ckmux target compiles with -Werror, so
# flipping this default before those sites carry explicit, checked narrowing
# would break the build for everyone rather than fix anything. The goal is to
# flip it once src/ is clean under it; until then it is the switch a cleanup
# pass builds with:
#
#     cmake -S . -B build-conversion -DCKMUX_WCONVERSION=ON
#
# Two notes for whoever does that pass. ckVision's headers are ordinary
# (non-SYSTEM) includes here, on purpose, so findings inside them are real
# findings — they belong upstream under the engineering standard's rule, not behind a
# SYSTEM include that would hide them. And -Wsign-conversion is deliberately
# not added alongside: the compilers differ on whether -Wconversion implies
# it, and one cleanup at a time is enough.
option(CKMUX_WCONVERSION "Add -Wconversion to the strict flag set" OFF)

# The libFuzzer lane (the testing plan §6, fuzz/CMakeLists.txt). Declared here because
# it changes the flags every ckmux target is built with, and used in the root
# to add the fuzz directory. Off by default: an ordinary build must not need a
# compiler runtime that supplies libFuzzer.
option(CKMUX_BUILD_FUZZERS "Build libFuzzer targets and bounded corpus tests (LLVM Clang only)" OFF)

include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-Wmissing-designated-field-initializers
                        CKMUX_HAS_WMISSING_DESIGNATED)

function(ckmux_strict target)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Werror)
    if(CKMUX_HAS_WMISSING_DESIGNATED)
        # Newer LLVM Clang puts this under -Wextra; AppleClang does not have it
        # at all. It is switched off on purpose, not left to compiler luck: the
        # suite's descriptor structs (ckVision's CommandDescriptor and
        # FieldDescriptor, and their ckmux counterparts) default every field so
        # a call site names only what differs — that is the API's design, and a
        # warning that forces every caller to enumerate every field would
        # dismantle it one -Werror build at a time.
        target_compile_options(${target} PRIVATE -Wno-missing-designated-field-initializers)
    else()
        # The same decision in the older spelling. A Clang without the
        # designated-specific flag — and GCC, which has no such flag at all —
        # folds the identical diagnostic into -Wmissing-field-initializers,
        # which -Wextra turns on. So the suppression was silently absent on
        # exactly the compilers that emit the warning (GCC found 18 sites in
        # client_app.cpp the first time ckmux met a Linux compiler) and present
        # on the ones that do not. Narrowed to this fallback rather than dropped
        # everywhere, so ordinary aggregate init keeps its warning where the
        # specific flag exists.
        target_compile_options(${target} PRIVATE -Wno-missing-field-initializers)
    endif()
    if(CKMUX_WCONVERSION)
        target_compile_options(${target} PRIVATE -Wconversion)
    endif()
    if(CKMUX_BUILD_FUZZERS)
        # Coverage instrumentation for every ckmux translation unit, so a
        # driver gets feedback from the library code it drives rather than only
        # from its own few lines. `fuzzer-no-link` is the half of -fsanitize=
        # fuzzer that adds the instrumentation without the libFuzzer main, so
        # the ordinary executable and the test binary still link normally in
        # this configuration; the fuzz targets add the main themselves.
        target_compile_options(${target} PRIVATE -fsanitize=fuzzer-no-link)
    endif()
    if(CKMUX_SANITIZE)
        # -fno-sanitize-recover=all: a finding must abort, not scroll past as
        # green-looking log noise.
        target_compile_options(${target} PRIVATE
            -fsanitize=${CKMUX_SANITIZE} -fno-sanitize-recover=all -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE
            -fsanitize=${CKMUX_SANITIZE} -fno-sanitize-recover=all)
    endif()
endfunction()
