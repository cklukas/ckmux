# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
#
# One cktest executable, registered per suite so a failure names the file it
# came from and each suite gets its own process and bounded timeout — the same
# shape ckVision's own suite uses, because ckmux inherits its verification
# culture (the testing plan).

set(CKMUX_TEST_SOURCES
    test_commands.cpp
    test_command_reachability.cpp
    test_attach.cpp
    test_budgets.cpp
    test_cli.cpp
    test_capability_grace.cpp
    test_client_pacing.cpp
    test_client_smoke.cpp
    test_config.cpp
    test_copy_mode.cpp
    test_core_promise.cpp
    test_diff_engine.cpp
    test_docs.cpp
    test_grid_delta.cpp
    test_kill_terminal.cpp
    test_keymap.cpp
    test_m4_tmux.cpp
    test_menu_commands.cpp
    test_minimize_chrome.cpp
    test_notifications.cpp
    test_paste_credit.cpp
    test_prefix.cpp
    test_ckspy.cpp
    test_print_acceptance.cpp
    test_printer_button.cpp
    test_printer_output.cpp
    test_process_stats.cpp
    test_paths.cpp
    test_proto.cpp
    test_reader_modes.cpp
    test_reader_modes_client.cpp
    test_reference_apps.cpp
    test_remote_terminal.cpp
    test_rename_terminal.cpp
    test_rename_wire.cpp
    test_session_desktop.cpp
    test_session_desktop_client.cpp
    test_session_names.cpp
    test_attach_flags.cpp
    test_server_lifecycle.cpp
    test_server_loop.cpp
    test_server_terminals.cpp
    test_shell.cpp
    test_stats_display.cpp
    test_stats_format.cpp
    test_stats_wire.cpp)

add_executable(ckmux_tests ${CKMUX_TEST_SOURCES} main.cpp)
target_link_libraries(ckmux_tests PRIVATE ckmux_client ckmux_server)
# The lifecycle tests run the real binary — a server that is not a real process
# is not a server (WP-2). The path is a definition rather than a search, so a
# test never finds a stale one from another build directory.
if(TARGET ckmux)
    target_compile_definitions(ckmux_tests PRIVATE CKMUX_BINARY_PATH="$<TARGET_FILE:ckmux>")
    # Two mouse tests drive ckVision's own `hello` example inside a ckmux pane,
    # to prove a click crosses the wire into a SEPARATE process's Application.
    # They need that example's binary, and the only honest way to know where it
    # is, is to ask the build that produced it — same reason and same mechanism
    # as CKMUX_BINARY_PATH above. When ckVision is built here with examples off
    # (the default for a dependency) the target does not exist, the definition
    # is absent, and the tests say so and skip rather than guess at a path.
    if(TARGET ckvision_hello)
        target_compile_definitions(ckmux_tests PRIVATE
            CKMUX_CKVISION_HELLO_PATH="$<TARGET_FILE:ckvision_hello>")
    endif()
    add_dependencies(ckmux_tests ckmux)
