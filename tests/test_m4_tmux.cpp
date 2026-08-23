// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// WP-21 §4: `tmux` inside ckmux.
//
// §0 governs this file: tmux is an oracle we may RUN and may not READ. Nothing
// here is explained by reference to tmux's source. Where ckmux and tmux
// disagree and the standard does not settle it, the observation goes to the
// divergence list (§6) rather than being resolved by reading the oracle.
//
// §4.2 governs the assertions: each clause names screen content only tmux
// could have produced, at a position only the correct behaviour could have put
// it. The lazier assertion is always easier to write and always passes against
// the defect — a status line matched anywhere passes against one stranded at
// the old row after a failed reflow, which IS the defect.
#include <tuple>  // std::ignore
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <vector>

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

// Where tmux is, if it is anywhere. Looked up rather than assumed: the harness
// gives the client a PATH of `/usr/bin:/bin`, and a Homebrew tmux is not on
// it.
std::string tmux_path() {
    for (const char* candidate :
         {"/opt/homebrew/bin/tmux", "/usr/local/bin/tmux", "/usr/bin/tmux"}) {
        if (::access(candidate, X_OK) == 0) return candidate;
    }
    return {};
}

// §4.1: the rig records `tmux -V` in every §4 report, and a report without it
// is not evidence. Status-line layout, copy-mode key tables and default
// bindings all move between releases, so a §4 failure after an upgrade is not
// automatically a ckmux regression — and without the version there is no way
// to tell, which is the exact situation where somebody starts reading the
// oracle to find out.
std::string tmux_version(const std::string& tmux) {
    std::string command = tmux + " -V 2>/dev/null";
    std::string version;
    if (std::FILE* pipe = ::popen(command.c_str(), "r")) {
        char buffer[128];
        if (std::fgets(buffer, sizeof buffer, pipe) != nullptr) version = buffer;
        (void)::pclose(pipe);
    }
    while (!version.empty() && (version.back() == '\n' || version.back() == '\r'))
        version.pop_back();
    return version;
}

// Waits until a shell is actually accepting input, which is NOT the same as
// ckmux having drawn its chrome. The footer appears the moment the client
// starts, so a test that gates on it types its first command into a shell that
// is still initialising, and the keystrokes go nowhere. That failure is
// invisible: the command simply never runs, and the assertion after it blames
// whatever it was checking.
//
// Two things this has to get right, both found the hard way here:
//
// The sentinel is UNIQUE PER PROBE. A fixed one is still on screen from the
// last probe — in the pane beside this one, after a split — so `sees()`
// matches the old text and reports a shell ready that has not started. An
// assertion satisfied by its own history is not an assertion.
//
// And it RETRIES rather than probing once. A pane created a moment ago has a
// shell that is still starting; the first sentinel is typed into nothing and
// lost for good, so a single probe can only fail. Retrying is what makes the
// wait a wait.
//
// `CKMUX""_READY_n` echoes to the screen as typed and prints joined, so
// finding the joined form proves the shell RAN it rather than proving the
// terminal drew the keystrokes back.
inline int probe_counter = 0;

bool shell_is_ready(Reader& reader, int attempts = 6) {
    const std::string tag = "R3ADY" + std::to_string(++probe_counter);
    for (int attempt = 0; attempt < attempts; ++attempt) {
        reader.press("echo " + tag.substr(0, 5) + "\"\"" + tag.substr(5) + "\r");
        if (reader.sees(tag, 2500)) return true;
    }
    return false;
}

// The oracle's server, on a socket path belonging to this run and nothing
// else.
//
// tmux's DEFAULT socket is the reader's own tmux server. A test that starts a
// session there outlives the test, collides with the next run — "duplicate
// session: m4", which is how this was found — and is indistinguishable from a
// session the reader started themselves. A rig that cleans up after itself on
// a shared socket is one bad name away from killing somebody's work.
//
// `-S` rather than `-L` because `-L` names a socket inside tmux's own
// directory, and this rig then has no supported way to remove the file when
// the server is gone. Thirty-three of them accumulated during development,
// one per run. `-S` puts the socket where this test already owns the
// directory, so teardown is a delete rather than a hope.
inline std::filesystem::path oracle_socket(const std::filesystem::path& near) {
    return near.parent_path() / "oracle.sock";
}

