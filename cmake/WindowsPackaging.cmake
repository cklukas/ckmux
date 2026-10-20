# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT

# MSI authoring is qualified with CMake 4.3 or newer. Normal source builds and
# ZIP generation retain the project's 3.28 minimum; enforce this at packaging.
set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_FILE_NAME "ckmux-${PROJECT_VERSION}-Windows-${CKMUX_WINDOWS_ARCHITECTURE}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "ckmux-${CKMUX_WINDOWS_ARCHITECTURE}")
# Use an advertised executable shortcut; CPack's shortcut adds a fixed HKCU key.
set(CPACK_PACKAGE_EXECUTABLES "")
set(CPACK_WIX_VERSION 4)
set(CPACK_WIX_ARCHITECTURE "${CKMUX_WINDOWS_ARCHITECTURE}")
# Generate ordinary file components. The template owns the actual dual scope.
set(CPACK_WIX_INSTALL_SCOPE perMachine)
set(CPACK_WIX_ROOT_FOLDER_ID ProgramFiles64Folder)
set(CPACK_WIX_TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/installer.wxs.in")
set(CPACK_WIX_PATCH_FILE "${CMAKE_CURRENT_BINARY_DIR}/packaging/user-path.xml")
if(CKMUX_WINDOWS_ARCHITECTURE STREQUAL "arm64")
    set(CPACK_WIX_UPGRADE_GUID B90B17C0-BCF9-4506-B5BB-457245804F10)
else()
    set(CPACK_WIX_UPGRADE_GUID 63F125D8-E36D-45C4-8E12-24E8E6A2EBA8)
endif()
foreach(kind IN ITEMS USER_ENV MACHINE_ENV)
    string(UUID CKMUX_WINDOWS_${kind}_GUID NAMESPACE "${CPACK_WIX_UPGRADE_GUID}"
        NAME "ckmux-${kind}" TYPE SHA1 UPPER)
endforeach()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/packaging")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/user-path.xml.in"
    "${CPACK_WIX_PATCH_FILE}" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    "${CMAKE_CURRENT_BINARY_DIR}/packaging/LICENSE.txt" COPYONLY)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_BINARY_DIR}/packaging/LICENSE.txt")
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/WindowsPackageGenerator.cmake")
