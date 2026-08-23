// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// A reader's ckmux, in a real terminal, with its screen decoded — the rig the
// end-to-end suites drive.
//
// This lived inside `test_menu_commands.cpp` until WP-21 §4 needed the same
// thing to run `tmux` inside a ckmux terminal. It is a header rather than a
// library because it is scaffolding: it exists to be compiled into whichever
// suite needs a client on a screen, and a test that needs to reach into it
// should be able to.
//
// The PTY is ckVision's, not ckmux's — `PosixTerminalSubsession::launch` is
// the `forkpty` + `execve`, and ckmux launches THROUGH it. Anyone grepping
// this repository for `forkpty` to find the harness will find nothing and
// should not conclude there isn't one (WP-21 §4.3).
#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "platform/socket.hpp"

#include "cvision/term/posix_terminal_subsession.hpp"
#include "cvision/testing/cktest.hpp"

namespace ckmtest {


using clock_type = std::chrono::steady_clock;

inline std::filesystem::path binary_path() {
#if defined(CKMUX_BINARY_PATH)
    return std::filesystem::path(CKMUX_BINARY_PATH);
#else
    return {};
#endif
}

inline std::filesystem::path private_socket(std::string_view name) {
    const char* const base = std::getenv("TMPDIR");
    std::filesystem::path directory =
        std::filesystem::path(base != nullptr && *base != '\0' ? base : "/tmp");
    directory /= "ckmux-m" + std::to_string(static_cast<unsigned long>(::getpid()));
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    return directory / (std::string(name) + ".sock");
}

inline void forget(const std::filesystem::path& socket) {
    std::error_code ignored;
    std::filesystem::remove(socket, ignored);
    std::filesystem::remove(std::filesystem::path(socket.string() + ".lock"), ignored);
    std::filesystem::remove(std::filesystem::path(socket.string() + ".log"), ignored);
    std::filesystem::remove(socket.parent_path(), ignored);
}

// Every server this harness forks, so none of them can outlive the run.
//
// A test that returns early after starting a server — `if (!frame.has_value())
// return;` and its several siblings — never reaches its end_process(), and the
// server stays. They accumulated: dozens of them were still running from
// earlier ctest invocations, and because these are end-to-end PTY tests with
// millisecond budgets, that load made later runs fail assertions that pass on
// an idle machine. A timing test measures the machine as much as the code, and
// a machine littered by previous runs is a different machine.
//
// Reaped at process exit rather than between tests: at exit nothing can still
// be in use, so this cannot kill a server a running test is talking to. It ends
// the accumulation ACROSS runs, which is what actually poisoned the results.
inline std::vector<::pid_t>& harness_servers() {
    static std::vector<::pid_t> servers;
    return servers;
}

inline void retire_harness_servers() {
    for (const ::pid_t pid : harness_servers()) {
        if (pid <= 0) continue;
        int status = 0;
        if (::waitpid(pid, &status, WNOHANG) == pid) continue;  // ended properly
        (void)::kill(pid, SIGKILL);
        (void)::waitpid(pid, &status, 0);
    }
    harness_servers().clear();
}

inline void remember_harness_server(::pid_t pid) {
    static const bool reaper_installed = [] { return std::atexit(&retire_harness_servers) == 0; }();
    (void)reaper_installed;
    if (pid > 0) harness_servers().push_back(pid);
}

inline ::pid_t start_server(const std::filesystem::path& socket) {
    const ::pid_t child = ::fork();
    if (child != 0) {
        remember_harness_server(child);
        return child;
    }
    const std::string program = binary_path().string();
    const std::string path = socket.string();
    char* const argv[] = {const_cast<char*>(program.c_str()), const_cast<char*>("--server"),
                          const_cast<char*>(path.c_str()), const_cast<char*>("--foreground"),
                          nullptr};
    // The SERVER is what launches Terminal 1's shell, so the server's
    // environment decides which shell that is. Inherited, it is the environment
    // of whoever ran ctest — and on Debian that resolves to a LOGIN /bin/bash,
    // whose stock ~/.bashrc builds a PS1 containing an OSC 0 title sequence.
    // ckmux honours that title, renames the terminal, and every assertion in
    // this suite that names "Terminal 1" then looks for a string the screen no
    // longer holds. Six failures across four tests on Linux, none on macOS,
    // from a shell nobody chose.
    //
    // Reader::start already pins exactly these values for the CLIENT half of
    // the rig. Pinning them here too means both halves agree on shell, home and
    // locale instead of one of them inheriting the developer's login.
    (void)::setenv("SHELL", "/bin/sh", 1);
    (void)::setenv("HOME", "/tmp", 1);
    (void)::setenv("LC_ALL", "C", 1);
    // STDOUT as well as STDERR, and this is not tidiness — it is why the suite
    // could pass when run by hand and time out under CTest on the same machine
    // in the same second. CTest does not wait for the test PROCESS; it waits for
    // the test's output pipe to reach end-of-file. A server forked here inherits
    // that pipe, and so does the shell the server forks for the terminal. One
    // server outliving its test therefore holds the pipe open forever, and CTest
    // reports a timeout for a binary that finished long ago — a report about the
    // plumbing that reads exactly like a report about the code.
    //
    // Pointing both at /dev/null severs that entirely: nothing this server or
    // its descendants inherit can hold the runner's pipe. Nothing is lost, since
    // the server's own diagnostics were already going to /dev/null.
    const int null_fd = ::open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
        (void)::dup2(null_fd, STDOUT_FILENO);
        (void)::dup2(null_fd, STDERR_FILENO);
        (void)::close(null_fd);
    }
    ::execv(program.c_str(), argv);
    ::_exit(127);
}

