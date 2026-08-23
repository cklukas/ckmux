// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// WP-20: the performance budget gates, freed by WP-16.
//
// Deterministic counters, never wall-clock (the testing plan
// §7). A gate that measures how long this machine took measures this machine;
// what these measure is how much the server SAYS and how much work it does to
// say it, which is the same number on a busy laptop and an idle one, and the
// same number under a sanitizer.
//
// Each gate prints its figure. A budget nobody can read is a budget nobody
// notices drifting, and the printed number is what makes a regression legible
// in a CI log rather than merely red — the ceilings here are set well above
// what is measured and far below what the unbounded version produced, because
// what is being asserted is a shape, not a machine.
//
// The flood gate that answers "does the server still ANSWER under load" lives
// in test_core_promise.cpp (`a_flooding_child_does_not_stop_the_server_answering`,
// a real `yes` and a real `Ping`) and is deliberately not duplicated here.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include <unistd.h>

#include "common/grid_delta.hpp"
#include "common/proto.hpp"
#include "server/diff_engine.hpp"
#include "server/terminals.hpp"

#include "cvision/testing/cktest.hpp"

namespace {

using ckm::server::DiffEngine;
using ckm::server::Terminal;
using ckm::server::TerminalId;
using ckm::server::Terminals;
using ckm::server::TerminalSpec;

ckm::Settings test_settings(int scrollback = 200) {
    ckm::Settings settings;
    settings.shell = "/bin/sh";
    settings.login_shell = false;
    settings.scrollback = scrollback;
    settings.term = "xterm-256color";
    return settings;
}

TerminalSpec spec_running(std::string command, int columns = 80, int rows = 24) {
    TerminalSpec spec;
    spec.command = std::move(command);
    spec.working_directory = "/";
    spec.columns = columns;
    spec.rows = rows;
    spec.pixel_width = columns * 9;
    spec.pixel_height = rows * 18;
    spec.environment = {{"TERM", "xterm-256color"}, {"PATH", "/usr/bin:/bin"}, {"LC_ALL", "C"}};
    return spec;
}

// Drains until `ready` or the budget runs out. Real time, because the children
// are real: the same loop spans milliseconds in one build and seconds under a
// sanitizer, and an attempt is not a unit of waiting.
template <typename Ready>
bool pump_until(Terminals& terminals, Ready ready, int budget_ms = 4000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
    for (;;) {
        terminals.drain(64 * 1024);
        if (ready()) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        (void)::usleep(2000);
    }
}

// What one tick costs on the wire, for the terminal that had news. Encoded
// rather than counted in ops, because bytes are what a reader's connection
// actually carries and what the payload cap is written in.
std::size_t delta_bytes(DiffEngine& diffs, TerminalId id, Terminal& terminal,
                        bool* carried_a_scroll = nullptr) {
    const DiffEngine::TerminalTick tick = diffs.flush(id, terminal.session());
    if (!tick.delta.has_value()) return 0;
    if (carried_a_scroll != nullptr) {
        *carried_a_scroll = false;
        for (const ckm::proto::GridOp& op : tick.delta->ops)
            if (std::holds_alternative<ckm::proto::ScrollOp>(op)) *carried_a_scroll = true;
    }
    return ckm::proto::encode(*tick.delta).size();
}

// Everything a terminal's screen says, as text, so a gate can wait for the
// child to have actually drawn before measuring what drawing cost.
std::string screen_of(Terminal& terminal) {
    std::string text;
    for (const ckv::Cell& cell : terminal.session().cells())
        if (!cell.is_continuation()) text += cell.grapheme();
    return text;
}

}  // namespace

