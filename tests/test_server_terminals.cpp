// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// The terminals a server owns (WP-3). These are PTY-level tests on purpose:
// the claims are about real children — what status they exited with, what
// happens to one that ignores SIGTERM, what a resize actually tells them, and
// above all that none of them outlives an explicit close. None of that can be
// established against a fake, because the whole package is the part where a
// real process is involved.
#include "server/terminals.hpp"

#if !defined(_WIN32)

#include <chrono>
#include <csignal>
#include <functional>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "cvision/testing/cktest.hpp"

using ckm::Settings;
using ckm::server::Terminal;
using ckm::server::Terminals;
using ckm::server::TerminalSpec;

namespace {

Settings test_settings() {
    Settings settings;
    // A shell that is on every machine this runs on, so the test does not
    // depend on whose login shell it is.
    settings.shell = "/bin/sh";
    settings.login_shell = false;
    settings.scrollback = 64;
    return settings;
}

TerminalSpec spec_running(std::string command) {
    TerminalSpec spec;
    spec.command = std::move(command);
    spec.working_directory = "/";
    spec.columns = 40;
    spec.rows = 10;
    return spec;
}

// Drains until `done` is true or the deadline passes. A child is a real
// process, so the only honest way to wait for one is to wait — bounded, and
// with the bound failing the test rather than hanging it.
//
// The deadline is real elapsed time, measured. An earlier version counted
// iterations and assumed each took the 5 ms it slept for; with a terminal
// running `yes` on the other end, one iteration took ten times that, so a
// "4 second" bound became forty seconds of drain and the suite blew through
// ctest's timeout while every case passed on its own. A budget that is not
// measured is not a budget. (Tests are tooling and may read the steady clock;
// library code may not — AGENTS.md.)
bool pump_until(Terminals& terminals, const std::function<bool()>& done, int milliseconds = 4000,
                std::size_t byte_budget = 64 * 1024) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        terminals.drain(byte_budget);
        if (done()) return true;
        ::usleep(2000);
    }
    return done();
}

std::string screen_text(const Terminal& terminal) {
    const ckv::term::TerminalSnapshot snapshot = terminal.snapshot();
    std::string text;
    for (const ckv::Cell& cell : snapshot.cell_buffer) text += cell.grapheme();
    return text;
}

}  // namespace

CK_TEST(a_terminal_gets_an_id_that_is_never_reused) {
    // A client naming a terminal after it closed must be told it is gone, not
    // handed a different one — a recycled id is how a paste lands in somebody
    // else's shell (plans/05, invariant 3).
    Terminals terminals(test_settings());
    const ckm::server::TerminalId first = terminals.open(spec_running("exit 0")).id();
    const ckm::server::TerminalId second = terminals.open(spec_running("exit 0")).id();
    CK_CHECK(first != 0U);  // zero means "no terminal" on the wire
    CK_CHECK(second != first);

    CK_CHECK(terminals.close(first));
    CK_CHECK(terminals.find(first) == nullptr);
    const ckm::server::TerminalId third = terminals.open(spec_running("exit 0")).id();
    CK_CHECK(third != first);
    CK_CHECK(third != second);
    // ...and closing something already gone is stale rather than wrong: it
    // happens every time a reattach races a close.
    CK_CHECK(!terminals.close(first));
}

CK_TEST(a_childs_exit_status_is_what_the_child_exited_with) {
    // "exited 1" and "exited 0" are different windows to a reader — one holds
    // with a banner and one closes (plans/03 on-exit) — so a server that could
    // not tell them apart would guess, and guess wrong half the time.
    Terminals terminals(test_settings());
    Terminal& zero = terminals.open(spec_running("exit 0"));
    Terminal& seven = terminals.open(spec_running("exit 7"));

    CK_CHECK(pump_until(terminals, [&] {
        return zero.exit_status().has_value() && seven.exit_status().has_value();
    }));
    CK_CHECK(zero.exit_status().has_value());
    CK_CHECK(seven.exit_status().has_value());
    if (zero.exit_status()) CK_CHECK(*zero.exit_status() == 0);
    if (seven.exit_status()) CK_CHECK(*seven.exit_status() == 7);
    CK_CHECK(!zero.live());
    CK_CHECK(!seven.live());
}

