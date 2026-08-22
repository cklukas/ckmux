// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// WP-21 §3: the reference-app matrix.
//
// Real programs, run inside a real ckmux, driven by keystrokes and read back
// off the screen. Each row asserts §3's universal three — the app draws what
// it draws on a bare host, detach-and-reattach reproduces the screen, and it
// exits cleanly leaving the terminal usable — plus that row's own condition.
//
// THE TRAP THIS FILE IS WRITTEN AGAINST, because §4 walked into it twice:
// "the app started and something appeared" is satisfied by any build that
// draws anything at all. `vim` opening is not `vim` usable. So every row
// names content only that program could have produced, at a position only
// correct behaviour could have put it — the rule §4.2 states, applied here.
//
// A program that is not installed is reported as NOT EXERCISED and the row is
// skipped loudly. A matrix that quietly reports green for rows it never ran is
// worth less than no matrix, because it is believed.
#include <tuple>  // std::ignore
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include "reader_harness.hpp"

#include "cvision/testing/cktest.hpp"

namespace {

using ckmtest::binary_path;
using ckmtest::end_process;
using ckmtest::forget;
using ckmtest::private_socket;
using ckmtest::Reader;
using ckmtest::start_server;
using ckmtest::wait_for_socket;

// Looked up rather than assumed: the harness gives the client a PATH of
// `/usr/bin:/bin`, and half the matrix lives in Homebrew's prefix.
std::string program_path(const std::string& name) {
    for (const char* const prefix : {"/usr/bin/", "/bin/", "/opt/homebrew/bin/", "/usr/local/bin/"}) {
        const std::string candidate = prefix + name;
        if (::access(candidate.c_str(), X_OK) == 0) return candidate;
    }
    return {};
}

// A vertical rule drawn INSIDE the terminal window, away from its edges.
//
// ckmux draws its own window frame in box characters — `╔═║` focused, `┌─│`
// unfocused — so "the screen contains a box character" says nothing about the
// program running in it. What only a panelled app produces is a vertical rule
// running down the MIDDLE of the window. Returns its column, or nothing.
std::optional<int> interior_vertical(Reader& reader) {
    const std::vector<std::string> lines = reader.rows();
    std::map<int, int> tally;
    int widest = 0;
    for (const std::string& line : lines) {
        int column = 0;
        for (std::size_t i = 0; i < line.size();) {
            const unsigned char lead = static_cast<unsigned char>(line[i]);
            std::size_t width = 1;
            if ((lead & 0xE0u) == 0xC0u) width = 2;
            else if ((lead & 0xF0u) == 0xE0u) width = 3;
            else if ((lead & 0xF8u) == 0xF0u) width = 4;
            if (line.compare(i, width, "\u2502") == 0 || line.compare(i, width, "\u2551") == 0)
                ++tally[column];
            i += width;
            ++column;
        }
        widest = std::max(widest, column);
    }
    for (const std::pair<const int, int>& entry : tally) {
        // Away from both edges, and running down a good part of the height:
        // the frame's own sides sit within a few columns of the screen edge.
        if (entry.first > 8 && entry.first < widest - 8 && entry.second >= 5) return entry.first;
    }
    return std::nullopt;
}

int probe_counter = 0;
bool shell_is_ready(Reader& reader, int attempts = 6) {
    const std::string tag = "R3FAPP" + std::to_string(++probe_counter);
    for (int attempt = 0; attempt < attempts; ++attempt) {
        reader.press("echo " + tag.substr(0, 6) + "\"\"" + tag.substr(6) + "\r");
        if (reader.sees(tag, 2500)) return true;
    }
    return false;
}

}  // namespace