CK_TEST(a_scroll_costs_the_rows_that_moved_not_the_cells_that_moved_with_them) {
    // The gate the testing plan §7 names first: a full-screen scroll must be O(rows),
    // not O(cells). A terminal scrolling is what `vim`, `less`, a build log and
    // a shell all do continuously, and a differ that answers it by re-stating
    // every cell turns the commonest thing a terminal does into the most
    // expensive — 80x24 of restated cells per line, at whatever rate the child
    // scrolls.
    Terminals terminals(test_settings());
    DiffEngine diffs;
    Terminal& terminal = terminals.open(spec_running("cat", 80, 24));
    const TerminalId id = terminal.id();

    // A full screen of distinct rows, so nothing below can be mistaken for an
    // unchanged cell that never needed stating.
    std::string screen;
    for (int row = 0; row < 24; ++row)
        screen += "row " + std::to_string(row) + " " + std::string(60, 'x') + "\r\n";
    terminal.session().feed_output(screen);
    CK_CHECK(pump_until(terminals, [&] { return screen_of(terminal).find("row 23") != std::string::npos; }));

    // The screen as it stands, stated once. This is the baseline the scroll is
    // measured against: what it costs to say a whole screen.
    const std::size_t whole_screen = delta_bytes(diffs, id, terminal);
    CK_CHECK(whole_screen > 0U);

    // Now one line scrolls off the top. Every cell on the screen has moved.
    terminal.session().feed_output("the line that pushed the rest up\r\n");
    CK_CHECK(pump_until(terminals, [&] {
        return screen_of(terminal).find("pushed the rest up") != std::string::npos;
    }));
    bool carried_a_scroll = false;
    const std::size_t one_scroll = delta_bytes(diffs, id, terminal, &carried_a_scroll);

    std::printf("  [budget] whole screen %zu bytes; one scrolled line %zu bytes (%.1f%%)\n",
                whole_screen, one_scroll,
                100.0 * static_cast<double>(one_scroll) / static_cast<double>(whole_screen));
    // A quarter is generous: a scroll op plus one restated row is a small
    // fraction of a screen, and a differ that lost its scroll op would land at
    // or above 100% — the failure this catches is a shape change, not a
    // marginal one.
    CK_CHECK(one_scroll > 0U);
    CK_CHECK(one_scroll * 4U < whole_screen);
    // And cheap for the RIGHT reason. A ratio on its own would be satisfied by
    // a delta that was small because it said too little — the positive partner
    // to the bound is that the saving comes from a scroll op actually being on
    // the wire, which is the mechanism the testing plan names.
    CK_CHECK(carried_a_scroll);
    terminals.close_all();
}

CK_TEST(one_character_changing_costs_one_character) {
    // "Cells-diffed-per-frame" (the testing plan §7). A cursor blinking, a clock
    // ticking, a spinner turning: the commonest delta a live terminal produces
    // is one cell, and it has to cost one cell. A differ that restated the
    // screen on any damage at all would still be correct and would still pass
    // every mirror test — this is the gate that says it is also cheap.
    Terminals terminals(test_settings());
    DiffEngine diffs;
    Terminal& terminal = terminals.open(spec_running("cat", 80, 24));
    const TerminalId id = terminal.id();

    std::string screen;
    for (int row = 0; row < 24; ++row)
        screen += "row " + std::to_string(row) + " " + std::string(60, 'y') + "\r\n";
    terminal.session().feed_output(screen);
    CK_CHECK(pump_until(terminals, [&] { return screen_of(terminal).find("row 23") != std::string::npos; }));
    const std::size_t whole_screen = delta_bytes(diffs, id, terminal);
    CK_CHECK(whole_screen > 0U);

    // One character, in place, changing nothing else.
    terminal.session().feed_output("\x1b[1;1HZ");
    CK_CHECK(pump_until(terminals, [&] { return screen_of(terminal).substr(0, 1) == "Z"; }));
    const std::size_t one_cell = delta_bytes(diffs, id, terminal);

    std::printf("  [budget] whole screen %zu bytes; one changed cell %zu bytes\n", whole_screen,
                one_cell);
    CK_CHECK(one_cell > 0U);
    // Two hundred bytes is a delta header, a cursor and a cell, with room to
    // spare. A screen-restating differ would be three orders of magnitude over.
    CK_CHECK(one_cell < 200U);
    terminals.close_all();
}