inline void kill_oracle(const std::string& tmux, const std::filesystem::path& oracle) {
    const std::string command = tmux + " -S " + oracle.string() + " kill-server >/dev/null 2>&1";
    std::ignore = std::system(command.c_str());
    std::error_code ignored;
    std::filesystem::remove(oracle, ignored);
}

// The column tmux drew its vertical pane border in, if it drew one.
//
// Found by counting box-drawing verticals per column rather than by
// arithmetic on the geometry: a divider asserted at "half the width" is an
// assertion about this test's idea of tmux's layout, and it passes or fails
// for reasons that have nothing to do with whether a split happened.
std::optional<int> divider_column(Reader& reader) {
    const std::vector<std::string> lines = reader.rows();
    if (lines.empty()) return std::nullopt;
    std::map<int, int> tally;
    for (const std::string& line : lines) {
        int column = 0;
        for (std::size_t i = 0; i < line.size();) {
            const unsigned char lead = static_cast<unsigned char>(line[i]);
            std::size_t width = 1;
            if ((lead & 0xE0u) == 0xC0u) width = 2;
            else if ((lead & 0xF0u) == 0xE0u) width = 3;
            else if ((lead & 0xF8u) == 0xF0u) width = 4;
            if (line.compare(i, width, "\u2502") == 0) ++tally[column];
            i += width;
            ++column;
        }
    }
    int best = -1;
    int best_count = 0;
    for (const std::pair<const int, int>& entry : tally)
        if (entry.second > best_count) { best_count = entry.second; best = entry.first; }
    // A frame corner or a stray glyph is one or two cells; a pane border runs
    // most of the terminal's height.
    if (best_count < 5) return std::nullopt;
    return best;
}

// The row tmux drew its status line on, and how far across it reached.
//
// Located by content only tmux produces — its session name in brackets — and
// then measured, because §4.2's clause is about WHERE it is, not that it
// exists. A substring match anywhere passes against a status line stranded at
// the old row after a failed reflow, which is the defect the clause is for.
std::optional<std::pair<int, int>> status_line(Reader& reader, const std::string& session) {
    const std::optional<std::pair<int, int>> at = reader.find_cell("[" + session + "]");
    if (!at.has_value()) return std::nullopt;
    const std::vector<std::string> lines = reader.rows();
    if (static_cast<std::size_t>(at->first) >= lines.size()) return std::nullopt;
    const std::string& line = lines[static_cast<std::size_t>(at->first)];
    // Counted in COLUMNS, not bytes. The frame around a ckmux terminal is
    // drawn in box-drawing characters, so a hundred-column row is a hundred
    // and twenty bytes long, and a byte count reports a width the screen does
    // not have.
    int columns = 0;
    int last_filled = 0;
    for (std::size_t i = 0; i < line.size();) {
        const unsigned char lead = static_cast<unsigned char>(line[i]);
        std::size_t width = 1;
        if ((lead & 0xE0u) == 0xC0u) width = 2;
        else if ((lead & 0xF0u) == 0xE0u) width = 3;
        else if ((lead & 0xF8u) == 0xF0u) width = 4;
        if (line.compare(i, width, " ") != 0) last_filled = columns;
        i += width;
        ++columns;
    }
    return std::make_pair(at->first, last_filled);
}