CK_TEST(less_takes_the_alternate_screen_and_gives_back_exactly_what_was_under_it) {
    if (binary_path().empty()) return;
    const std::string less = program_path("less");
    if (less.empty()) {
        std::printf("  [WP-21 §3] `less` absent; row NOT EXERCISED\n");
        return;
    }
    std::printf("  [WP-21 §3] less: %s\n", less.c_str());

    const std::filesystem::path socket = private_socket("refless");
    forget(socket);
    const ::pid_t server = start_server(socket);
    CK_CHECK(wait_for_socket(socket));

    Reader reader;
    CK_CHECK(reader.start(socket, ckv::Size{100, 30}));
    CK_CHECK(reader.sees("new term"));
    CK_CHECK(shell_is_ready(reader));

    // The file first, so that everything the shell prints is on screen BEFORE
    // the baseline is taken. The first version of this test captured the
    // baseline and then ran two more commands, and then reported `less` for
    // the difference they made.
    reader.press("printf 'LESSLINE-%s\\n' A B C D E F > /tmp/ckmux-ref-less.txt\r");
    CK_CHECK(shell_is_ready(reader));

    // Something distinctive underneath, so "the screen came back" is a claim
    // about content rather than about the screen being non-empty.
    reader.press("clear; echo UNDER\"\"NEATH-1; echo UNDER\"\"NEATH-2\r");
    CK_CHECK(reader.sees("UNDERNEATH-2", 8000));
    reader.settle(800);
    const std::optional<std::pair<int, int>> first_before = reader.find_cell("UNDERNEATH-1");
    const std::optional<std::pair<int, int>> second_before = reader.find_cell("UNDERNEATH-2");
    CK_CHECK(first_before.has_value());
    CK_CHECK(second_before.has_value());

    reader.press(less + " /tmp/ckmux-ref-less.txt\r");
    reader.settle(2000);

    // THE ROW'S CONDITION, first half: `less` is on the alternate screen, so
    // its content is visible AND what was underneath is gone. Asserting only
    // the first would pass against a `less` drawing over the primary screen,
    // which is the defect this row exists for.
    CK_CHECK(reader.sees("LESSLINE-A", 8000));
    CK_CHECK(reader.sees("LESSLINE-F", 4000));
    CK_CHECK(!reader.sees("UNDERNEATH-1", 1200));

    reader.press("q");
    reader.settle(2000);

    // Second half, which §3 calls the most common alternate-screen defect: the
    // screen underneath comes back.
    //
    // Asserted by POSITION and content of what was there, not by comparing the
    // whole screen. A whole-screen comparison cannot hold and would be a
    // dishonest test: `less` exits, the shell prints a fresh prompt, and the
    // command line the reader typed is still echoed where they typed it — so
    // the restored screen legitimately differs from the saved one. What 1049
    // promises is that the CONTENT beneath is returned unaltered, and that is
    // what these four assertions say.
    CK_CHECK(reader.sees("UNDERNEATH-1", 8000));
    const std::optional<std::pair<int, int>> first_after = reader.find_cell("UNDERNEATH-1");
    const std::optional<std::pair<int, int>> second_after = reader.find_cell("UNDERNEATH-2");
    CK_CHECK(first_after.has_value());
    CK_CHECK(second_after.has_value());
    if (first_before.has_value() && first_after.has_value()) {
        CK_CHECK(first_after->first == first_before->first);
        CK_CHECK(first_after->second == first_before->second);
    }
    if (second_before.has_value() && second_after.has_value()) {
        CK_CHECK(second_after->first == second_before->first);
        CK_CHECK(second_after->second == second_before->second);
    }
    // And `less` left no trace of itself behind.
    CK_CHECK(!reader.sees("LESSLINE-A", 1200));

    // §3's universal third: the terminal is still usable afterwards.
    CK_CHECK(shell_is_ready(reader));

    reader.quit();
    end_process(server);
    forget(socket);
}

