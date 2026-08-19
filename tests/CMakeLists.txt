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
    test_client_pacing.cpp
    test_client_smoke.cpp
    test_config.cpp
    test_copy_mode.cpp
    test_core_promise.cpp
    test_diff_engine.cpp
    test_grid_delta.cpp
    test_keymap.cpp
    test_menu_commands.cpp
    test_minimize_chrome.cpp
    test_prefix.cpp
    test_proto.cpp
    test_process_stats.cpp
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
# or nobody can trust it to be the fast lane. Labelling all twenty-two `unit`
# said the opposite of the truth about fifteen of them.
#
# A named exception REPLACES `unit` rather than adding to it, because `ctest
# -L unit` matches every suite carrying that label: a `unit;pty` would leave
# the PTY suites in the fast lane and change nothing a reader selects by. So
# only the exceptions are named, everything else is `unit`, and a suite added
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
#
# The `pty` set was established by running every suite under an interposed
# `forkpty` and counting what it forked, rather than by reading it — because
# five of the ten below never name a terminal at all. They reach the fork
# through `ClientApp`'s constructor, which opens a terminal window at startup
# (`open_terminal_at_startup`, default true) unless the test injects its own
# `terminal_source`. test_remote_terminal injects at all three of its client
# sites, forks nothing, and is the one client suite that is honestly `unit`.
set(CKMUX_LABELS_test_cli "script;e2e")
set(CKMUX_LABELS_test_server_lifecycle "script;e2e")
set(CKMUX_LABELS_test_client_pacing "script;pty;e2e")
set(CKMUX_LABELS_test_menu_commands "script;pty;e2e")
set(CKMUX_LABELS_test_core_promise "script;pty;e2e;golden")
# Server side: `Terminals::open` and `Server::open_terminal` both go straight
# to `PosixTerminalSubsession::launch`, which is forkpty + execve.
set(CKMUX_LABELS_test_attach "pty")
set(CKMUX_LABELS_test_diff_engine "pty")
set(CKMUX_LABELS_test_rename_wire "pty")
set(CKMUX_LABELS_test_server_loop "pty")
set(CKMUX_LABELS_test_server_terminals "pty")
# Client side: a `ClientApp` built with no `terminal_source` opens a real
# terminal window while it is being constructed, so the fixture has already
# forked its child before the first case in the file runs.
set(CKMUX_LABELS_test_client_smoke "pty")
set(CKMUX_LABELS_test_command_reachability "pty")
set(CKMUX_LABELS_test_copy_mode "pty")
set(CKMUX_LABELS_test_minimize_chrome "pty")
set(CKMUX_LABELS_test_prefix "pty")
set(CKMUX_LABELS_test_rename_terminal "pty")

foreach(source ${CKMUX_TEST_SOURCES})
# test_process_stats forks bare children of its own (fork, no exec, no PTY) —
# a process primitive available anywhere the suite itself can run, so it stays
# in the fast lane rather than joining `pty`, whose reason is a sandbox with
# no pseudo-terminals. Unlike the ClientApp suites above, its forks are its
# own explicit `::fork()` calls, so reading the file does tell you.
    get_filename_component(suite ${source} NAME_WE)
    add_test(NAME suite_${suite} COMMAND ckmux_tests --suite ${source})
    set(CKMUX_SUITE_LABELS "unit")
    if(DEFINED CKMUX_LABELS_${suite})
        set(CKMUX_SUITE_LABELS "${CKMUX_LABELS_${suite}}")
    endif()
    set_tests_properties(suite_${suite} PROPERTIES LABELS "${CKMUX_SUITE_LABELS}" TIMEOUT 120)
endforeach()
