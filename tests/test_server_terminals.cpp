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
#include <cstdio>
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

CK_TEST(the_drain_budget_is_for_the_pass_and_not_for_each_child) {
    // Parsing IS what a drain costs — 64 KiB of a flooding child is 22 ms of
    // real work in an optimised build, two thirds of a frame at 30 fps — so a
    // budget handed to every terminal separately means a pass that grows with
    // the number of busy children until the loop answers nothing else. That was
    // measured, not imagined: it is what made a `Ping` take half a second to
    // come back with one flood running (WP-7's gate).
    Terminals terminals(test_settings());
    Terminal& first = terminals.open(spec_running("yes first-child"));
    Terminal& second = terminals.open(spec_running("yes second-child"));
    Terminal& third = terminals.open(spec_running("yes third-child"));
    CK_CHECK(pump_until(terminals, [&] {
        return !screen_text(first).empty() && !screen_text(second).empty() &&
               !screen_text(third).empty();
    }));

    // How much a pass actually absorbs, counted in the lines each emulator says
    // entered its history — which is the only honest measure of "how much did
    // this pass parse".
    const std::size_t budget = 64 * 1024;
    std::size_t lines = 0;
    for (int pass = 0; pass < 6; ++pass) {
        // A moment for the children to refill their PTYs. `yes` is blocked in
        // write() the instant the buffer is full, and a pass that measures
        // immediately after another one measures the scheduler rather than the
        // budget.
        (void)::usleep(20000);
        for (const ckm::server::TerminalId id : terminals.ids())
            if (Terminal* terminal = terminals.find(id)) terminal->clear_damage();
        (void)terminals.drain(budget);
        std::size_t pass_lines = 0;
        for (const ckm::server::TerminalId id : terminals.ids())
            if (Terminal* terminal = terminals.find(id))
                pass_lines += terminal->damage().scrollback_pushed;
        lines = std::max(lines, pass_lines);
    }
    // The shortest line these children write is twelve bytes ("first-child" and
    // a newline), so a pass that honoured the budget across all three cannot
    // have taken in many more than budget/12 lines. Slack for a partial line at
    // each end and for the screens themselves — and still nowhere near the three
    // times over that a per-terminal budget would produce, which is the thing
    // being pinned.
    const std::size_t lines_in_budget = budget / 12;
    CK_CHECK(lines > 0U);
    CK_CHECK(lines <= lines_in_budget + 3U * 24U);
    CK_CHECK(lines < 2U * lines_in_budget);

    // And every one of them is served: a share has a floor, so a busy neighbour
    // cannot starve a terminal down to nothing.
    CK_CHECK(Terminals::kMinimumDrainShare > 0U);
    for (int pass = 0; pass < 8; ++pass) (void)terminals.drain(budget);
    CK_CHECK(!screen_text(first).empty());
    CK_CHECK(!screen_text(second).empty());
    CK_CHECK(!screen_text(third).empty());
}

CK_TEST(the_name_a_reader_gives_a_terminal_is_kept_beside_what_the_child_claims) {
    // A custom title is session state, so it lives on the terminal the session
    // holds rather than in the client that set it (plans/03). What it must NOT
    // do is replace what the child says: the override is resolved into a
    // caption by the client, and "use the default title again" needs a current
    // answer to hand back to.
    Terminals terminals(test_settings());
    Terminal& terminal = terminals.open(spec_running("sleep 30"));
    CK_CHECK(terminal.custom_title().empty());
    // Nothing to announce until a reader names it, the same way a terminal
    // nobody has moved has no place to state.
    CK_CHECK(terminal.custom_title_announced());

    CK_CHECK(terminal.set_custom_title("deploy"));
    CK_CHECK(terminal.custom_title() == "deploy");
    CK_CHECK(!terminal.custom_title_announced());
    terminal.note_custom_title_announced();
    CK_CHECK(terminal.custom_title_announced());

    // Edge-triggered: an unchanged name costs no message. Without this a
    // producer that restated it would ping-pong forever with the client that
    // applies what it is told, which is what the layout's own flag exists for.
    CK_CHECK(!terminal.set_custom_title("deploy"));
    CK_CHECK(terminal.custom_title_announced());

    // The child goes on naming itself underneath, and the two never meet.
    terminal.session().feed_output("\x1b]2;make -j8\a");
    CK_CHECK(pump_until(terminals, [&] { return terminal.status().title == "make -j8"; }));
    CK_CHECK(terminal.custom_title() == "deploy");

    // Empty is the reader handing the name back — a value, and news like any
    // other. (A SESSION rename reads empty as "leave it alone", because a
    // session with no name is a row in the picker with nothing to point at;
    // this is the one place the two renames deliberately differ.)
    CK_CHECK(terminal.set_custom_title(""));
    CK_CHECK(terminal.custom_title().empty());
    CK_CHECK(!terminal.custom_title_announced());

    terminals.close_all();
}

CK_TEST(a_name_longer_than_the_wire_allows_is_cut_on_a_character_boundary) {
    // Clamped where it is KEPT rather than on the way out: a server that
    // stored megabytes per terminal and trimmed them at the socket would still
    // be holding megabytes. Through the same function the child's own title
    // goes through, so the cut cannot land inside a UTF-8 sequence and leave a
    // caption ending in half a character.
    Terminals terminals(test_settings());
    Terminal& terminal = terminals.open(spec_running("sleep 30"));

    // Two-byte characters, so a byte-count cut lands mid-character unless
    // something stops it. The limit is even and the character is two bytes, so
    // an honest clamp ends exactly on the boundary.
    std::string huge;
    while (huge.size() < ckm::proto::kMaxTitleBytes + 64) huge += "ä";
    CK_CHECK(terminal.set_custom_title(huge));
    CK_CHECK(terminal.custom_title().size() <= ckm::proto::kMaxTitleBytes);
    CK_CHECK(!terminal.custom_title().empty());
    // Every byte is either an ASCII byte or part of a whole sequence: no
    // trailing continuation byte without its leader.
    const std::string& kept = terminal.custom_title();
    std::size_t index = 0;
    std::size_t characters = 0;
    while (index < kept.size()) {
        const unsigned char lead = static_cast<unsigned char>(kept[index]);
        const std::size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0b110 ? 2 : (lead >> 4) == 0b1110 ? 3 : 4;
        index += length;
        ++characters;
    }
    CK_CHECK(index == kept.size());
    CK_CHECK(characters > 0);

    terminals.close_all();
}