endif()
if(TARGET ckmux_docs)
    target_compile_definitions(ckmux_tests PRIVATE
        CKMUX_GENERATED_MAN_PATH="${CKMUX_GENERATED_MAN}"
        CKMUX_GENERATED_KEYS_PATH="${CKMUX_GENERATED_KEYS}"
        CKMUX_SOURCE_KEYS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../docs/keys.md")
    add_dependencies(ckmux_tests ckmux_docs)
endif()
ckmux_strict(ckmux_tests)

# ckspy: the DYLD interposer that answers "what did this process actually open
# and actually spawn" (tools/ckspy.c). Apple only, because that is where
# `DYLD_INSERT_LIBRARIES` exists at all.
#
# SHARED and not MODULE, which is the counter-intuitive half. MODULE is
# CMake's word for "loaded at runtime rather than linked against", which
# describes this exactly — and it produces a Mach-O BUNDLE, which dyld refuses
# to insert: `cannot link against bundle`, and the target process aborts
# before `main`. The failure is loud but it names the inserted library rather
# than the build type, so it reads as a broken interposer rather than a
# mis-declared target. SHARED produces a real dylib and works.
#
# `ckmux_strict` is deliberately NOT applied: the technique is a non-standard
# section holding function pointers with deliberately mismatched linkage, and
# the project's warning set exists to forbid precisely that everywhere else.
if(APPLE)
    # The project is C++ only, so the C compiler has to be asked for before a
    # `.c` target can be linked at all.
    enable_language(C)
    add_library(ckspy SHARED ${CMAKE_CURRENT_SOURCE_DIR}/../tools/ckspy.c)
    set_target_properties(ckspy PROPERTIES PREFIX "" SUFFIX ".dylib")
    target_compile_definitions(ckmux_tests PRIVATE CKSPY_DYLIB_PATH="$<TARGET_FILE:ckspy>")
    add_dependencies(ckmux_tests ckspy)
endif()

# Labels are the testing plan's own taxonomy — `unit`, `golden`, `script`, `pty`,
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
# test_cli gained `pty` with WP-11: its subcommands used to bounce off the
# unknown-argument refusal and touch nothing, and now `ckmux new` makes a
# session whose first terminal is a real child. Measured at 4 PTY children
# across 3 processes, not inferred — a suite that changes category mid-package
# is exactly what this block exists to catch.
set(CKMUX_LABELS_test_kill_terminal "pty")
set(CKMUX_LABELS_test_cli "script;pty;e2e")
set(CKMUX_LABELS_test_server_lifecycle "script;e2e")
# WP-20's budget gates. `benchmark` because the assertions are deterministic
# counters rather than behaviour, and `pty` because Terminals::open forks a real
# child for every terminal a gate measures.
set(CKMUX_LABELS_test_budgets "benchmark;pty")
set(CKMUX_LABELS_test_client_pacing "script;pty;e2e")
# WP-21 §4, the M4 acceptance: `tmux` inside a ckmux terminal. Forks the real
# client through ckVision's PTY and runs a real tmux inside it, so it is `pty`
# and `e2e` both, and it needs the oracle on the machine (§4.1).
# WP-21 §5, the printing acceptance: the real client, a real child printing,
# and the process observed from outside for the two negatives (§5 requires
# the negatives be established by watching the run, not by reading source).
set(CKMUX_LABELS_test_print_acceptance "pty;e2e")
# ckspy proves itself against a real ckmux server, which forks a real child,
# so it is `pty` as well as `unit` — and it is the instrument WP-21 §5's
# negatives depend on, so a broken one must fail loudly rather than quietly.
set(CKMUX_LABELS_test_ckspy "unit;pty")
# WP-21 §3, the reference-app matrix: real programs inside a real ckmux,
# driven by keystrokes and read off the screen.
set(CKMUX_LABELS_test_reference_apps "pty;e2e")
# Eleven rows, each starting a server, a client and a real program.
set(CKMUX_TIMEOUT_test_reference_apps 900)
set(CKMUX_LABELS_test_m4_tmux "pty;e2e")
set(CKMUX_LABELS_test_menu_commands "script;pty;e2e")
set(CKMUX_LABELS_test_core_promise "script;pty;e2e;golden")
# Server side: `Terminals::open` and `Server::open_terminal` both go straight
# to `PosixTerminalSubsession::launch`, which is forkpty + execve.
set(CKMUX_LABELS_test_attach "pty")
set(CKMUX_LABELS_test_diff_engine "pty")
set(CKMUX_LABELS_test_rename_wire "pty")
set(CKMUX_LABELS_test_server_loop "pty")
set(CKMUX_LABELS_test_server_terminals "pty")
set(CKMUX_LABELS_test_stats_wire "pty")
# Client side: a `ClientApp` built with no `terminal_source` opens a real
# terminal window while it is being constructed, so the fixture has already
# forked its child before the first case in the file runs.
set(CKMUX_LABELS_test_client_smoke "pty")
set(CKMUX_LABELS_test_command_reachability "pty")
set(CKMUX_LABELS_test_copy_mode "pty")
set(CKMUX_LABELS_test_minimize_chrome "pty")
set(CKMUX_LABELS_test_notifications "pty")
set(CKMUX_LABELS_test_prefix "pty")
set(CKMUX_LABELS_test_rename_terminal "pty")
set(CKMUX_LABELS_test_stats_display "pty")
set(CKMUX_LABELS_test_session_desktop_client "pty")
set(CKMUX_LABELS_test_reader_modes_client "pty")
set(CKMUX_LABELS_test_attach_flags "unit")
set(CKMUX_LABELS_test_session_names "unit")
# test_process_stats forks bare children of its own (fork, no exec, no PTY) —
# a process primitive available anywhere the suite itself can run, so it stays
# in the fast lane rather than joining `pty`, whose reason is a sandbox with
# no pseudo-terminals. Unlike the ClientApp suites above, its forks are its
# own explicit `::fork()` calls, so reading the file does tell you.

foreach(source ${CKMUX_TEST_SOURCES})
    get_filename_component(suite ${source} NAME_WE)
    add_test(NAME suite_${suite} COMMAND ckmux_tests --suite ${source})
    set(CKMUX_SUITE_LABELS "unit")
    if(DEFINED CKMUX_LABELS_${suite})
        set(CKMUX_SUITE_LABELS "${CKMUX_LABELS_${suite}}")
    endif()
    # Two minutes suits every suite that drives data structures, and none that
    # drives a matrix of real applications: WP-21 §3 starts a server, a client
    # and a program per row, and waits on each one's own idea of ready. A suite
    # may name its own budget rather than have its assertions trimmed to fit a
    # number chosen for a different kind of test — an acceptance row shortened
    # until it fits a timeout is an acceptance row that has stopped asserting
    # what it was written for.
    set(CKMUX_SUITE_TIMEOUT 120)
    if(DEFINED CKMUX_TIMEOUT_${suite})
        set(CKMUX_SUITE_TIMEOUT ${CKMUX_TIMEOUT_${suite}})
    endif()
    set_tests_properties(suite_${suite} PROPERTIES LABELS "${CKMUX_SUITE_LABELS}"
                         TIMEOUT ${CKMUX_SUITE_TIMEOUT})
endforeach()