// The row ckmux drew the terminal window's bottom border on, and the column
// its right border sits in. The window is the geometry tmux reflows into, so
// "the last row of the new geometry" is a fact about the FRAME, not about the
// reader's screen — the screen has desktop below the window.
std::optional<std::pair<int, int>> frame_bottom(Reader& reader) {
    const std::optional<std::pair<int, int>> corner = reader.find_cell("\u255A");
    if (!corner.has_value()) return std::nullopt;
    const std::vector<std::string> lines = reader.rows();
    if (static_cast<std::size_t>(corner->first) >= lines.size()) return std::nullopt;
    const std::string& line = lines[static_cast<std::size_t>(corner->first)];
    int columns = 0;
    int right = corner->second;
    for (std::size_t i = 0; i < line.size();) {
        const unsigned char lead = static_cast<unsigned char>(line[i]);
        std::size_t width = 1;
        if ((lead & 0xE0u) == 0xC0u) width = 2;
        else if ((lead & 0xF0u) == 0xE0u) width = 3;
        else if ((lead & 0xF8u) == 0xF0u) width = 4;
        if (line.compare(i, width, "\u2550") == 0) right = columns;
        i += width;
        ++columns;
    }
    return std::make_pair(corner->first, right);
}

// A key to the inner multiplexer: ckmux's prefix is ^B and so is tmux's, so
// `^B ^B` (ckmux's send-prefix, the interface spec) puts a literal ^B in front of
// the program and the key after it is tmux's to interpret. This is the whole
// of what "nesting depth of one" means in practice.
void oracle_key(Reader& reader, const std::string& key) {
    reader.press("\x02");
    reader.press("\x02");
    reader.press(key);
    reader.settle(1200);
}

}  // namespace