CK_TEST(a_child_that_draws_sixel_without_asking_cannot_garble_a_host_that_has_none) {
    if (binary_path().empty()) return;
    const std::string img2sixel = program_path("img2sixel");
    if (img2sixel.empty()) {
        std::printf("  [WP-21 §3] `img2sixel` absent; Sixel row NOT EXERCISED\n");
        return;
    }
    std::printf("  [WP-21 §3] img2sixel: %s\n", img2sixel.c_str());

    // A four-pixel PNG written here rather than found on the machine, so the
    // row does not depend on which pictures happen to be installed. This
    // build of img2sixel has no libpng/libjpeg (§3.1) but reads this.
    const char* const make_png =
        "python3 -c \"import struct,zlib\n"
        "def c(t,d):\n"
        " b=t+d\n"
        " return struct.pack('>I',len(d))+b+struct.pack('>I',zlib.crc32(b)&0xffffffff)\n"
        "raw=b''.join(b'\\\\x00'+bytes([255,0,0]*4) for _ in range(4))\n"
        "open('/tmp/ckmux-ref-tiny.png','wb').write(b'\\\\x89PNG\\\\r\\\\n\\\\x1a\\\\n'"
        "+c(b'IHDR',struct.pack('>IIBBBBB',4,4,8,2,0,0,0))+c(b'IDAT',zlib.compress(raw))"
        "+c(b'IEND',b''))\"";
    std::ignore = std::system(make_png);

    const std::filesystem::path socket = private_socket("refsixel");
    forget(socket);
    const ::pid_t server = start_server(socket);
    CK_CHECK(wait_for_socket(socket));

    Reader reader;
    // THE HOST CANNOT RENDER PICTURES. This is the whole row: §3.1 measured
    // that `img2sixel 1.10.5` emits Sixel immediately, with no DA1 and no
    // query of any kind, identically under every TERM — so the child will
    // draw whatever ckmux lets through, and ckmux is the only thing between a
    // non-graphical screen and a screenful of raster bytes shown as text.
    reader.host_profile.sixel = false;
    CK_CHECK(reader.start(socket, ckv::Size{100, 30}));
    CK_CHECK(reader.sees("new term"));
    CK_CHECK(shell_is_ready(reader));

    // THE POSITIVE PARTNER, without which the assertion below is worthless.
    // These are ordinary printable characters, and this proves the screen
    // shows them when a program prints them — so their ABSENCE after
    // img2sixel means ckmux removed them, not that they could never appear.
    reader.press("echo '#0;2;97;0;0'\r");
    CK_CHECK(reader.sees("#0;2;97;0;0", 8000));
    reader.press("clear\r");
    reader.settle(800);
    CK_CHECK(!reader.sees("#0;2;97;0;0", 1200));

    // Run it, and CAPTURE ITS EXIT STATUS. Without this the row passes
    // vacuously against an img2sixel that failed outright: a program that drew
    // nothing because it could not read its file also garbles nothing, and
    // the assertions below cannot tell the two apart.
    reader.press(img2sixel + " /tmp/ckmux-ref-tiny.png; echo RC\"\"=$?\r");
    reader.settle(2500);
    CK_CHECK(reader.sees("RC=0", 8000));

    // NOTHING GARBLED. The payload's own bytes — `#0;2;97;0;0` is this
    // image's colour introducer, and `"1;1;4;4` its raster attribute — must
    // not appear as text on a host that cannot draw them.
    CK_CHECK(!reader.sees("#0;2;97;0;0", 1500));
    CK_CHECK(!reader.sees("\"1;1;4;4", 1200));
    // Nor the sixel data characters that follow, which are what a reader
    // actually sees when a raster lands on a text screen.
    CK_CHECK(!reader.sees("!4N", 1200));

    // AND NOTHING DRAWN is not the same as nothing working: the terminal is
    // still usable, which is §3's universal third and the thing a rig that
    // simply swallowed everything would also satisfy.
    CK_CHECK(shell_is_ready(reader));

    reader.quit();
    end_process(server);
    forget(socket);
}