inline bool wait_for_socket(const std::filesystem::path& socket, int budget_ms = 5000) {
    const clock_type::time_point deadline = clock_type::now() + std::chrono::milliseconds(budget_ms);
    for (;;) {
        ckm::platform::ConnectResult probe = ckm::platform::connect_to_server(socket);
        if (probe.status == ckm::platform::ConnectStatus::Connected) {
            (void)::close(probe.fd);
            return true;
        }
        if (clock_type::now() >= deadline) return false;
        ::usleep(5000);
    }
}

inline void end_process(::pid_t child) {
    if (child <= 0) return;
    (void)::kill(child, SIGTERM);
    const clock_type::time_point deadline = clock_type::now() + std::chrono::milliseconds(3000);
    for (;;) {
        int status = 0;
        if (::waitpid(child, &status, WNOHANG) != 0) return;
        if (clock_type::now() >= deadline) {
            (void)::kill(child, SIGKILL);
            (void)::waitpid(child, &status, 0);
            return;
        }
        ::usleep(5000);
    }
}

// The real client, in a terminal, with its screen decoded — a reader's ckmux.
struct Reader {
    std::unique_ptr<ckv::term::PosixTerminalSubsession> client;

    // What the client is told its HOST can do. Settable because WP-21 §3's
    // Sixel row is about a host that cannot render a picture: the program
    // draws unconditionally — `img2sixel 1.10.5` sends no probe at all
    // (§3.1) — so it is ckmux, not the child's manners, that has to keep a
    // non-graphical screen clean. A test that could not lower this could only
    // ever exercise the easy half.
    ckv::term::TerminalCapabilityProfile host_profile =
        ckv::term::embedded_xterm_sixel_profile();

    // The host terminal's own size. Defaulted to something roomy so every
    // existing case is unchanged, and settable because the interesting
    // question for a dialog is what happens when the screen is SMALL — the
    // case no arithmetic test can ask, because an assertion over known cell
    // heights has already decided the content's size before asking it.
    // `extra_environment` is how a test says something to the client that only
    // its environment can carry — `CKMUX_CONFIG` above all, which is the one
    // way to give a client its own configuration without writing into the
    // shared HOME every other e2e suite is also using.
    bool start(const std::filesystem::path& socket, ckv::Size cells = ckv::Size{110, 32},
               const std::vector<std::pair<std::string, std::string>>& extra_environment = {}) {
        ckv::term::TerminalLaunchSpec spec =
            ckv::term::TerminalLaunchSpec::program(binary_path().string(), {});
        spec.working_directory = "/tmp";
        spec.environment = {{"TERM", "xterm-256color"}, {"PATH", "/usr/bin:/bin"},
                            {"SHELL", "/bin/sh"},       {"HOME", "/tmp"},
                            {"LC_ALL", "C"},            {"CKMUX_SOCKET", socket.string()}};
        for (const std::pair<std::string, std::string>& entry : extra_environment)
            spec.environment.push_back({entry.first, entry.second});
        spec.profile = host_profile;
        spec.profile.cells = cells;
        spec.profile.cell_pixels = ckv::Size{9, 18};
        spec.exit_policy = ckv::core::TerminalExitPolicy::TerminateAfterGrace;
        ckv::term::TerminalSubsessionOptions options;
        options.max_output_bytes = 1u << 20u;
        options.max_parser_work_per_step = 256u << 10u;
        client = ckv::term::PosixTerminalSubsession::launch(spec, options);
        return client != nullptr;
    }