CK_TEST(a_terminal_with_a_program_still_in_it_is_live_before_it_says_anything) {
    // Ready counts as live. A session becomes Running only once its child has
    // produced output, so a shell at a prompt it has not drawn — or anything
    // reading stdin — is Ready, and treating that as dead would close windows
    // out from under readers.
    Terminals terminals(test_settings());
    Terminal& quiet = terminals.open(spec_running("read line; exit 0"));
    CK_CHECK(quiet.live());
    CK_CHECK(!quiet.exit_status().has_value());
    // It is reading stdin, so it ends when it is given a line.
    quiet.send_input("hello\n");
    CK_CHECK(pump_until(terminals, [&] { return quiet.exit_status().has_value(); }));
    CK_CHECK(!quiet.live());
}

CK_TEST(a_resize_reaches_the_pty_with_its_pixel_fields) {
    // The pixel fields are not decoration: a Sixel program asks for them with
    // XTWINOPS 14/16 and sizes its picture from the answer, so a resize that
    // dropped them would leave every image the wrong size.
    Terminals terminals(test_settings());
    Terminal& terminal = terminals.open(spec_running("read line; exit 0"));

    // 900x540 of text area over a 100x30 grid is a 9x18 cell, which is what
    // ckVision is told — and it multiplies back up for TIOCSWINSZ, so the pty
    // ends up with the text area the client actually has.
    terminal.resize(100, 30, 900, 540);
    CK_CHECK(terminal.columns() == 100);
    CK_CHECK(terminal.rows() == 30);
    CK_CHECK(terminal.cell_pixels() == (ckv::Size{9, 18}));

    // Read back from the pty itself rather than from the emulator's copy: the
    // claim is about what the child was told.
    struct winsize size{};
    CK_CHECK(::ioctl(terminal.file_descriptor(), TIOCGWINSZ, &size) == 0);
    CK_CHECK(size.ws_col == 100);
    CK_CHECK(size.ws_row == 30);
    CK_CHECK(size.ws_xpixel == 900);
    CK_CHECK(size.ws_ypixel == 540);

    // A resize that does not know the pixels keeps the last text area it was
    // told, rather than telling the child its terminal is nothing across. The
    // cell metric is re-derived against the new grid, so the pty's pixel
    // fields follow the columns.
    terminal.resize(80, 24, 0, 0);
    CK_CHECK(::ioctl(terminal.file_descriptor(), TIOCGWINSZ, &size) == 0);
    CK_CHECK(size.ws_col == 80);
    CK_CHECK(size.ws_xpixel == static_cast<unsigned short>(terminal.cell_pixels().width * 80));
    CK_CHECK(size.ws_xpixel > 0);
}

CK_TEST(a_child_that_ignores_sigterm_is_still_gone_after_a_close) {
    // The grace path, which is the reason the close policy has three signals.
    // A server that waited forever for a program that traps SIGTERM would have
    // stopped being a server.
    //
    // The child prints its own pid, so this can ask the operating system
    // whether it is still there rather than trusting the thing under test.
    Terminals terminals(test_settings());
    Terminal& stubborn =
        terminals.open(spec_running("trap '' TERM HUP; echo READY $$; while :; do sleep 1; done"));

    CK_CHECK(pump_until(terminals, [&] { return screen_text(stubborn).find("READY") != std::string::npos; }));
    const std::string text = screen_text(stubborn);
    const std::size_t marker = text.find("READY ");
    CK_CHECK(marker != std::string::npos);
    if (marker == std::string::npos) return;
    const ::pid_t child = static_cast<::pid_t>(std::stol(text.substr(marker + 6)));
    CK_CHECK(child > 1);
    // It really is alive, and really is ignoring the polite signals.
    CK_CHECK(::kill(child, 0) == 0);
    CK_CHECK(::kill(child, SIGTERM) == 0);
    ::usleep(200000);
    CK_CHECK(::kill(child, 0) == 0);

    stubborn.close();
    bool gone = false;
    for (int elapsed = 0; elapsed < 4000 && !gone; elapsed += 10) {
        if (::kill(child, 0) != 0) gone = true;
        else ::usleep(10000);
    }
    CK_CHECK(gone);
}