CK_TEST(vim_survives_a_ckmux_detach_mid_edit_with_the_buffer_where_it_was) {
    if (binary_path().empty()) return;
    const std::string vim = program_path("vim");
    if (vim.empty()) {
        std::printf("  [WP-21 §3] `vim` absent; row NOT EXERCISED\n");
        return;
    }
    std::printf("  [WP-21 §3] vim: %s\n", vim.c_str());

    const std::filesystem::path socket = private_socket("refvim");
    forget(socket);
    const ::pid_t server = start_server(socket);
    CK_CHECK(wait_for_socket(socket));

    std::optional<std::pair<int, int>> marker_before;
    {
        Reader reader;
        CK_CHECK(reader.start(socket, ckv::Size{100, 30}));
        CK_CHECK(reader.sees("new term"));
        CK_CHECK(shell_is_ready(reader));

        // `-u NONE` so the row tests ckmux rather than whatever vimrc this
        // machine has, and `-N` to keep vim out of compatible mode, which
        // changes the key handling the rest of this depends on.
        reader.press(vim + " -u NONE -N /tmp/ckmux-ref-vim.txt\r");
        reader.settle(2500);
        // Something only vim draws: its filler column down the left of an
        // empty buffer. "The screen changed" would pass against a vim that
        // failed to start and left an error.
        CK_CHECK(reader.sees("~", 8000));

        reader.press("i");
        reader.settle(500);
        reader.press("VIMBUFFER-MARKER");
        reader.settle(800);
        reader.press("\x1b");  // back to normal mode, buffer unsaved
        reader.settle(800);
        CK_CHECK(reader.sees("VIMBUFFER-MARKER", 6000));
        marker_before = reader.find_cell("VIMBUFFER-MARKER");
        CK_CHECK(marker_before.has_value());

        // Detach by ending this client, mid-edit and with the buffer unsaved.
        // The session and everything running in it belong to the server.
        reader.quit();
    }

    // A second reader, arriving at the same session the way a reader does
    // after closing a laptop.
    {
        Reader reader;
        CK_CHECK(reader.start(socket, ckv::Size{100, 30}));
        CK_CHECK(reader.sees("new term"));

        // THE ROW'S CONDITION: the buffer is where it was. Position compared,
        // not merely presence — a session whose screen was rebuilt from
        // scratch would also contain the text, somewhere.
        CK_CHECK(reader.sees("VIMBUFFER-MARKER", 15000));
        const std::optional<std::pair<int, int>> marker_after =
            reader.find_cell("VIMBUFFER-MARKER");
        CK_CHECK(marker_after.has_value());
        if (marker_before.has_value() && marker_after.has_value()) {
            CK_CHECK(marker_after->first == marker_before->first);
            CK_CHECK(marker_after->second == marker_before->second);
        }
        // And vim is still vim, not a corpse whose last frame happens to be on
        // screen: it still answers its own keys.
        reader.press(":q!\r");
        reader.settle(2000);
        CK_CHECK(!reader.sees("VIMBUFFER-MARKER", 2000));
        // §3's universal third.
        CK_CHECK(shell_is_ready(reader));
        reader.quit();
    }

    end_process(server);
    forget(socket);
}

CK_TEST(htop_redraws_on_a_timer_without_the_screen_drifting) {
    if (binary_path().empty()) return;
    const std::string htop = program_path("htop");
    if (htop.empty()) {
        std::printf("  [WP-21 §3] `htop` absent; row NOT EXERCISED\n");
        return;
    }
    std::printf("  [WP-21 §3] htop: %s\n", htop.c_str());

    const std::filesystem::path socket = private_socket("refhtop");
    forget(socket);
    const ::pid_t server = start_server(socket);
    CK_CHECK(wait_for_socket(socket));

    Reader reader;
    CK_CHECK(reader.start(socket, ckv::Size{100, 30}));
    CK_CHECK(reader.sees("new term"));
    CK_CHECK(shell_is_ready(reader));

    reader.press(htop + "\r");
    reader.settle(3000);
    // Column headers only htop draws, and it draws them because it is running
    // rather than because it started: they are repainted on every tick.
    CK_CHECK(reader.sees("PID", 10000));
    CK_CHECK(reader.sees("CPU%", 6000));

    const std::optional<std::pair<int, int>> pid_before = reader.find_cell("PID");
    const std::optional<std::pair<int, int>> cpu_before = reader.find_cell("CPU%");
    CK_CHECK(pid_before.has_value());
    CK_CHECK(cpu_before.has_value());

    // Let it redraw many times. §3 asks for sixty seconds; this takes a
    // shorter window deliberately and says so rather than pretending
    // otherwise — what it can show is that repeated full repaints do not move
    // the frame, which is the mechanism drift comes from. A sixty-second
    // version belongs in a soak lane, not in a suite every session runs.
    reader.settle(12000);

    // THE ROW'S CONDITION: the layout has not moved. Drift is cells being
    // lost between repaints, and its visible form is a frame that creeps —
    // headers sliding a column, a row at a time. Comparing POSITIONS across
    // many repaints catches that; comparing content cannot, because htop's
    // numbers are supposed to change.
    const std::optional<std::pair<int, int>> pid_after = reader.find_cell("PID");
    const std::optional<std::pair<int, int>> cpu_after = reader.find_cell("CPU%");
    CK_CHECK(pid_after.has_value());
    CK_CHECK(cpu_after.has_value());
    if (pid_before.has_value() && pid_after.has_value()) {
        CK_CHECK(pid_after->first == pid_before->first);
        CK_CHECK(pid_after->second == pid_before->second);
    }
    if (cpu_before.has_value() && cpu_after.has_value()) {
        CK_CHECK(cpu_after->first == cpu_before->first);
        CK_CHECK(cpu_after->second == cpu_before->second);
    }

    // Quits cleanly and leaves the terminal usable (§3's universal third).
    reader.press("q");
    reader.settle(2000);
    CK_CHECK(!reader.sees("CPU%", 2000));
    CK_CHECK(shell_is_ready(reader));

    reader.quit();
    end_process(server);
    forget(socket);
}