    void press(std::string_view keys) {
        client->send_input(keys);
        settle(400);
    }

    void settle(int ms) {
        const clock_type::time_point until = clock_type::now() + std::chrono::milliseconds(ms);
        while (clock_type::now() < until) {
            (void)client->drain(64 * 1024);
            ::usleep(10000);
        }
    }

    std::string screen() {
        std::string text;
        const ckv::core::TerminalSnapshot snapshot = client->snapshot();
        for (const ckv::Cell& cell : snapshot.cell_buffer)
            if (!cell.is_continuation()) text += cell.grapheme();
        return text;
    }

    // The screen as rows, which is how a reader sees it — and the only way to
    // ask "are these two things side by side".
    std::vector<std::string> rows() {
        std::vector<std::string> lines;
        const ckv::core::TerminalSnapshot snapshot = client->snapshot();
        for (int y = 0; y < snapshot.cells.height; ++y) {
            std::string line;
            for (int x = 0; x < snapshot.cells.width; ++x) {
                const ckv::Cell& cell =
                    snapshot.cell_buffer[static_cast<std::size_t>(y * snapshot.cells.width + x)];
                if (!cell.is_continuation()) line += cell.grapheme();
            }
            lines.push_back(std::move(line));
        }
        return lines;
    }

    bool sees(std::string_view needle, int budget_ms = 6000) {
        const clock_type::time_point deadline =
            clock_type::now() + std::chrono::milliseconds(budget_ms);
        for (;;) {
            (void)client->drain(64 * 1024);
            if (screen().find(needle) != std::string::npos) return true;
            if (clock_type::now() >= deadline) return false;
            ::usleep(20000);
        }
    }

    bool gone(int budget_ms = 5000) {
        const clock_type::time_point deadline =
            clock_type::now() + std::chrono::milliseconds(budget_ms);
        while (clock_type::now() < deadline) {
            (void)client->drain(64 * 1024);
            if (client->state() == ckv::core::TerminalSubsessionState::Exited) return true;
            ::usleep(20000);
        }
        return false;
    }

    void quit() {
        if (client != nullptr) client->close();
    }

    // The cell holding `needle`'s first character, found in the real decoded
    // grid rather than the flattened `rows()`/`screen()` text: a string
    // index skips continuation cells, so a wide glyph anywhere before the
    // target on the same row would silently shift a string-based index off
    // by a column.
    std::optional<std::pair<int, int>> find_cell(std::string_view needle) {
        const ckv::core::TerminalSnapshot snapshot = client->snapshot();
        for (int y = 0; y < snapshot.cells.height; ++y) {
            std::string text;
            std::vector<int> col_of;
            for (int x = 0; x < snapshot.cells.width; ++x) {
                const ckv::Cell& cell = snapshot.cell_buffer[static_cast<std::size_t>(y) *
                                                              static_cast<std::size_t>(
                                                                  snapshot.cells.width) +
                                                              static_cast<std::size_t>(x)];
                if (cell.is_continuation()) continue;
                const std::string_view grapheme = cell.grapheme();
                for (std::size_t i = 0; i < grapheme.size(); ++i) col_of.push_back(x);
                text += grapheme;
            }
            const std::size_t at = text.find(needle);
            if (at != std::string::npos) return std::make_pair(y, col_of[at]);
        }
        return std::nullopt;
    }

    // A real SGR left click — press then release, 1-based coordinates,
    // exactly what a real host terminal sends for a real click.
    void click(int row, int col) {
        press("\x1b[<0;" + std::to_string(col + 1) + ";" + std::to_string(row + 1) + "M");
        press("\x1b[<0;" + std::to_string(col + 1) + ";" + std::to_string(row + 1) + "m");
    }

    // And the right button (SGR button 2), which is what a reader's terminal
    // sends for a context-menu press.
    void right_click(int row, int col) {
        press("\x1b[<2;" + std::to_string(col + 1) + ";" + std::to_string(row + 1) + "M");
        press("\x1b[<2;" + std::to_string(col + 1) + ";" + std::to_string(row + 1) + "m");
    }
};

}  // namespace ckmtest