CK_TEST(no_child_outlives_the_collection_that_owned_it) {
    // The one outcome this file exists to prevent. A destructor that merely
    // released its sessions would leave the children running, re-parented to
    // init, with nobody's terminal to write to — which is precisely the mess a
    // multiplexer is supposed to prevent rather than create.
    ::pid_t child = 0;
    {
        Terminals terminals(test_settings());
        Terminal& stubborn =
            terminals.open(spec_running("trap '' TERM HUP; echo READY $$; while :; do sleep 1; done"));
        CK_CHECK(pump_until(terminals,
                            [&] { return screen_text(stubborn).find("READY") != std::string::npos; }));
        const std::string text = screen_text(stubborn);
        const std::size_t marker = text.find("READY ");
        CK_CHECK(marker != std::string::npos);
        if (marker == std::string::npos) return;
        child = static_cast<::pid_t>(std::stol(text.substr(marker + 6)));
        CK_CHECK(::kill(child, 0) == 0);
    }  // the collection goes out of scope here, and with it the child

    bool gone = false;
    for (int elapsed = 0; elapsed < 4000 && !gone; elapsed += 10) {
        if (::kill(child, 0) != 0) gone = true;
        else ::usleep(10000);
    }
    CK_CHECK(gone);
}

CK_TEST(draining_is_bounded_so_one_noisy_child_cannot_starve_the_others) {
    // Fairness, not throughput: a terminal running `yes` must not be able to
    // hold the server's loop while every other terminal waits. The budget is
    // per terminal per drain, so a flooding child gets exactly its share.
    Terminals terminals(test_settings());
    Terminal& flood = terminals.open(spec_running("yes ckmux-flood-line"));
    Terminal& quiet = terminals.open(spec_running("echo quiet-one; read line"));

    // Both make progress, which is the whole claim: the quiet terminal's one
    // line arrives while the flood is running, and the flood really is running
    // — so this is a statement about a loaded server rather than an idle one.
    // Waiting for only the quiet one would prove nothing about fairness,
    // because it might simply have won the race.
    CK_CHECK(pump_until(
        terminals,
        [&] {
            return screen_text(quiet).find("quiet-one") != std::string::npos &&
                   screen_text(flood).find("ckmux-flood-line") != std::string::npos;
        },
        4000, 4 * 1024));
    CK_CHECK(screen_text(quiet).find("quiet-one") != std::string::npos);
    CK_CHECK(screen_text(flood).find("ckmux-flood-line") != std::string::npos);

    // Bounded means bounded: one drain of a small budget reads at most that
    // much, however much the child has queued.
    const std::size_t before = screen_text(flood).size();
    terminals.drain(64);
    const std::size_t after = screen_text(flood).size();
    CK_CHECK(after >= before);
    CK_CHECK(after - before <= 64U + 64U);  // the budget, plus a partial line already buffered
}

CK_TEST(a_launch_that_fails_is_a_terminal_in_failed_state_rather_than_a_gap) {
    // A client has already opened a window for this, and a reader needs to be
    // told what went wrong in that window — not left with a hole where one was
    // about to be.
    Settings settings = test_settings();
    settings.shell = "/nonexistent/shell-that-is-not-there";
    Terminals terminals(settings);
    TerminalSpec spec;
    spec.working_directory = "/";
    Terminal& failed = terminals.open(spec);
    CK_CHECK(terminals.size() == 1U);
    CK_CHECK(pump_until(terminals, [&] { return !failed.live(); }, 2000));
    CK_CHECK(!failed.live());
}