CK_TEST(mc_draws_its_frames_in_box_characters_and_its_subshell_gives_the_screen_back) {
    if (binary_path().empty()) return;
    const std::string mc = program_path("mc");
    if (mc.empty()) {
        std::printf("  [WP-21 §3] `mc` absent; row NOT EXERCISED\n");
        return;
    }
    std::printf("  [WP-21 §3] mc: %s\n", mc.c_str());

    const std::filesystem::path socket = private_socket("refmc");
    forget(socket);
    const ::pid_t server = start_server(socket);
    CK_CHECK(wait_for_socket(socket));

    Reader reader;
    CK_CHECK(reader.start(socket, ckv::Size{100, 30}));
    CK_CHECK(reader.sees("new term"));
    CK_CHECK(shell_is_ready(reader));

    reader.press(mc + "\r");
    reader.settle(4000);

    // THE ROW'S CONDITION, first half: the frames are BOX CHARACTERS. mc is
    // the matrix's line-drawing row, and the failure it exists to catch is a
    // charset fallback that renders them as `q`, `x` and `l` — the VT100
    // alternate character set arriving unmapped. Asserting "mc drew
    // something" would pass against exactly that screen.
    // mc's own text first, so the line-drawing check below is known to be
    // about a running mc rather than about whatever else is on screen.
    CK_CHECK(reader.sees("Name", 12000));
    // Then the panel divider: a vertical rule down the MIDDLE of the window.
    // `sees("│")` would match ckmux's own unfocused window frame, which is
    // drawn in exactly these characters and says nothing about mc.
    CK_CHECK(interior_vertical(reader).has_value());
    // And the tell-tale of the failure is absent: a row of unmapped ACS.
    CK_CHECK(!reader.sees("qqqqqqqq", 1500));

    // Second half: the subshell. `Ctrl-O` drops mc to the shell underneath and
    // presses it again to come back — §3's "the subshell inherits and restores
    // the screen".
    reader.press("\x0f");
    reader.settle(2000);
    reader.press("echo MCSUB\"\"SHELL\r");
    const bool subshell_ran = reader.sees("MCSUBSHELL", 8000);
    CK_CHECK(subshell_ran);
    reader.press("\x0f");
    reader.settle(2500);
    // Back in mc, with its frames again — the restore half, which a subshell
    // that painted over the screen would fail.
    CK_CHECK(reader.sees("Name", 8000));
    CK_CHECK(interior_vertical(reader).has_value());

    // Leaves cleanly. mc asks before quitting, so the confirmation is part of
    // exiting it the way a reader does.
    reader.press("\x1b");
    reader.settle(600);
    reader.press("F10");
    reader.settle(600);
    reader.press("\x1b" "0");
    reader.settle(2000);
    reader.press("\r");
    reader.settle(2500);
    CK_CHECK(shell_is_ready(reader));

    reader.quit();
    end_process(server);
    forget(socket);
}

