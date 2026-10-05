# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
include_guard(GLOBAL)

# Application distribution policy, not a second ConPTY implementation. Obtain
# this exact official archive separately; ckVision owns validation/deployment.
set(CKMUX_CONPTY_VERSION 1.24.261001001)
set(CKMUX_CONPTY_SHA256 4d6aaddc1d2385c9f5897df28f33879f699f8f2783315d5204cf3d8c3616ac5f)
set(CKMUX_CONPTY_ARCHIVE "" CACHE FILEPATH "Approved Microsoft ConPTY archive; empty uses the inbox fallback")
set(CKMUX_WINDOWS_ARCHITECTURE "" CACHE STRING "Windows target architecture: x64 or arm64")
set_property(CACHE CKMUX_WINDOWS_ARCHITECTURE PROPERTY STRINGS x64 arm64)

if(WIN32)
    if(CKMUX_WINDOWS_ARCHITECTURE STREQUAL "")
        # The target architecture, never the host architecture under emulation.
        if(CMAKE_VS_PLATFORM_NAME)
            string(TOLOWER "${CMAKE_VS_PLATFORM_NAME}" CKMUX_WINDOWS_ARCHITECTURE)
        elseif(CMAKE_CXX_COMPILER_ARCHITECTURE_ID)
            string(TOLOWER "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}" CKMUX_WINDOWS_ARCHITECTURE)
        endif()
    endif()
    if(NOT CKMUX_WINDOWS_ARCHITECTURE MATCHES "^(x64|arm64)$")
        message(FATAL_ERROR "ckmux Windows builds require an explicit x64 or arm64 target architecture")
    endif()
elseif(NOT CKMUX_CONPTY_ARCHIVE STREQUAL "")
    message(FATAL_ERROR "CKMUX_CONPTY_ARCHIVE is only valid for a Windows target")
endif()

function(ckmux_deploy_windows_runtime target destination)
    if(NOT WIN32 OR CKMUX_CONPTY_ARCHIVE STREQUAL "")
        return()
    endif()
    if(NOT COMMAND ckvision_deploy_conpty)
        message(FATAL_ERROR "App-local ConPTY deployment requires ckVision's deployment helper")
    endif()
    ckvision_deploy_conpty(TARGET "${target}"
        ARCHIVE "${CKMUX_CONPTY_ARCHIVE}"
        SHA256 "${CKMUX_CONPTY_SHA256}"
        ARCHITECTURE "${CKMUX_WINDOWS_ARCHITECTURE}"
        INSTALL_DESTINATION "${destination}")
endfunction()
