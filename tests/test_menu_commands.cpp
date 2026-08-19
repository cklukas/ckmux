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
#include <optional>
#include <string>
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

    // ^B v — Tile Vertically: full-height bands side by side, so both windows
    // are on screen at once and the two titles share a row. This is exactly
    // the arrangement `^B t` produced before WP-31; what changed is that the
    // menu and the key now say WHICH tiling a reader is asking for, and the
    // unqualified one — the same arrangement under a second name — is gone.
    reader.press("\x02" "v");
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
        // One spelling for one key: the accelerator read "^B C-b" while the
        // two halves are the same chord, because the prefix and the chord
        // after it were written by two different rules (plans/06 writes ^B).
        CK_CHECK(screen.find("^B ^B") != std::string::npos);
    }

    // → the Window menu, which is where the reader looks for the tilings and
    // where they were plainly missing while ckVision already had them
    // (WP-31). Three arrangements, three names, three chords — and no fourth
    // entry offering one of them a second time under the bare word `Tile`,
    // which is why `^B t` is not among the accelerators on this menu.
    reader.press("\x1b[C");
    CK_CHECK(reader.sees("Tile Horizontally"));
    {
        const std::string screen = reader.screen();
        CK_CHECK(screen.find("Tile Vertically") != std::string::npos);
        CK_CHECK(screen.find("Tile Grid") != std::string::npos);
        CK_CHECK(screen.find("Cascade") != std::string::npos);
        CK_CHECK(screen.find("^B h") != std::string::npos);
        CK_CHECK(screen.find("^B v") != std::string::npos);
        CK_CHECK(screen.find("^B g") != std::string::npos);
        CK_CHECK(screen.find("^B t") == std::string::npos);
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

CK_TEST(closing_the_last_terminal_tells_the_whole_truth_and_ends_the_session) {
    // The reworked ^B x, end to end: the dialog explains what closing does,
    // offers the kill checkbox and the move alternative, and — because this
    // is the session's last terminal — says the session ends with it. Enter
    // takes the default: asked to quit, killed after the grace only if it
    // lingers. The shell obliges the asking, kill-empty-session ends the
    // emptied session, the server goes with its last session, and the client
    // lands on the picker. Every claim in the dialog, watched coming true.
    const std::filesystem::path socket = private_socket("closedialog");
    forget(socket);
    if (binary_path().empty()) return;
    const ::pid_t server = start_server(socket);
    CK_CHECK(wait_for_socket(socket));

    Reader reader;
    CK_CHECK(reader.start(socket));
    CK_CHECK(reader.sees("Terminal 1"));

    reader.press("\x02" "x");
    CK_CHECK(reader.sees("Close terminal"));
    CK_CHECK(reader.screen().find("asks it to quit") != std::string::npos);
    CK_CHECK(reader.screen().find("Kill it if it has not quit") != std::string::npos);
    CK_CHECK(reader.screen().find("Move instead") != std::string::npos);
    CK_CHECK(reader.screen().find("This is the last terminal") != std::string::npos);
    reader.press("\r");

    // The shell dies on the asking, its session dies empty, and the client is
    // still a running ckmux: the picker, saying what a reader can do next.
    CK_CHECK(reader.sees("No sessions are running yet"));

    const clock_type::time_point deadline = clock_type::now() + std::chrono::milliseconds(4000);
    bool server_gone = false;
    while (!server_gone && clock_type::now() < deadline) {
        int status = 0;
        server_gone = ::waitpid(server, &status, WNOHANG) == server;
        if (!server_gone) ::usleep(20000);
    }
    CK_CHECK(server_gone);
    reader.quit();
    forget(socket);
}

CK_TEST(moving_the_last_terminal_rescues_the_program_into_a_new_session) {
    // The dialog's other answer, end to end: ^B x, Alt+M instead of Enter,
    // "a new session" in the picker — and the SAME shell is still running
    // afterwards, reachable through the session picker, with the emptied
    // session gone. The shell variable is the proof of identity: a fresh
    // shell would not know it.
    const std::filesystem::path socket = private_socket("movedialog");
    forget(socket);
    if (binary_path().empty()) return;
    const ::pid_t server = start_server(socket);
    CK_CHECK(wait_for_socket(socket));

    Reader reader;
    CK_CHECK(reader.start(socket));
    CK_CHECK(reader.sees("Terminal 1"));
    reader.press("export SURVIVOR=yes\n");
    CK_CHECK(reader.sees("SURVIVOR=yes"));

    reader.press("\x02" "x");
    CK_CHECK(reader.sees("Close terminal"));
    reader.press("\x1bm");  // Alt+M: the Move instead… button
    CK_CHECK(reader.sees("Move terminal"));
    CK_CHECK(reader.screen().find("a new session") != std::string::npos);
    reader.press("\r");

    // The old session emptied and ended, so the picker appears — offering
    // the created session, which holds the moved terminal.
    CK_CHECK(reader.sees("1 terminal"));
    reader.press("\r");  // attach to it

    // The window comes back with the screen it had before the move — the
    // export is still on it. Not a prompt-shape assert: whose shell runs the
    // test is not this test's business.
    CK_CHECK(reader.sees("export SURVIVOR=yes"));
    reader.press("echo RESCUED-$SURVIVOR\n");
    CK_CHECK(reader.sees("RESCUED-yes"));

    // End the rescued session so nothing outlives the test.
    reader.press("\x02" "K");
    CK_CHECK(reader.sees("End session"));
    reader.press("\r");
    reader.settle(800);
    reader.quit();
    const clock_type::time_point deadline = clock_type::now() + std::chrono::milliseconds(4000);
    bool server_gone = false;
    while (!server_gone && clock_type::now() < deadline) {
        int status = 0;
        server_gone = ::waitpid(server, &status, WNOHANG) == server;
        if (!server_gone) ::usleep(20000);
    }
    CK_CHECK(server_gone);
    forget(socket);
}

CK_TEST(a_mouse_click_on_the_terminal_report_close_button_closes_it) {
    // Field report (2026-08-18, night): a reader clicking inside ckmux says
    // nothing responds, even though the Terminal Report's own diagnostics show
    // "Mouse events seen" and "Mouse reports decoded" at healthy, matching
    // counts. plans/13's field-defects note ruled out several hypotheses via
    // synthetic harnesses (direct on_mouse calls, a HeadlessTerminal dispatch)
    // that check bytes reach `proto::Input` — none of them puts a real SGR
    // byte sequence through a real PTY-hosted client and checks that a real
    // rendered widget reacts, which is the doc's own named next step. This
    // does that: open ckmux's own Terminal Report dialog by keyboard (a
    // trusted path, per every_prefix_command... above), find where "Close"
    // actually rendered, and click it the way a reader's terminal would.
    const std::filesystem::path socket = private_socket("mouseclick");
    forget(socket);
    if (binary_path().empty()) return;
    const ::pid_t server = start_server(socket);
    CK_CHECK(wait_for_socket(socket));

    Reader reader;
    CK_CHECK(reader.start(socket));
    CK_CHECK(reader.sees("Terminal 1"));

    // ^B m focuses the menu bar on Session; Down opens it. Four Rights walk
    // the bar to Help (Session, Terminal, Window, Settings, Help), opening
    // each as they pass it — the same navigation
    // every_prefix_command_a_reader_can_press_does_what_it_says already
    // trusts. One more Down reaches Terminal Report under Key Reference;
    // Enter activates it.
    reader.press("\x02" "m");
    reader.press("\x1b[B");
    reader.press("\x1b[C\x1b[C\x1b[C\x1b[C");
    reader.press("\x1b[B");
    reader.press("\r");
    CK_CHECK(reader.sees("Terminal report"));

    const std::optional<std::pair<int, int>> close = reader.find_cell("Close");
    CK_CHECK(close.has_value());
    if (!close.has_value()) {
        reader.quit();
        end_process(server);
        forget(socket);
        return;
    }

    // Aimed a couple of cells into the word so a one-cell aim error still
    // lands on the button rather than its edge.
    reader.click(close->first, close->second + 2);
    reader.settle(400);

    CK_CHECK(reader.screen().find("Terminal report") == std::string::npos);

    reader.quit();
    end_process(server);
    forget(socket);
}

CK_TEST(a_shift_click_on_ckmuxs_own_button_still_activates_it) {
    // TerminalView::on_mouse reserves Shift+Left for host-side selection —
    // but ckmux's own dialogs are plain widgets::Button, not TerminalView,
    // so Shift has no special meaning to them and a click is a click. Pins
    // that asymmetry against the sibling case below, where Shift+Left over a
    // pane's content IS reserved and does NOT reach the pane's child.
    const std::filesystem::path socket = private_socket("shiftclick");
    forget(socket);
    if (binary_path().empty()) return;
    const ::pid_t server = start_server(socket);
    CK_CHECK(wait_for_socket(socket));

    Reader reader;
    CK_CHECK(reader.start(socket));
    CK_CHECK(reader.sees("Terminal 1"));

    reader.press("\x02" "m");
    reader.press("\x1b[B");
    reader.press("\x1b[C\x1b[C\x1b[C\x1b[C");
    reader.press("\x1b[B");
    reader.press("\r");
    CK_CHECK(reader.sees("Terminal report"));

    const std::optional<std::pair<int, int>> close = reader.find_cell("Close");
    CK_CHECK(close.has_value());
    if (close.has_value()) {
        const int col = close->second + 2 + 1;
        const int row = close->first + 1;
        // SGR button byte 4 = Shift held (encode_mouse: button |= 4).
        reader.press("\x1b[<4;" + std::to_string(col) + ";" + std::to_string(row) + "M");
        reader.press("\x1b[<4;" + std::to_string(col) + ";" + std::to_string(row) + "m");
        reader.settle(400);
        CK_CHECK(reader.screen().find("Terminal report") == std::string::npos);
    }

    reader.quit();
    end_process(server);
    forget(socket);
}

CK_TEST(a_mouse_click_reaches_a_real_child_apps_own_button) {
    // The remaining untested leg (plans/13's field-defects note, and the
    // sibling test above): a click on ckmux's OWN chrome now has a real,
    // real-PTY regression test. What nobody has driven end to end is the
    // M2 seam itself — a click forwarded through `TerminalView::encode_mouse`
    // and the wire to a SEPARATE real process's OWN ckVision `Application`,
    // hit-tested and dispatched entirely inside that other process. The
    // existing remote-terminal tests stop at "bytes reached `proto::Input`";
    // this drives a second real process (ckVision's own `hello` example,
    // chosen for its one-button message box) inside a ckmux pane and clicks
    // its OK button for real.
    const std::filesystem::path hello =
        "/Volumes/PRO-BLADE/git/ckvision/build/examples/ckvision_hello";
    if (!std::filesystem::exists(hello)) return;  // built from a sibling checkout; not everywhere

    const std::filesystem::path socket = private_socket("mouseclick2");
    forget(socket);
    if (binary_path().empty()) return;
    const ::pid_t server = start_server(socket);
    CK_CHECK(wait_for_socket(socket));

    Reader reader;
    CK_CHECK(reader.start(socket));
    CK_CHECK(reader.sees("Terminal 1"));

    // Plain text into the shell ckmux opened for Terminal 1's PTY — no
    // prefix chord involved, so none of ckmux's own key-stealing applies.
    reader.press(hello.string() + "\r");
    CK_CHECK(reader.sees("File"));

    // Alt+G, the greeting command's own chord — Alt/Esc sequences are named
    // in plans/06's mouse-rules table as flowing through untouched, unlike
    // the ^B prefix, so this reaches the child with no escaping.
    reader.press("\x1b" "g");
    CK_CHECK(reader.sees("How are you?"));

    const std::optional<std::pair<int, int>> ok = reader.find_cell("OK");
    CK_CHECK(ok.has_value());
    if (!ok.has_value()) {
        reader.quit();
        end_process(server);
        forget(socket);
        return;
    }

    reader.click(ok->first, ok->second + 1);
    reader.settle(400);

    CK_CHECK(reader.screen().find("How are you?") == std::string::npos);

    reader.press("\x1b" "x");  // Alt+X: the example's own Exit, tidying its pane
    reader.quit();
    end_process(server);
    forget(socket);
}

CK_TEST(a_shift_click_into_a_pane_is_reserved_for_selection_not_forwarded) {
    // Field report (2026-08-18): a reader's click was seen and decoded at
    // the byte-stream layer on both sides of the wire, yet a real child
    // app's button never reacted. Neither of the two seams the plain click
    // above already proved sound (encode-and-forward; a second process's own
    // hit-test) is where that gap can live — so this checks the one branch
    // ahead of both that a live report can hit without anyone intending to:
    // TerminalView::on_mouse reserves Shift+Left for host-side selection,
    // unconditionally, before mouse-reporting is even consulted, and does
    // NOT forward it — which reads, from the reader's side, exactly like "I
    // clicked and nothing happened," regardless of whether the Shift was a
    // deliberate drag-to-select or a modifier the terminal/input path added
    // on its own.
    const std::filesystem::path hello =
        "/Volumes/PRO-BLADE/git/ckvision/build/examples/ckvision_hello";
    if (!std::filesystem::exists(hello)) return;

    const std::filesystem::path socket = private_socket("shiftclick2");
    forget(socket);
    if (binary_path().empty()) return;
    const ::pid_t server = start_server(socket);
    CK_CHECK(wait_for_socket(socket));

    Reader reader;
    CK_CHECK(reader.start(socket));
    CK_CHECK(reader.sees("Terminal 1"));

    reader.press(hello.string() + "\r");
    CK_CHECK(reader.sees("File"));
    reader.press("\x1b" "g");
    CK_CHECK(reader.sees("How are you?"));

    const std::optional<std::pair<int, int>> ok = reader.find_cell("OK");
    CK_CHECK(ok.has_value());
    if (ok.has_value()) {
        const int col = ok->second + 1 + 1;
        const int row = ok->first + 1;
        // SGR button byte 4 = Shift held.
        reader.press("\x1b[<4;" + std::to_string(col) + ";" + std::to_string(row) + "M");
        reader.press("\x1b[<4;" + std::to_string(col) + ";" + std::to_string(row) + "m");
        reader.settle(400);
        // Reserved for selection, not forwarded: the box is still up.
        CK_CHECK(reader.screen().find("How are you?") != std::string::npos);
    }

    reader.quit();
    end_process(server);
    forget(socket);
}