CK_TEST(fzf_filters_incrementally_and_gives_the_screen_back_on_exit) {
    if (binary_path().empty()) return;
    const std::string fzf = program_path("fzf");
    if (fzf.empty()) {
        std::printf("  [WP-21 §3] `fzf` absent; row NOT EXERCISED\n");
        return;
    }
    std::printf("  [WP-21 §3] fzf: %s\n", fzf.c_str());

    const std::filesystem::path socket = private_socket("reffzf");
    forget(socket);
    const ::pid_t server = start_server(socket);
    CK_CHECK(wait_for_socket(socket));

    Reader reader;
    CK_CHECK(reader.start(socket, ckv::Size{100, 30}));
    CK_CHECK(reader.sees("new term"));
    CK_CHECK(shell_is_ready(reader));

    reader.press("clear; echo UNDER\"\"FZF\r");
    CK_CHECK(reader.sees("UNDERFZF", 8000));
    reader.settle(600);
    const std::optional<std::pair<int, int>> under_before = reader.find_cell("UNDERFZF");
    CK_CHECK(under_before.has_value());

    reader.press("printf 'ALPHAROW\\nBRAVOROW\\nCHARLIEROW\\n' | " + fzf + " > /tmp/ckmux-ref-fzf.out\r");
    reader.settle(3000);
    // All three candidates are up, and the screen underneath is gone: fzf
    // takes the alternate screen.
    CK_CHECK(reader.sees("ALPHAROW", 10000));
    CK_CHECK(reader.sees("CHARLIEROW", 4000));
    CK_CHECK(!reader.sees("UNDERFZF", 1200));

    // THE ROW'S CONDITION: type-and-filter, and the list actually narrows.
    // Asserting only that the match survives would pass against a build that
    // redrew nothing at all — what makes this an incremental-redraw test is
    // that the NON-matches are gone from the screen afterwards.
    reader.press("BRAVO");
    reader.settle(2000);
    CK_CHECK(reader.sees("BRAVOROW", 6000));
    CK_CHECK(!reader.sees("ALPHAROW", 1500));
    CK_CHECK(!reader.sees("CHARLIEROW", 1200));

    reader.press("\r");
    reader.settle(2500);

    // Exit restores what was underneath, at the same place.
    CK_CHECK(reader.sees("UNDERFZF", 8000));
    const std::optional<std::pair<int, int>> under_after = reader.find_cell("UNDERFZF");
    CK_CHECK(under_after.has_value());
    if (under_before.has_value() && under_after.has_value()) {
        CK_CHECK(under_after->first == under_before->first);
        CK_CHECK(under_after->second == under_before->second);
    }
    // And fzf chose what the reader typed rather than merely exiting: the
    // selection reached the file, which is the only proof the filter meant
    // anything.
    CK_CHECK(shell_is_ready(reader));
    reader.press("cat /tmp/ckmux-ref-fzf.out\r");
    CK_CHECK(reader.sees("BRAVOROW", 6000));

    reader.quit();
    end_process(server);
    forget(socket);
}

CK_TEST(btop_redraws_on_a_timer_without_the_screen_drifting) {
    if (binary_path().empty()) return;
    const std::string btop = program_path("btop");
    if (btop.empty()) {
        std::printf("  [WP-21 §3] `btop` absent; row NOT EXERCISED\n");
        return;
    }
    std::printf("  [WP-21 §3] btop: %s\n", btop.c_str());

    const std::filesystem::path socket = private_socket("refbtop");
    forget(socket);
    const ::pid_t server = start_server(socket);
    CK_CHECK(wait_for_socket(socket));

    Reader reader;
    // A larger host than the other rows use. btop measures the terminal it is
    // given and refuses to draw below a minimum — at this suite's usual
    // 100x30 screen the window's interior is 90x22 and btop renders
    // "Terminal size too small" instead of a UI. That is btop behaving
    // correctly, and a row that asserted against it would be asserting on an
    // error message.
    CK_CHECK(reader.start(socket, ckv::Size{170, 52}));
    CK_CHECK(reader.sees("new term"));
    CK_CHECK(shell_is_ready(reader));

    reader.press(btop + "\r");
    reader.settle(5000);
    // btop draws boxed panels with titles; `cpu` is the one it always shows.
    CK_CHECK(reader.sees("cpu", 12000));
    const std::optional<std::pair<int, int>> cpu_before = reader.find_cell("cpu");
    const std::optional<int> rule_before = interior_vertical(reader);
    CK_CHECK(cpu_before.has_value());

    reader.settle(12000);

    // Same condition as the htop row and for the same reason: drift shows as a
    // frame that creeps between repaints, so positions are compared while
    // content is expected to change.
    const std::optional<std::pair<int, int>> cpu_after = reader.find_cell("cpu");
    const std::optional<int> rule_after = interior_vertical(reader);
    CK_CHECK(cpu_after.has_value());
    if (cpu_before.has_value() && cpu_after.has_value()) {
        CK_CHECK(cpu_after->first == cpu_before->first);
        CK_CHECK(cpu_after->second == cpu_before->second);
    }
    CK_CHECK(rule_after.has_value() == rule_before.has_value());
    if (rule_before.has_value() && rule_after.has_value()) CK_CHECK(*rule_after == *rule_before);

    reader.press("q");
    reader.settle(2500);
    CK_CHECK(shell_is_ready(reader));

    reader.quit();
    end_process(server);
    forget(socket);
}

