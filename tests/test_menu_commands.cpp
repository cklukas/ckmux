// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
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

#include "reader_harness.hpp"

#include "cvision/testing/cktest.hpp"

namespace {
using ckmtest::binary_path;
using ckmtest::clock_type;
using ckmtest::end_process;
using ckmtest::forget;
using ckmtest::private_socket;
using ckmtest::Reader;
using ckmtest::start_server;
using ckmtest::wait_for_socket;


// Opens Help ▸ Terminal Report from the keyboard, WITHOUT counting keystrokes.
//
// The old version pressed five Rights to reach Help and "one more Down" to
// reach the item, and both counts were positional: a package added a View menu
// ahead of Window, another added a third Help entry, and this walk quietly
// landed somewhere else. The failure then arrives disguised as "the terminal
// report does not open" rather than as "the menu moved".
//
// So: walk the bar until the HELP MENU'S OWN CONTENTS are on screen, bounded
// so a wrong turn ends the loop rather than the run, then press the item's
// mnemonic — which a dropdown answers by name. Both halves survive a menu
// being added anywhere.
bool open_terminal_report(Reader& reader) {
    reader.press("\x02" "m");   // focus the menu bar
    reader.press("\x1b[B");     // open the highlighted menu
    for (int steps = 0; steps < 10; ++steps) {
        if (reader.screen().find("Terminal Report") != std::string::npos) break;
        reader.press("\x1b[C");  // on to the next menu, opening it as it passes
    }
    if (reader.screen().find("Terminal Report") == std::string::npos) return false;
    reader.press("t");  // "&Terminal Report" — its own mnemonic, not its index
    return reader.sees("Terminal report");
}

}  // namespace