CK_TEST(the_replies_a_child_asked_for_go_back_to_the_child) {
    // DA1, DSR and XTWINOPS answers are for the program that asked, not for a
    // client. A server that forwarded them would leave every probing program
    // waiting for a reply that went to the wrong end of the socket.
    //
    // The child asks where the cursor is and then holds its terminal open. The
    // answer arrives on the child's own terminal INPUT, and a tty with echo on
    // — which is every terminal a shell was just started in — writes what
    // arrives straight back out, so it lands on the screen this test reads, as
    // `^[[1;1R` with the escape shown the way a terminal shows a control
    // character. That echo is the evidence: those bytes can only be on the
    // screen if the server wrote the emulator's answer down the pty instead of
    // sending it to a client.
    //
    // The reply is `1;1R` and not `6;`: `6n` is the question ("where is the
    // cursor"), and the answer carries the position, which is home. Reading
    // the reply in the child instead would need raw mode — a DSR reply has no
    // newline, so a canonical-mode read never sees it — and would prove no
    // more than the echo does.
    Terminals terminals(test_settings());
    Terminal& terminal = terminals.open(spec_running("printf '\033[6n'; cat > /dev/null"));
    CK_CHECK(pump_until(terminals,
                        [&] { return screen_text(terminal).find("[1;1R") != std::string::npos; }));
    CK_CHECK(screen_text(terminal).find("[1;1R") != std::string::npos);
}

CK_TEST(configuration_decides_what_a_terminal_tells_its_child_it_can_do) {
    // The server owns the emulator, so the server owns the policy (plans/08).
    // A client cannot be trusted with it, and a program asks a terminal what it
    // supports and then behaves as though the answer were true.
    Settings settings = test_settings();
    settings.mouse = false;
    settings.osc52 = false;
    settings.sixel = ckm::SixelMode::Off;
    settings.scrollback = 0;
    settings.printer_mode = ckm::PrinterMode::Off;

    const ckv::term::TerminalLaunchSpec launch =
        ckm::server::launch_spec_for(settings, spec_running("exit 0"));
    CK_CHECK(!launch.profile.mouse_reporting);
    CK_CHECK(!launch.profile.sixel);
    CK_CHECK(launch.profile.clipboard_policy == ckv::core::TerminalClipboardPolicy::Deny);
    CK_CHECK(launch.profile.printer_policy == ckv::core::TerminalPrinterPolicy::Deny);
    // The title policy is not configurable: a multiplexer's window captions
    // follow the running program, which is what makes them useful.
    CK_CHECK(launch.profile.osc_policy == ckv::core::TerminalOscPolicy::StoreMetadata);
    // And a child is terminated after a grace period rather than waited for.
    CK_CHECK(launch.exit_policy == ckv::core::TerminalExitPolicy::TerminateAfterGrace);

    const ckv::term::TerminalSubsessionOptions options = ckm::server::subsession_options_for(settings);
    CK_CHECK(options.max_scrollback_lines == 0U);  // "remember nothing" really is zero
}

CK_TEST(a_terminal_reports_only_what_changed_since_the_host_caught_up) {
    // U0-b, through the seam the server actually holds. This is what WP-4b
    // will read every tick, so it is worth pinning that it arrives here
    // unchanged rather than only in the library's own tests.
    Terminals terminals(test_settings());
    Terminal& terminal = terminals.open(spec_running("read line; exit 0"));
    CK_CHECK(terminal.damage().full);  // a fresh terminal owes everything
    terminal.clear_damage();
    CK_CHECK(!terminal.damage().any());

    terminal.send_input("x\n");
    CK_CHECK(pump_until(terminals, [&] { return terminal.damage().any(); }));
    CK_CHECK(terminal.damage().any());
    // Borrowed, not copied: the grid is as wide as the terminal was opened.
    CK_CHECK(terminal.cells().size() == 40U * 10U);
}

#endif  // !_WIN32
