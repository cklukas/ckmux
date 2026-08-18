// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// Every command a reader can reach, driven the way a reader reaches it.
//
// This file exists because of a report that was entirely fair: "it seems the
// app is like a prototype where you say it's done but actually nothing works or
// is tested". Everything under it was tested — the protocol, the mirror, the
// diff engine, the server — and the tests that covered the COMMANDS asked
// whether a handler existed and whether a window appeared. A handler that opens
// a dialog nobody can see passes both. So does a menu item wired to a callback
// that was never set.
//
// So these tests run the real `ckmux` binary, in a real terminal, against a real
// server, press the keys, and read the screen. They are slower than the rest of
// the suite and they are the only ones that can say a reader's ckmux works.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "platform/socket.hpp"

#include "cvision/term/posix_terminal_subsession.hpp"
#include "cvision/testing/cktest.hpp"

namespace {

using clock_type = std::chrono::steady_clock;

std::filesystem::path binary_path() {
#if defined(CKMUX_BINARY_PATH)
    return std::filesystem::path(CKMUX_BINARY_PATH);
#else
    return {};
#endif
}

std::filesystem::path private_socket(std::string_view name) {
    const char* const base = std::getenv("TMPDIR");
    std::filesystem::path directory =
        std::filesystem::path(base != nullptr && *base != '\0' ? base : "/tmp");
    directory /= "ckmux-m" + std::to_string(static_cast<unsigned long>(::getpid()));
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    return directory / (std::string(name) + ".sock");
}

void forget(const std::filesystem::path& socket) {
    std::error_code ignored;
    std::filesystem::remove(socket, ignored);
    std::filesystem::remove(std::filesystem::path(socket.string() + ".lock"), ignored);
    std::filesystem::remove(std::filesystem::path(socket.string() + ".log"), ignored);
    std::filesystem::remove(socket.parent_path(), ignored);
}

::pid_t start_server(const std::filesystem::path& socket) {
    const ::pid_t child = ::fork();
    if (child != 0) return child;
    const std::string program = binary_path().string();
    const std::string path = socket.string();
    char* const argv[] = {const_cast<char*>(program.c_str()), const_cast<char*>("--server"),
                          const_cast<char*>(path.c_str()), const_cast<char*>("--foreground"),
                          nullptr};
    const int null_fd = ::open("/dev/null", O_WRONLY);
    if (null_fd >= 0) {
        (void)::dup2(null_fd, STDERR_FILENO);
        (void)::close(null_fd);
    }
    ::execv(program.c_str(), argv);
    ::_exit(127);
}

bool wait_for_socket(const std::filesystem::path& socket, int budget_ms = 5000) {
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

void end_process(::pid_t child) {
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

    bool start(const std::filesystem::path& socket) {
        ckv::term::TerminalLaunchSpec spec =
            ckv::term::TerminalLaunchSpec::program(binary_path().string(), {});
        spec.working_directory = "/tmp";
        spec.environment = {{"TERM", "xterm-256color"}, {"PATH", "/usr/bin:/bin"},
                            {"SHELL", "/bin/sh"},       {"HOME", "/tmp"},
                            {"LC_ALL", "C"},            {"CKMUX_SOCKET", socket.string()}};
        spec.profile = ckv::term::embedded_xterm_sixel_profile();
        spec.profile.cells = ckv::Size{110, 32};
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
};

}  // namespace

CK_TEST(every_prefix_command_a_reader_can_press_does_what_it_says) {
    // One client, one pass through the keys on the footer and in the menus. Each
    // step asserts something a reader would SEE, because that is the only thing
    // any of this is for.
    const std::filesystem::path socket = private_socket("menus");
    forget(socket);
    if (binary_path().empty()) return;
    const ::pid_t server = start_server(socket);
    CK_CHECK(wait_for_socket(socket));

    Reader reader;
    CK_CHECK(reader.start(socket));
    CK_CHECK(reader.sees("Terminal 1"));

    // ^B c — a second terminal, numbered and NOT on top of the first. Both of
    // those were wrong: every remote window was called "Terminal" and every one
    // of them landed at the same spot, so two terminals looked like one.
    reader.press("\x02" "c");
    CK_CHECK(reader.sees("Terminal 2"));
    CK_CHECK(reader.screen().find("Terminal 1") != std::string::npos);

    // ^B w — the window list, with both windows in it. It shipped listing
    // nothing at all.
    reader.press("\x02" "w");
    CK_CHECK(reader.sees("Window List"));
    {
        const std::string screen = reader.screen();
        const std::size_t first = screen.find("Terminal 1");
        const std::size_t second = screen.find("Terminal 2");
        CK_CHECK(first != std::string::npos);
        CK_CHECK(second != std::string::npos);
    }
    reader.press("\x1b");  // Esc closes it

    // ^B s — the sessions picker: what is running, which one this client is
    // watching, and the three things a reader may do about it. It shipped as a
    // greyed menu item, and then as a list that could only be read.
    reader.press("\x02" "s");
    CK_CHECK(reader.sees("Sessions"));
    CK_CHECK(reader.screen().find("(this client)") != std::string::npos);
    CK_CHECK(reader.screen().find("2 terminals") != std::string::npos);
    CK_CHECK(reader.screen().find("Attach") != std::string::npos);
    // No create button here: the picker chooses among what exists, and New
    // Session lives on the Session menu.
    CK_CHECK(reader.screen().find("New session") == std::string::npos);
    reader.press("\x1b");

    // ^B t — tiled: both windows on screen at once, side by side, so the two
    // titles share a row.
    reader.press("\x02" "t");
    reader.settle(400);
    {
        bool shared_row = false;
        for (const std::string& line : reader.rows())
            if (line.find("Terminal 1") != std::string::npos &&
                line.find("Terminal 2") != std::string::npos)
                shared_row = true;
        CK_CHECK(shared_row);
    }

    // The frame scrollbar — on the window's right border exactly once the
    // shell has printed more rows than the window shows (plans/06 mouse
    // rules; ckVision D-051), where it costs the terminal nothing.
    reader.press("i=0; while [ $i -lt 80 ]; do echo scroll-line-$i; i=$((i+1)); done\r");
    CK_CHECK(reader.sees("scroll-line-79"));
    CK_CHECK(reader.sees("▲"));
    CK_CHECK(reader.screen().find("▼") != std::string::npos);

    // ^B [ — copy mode, which says so on the window and in the footer.
    reader.press("\x02" "[");
    CK_CHECK(reader.sees("COPY"));
    reader.press("q");  // and leaves it

    // ^B m — the menu bar, and Down opens the menu under it with the commands
    // this milestone actually has.
    reader.press("\x02" "m");
    reader.press("\x1b[B");
    CK_CHECK(reader.sees("New Session"));
    CK_CHECK(reader.screen().find("Detach") != std::string::npos);
    CK_CHECK(reader.screen().find("Sessions") != std::string::npos);
    CK_CHECK(reader.screen().find("Quit ckmux") != std::string::npos);
    // And no New Terminal here: that item is the Terminal menu's, once.
    CK_CHECK(reader.screen().find("New Terminal") == std::string::npos);

    // → the Terminal menu. Rule 2 of plans/06: everything is in the menu,
    // with the chord that reaches it beside it — Copy Mode, Paste and Send
    // Prefix each carry their prefix chord, and Send Prefix's chord is the
    // prefix itself.
    reader.press("\x1b[C");
    CK_CHECK(reader.sees("Copy Mode"));
    {
        const std::string screen = reader.screen();
        CK_CHECK(screen.find("^B [") != std::string::npos);
        CK_CHECK(screen.find("Paste") != std::string::npos);
        CK_CHECK(screen.find("^B ]") != std::string::npos);
        CK_CHECK(screen.find("Send Prefix to Program") != std::string::npos);
        CK_CHECK(screen.find("^B C-b") != std::string::npos);
    }
    reader.press("\x1b");

    reader.quit();
    end_process(server);
    forget(socket);
}

CK_TEST(ending_the_last_session_stops_the_server_and_the_client_offers_a_fresh_start) {
    // The whole lifecycle the Session menu promises, in one pass: ^B K opens
    // the End session confirmation with its kill checkbox, Enter ends the
    // session, the SERVER exits with its last session — a server with nothing
    // to serve is a process holding a socket for nobody — and the client
    // outlives it: an empty desktop, and the picker offering a new session
    // rather than a corpse or a crash.
    const std::filesystem::path socket = private_socket("endsession");
    forget(socket);
    if (binary_path().empty()) return;
    const ::pid_t server = start_server(socket);
    CK_CHECK(wait_for_socket(socket));

    Reader reader;
    CK_CHECK(reader.start(socket));
    CK_CHECK(reader.sees("Terminal 1"));

    reader.press("\x02" "K");
    CK_CHECK(reader.sees("End session"));
    CK_CHECK(reader.screen().find("asked to quit") != std::string::npos);
    CK_CHECK(reader.screen().find("Kill anything still running") != std::string::npos);
    reader.press("\r");

    // The session is gone, the server goes with it, and the client is still a
    // running ckmux: no session, so the picker offers what a reader can do.
    CK_CHECK(reader.sees("No sessions are running yet"));
    // The empty picker says where to go, rather than growing a button the
    // populated one does not have.
    CK_CHECK(reader.screen().find("New Session") != std::string::npos);

    const clock_type::time_point deadline = clock_type::now() + std::chrono::milliseconds(4000);
    bool server_gone = false;
    while (!server_gone && clock_type::now() < deadline) {
        int status = 0;
        server_gone = ::waitpid(server, &status, WNOHANG) == server;
        if (!server_gone) ::usleep(20000);
    }
    CK_CHECK(server_gone);

    // Creating a session — Session menu, or ^B S — starts a fresh server:
    // "end the last session, then start another" is one uninterrupted thing.
    reader.press("\x1b");  // leave the picker; the menu is where New Session lives
    reader.press("\x02" "S");
    CK_CHECK(reader.sees("New session"));
    reader.press("\r");
    CK_CHECK(reader.sees("Terminal 1"));
    reader.press("echo FRESH-SERVER\n");
    CK_CHECK(reader.sees("FRESH-SERVER"));

    reader.quit();
    // The respawned server holds the new session; end it so nothing leaks.
    Reader closer;
    CK_CHECK(closer.start(socket));
    CK_CHECK(closer.sees("Terminal 1"));
    closer.press("\x02" "K");
    CK_CHECK(closer.sees("End session"));
    closer.press("\r");
    closer.settle(800);
    closer.quit();
    forget(socket);
}

CK_TEST(quitting_ckmux_leaves_the_programs_running_and_asks_nothing) {
    // The report that started this: quitting ckmux with `htop` running asked
    // "Terminal is still running a program. Close it anyway?" — a question about
    // the wrong thing entirely. The program is not being closed; the view of it
    // is. Quitting an attached client detaches it.
    const std::filesystem::path socket = private_socket("quit");
    forget(socket);
    if (binary_path().empty()) return;
    const ::pid_t server = start_server(socket);
    CK_CHECK(wait_for_socket(socket));

    Reader reader;
    CK_CHECK(reader.start(socket));
    CK_CHECK(reader.sees("Terminal 1"));
    reader.press("echo MARKER-BEFORE-QUIT\n");
    CK_CHECK(reader.sees("MARKER-BEFORE-QUIT"));
    reader.press("sleep 300\n");  // something plainly still running

    reader.press("\x02" "q");
    CK_CHECK(reader.gone());
    // Not one word about closing anything.
    CK_CHECK(reader.screen().find("still running a program") == std::string::npos);

    // And the terminal is still there, with what was on it, for the next client.
    Reader again;
    CK_CHECK(again.start(socket));
    CK_CHECK(again.sees("MARKER-BEFORE-QUIT"));
    again.press("\x02" "d");  // ^B d detaches too
    CK_CHECK(again.gone());

    end_process(server);
    forget(socket);
}

CK_TEST(restarting_ckmux_restores_the_session_without_gaining_a_terminal) {
    // What a reader expects on restart: their session, as they left it. What
    // ckmux did instead was hand it back AND open a fresh terminal every time,
    // because the client opened one at startup the way a single-process
    // application would — so a reader gained a stray shell per restart.
    const std::filesystem::path socket = private_socket("restart");
    forget(socket);
    if (binary_path().empty()) return;
    const ::pid_t server = start_server(socket);
    CK_CHECK(wait_for_socket(socket));

    Reader first;
    CK_CHECK(first.start(socket));
    CK_CHECK(first.sees("Terminal 1"));
    first.press("\x02" "c");
    CK_CHECK(first.sees("Terminal 2"));
    first.press("echo MARKER-IN-SECOND\n");
    CK_CHECK(first.sees("MARKER-IN-SECOND"));
    first.press("\x02" "d");
    CK_CHECK(first.gone());

    Reader second;
    CK_CHECK(second.start(socket));
    // Both terminals, with their content.
    CK_CHECK(second.sees("MARKER-IN-SECOND"));
    CK_CHECK(second.screen().find("Terminal 1") != std::string::npos);
    CK_CHECK(second.screen().find("Terminal 2") != std::string::npos);
    // And no third one: the session had two, and attaching to a session is not
    // a reason to start anything.
    CK_CHECK(second.screen().find("Terminal 3") == std::string::npos);
    second.press("\x02" "s");
    CK_CHECK(second.sees("2 terminals"));
    second.press("\x1b");
    second.press("\x02" "d");
    CK_CHECK(second.gone());

    end_process(server);
    forget(socket);
}