// Everything §4 claims, in one session, because "a tmux session does all of
// this" is the claim — not "each of these works in a session of its own".
CK_TEST(tmux_splits_switches_and_survives_a_reattach_inside_ckmux) {
    if (binary_path().empty()) return;
    const std::string tmux = tmux_path();
    if (tmux.empty()) {
        // Loud rather than silent. A §4 run on a machine without the oracle
        // has not exercised §4, and a suite that reports green having skipped
        // it is exactly the instrument this file exists to avoid being.
        std::printf("  [WP-21 §4] no tmux on this machine; §4 NOT EXERCISED\n");
        return;
    }
    // §4.1: a §4 report without the oracle's version is not evidence. Status
    // line layout, copy-mode key tables and default bindings all move between
    // releases, so a failure after an upgrade is not automatically a ckmux
    // regression — and without the version there is no way to tell, which is
    // the situation where somebody starts reading the oracle to find out.
    std::printf("  [WP-21 §4] oracle: %s\n", tmux_version(tmux).c_str());

    const std::filesystem::path socket = private_socket("m4tmux");
    forget(socket);
    const ::pid_t server = start_server(socket);
    CK_CHECK(wait_for_socket(socket));

    Reader reader;
    CK_CHECK(reader.start(socket, ckv::Size{100, 30}));
    // ckmux's own footer, not a shell prompt: the prompt belongs to whichever
    // shell the SERVER's environment names, and gating on `$` fails against a
    // reader whose shell prompts with `%`.
    CK_CHECK(reader.sees("new term"));
    CK_CHECK(shell_is_ready(reader));

    const std::filesystem::path oracle = oracle_socket(socket);
    kill_oracle(tmux, oracle);
    reader.press(tmux + " -f /dev/null -S " + oracle.string() + " new-session -s m4\r");
    CK_CHECK(reader.sees("[m4]", 15000));
    CK_CHECK(shell_is_ready(reader));

    // --- panes and splits -------------------------------------------------
    // §4.2: the divider at a real column, plus a DISTINCT marker in each pane
    // located on opposite sides of it. "The screen changed" would pass against
    // a redraw that split nothing.
    oracle_key(reader, "%");
    CK_CHECK(shell_is_ready(reader));
    reader.press("echo RIGHT\"\"PANE\r");
    CK_CHECK(reader.sees("RIGHTPANE"));

    oracle_key(reader, "o");
    CK_CHECK(shell_is_ready(reader));
    reader.press("echo LEFT\"\"PANE\r");
    CK_CHECK(reader.sees("LEFTPANE"));

    const std::optional<int> divider = divider_column(reader);
    const std::optional<std::pair<int, int>> left = reader.find_cell("LEFTPANE");
    const std::optional<std::pair<int, int>> right = reader.find_cell("RIGHTPANE");
    CK_CHECK(divider.has_value());
    CK_CHECK(left.has_value());
    CK_CHECK(right.has_value());
    if (divider.has_value() && left.has_value() && right.has_value()) {
        // Opposite sides OF THE DIVIDER, not merely different columns: two
        // markers in the same pane also have different columns.
        CK_CHECK(left->second < *divider);
        CK_CHECK(*divider < right->second);
    }

    // --- window switching --------------------------------------------------
    // §4.2: present, absent, present again. A bare absence passes vacuously
    // the moment the marker changes for an unrelated reason, so the
    // switch-back leg is what makes the absence mean anything.
    //
    // The marker is written fresh AND the pane is cleared first. An older
    // marker flakes: every readiness probe prints a line into this pane, a
    // slow run retries more, and the marker scrolls out of a twenty-two row
    // pane on its own. The test then fails on the switch-back leg and blames
    // window switching for a scroll — which is what it did, 1 run in 5, until
    // this line changed. `clear` puts the marker at the top of an empty pane,
    // where nothing printed afterwards can push it off.
    reader.press("clear; echo WIN\"\"MARK\r");
    CK_CHECK(reader.sees("WINMARK", 8000));
    oracle_key(reader, "c");
    CK_CHECK(shell_is_ready(reader));
    CK_CHECK(!reader.sees("WINMARK", 1500));
    oracle_key(reader, "p");
    CK_CHECK(reader.sees("WINMARK", 8000));

    // --- resize, with the status line intact -------------------------------
    // §4.2: the status line has to end up on the LAST ROW OF THE NEW GEOMETRY
    // and span it, and the panes have to reflow to the new column count. The
    // host terminal is what resizes — a reader dragging their window — and
    // everything inside is expected to follow.
    const std::optional<std::pair<int, int>> status_before = status_line(reader, "m4");
    CK_CHECK(status_before.has_value());

    // Zoom, not a host-screen resize. ckmux is a WINDOWING multiplexer: its
    // terminals are windows on a desktop, so growing the reader's screen adds
    // desktop around the window and does not resize the child at all. This was
    // measured rather than assumed — resizing the host from 30 rows to 40 grew
    // ckmux's screen and left tmux's status line exactly where it was, which
    // is correct behaviour and would have been reported as a resize defect by
    // a test that asserted on the screen instead of the window.
    //
    // `^B z` is ckmux's OWN chord and deliberately not sent through
    // send-prefix: the reader resizing the window is ckmux's action, and the
    // reflow inside it is tmux's answer to it.
    reader.press("\x02");
    reader.press("z");
    reader.settle(2500);
    CK_CHECK(shell_is_ready(reader));

    const std::optional<std::pair<int, int>> status_after = status_line(reader, "m4");
    const std::optional<std::pair<int, int>> frame = frame_bottom(reader);
    CK_CHECK(status_after.has_value());
    CK_CHECK(frame.has_value());
    if (status_before.has_value() && status_after.has_value() && frame.has_value()) {
        // It moved down with the window...
        CK_CHECK(status_after->first > status_before->first);
        // ...to the LAST ROW of the window, immediately above the frame's
        // bottom border. A status line stranded at the old row after a failed
        // reflow is the defect, and "it is present somewhere" cannot see it.
        CK_CHECK(status_after->first == frame->first - 1);
        // And it spans the window rather than stopping at the old width. Left
        // as a margin rather than an equality because the rightmost columns
        // belong to tmux's own right-aligned clock, not to this test.
        CK_CHECK(status_after->second >= frame->second - 3);
    }
    // The split survived the resize and is still a split. Asserted with a
    // FRESH marker rather than the one written before the resize: every
    // readiness probe adds lines to the pane, so an old marker scrolls out of
    // a small pane on its own, and a test that waits for it is asserting that
    // nothing has been printed since.
    CK_CHECK(divider_column(reader).has_value());
    reader.press("echo AFTER\"\"ZOOM\r");
    CK_CHECK(reader.sees("AFTERZOOM"));

    // --- copy mode ---------------------------------------------------------
    // §4.2: the indicator alone is drawn on entry and says nothing about
    // whether the mode WORKS, so the assertion is a key that only means
    // something in copy mode having a visible effect — scrolling to content
    // that is no longer on the live screen — and the live screen returning on
    // exit.
    reader.press("seq 1 400 | tail -60\r");
    CK_CHECK(reader.sees("400", 10000));
    // With 400 lines printed, the earliest are far above the viewport.
    CK_CHECK(!reader.sees("CKM4TOP", 800));
    reader.press("echo CKM4\"\"TOP; seq 1 400 | tail -60\r");
    CK_CHECK(reader.sees("400", 10000));
    CK_CHECK(!reader.sees("CKM4TOP", 800));
    oracle_key(reader, "[");
    // Page Up is only a scroll in copy mode; on the live screen the shell
    // either ignores it or answers with an escape sequence.
    reader.press("\x1b[5~");
    reader.settle(900);
    reader.press("\x1b[5~");
    reader.settle(900);
    const bool scrolled_back = reader.sees("CKM4TOP", 4000);
    CK_CHECK(scrolled_back);
    reader.press("q");
    reader.settle(900);
    // Leaving copy mode puts the live screen back — the bottom of the output,
    // which is where the shell prompt is.
    CK_CHECK(reader.sees("400", 4000));
    CK_CHECK(!reader.sees("CKM4TOP", 800));

    // --- detach and reattach ----------------------------------------------
    // §4: the panes have to come back WHERE THEY WERE. "tmux is still
    // running" and "the session name is back" are both true of a session
    // whose layout was rebuilt wrongly, so the geometry is captured before
    // and compared after.
    // A marker written HERE, not reused from an earlier clause. The copy-mode
    // clause prints four hundred lines through this pane, so any anchor older
    // than it has scrolled away — and an assertion waiting for one is really
    // asserting that nothing has been printed since.
    reader.press("clear; echo DETACH\"\"MARK\r");
    CK_CHECK(reader.sees("DETACHMARK", 8000));
    const int divider_before = divider_column(reader).value_or(-1);
    const std::optional<std::pair<int, int>> anchor_before = reader.find_cell("DETACHMARK");
    CK_CHECK(anchor_before.has_value());
    oracle_key(reader, "d");
    CK_CHECK(reader.sees("[detached", 8000) || !reader.sees("[m4]", 1500));
    CK_CHECK(shell_is_ready(reader));
    reader.press(tmux + " -f /dev/null -S " + oracle.string() + " attach -t m4\r");
    CK_CHECK(reader.sees("DETACHMARK", 15000));

    const std::optional<int> divider_after = divider_column(reader);
    const std::optional<std::pair<int, int>> anchor_after = reader.find_cell("DETACHMARK");
    CK_CHECK(divider_after.has_value());
    CK_CHECK(anchor_after.has_value());
    // Where they were, not merely that they are: a session whose layout was
    // rebuilt wrongly still has a divider and still has the text.
    if (divider_after.has_value()) CK_CHECK(*divider_after == divider_before);
    if (anchor_after.has_value() && anchor_before.has_value()) {
        CK_CHECK(anchor_after->first == anchor_before->first);
        CK_CHECK(anchor_after->second == anchor_before->second);
    }

    kill_oracle(tmux, oracle);
    reader.quit();
    end_process(server);
    forget(socket);
}