CK_TEST(the_ssh_row_is_not_run_and_says_why) {
    // §3's `ssh` + remote tmux row, recorded as NOT RUN rather than skipped
    // silently or deleted.
    //
    // Nothing is listening on TCP 22 on the machine of record, and enabling
    // remote login is a change to somebody's SYSTEM — it needs administrator
    // access and it opens a listening network service. **No acceptance suite
    // is worth that**, and pointing the row off this machine instead would
    // make a matrix cell that fails for network reasons, which is a cell
    // everyone learns to ignore.
    //
    // It is not deleted because a deleted row is a silent claim that the
    // matrix is complete. §7's fourth cell state exists for exactly this: `not
    // run` is neither a pass nor a failure but an unfinished milestone, and it
    // must never render blank. Same category as `emacs`, `chafa` and `lsix` —
    // prerequisites this machine lacks, recorded rather than worked around.
    std::printf("  [WP-21 §3] ssh + remote tmux: NOT RUN — sshd is not enabled on the machine of\n"
                "             record; enabling it is a system change, not a test fixture\n");
}

CK_TEST(lazygit_renders_its_panels_and_the_reader_can_select_one) {
    if (binary_path().empty()) return;
    const std::string lazygit = program_path("lazygit");
    if (lazygit.empty()) {
        std::printf("  [WP-21 §3] `lazygit` absent; row NOT EXERCISED\n");
        return;
    }
    std::printf("  [WP-21 §3] lazygit: %s\n", lazygit.c_str());

    // Its own repository, made here: lazygit shows nothing useful outside one,
    // and pointing the row at whatever repository the machine happens to have
    // would make the assertions depend on somebody's working tree.
    std::ignore = std::system("rm -rf /tmp/ckmux-ref-git && mkdir -p /tmp/ckmux-ref-git && "
                      "cd /tmp/ckmux-ref-git && git init -q && "
                      "echo LAZYFILECONTENT > LAZYTRACKEDFILE && git add . && "
                      "git -c user.email=t@e -c user.name=t commit -qm 'LAZYCOMMITSUBJECT' && "
                      // Left DIRTY on purpose: a committed file does not appear
                      // in the Files panel, and the selection half of this row
                      // needs something there to select.
                      "echo LAZYCHANGE >> LAZYTRACKEDFILE && "
                      // A SECOND dirty file, so the selection can be moved
                      // between two things and the move can be seen.
                      "echo LAZYSECONDCHANGE > LAZYSECONDFILE");

    const std::filesystem::path socket = private_socket("reflazy");
    forget(socket);
    const ::pid_t server = start_server(socket);
    CK_CHECK(wait_for_socket(socket));

    Reader reader;
    CK_CHECK(reader.start(socket, ckv::Size{140, 44}));
    CK_CHECK(reader.sees("new term"));
    CK_CHECK(shell_is_ready(reader));

    reader.press("cd /tmp/ckmux-ref-git && " + lazygit + "\r");
    reader.settle(5000);
    // lazygit greets a first-time reader with a dialog that covers the panels
    // underneath. Dismissed rather than asserted around, because a row that
    // worked only while the welcome box happened to be up would break the
    // first time somebody ran the suite twice with a real config directory.
    if (reader.sees("Thanks for using lazygit", 4000)) {
        reader.press("\r");
        reader.settle(1500);
    }

    // THE ROW'S CONDITION, first half: the panels render. Asserted through
    // content only this repository could produce — the commit subject made
    // above — plus a vertical rule down the middle, which is what a panelled
    // layout is. Either alone is weak: the text could appear in a plain `git
    // log`, and the rule could be ckmux's own frame.
    CK_CHECK(reader.sees("LAZYCOMMITSUBJECT", 15000));
    CK_CHECK(interior_vertical(reader).has_value());

    // Both files are listed, which is the panel doing its job.
    CK_CHECK(reader.sees("LAZYTRACKEDFILE", 8000));
    CK_CHECK(reader.sees("LAZYSECONDFILE", 4000));

    // THE SELECTION HALF OF THIS ROW IS NOT COVERED, and it is left uncovered
    // deliberately rather than asserted weakly.
    //
    // §3's condition is "panels render; mouse selects". The rendering half is
    // above and is real. For the selection half I tried three assertions and
    // mutation testing killed all three as worthless: looking for the tracked
    // file after a keypress passed WITHOUT the keypress (lazygit lists a dirty
    // file from the start); looking for its diff passed too (lazygit selects a
    // file on entry and shows that diff immediately); and moving the selection
    // — by arrow key and by `k` — did not produce a screen this rig could tell
    // apart from the one before it.
    //
    // What I could have written instead is an assertion that passes: press a
    // key, confirm lazygit is still drawing. That asserts nothing about
    // selection and would sit here looking like coverage. A row that says
    // plainly which half is unverified is worth more than one that quietly
    // verifies the easy half — the same reason `ssh` is recorded `not run`
    // rather than deleted.
    //
    // What it needs is a way to read lazygit's SELECTION rather than its
    // content — the highlighted line's attributes, which `grapheme()` cannot
    // see (§4.3: reverse-video is in the cell styles, not the characters). A
    // style-aware read of the harness would close this row and the copy-mode
    // indicator in §4 at the same time.
    std::printf("  [WP-21 §3] lazygit: panels asserted; SELECTION NOT COVERED — needs a\n"
                "             style-aware screen read, see the comment in this test\n");

    reader.press("q");
    reader.settle(2500);
    CK_CHECK(!reader.sees("LAZYCOMMITSUBJECT", 2000));
    CK_CHECK(shell_is_ready(reader));

    reader.quit();
    end_process(server);
    forget(socket);
    std::ignore = std::system("rm -rf /tmp/ckmux-ref-git");
}

