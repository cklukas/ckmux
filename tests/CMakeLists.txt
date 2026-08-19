# Copyright (c) 2026 C. Klukas. All rights reserved.
#
# One cktest executable, registered per suite so a failure names the file it
# came from and each suite gets its own process and bounded timeout — the same
# shape ckVision's own suite uses, because ckmux inherits its verification
# culture (plans/09-testing.md).

set(CKMUX_TEST_SOURCES
    test_commands.cpp
    test_command_reachability.cpp
    test_attach.cpp
    test_cli.cpp
    test_client_smoke.cpp
    test_config.cpp
    test_copy_mode.cpp
    test_core_promise.cpp
    test_diff_engine.cpp
    test_grid_delta.cpp
    test_keymap.cpp
    test_menu_commands.cpp
    test_prefix.cpp
    test_proto.cpp
    test_remote_terminal.cpp
    test_rename_terminal.cpp
    test_rename_wire.cpp
    test_server_lifecycle.cpp
    test_server_loop.cpp
    test_server_terminals.cpp
    test_shell.cpp)

add_executable(ckmux_tests ${CKMUX_TEST_SOURCES} main.cpp)
target_link_libraries(ckmux_tests PRIVATE ckmux_client ckmux_server ckvision::cvision)
# The lifecycle tests run the real binary — a server that is not a real process
# is not a server (WP-2). The path is a definition rather than a search, so a
# test never finds a stale one from another build directory.
if(TARGET ckmux)
    target_compile_definitions(ckmux_tests PRIVATE CKMUX_BINARY_PATH="$<TARGET_FILE:ckmux>")
    add_dependencies(ckmux_tests ckmux)
endif()
ckmux_strict(ckmux_tests)

# Labels are plans/09's own taxonomy — `unit`, `golden`, `script`, `pty`,
# `e2e`, `benchmark`, `fuzz-corpus`, `portable` — and they have to say what a
# suite actually does, because they are what a reader selects by: `ctest -L
# unit` has to mean the suites that need nothing but the process they run in,
# or nobody can trust it to be the fast lane. Labelling all nineteen `unit`
# said the opposite of the truth about four of them.
#
# Only the exceptions are named; everything else is `unit`, so a suite added
# tomorrow is labelled by default rather than unlabelled. Each entry below is
# derived from what the file does, not from its name:
#
#   * `script` + `e2e`: spawns the real ckmux binary through CKMUX_BINARY_PATH
#     and drives it end to end.
#   * `pty`: launches a real PTY child (ckVision's PosixTerminalSubsession),
#     so it needs a pseudo-terminal to exist and cannot run in a sandbox that
#     has none.
#   * `golden`: pins a decoded frame against a checked-in golden
#     (`cvision/core/golden.hpp`, ckVision D-014).
set(CKMUX_LABELS_test_cli "script;e2e")
set(CKMUX_LABELS_test_server_lifecycle "script;e2e")
set(CKMUX_LABELS_test_menu_commands "script;pty;e2e")
set(CKMUX_LABELS_test_core_promise "script;pty;e2e;golden")

foreach(source ${CKMUX_TEST_SOURCES})
    get_filename_component(suite ${source} NAME_WE)
    add_test(NAME suite_${suite} COMMAND ckmux_tests --suite ${source})
    set(CKMUX_SUITE_LABELS "unit")
    if(DEFINED CKMUX_LABELS_${suite})
        set(CKMUX_SUITE_LABELS "${CKMUX_LABELS_${suite}}")
    endif()
    set_tests_properties(suite_${suite} PROPERTIES LABELS "${CKMUX_SUITE_LABELS}" TIMEOUT 120)
endforeach()