CK_TEST(an_attach_snapshot_for_ten_terminals_with_deep_history_stays_inside_its_budget) {
    // The testing plan §7's "attach-snapshot size for 10 terminals x 10k scrollback".
    // The snapshot is ONE message and the decoder refuses a frame past its cap,
    // so a session that grew too big to snapshot is a session nobody can ever
    // attach to again — a permanently unreachable reader, not a slow one
    // (the protocol spec: "bounding it is what makes the 16 MiB decoder cap an invariant
    // instead of a hope").
    Terminals terminals(test_settings(/*scrollback=*/10'000));
    DiffEngine diffs;
    std::vector<TerminalId> ids;
    for (int index = 0; index < 10; ++index) {
        Terminal& terminal = terminals.open(spec_running("cat", 80, 24));
        ids.push_back(terminal.id());
        // Deep history, in lines wide enough that the budget has to do real
        // work rather than fitting everything by accident.
        std::string history;
        for (int line = 0; line < 400; ++line)
            history += "history line " + std::to_string(line) + " " + std::string(60, 'h') + "\r\n";
        terminal.session().feed_output(history);
    }
    for (const TerminalId id : ids) {
        Terminal* const terminal = terminals.find(id);
        CK_CHECK(terminal != nullptr);
        if (terminal == nullptr) continue;
        CK_CHECK(pump_until(terminals, [&] {
            return screen_of(*terminal).find("history line 399") != std::string::npos;
        }));
    }

    // The snapshot the way the server builds one: every terminal's mandatory
    // part first, then the history sharing what is left. Reproduced here rather
    // than reached through `Server` so the number is about the payload and not
    // about a socket.
    ckm::proto::Attached attached;
    attached.session = 1;
    for (const TerminalId id : ids) {
        Terminal* const terminal = terminals.find(id);
        if (terminal == nullptr) continue;
        attached.snapshot.terminals.push_back(diffs.snapshot(id, terminal->session()));
    }
    // The server's own arithmetic (`Server::send_snapshot`): the mandatory part
    // measured whole, then whatever is left of the budget shared out for
    // history. Measured the same way here, or the gate would be about a number
    // the server never computes.
    const std::size_t spent = 8 + ckm::proto::encoded_size(attached.snapshot);
    std::printf("  [budget] the mandatory part of ten snapshots: %zu bytes (budget %u)\n", spent,
                ckm::proto::kSnapshotPayloadBudget);
    CK_CHECK(spent <= ckm::proto::kSnapshotPayloadBudget);
    std::size_t remaining =
        ckm::proto::kSnapshotPayloadBudget > spent ? ckm::proto::kSnapshotPayloadBudget - spent : 0;
    std::size_t left = attached.snapshot.terminals.size();
    for (ckm::proto::TerminalState& state : attached.snapshot.terminals) {
        Terminal* const terminal = terminals.find(state.term);
        const std::size_t share = remaining / std::max<std::size_t>(1, left);
        if (terminal != nullptr)
            remaining -= diffs.fill_history(state.term, state, terminal->session(), share);
        --left;
    }

    bool oversize = false;
    const std::size_t snapshot_bytes = ckm::proto::encode(attached, &oversize).size();
    std::printf("  [budget] attach snapshot, 10 terminals x 10k scrollback: %zu bytes (cap %u)\n",
                snapshot_bytes, ckm::proto::kMaxSnapshotPayloadBytes);
    // Not oversize is the invariant that matters — an oversize frame is one the
    // peer's decoder refuses, which loses the message AND the connection.
    CK_CHECK(!oversize);
    CK_CHECK(snapshot_bytes > 0U);
    CK_CHECK(snapshot_bytes <= ckm::proto::kMaxSnapshotPayloadBytes);
    terminals.close_all();
}

CK_TEST(a_session_nobody_is_watching_builds_no_pictures_at_all) {
    // The picture half of the budget, and the one WP-16 freed. A child that
    // draws continuously — ckvision_spin redrawing a cube — makes the differ
    // compare a megabyte and copy a megabyte into chunk messages, per tick, per
    // picture. For a session with no client attached, every one of those bytes
    // is built to be thrown away, and a detached session left running an
    // animation would burn that forever.
    //
    // Measured rather than argued: `DiffEngine::picture_bytes_built` counts what
    // the flush tick copied, and the readiness question the engine asks first is
    // what keeps it at zero.
    Terminals terminals(test_settings());
    DiffEngine diffs;
    Terminal& terminal = terminals.open(spec_running("cat", 80, 24));
    const TerminalId id = terminal.id();

    // A full-screen picture, redrawn at the same place in a different colour
    // each time — which is what an animation IS, and what makes every frame a
    // genuinely new payload rather than a cache hit.
    const auto frame = [](int shade) {
        std::string sixel = "\x1b[H\x1bPq#0;2;" + std::to_string(shade % 101) + ";20;60#0";
        for (int band = 0; band < 72; ++band) sixel += "!720~-";
        return sixel + "\x1b\\";
    };

    terminal.session().feed_output(frame(1));
    CK_CHECK(pump_until(terminals, [&] { return !terminal.session().rasters().empty(); }));

    // Nobody can take a picture, because nobody is attached: the readiness
    // question has no client to say yes.
    const auto nobody_is_ready = [](TerminalId, std::uint64_t) { return false; };
    // The first flush introduces the placement — a wire id that does not exist
    // yet cannot be "already owed", so its payload is built once and that is
    // correct. What is being gated is every frame after it.
    (void)diffs.flush(id, terminal.session(), nobody_is_ready);
    const std::size_t after_the_first = diffs.picture_bytes_built();

    for (int shade = 2; shade <= 60; ++shade) {
        terminal.session().feed_output(frame(shade));
        CK_CHECK(pump_until(terminals, [&] { return !terminal.session().rasters().empty(); }));
        (void)diffs.flush(id, terminal.session(), nobody_is_ready);
    }
    const std::size_t built = diffs.picture_bytes_built() - after_the_first;

    std::printf("  [budget] 59 animation frames with nobody attached: %zu bytes built\n", built);
    // Exactly zero. Not "small": there is no client, so there is no frame worth
    // one byte of memcmp or one byte of copying, and any number above zero is
    // the engine doing work for nobody.
    CK_CHECK(built == 0U);
    terminals.close_all();
}

CK_TEST(a_terminal_with_nothing_happening_says_nothing_and_costs_nothing) {
    // The testing plan §7's "allocations per idle minute (target: zero)", gated at the
    // level this suite can honestly reach: not the allocator, but what the
    // allocator would be asked for. An idle terminal that produced a delta per
    // tick would be a server talking to itself, and every one of those deltas
    // is an encode, a queue and a wakeup on every attached client.
    //
    // Stated plainly because the difference matters: this does NOT count
    // allocations. Counting them means replacing global operator new, which
    // collides with the sanitizer lane this suite is also required to pass
    // under. What it gates is the behaviour that would cause them.
    Terminals terminals(test_settings());
    DiffEngine diffs;
    Terminal& terminal = terminals.open(spec_running("sleep 30", 80, 24));
    const TerminalId id = terminal.id();

    terminal.session().feed_output("a line, and then nothing at all\r\n");
    CK_CHECK(pump_until(terminals, [&] { return screen_of(terminal).find("nothing at all") != std::string::npos; }));
    (void)diffs.flush(id, terminal.session());  // the news absorbed

    std::size_t spoke = 0;
    std::size_t bytes = 0;
    for (int tick = 0; tick < 200; ++tick) {
        terminals.drain(64 * 1024);
        const DiffEngine::TerminalTick quiet = diffs.flush(id, terminal.session());
        if (quiet.delta.has_value()) {
            ++spoke;
            bytes += ckm::proto::encode(*quiet.delta).size();
        }
        spoke += quiet.images.size();
    }

    std::printf("  [budget] 200 idle ticks: %zu deltas/ops, %zu bytes\n", spoke, bytes);
    CK_CHECK(spoke == 0U);
    CK_CHECK(bytes == 0U);
    terminals.close_all();
}