// Where ckVision's `hello` example actually is, according to the build that
// built it — never a guess. This used to be an absolute path into one
// developer's Mac checkout, guarded by exists(). On that machine it worked; on
// the Linux VM the guard was worse than useless, because that path is MOUNTED
// there, so the file exists — it is simply a Mach-O binary Linux cannot
// execute. The guard passed, the child never started, and two tests spent the
// full sees() budget twice over waiting for a program that was never going to
// appear. A path that exists is not a program that runs.
//
// Empty means the example is not part of this build — ckVision is built here as
// a dependency, with its examples off by default — and the caller skips.
std::filesystem::path ckvision_hello_path() {
#if defined(CKMUX_CKVISION_HELLO_PATH)
    {
        const std::filesystem::path built = CKMUX_CKVISION_HELLO_PATH;
        std::error_code ignored;
        if (std::filesystem::exists(built, ignored)) return built;
    }
#endif
    // An explicit override, for a reader who has the example built elsewhere and
    // wants these two rows to run. Opt-in on purpose: nothing is inferred.
    if (const char* const from_environment = std::getenv("CKMUX_CKVISION_HELLO");
        from_environment != nullptr && *from_environment != '\0') {
        std::error_code ignored;
        if (std::filesystem::exists(from_environment, ignored)) return from_environment;
    }
    return {};
}

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
    // shell has printed more rows than the window shows (the interface spec mouse
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

    // → the Terminal menu. Rule 2 of the interface spec: everything is in the menu,
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
        // after it were written by two different rules (the interface spec writes ^B).
        CK_CHECK(screen.find("^B ^B") != std::string::npos);
    }

    // → the View menu (WP-39): the three readout toggles, checkable and off
    // by default, each with no accelerator of its own — a display toggle is
    // not a muscle-memory operation, and an unbound command still shows in
    // the menu that reaches it.
    reader.press("\x1b[C");
    CK_CHECK(reader.sees("Show CPU Usage"));
    {
        const std::string screen = reader.screen();
        CK_CHECK(screen.find("Show Memory Usage (RSS)") != std::string::npos);
        CK_CHECK(screen.find("Show Memory Usage (Real)") != std::string::npos);
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
    // Cleanup that does not depend on the assertion passing. `CK_CHECK`
    // RECORDS a failure and carries on, so a server that did not exit on its
    // own was reported and then abandoned — and the `forget()` below unlinks
    // its socket, leaving it alive and permanently unreachable, which is how
    // fifteen of these accumulated on one machine. `end_process` is safe on a
    // pid already reaped.
    end_process(server);

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

CK_TEST(a_second_terminal_puts_a_window_bar_on_screen_above_the_footer) {
    // WP-32 on a real screen, in a real ckmux, with a real mouse. The headless
    // suite pins the geometry and the targeting; this pins the one thing it
    // cannot — that the reader SEES the row, in the place they look for it.
    // A bar composed correctly and never drawn is exactly the class of failure
    // this file exists for.
    const std::filesystem::path socket = private_socket("windowbar");
    forget(socket);
    if (binary_path().empty()) return;
    const ::pid_t server = start_server(socket);
    CK_CHECK(wait_for_socket(socket));

    Reader reader;
    CK_CHECK(reader.start(socket));
    CK_CHECK(reader.sees("Terminal 1"));

    // One terminal is nothing to switch between, so the row above the footer is
    // still bare desktop.
    {
        const std::vector<std::string> lines = reader.rows();
        CK_CHECK(lines.size() >= 2U);
        if (lines.size() < 2U) return;
        CK_CHECK(lines[lines.size() - 2].find("Terminal 1") == std::string::npos);
    }

    reader.press("\x02" "c");
    CK_CHECK(reader.sees("Terminal 2"));

    // Both captions, on one row, immediately above the footer — which is the
    // whole of "positioned above the existing footer".
    const std::vector<std::string> lines = reader.rows();
    CK_CHECK(lines.size() >= 2U);
    if (lines.size() < 2U) return;
    const std::size_t bar_row = lines.size() - 2;
    const std::size_t at = lines[bar_row].find("Terminal 1");
    CK_CHECK(at != std::string::npos);
    CK_CHECK(lines[bar_row].find("Terminal 2") != std::string::npos);
    if (at == std::string::npos) return;
    // A string index rather than `find_cell`, because the search has to be
    // confined to THIS row: "Terminal 1" is also written on the window's own
    // title bar, and find_cell would answer with that one. Safe here because
    // the row is ASCII, where an index and a column are the same number.
    const int row = static_cast<int>(bar_row);
    const int on_first = static_cast<int>(at) + 3;

    // Right-click the row of the terminal that is NOT in front. The menu is the
    // reader's own words for what can be done to it.
    reader.right_click(row, on_first);
    CK_CHECK(reader.sees("Maximize"));
    CK_CHECK(reader.screen().find("Close") != std::string::npos);
    CK_CHECK(reader.screen().find("Move to session") != std::string::npos);
    // Dismissed by pressing somewhere else, the way any menu is: the bar's own
    // empty run past the last entry will do.
    reader.click(row, 100);

    // And a plain click on that row brings the terminal forward and hands it
    // the keyboard — which is what `^B x` then proves, by naming the window it
    // is about.
    reader.click(row, on_first);
    reader.press("\x02" "x");
    CK_CHECK(reader.sees("Close terminal"));
    CK_CHECK(reader.screen().find("\"Terminal 1\" is still") != std::string::npos);
    CK_CHECK(reader.screen().find("\"Terminal 2\" is still") == std::string::npos);
    reader.press("\x1b");  // nothing closed; the click was the point

    reader.press("\x02" "d");
    CK_CHECK(reader.gone());

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
    // Cleanup that does not depend on the assertion passing. `CK_CHECK`
    // RECORDS a failure and carries on, so a server that did not exit on its
    // own was reported and then abandoned — and the `forget()` below unlinks
    // its socket, leaving it alive and permanently unreachable, which is how
    // fifteen of these accumulated on one machine. `end_process` is safe on a
    // pid already reaped.
    end_process(server);
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
    // Cleanup that does not depend on the assertion passing. `CK_CHECK`
    // RECORDS a failure and carries on, so a server that did not exit on its
    // own was reported and then abandoned — and the `forget()` below unlinks
    // its socket, leaving it alive and permanently unreachable, which is how
    // fifteen of these accumulated on one machine. `end_process` is safe on a
    // pid already reaped.
    end_process(server);
    forget(socket);
}

CK_TEST(a_mouse_click_on_the_terminal_report_close_button_closes_it) {
    // Field report (2026-08-18, night): a reader clicking inside ckmux says
    // nothing responds, even though the Terminal Report's own diagnostics show
    // "Mouse events seen" and "Mouse reports decoded" at healthy, matching
    // counts. The architecture review's field-defects note ruled out several hypotheses via
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

    CK_CHECK(open_terminal_report(reader));

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

    CK_CHECK(open_terminal_report(reader));

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
    // The remaining untested leg (the architecture review's field-defects note, and the
    // sibling test above): a click on ckmux's OWN chrome now has a real,
    // real-PTY regression test. What nobody has driven end to end is the
    // M2 seam itself — a click forwarded through `TerminalView::encode_mouse`
    // and the wire to a SEPARATE real process's OWN ckVision `Application`,
    // hit-tested and dispatched entirely inside that other process. The
    // existing remote-terminal tests stop at "bytes reached `proto::Input`";
    // this drives a second real process (ckVision's own `hello` example,
    // chosen for its one-button message box) inside a ckmux pane and clicks
    // its OK button for real.
    const std::filesystem::path hello = ckvision_hello_path();
    if (hello.empty()) return;  // ckVision's examples are not part of this build

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
    // in the interface spec's mouse-rules table as flowing through untouched, unlike
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
    const std::filesystem::path hello = ckvision_hello_path();
    if (hello.empty()) return;  // ckVision's examples are not part of this build

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

CK_TEST(the_settings_dialog_shows_its_buttons_on_a_cramped_terminal) {
    // The assertion no arithmetic test in this project can make, and the one
    // a reader made by looking: on a small host, with the window switcher bar
    // taking a row, are Save and Cancel ACTUALLY ON THE SCREEN?
    //
    // Every layout test we have pins cells and offsets against known heights
    // — which is exactly why none of them could have caught a dialog whose
    // content measures itself (wrapped notes) and so out-measures the space
    // it was given. A sibling project lost the last two paragraphs of a pane
    // for the same reason with every one of its offset assertions green; what
    // found it was running the binary on an 18-row terminal. This is that,
    // automated: real binary, real pty, buttons read off the decoded grid.
    //
    // 80x24 is the size the reader reported, and the switcher bar appears at
    // two terminals — leaving the desktop 21 rows for a dialog drawn for 22,
    // which is precisely the case U4-g's scrolling exists to survive.
    const std::filesystem::path socket = private_socket("cramped-settings");
    forget(socket);
    if (binary_path().empty()) return;
    const ::pid_t server = start_server(socket);
    CK_CHECK(wait_for_socket(socket));

    Reader reader;
    CK_CHECK(reader.start(socket, ckv::Size{80, 24}));
    CK_CHECK(reader.sees("Terminal 1"));
    // A second terminal, so the switcher bar is on screen and the desktop is
    // one row shorter than the dialog wants.
    reader.press("\x02" "c");
    CK_CHECK(reader.sees("Terminal 2"));

    // ^B m opens the menu bar on Session; four Rights reach Settings (past
    // View, WP-39); Down then Enter opens General… — the same walk the other
    // menu cases use.
    reader.press("\x02" "m");
    reader.press("\x1b[B");
    reader.press("\x1b[C\x1b[C\x1b[C\x1b[C");
    reader.press("\x1b[B");
    reader.press("\r");

    const std::string screen = reader.screen();
    // The dialog is up — its first field, which is above the fold.
    CK_CHECK(screen.find("Start terminals") != std::string::npos);
    // Its buttons are on the screen WITH it, which is the whole point: before
    // U4-g a dialog taller than the desktop simply lost its bottom rows, and
    // what a reader lost with them was every way of answering it.
    CK_CHECK(screen.find("Save") != std::string::npos);
    CK_CHECK(screen.find("Cancel") != std::string::npos);
    // And the last field is NOT on screen, which is what proves the buttons
    // above are visible because the form scrolled rather than because it
    // happened to fit. `Colour theme` is the final row of the form.
    CK_CHECK(screen.find("Colour theme") == std::string::npos);

    reader.press("\x1b");
    reader.quit();
    end_process(server);
    forget(socket);
}

CK_TEST(a_roomy_terminal_shows_the_whole_settings_form_without_scrolling) {
    // The other half of U4-g's contract, and the acceptance bar the reader
    // set: a dialog that FITS must look exactly as it did before scrolling
    // existed. Measured by sweeping terminal heights: the form fits at 26
    // rows and scrolls at 24, so this pins the fitting side of that boundary
    // while `the_settings_dialog_shows_its_buttons_on_a_cramped_terminal`
    // pins the other. Two rows apart, so neither sits on the threshold.
    const std::filesystem::path socket = private_socket("roomy-settings");
    forget(socket);
    if (binary_path().empty()) return;
    const ::pid_t server = start_server(socket);
    CK_CHECK(wait_for_socket(socket));

    Reader reader;
    CK_CHECK(reader.start(socket, ckv::Size{80, 26}));
    CK_CHECK(reader.sees("Terminal 1"));
    reader.press("\x02" "c");
    CK_CHECK(reader.sees("Terminal 2"));

    reader.press("\x02" "m");
    reader.press("\x1b[B");
    reader.press("\x1b[C\x1b[C\x1b[C\x1b[C");
    reader.press("\x1b[B");
    reader.press("\r");

    const std::string screen = reader.screen();
    // Every row of the form is on screen at once — first field, last field,
    // and the buttons. Nothing scrolled because nothing needed to.
    CK_CHECK(screen.find("Start terminals") != std::string::npos);
    CK_CHECK(screen.find("Colour theme") != std::string::npos);
    CK_CHECK(screen.find("Save") != std::string::npos);
    CK_CHECK(screen.find("Cancel") != std::string::npos);

    reader.press("\x1b");
    reader.quit();
    end_process(server);
    forget(socket);
}

CK_TEST(a_reader_names_a_terminal_and_the_name_stays_while_the_program_renames_itself) {
    // WP-36 on a real screen, in a real ckmux, against a real server. The
    // headless suites pin the precedence and the wire; this pins the one thing
    // they cannot — that a reader can reach the command, see the prompt, type a
    // name, and watch it hold while the program in that terminal goes on
    // calling itself something else.
    const std::filesystem::path socket = private_socket("rename");
    forget(socket);
    if (binary_path().empty()) return;
    const ::pid_t server = start_server(socket);
    CK_CHECK(wait_for_socket(socket));

    Reader reader;
    CK_CHECK(reader.start(socket));
    CK_CHECK(reader.sees("Terminal 1"));

    // The shell in that terminal names its own window, which is the ordinary
    // case and the thing the override has to survive.
    reader.press("printf '\\033]2;first\\007'\r");
    CK_CHECK(reader.sees("first"));

    // ^B , — the chord the interface spec gives it. The prompt starts from the caption
    // the reader can see, so it is cleared before the new name is typed.
    reader.press("\x02" ",");
    CK_CHECK(reader.sees("Rename"));
    CK_CHECK(reader.screen().find("Use Default Title") != std::string::npos);
    for (int i = 0; i < 40; ++i) reader.client->send_input("\x7f");
    reader.settle(300);
    reader.press("the important one\r");
    CK_CHECK(reader.sees("the important one"));

    // The program renames itself again and is not shown: the reader's name is
    // an override, not one more writer of the caption.
    reader.press("printf '\\033]2;second\\007'\r");
    reader.settle(600);
    CK_CHECK(reader.screen().find("the important one") != std::string::npos);

    // Use Default Title hands the caption back — to what the program is
    // calling itself NOW, not to what it said when the name was pinned.
    reader.press("\x02" ",");
    CK_CHECK(reader.sees("Use Default Title"));
    reader.press("\x1b" "d");  // the button's own mnemonic
    CK_CHECK(reader.sees("second"));
    CK_CHECK(reader.screen().find("the important one") == std::string::npos);

    reader.press("\x02" "d");
    CK_CHECK(reader.gone());
    end_process(server);
    forget(socket);
}

CK_TEST(the_window_bars_context_menu_offers_a_rename_for_the_row_that_was_clicked) {
    // The other route to the same command, and the one that must not act on
    // `active_window()`: every entry on that menu is bound to the row it
    // belongs to (ckVision's WindowSwitcherTarget), so renaming from a
    // BACKGROUND row must name the background terminal.
    const std::filesystem::path socket = private_socket("barrename");
    forget(socket);
    if (binary_path().empty()) return;
    const ::pid_t server = start_server(socket);
    CK_CHECK(wait_for_socket(socket));

    Reader reader;
    CK_CHECK(reader.start(socket));
    CK_CHECK(reader.sees("Terminal 1"));
    reader.press("\x02" "c");
    CK_CHECK(reader.sees("Terminal 2"));

    const std::vector<std::string> lines = reader.rows();
    CK_CHECK(lines.size() >= 2U);
    if (lines.size() < 2U) return;
    const std::size_t bar_row = lines.size() - 2;
    const std::size_t at = lines[bar_row].find("Terminal 1");
    CK_CHECK(at != std::string::npos);
    if (at == std::string::npos) return;

    // Right-click the row of the terminal that is NOT in front.
    reader.right_click(static_cast<int>(bar_row), static_cast<int>(at) + 3);
    CK_CHECK(reader.sees("Rename"));
    // Clicked, not typed. This popup does not take the keyboard — the existing
    // window-bar case dismisses it with a click for the same reason — so the
    // reader's route to an item on it is the pointer.
    //
    // `find_cell` rather than an index into the row: the popup sits on a
    // shaded desktop and inside a window frame, so the bytes before the word
    // are not the columns before it, and "Rename…" is the only place that word
    // appears on this screen.
    const std::optional<std::pair<int, int>> entry = reader.find_cell("Rename…");
    CK_CHECK(entry.has_value());
    if (!entry) return;
    reader.click(entry->first, entry->second);
    CK_CHECK(reader.sees("Use Default Title"));
    for (int i = 0; i < 40; ++i) reader.client->send_input("\x7f");
    reader.settle(300);
    reader.press("the background one\r");
    CK_CHECK(reader.sees("the background one"));

    // The one in front kept its own name, and the reader was never taken to
    // the window they only pointed at.
    CK_CHECK(reader.screen().find("Terminal 2") != std::string::npos);
    CK_CHECK(reader.screen().find("Terminal 1") == std::string::npos);

    reader.press("\x02" "d");
    CK_CHECK(reader.gone());
    end_process(server);
    forget(socket);
}

CK_TEST(minimizing_the_only_terminal_puts_the_window_bar_on_screen) {
    // The reader's report, on a real screen: "the window bar does not appear
    // when a single terminal window in a session is minimized" (WP-34). The
    // headless suite pins the rule; this pins the only thing that answers the
    // report — that a reader who puts their one terminal away can SEE where it
    // went, and get it back.
    const std::filesystem::path socket = private_socket("minimize");
    forget(socket);
    if (binary_path().empty()) return;
    const ::pid_t server = start_server(socket);
    CK_CHECK(wait_for_socket(socket));

    Reader reader;
    CK_CHECK(reader.start(socket));
    CK_CHECK(reader.sees("Terminal 1"));
    const std::size_t rows = reader.rows().size();
    CK_CHECK(rows >= 3U);
    if (rows < 3U) return;
    const int bar_row = static_cast<int>(rows) - 2;

    // Before: one terminal, nothing hidden, no bar. The only "Terminal 1" on
    // the screen is on the window's own title bar, above the bar's row.
    {
        const std::optional<std::pair<int, int>> caption = reader.find_cell("Terminal 1");
        CK_CHECK(caption.has_value());
        if (!caption) return;
        CK_CHECK(caption->first < bar_row);
        CK_CHECK(reader.rows()[static_cast<std::size_t>(bar_row)].find("Terminal 1") ==
                 std::string::npos);
    }

    // `^B _`, the chord named for the `_` control on the window's own frame.
    reader.press("\x02" "_");
    reader.settle(300);

    // After: the window is off the screen — the ONLY "Terminal 1" left is on
    // the bar's row — and the bar is there, saying the terminal is put away.
    const std::optional<std::pair<int, int>> listed = reader.find_cell("Terminal 1");
    CK_CHECK(listed.has_value());
    if (!listed) return;
    CK_CHECK(listed->first == bar_row);
    // The minimized glyph, one cell left of the name it belongs to (U4-j).
    CK_CHECK(reader.rows()[static_cast<std::size_t>(bar_row)].find("▄ Terminal 1") !=
             std::string::npos);

    // And the row is the way back. A click on it restores the terminal, at
    // which point there is one terminal and nothing hidden again — so the bar
    // has done its job and goes.
    reader.click(bar_row, listed->second);
    reader.settle(400);
    const std::optional<std::pair<int, int>> restored = reader.find_cell("Terminal 1");
    CK_CHECK(restored.has_value());
    if (!restored) return;
    CK_CHECK(restored->first < bar_row);
    CK_CHECK(reader.rows()[static_cast<std::size_t>(bar_row)].find("Terminal 1") ==
             std::string::npos);

    // And the route the reader most likely took to report this: the `_`
    // control on the window's own frame, clicked with the mouse. It is the
    // first underscore on the screen — the menu bar above it has none, and
    // the child is /bin/cat, which writes nothing.
    const std::optional<std::pair<int, int>> control = reader.find_cell("_");
    CK_CHECK(control.has_value());
    if (!control) return;
    CK_CHECK(control->first < bar_row);  // on the frame, not in the bar
    reader.click(control->first, control->second);
    reader.settle(400);
    const std::optional<std::pair<int, int>> away = reader.find_cell("Terminal 1");
    CK_CHECK(away.has_value());
    if (!away) return;
    CK_CHECK(away->first == bar_row);
    CK_CHECK(reader.rows()[static_cast<std::size_t>(bar_row)].find("▄ Terminal 1") !=
             std::string::npos);

    reader.press("\x02" "d");
    CK_CHECK(reader.gone());
    end_process(server);
    forget(socket);
}

CK_TEST(hiding_the_status_bar_drops_the_window_bar_onto_the_last_row) {
    // WP-35, and the second half of the same request: a way to push the window
    // list down and hide the status bar. What it buys is one row of terminal,
    // so what this checks is where the rows actually are.
    const std::filesystem::path socket = private_socket("chrome");
    forget(socket);
    if (binary_path().empty()) return;
    const ::pid_t server = start_server(socket);
    CK_CHECK(wait_for_socket(socket));

    Reader reader;
    CK_CHECK(reader.start(socket));
    CK_CHECK(reader.sees("Terminal 1"));
    reader.press("\x02" "c");
    CK_CHECK(reader.sees("Terminal 2"));

    const std::size_t rows = reader.rows().size();
    CK_CHECK(rows >= 3U);
    if (rows < 3U) return;
    const int last_row = static_cast<int>(rows) - 1;

    // Expanded: the bar above the footer, and the footer carrying its hints.
    {
        const std::vector<std::string> lines = reader.rows();
        CK_CHECK(lines[static_cast<std::size_t>(last_row) - 1U].find("Terminal 2") !=
                 std::string::npos);
        CK_CHECK(lines[static_cast<std::size_t>(last_row)].find("Terminal 2") == std::string::npos);
        CK_CHECK(lines[static_cast<std::size_t>(last_row)].find("menu") != std::string::npos);
    }

    reader.press("\x02" "b");
    reader.settle(300);

    // Collapsed: the footer is gone and the bar is on the last row — the row
    // the footer was spending.
    {
        const std::vector<std::string> lines = reader.rows();
        CK_CHECK(lines[static_cast<std::size_t>(last_row)].find("Terminal 2") != std::string::npos);
        CK_CHECK(lines[static_cast<std::size_t>(last_row)].find("menu") == std::string::npos);
        // And the row above it is the reader's desktop now, not chrome.
        CK_CHECK(lines[static_cast<std::size_t>(last_row) - 1U].find("Terminal 2") ==
                 std::string::npos);
        // ▲ rather than ▼: the toggle says which way it goes next.
        CK_CHECK(lines[static_cast<std::size_t>(last_row)].find("▲") != std::string::npos);
    }

    // And back, both together.
    reader.press("\x02" "b");
    reader.settle(300);
    {
        const std::vector<std::string> lines = reader.rows();
        CK_CHECK(lines[static_cast<std::size_t>(last_row) - 1U].find("Terminal 2") !=
                 std::string::npos);
        CK_CHECK(lines[static_cast<std::size_t>(last_row)].find("menu") != std::string::npos);
    }

    reader.press("\x02" "d");
    CK_CHECK(reader.gone());
    end_process(server);
    forget(socket);
}
