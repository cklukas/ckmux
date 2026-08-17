# Copyright (c) 2026 C. Klukas. All rights reserved.
#
# One cktest executable, registered per suite so a failure names the file it
# came from and each suite gets its own process and bounded timeout — the same
# shape ckVision's own suite uses, because ckmux inherits its verification
# culture (plans/09-testing.md).

set(CKMUX_TEST_SOURCES
    test_commands.cpp
    test_command_reachability.cpp
    test_client_smoke.cpp
    test_config.cpp
    test_copy_mode.cpp
    test_keymap.cpp
    test_prefix.cpp
    test_shell.cpp)

add_executable(ckmux_tests ${CKMUX_TEST_SOURCES} main.cpp)
target_link_libraries(ckmux_tests PRIVATE ckmux_client ckvision::cvision)
ckmux_strict(ckmux_tests)

foreach(source ${CKMUX_TEST_SOURCES})
    get_filename_component(suite ${source} NAME_WE)
    add_test(NAME suite_${suite} COMMAND ckmux_tests --suite ${source})
    set_tests_properties(suite_${suite} PROPERTIES LABELS "unit" TIMEOUT 120)
endforeach()