CK_TEST(a_flooding_child_leaves_the_reader_able_to_work) {
    if (binary_path().empty()) return;
    std::printf("  [WP-21 §3] flood row: `yes` in a terminal\n");

    const std::filesystem::path socket = private_socket("refflood");
    forget(socket);
    const ::pid_t server = start_server(socket);
    CK_CHECK(wait_for_socket(socket));

    Reader reader;
    CK_CHECK(reader.start(socket, ckv::Size{100, 30}));
    CK_CHECK(reader.sees("new term"));
    CK_CHECK(shell_is_ready(reader));

    reader.press("yes CKMUX-FLOOD-ROW\r");
    reader.settle(2500);
    CK_CHECK(reader.sees("CKMUX-FLOOD-ROW", 8000));

    // THE ROW'S CONDITION: the reader can still work. The budget gates are
    // already covered at protocol level (WP-20, test_core_promise); what this
    // row adds is the app-level fact — that while a child is writing as fast
    // as it can, ckmux's own interface still answers a key.
    //
    // Asserted through ckmux's OWN chrome rather than the shell, because the
    // shell is behind the flood and its prompt is not the question. `^B m`
    // opens the menu bar, and the menu is drawn by the client.
    // The prefix, and then the FOOTER — which changes from the idle hint line
    // to the prefix key list only while ckmux is waiting for the next key.
    //
    // Not `sees("Session")`: the menu bar carries that word permanently at the
    // top of the screen, so it is on the screen whether or not ckmux answered
    // anything, and the assertion would pass against a client that had stopped
    // reading the keyboard entirely. `w windows` appears nowhere except the
    // prefix footer.
    CK_CHECK(!reader.sees("w windows", 1000));
    reader.press("\x02");
    reader.settle(1500);
    const bool interface_answers = reader.sees("w windows", 8000);
    CK_CHECK(interface_answers);
    reader.press("\x1b");
    reader.settle(800);
    CK_CHECK(!reader.sees("w windows", 1500));

    // And the flood can be stopped, which is the other half of usable.
    reader.press("\x03");
    reader.settle(1500);
    CK_CHECK(shell_is_ready(reader));

    reader.quit();
    end_process(server);
    forget(socket);
}
