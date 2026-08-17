# Copyright (c) 2026 C. Klukas. All rights reserved.
#
# Central strict build settings for ckmux. Deliberately identical in shape to
# ckVision's own CkVisionSettings.cmake: one place defines the flag set, and
# warnings are errors (plans/11-conventions.md).

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CKMUX_SANITIZE "" CACHE STRING
    "Comma-separated sanitizer list applied to all ckmux targets, e.g. address,undefined")

function(ckmux_strict target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Werror)
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
