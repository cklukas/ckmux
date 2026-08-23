// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// WP-4b: the diff algebra on the live path. Real PTYs, real children, and a
// mirror built only out of the deltas the server produced — because the claim
// is not "the server thinks it sent the right thing", it is "a client that saw
// only these bytes holds what the terminal holds".
#include <algorithm>
#include <chrono>
#include <memory>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "common/grid_delta.hpp"
#include "cvision/term/terminal_emulator.hpp"
#include "server/diff_engine.hpp"
#include "server/terminals.hpp"

#include "cvision/testing/cktest.hpp"

namespace {

using ckm::GridState;
using ckm::same_state;
using ckm::server::DiffEngine;
using ckm::server::Terminal;
using ckm::server::TerminalId;
using ckm::server::Terminals;
using ckm::server::TerminalSpec;

ckm::Settings test_settings() {
    ckm::Settings settings;
    settings.shell = "/bin/sh";
    settings.login_shell = false;
    settings.scrollback = 200;
    settings.term = "xterm-256color";
    return settings;
}

TerminalSpec spec_running(std::string command) {
    TerminalSpec spec;
    spec.command = std::move(command);
    spec.working_directory = "/";
    spec.columns = 40;
    spec.rows = 8;
    spec.pixel_width = 40 * 9;
    spec.pixel_height = 8 * 18;
    spec.environment = {{"TERM", "xterm-256color"}, {"PATH", "/usr/bin:/bin"}, {"LC_ALL", "C"}};
    return spec;
}

// Drains until `ready` or the budget runs out, in real time. An attempt is not
// a unit of waiting: the same loop spans milliseconds in one build and seconds
// in another.
template <typename Ready>
bool pump_until(Terminals& terminals, Ready ready, int budget_ms = 4000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
    for (;;) {
        terminals.drain(32 * 1024);
        if (ready()) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        (void)::usleep(2000);
    }
}

// A client's mirror, built from nothing but the deltas it received. This is the
// only honest check available at this layer: comparing the server's belief with
// the terminal would compare the server against itself.
struct Client {
    GridState mirror;
    std::uint32_t last_seq = 0;
    int gaps = 0;
    int refusals = 0;

    void attach(const ckm::proto::TerminalState& state) {
        mirror = ckm::blank_state(ckv::Size{state.columns, state.rows}, 200);
        (void)ckm::decode_grid(state.grid, mirror.cells, mirror);
        mirror.max_scrollback_lines = 200;
        mirror.scrollback.clear();
        mirror.scrollback_start = 0;
        for (const std::vector<ckm::proto::CellRun>& line : state.scrollback)
            ckm::push_scrollback_line(mirror, ckm::proto::from_runs(line));
        mirror.cursor = ckm::GridCursor{state.cursor.column, state.cursor.row, state.cursor.style,
                                        state.cursor.visible, state.cursor.blink};
        mirror.modes = state.modes;
        mirror.title = state.title;
        last_seq = 0;
    }

    void receive(const ckm::proto::GridDelta& delta) {
        if (delta.seq != last_seq + 1) ++gaps;
        last_seq = delta.seq;
        if (!ckm::apply_delta(delta.ops, mirror)) ++refusals;
    }
};

std::string screen_text(const Terminal& terminal) {
    std::string text;
    for (const ckv::Cell& cell : terminal.session().cells())
        if (!cell.is_continuation()) text += cell.grapheme();
    return text;
}

std::string text_of(const GridState& state) {
    std::string text;
    for (const ckv::Cell& cell : state.grid)
        if (!cell.is_continuation()) text += cell.grapheme();
    return text;
}

ckv::Cell cell_of(std::string_view text) {
    return ckv::Cell::from_grapheme(text, ckv::Style{});
}

// An emulator whose history holds `lines` rows of `columns` cells, no two
// neighbours alike. Varied on purpose: a screenful of blanks run-length
// encodes to almost nothing, so a history built out of them would make every
// size claim below pass without saying anything about a real session.
ckv::term::TerminalEmulator crowded_emulator(int columns, int rows, std::size_t lines) {
    ckv::term::TerminalCapabilityProfile profile = ckv::term::embedded_xterm_sixel_profile();
    profile.cells = ckv::Size{columns, rows};
    profile.cell_pixels = ckv::Size{9, 18};
    ckv::term::TerminalSubsessionOptions options;
    options.max_scrollback_lines = lines;
    options.max_output_bytes = 1U << 20U;
    options.max_parser_work_per_step = 128U << 10U;
    ckv::term::TerminalEmulator emulator(profile, options);

    std::string chunk;
    // The screen's own rows on top of the history's, so the history really
    // fills rather than stopping a screenful short. One column short of the
    // width, so that no line depends on how the emulator treats a write that
    // lands exactly on the last column — a deferred wrap and an immediate one
    // differ by a blank row per line, and this measures bytes.
    for (std::size_t line = 0; line < lines + static_cast<std::size_t>(rows); ++line) {
        for (int column = 0; column + 1 < columns; ++column)
            chunk.push_back(static_cast<char>('a' + ((line * 7 + static_cast<std::size_t>(column)) % 26)));
        chunk += "\r\n";
        if (chunk.size() < (32U << 10U)) continue;
        emulator.feed_output(chunk);
        chunk.clear();
    }
    if (!chunk.empty()) emulator.feed_output(chunk);
    return emulator;
}

// What the whole of a terminal's history would cost on the wire, measured a
// row at a time so that measuring it never costs what sending it would.
std::size_t whole_history_bytes(const ckv::core::TerminalSubsession& source, int columns) {
    const std::span<const ckv::Cell> history = source.scrollback();
    const std::size_t width = static_cast<std::size_t>(std::max(1, columns));
    std::size_t total = 4;  // the line count
    for (std::size_t line = 0; line + width <= history.size(); line += width) {
        const std::span<const ckv::Cell> row = history.subspan(line, width);
        total += ckm::proto::encoded_size(
            ckm::proto::to_runs(std::vector<ckv::Cell>(row.begin(), row.end())));
    }
    return total;
}

}  // namespace

CK_TEST(a_client_that_saw_only_the_deltas_holds_what_the_terminal_holds) {
    Terminals terminals(test_settings());
    DiffEngine engine;
    Terminal& terminal =
        terminals.open(spec_running("printf 'first line\\r\\n'; sleep 0.2; printf 'second\\r\\n'; "
                                    "sleep 0.2; printf 'third\\r\\n'; sleep 5"));
    Client client;

    // The attach snapshot first, then every tick's delta — which is the order a
    // client experiences and the only order the sequence check means anything
    // in.
    // Waiting for damage would not do: damage starts FULL on a fresh terminal,
    // so it is already set before the child has said a word.
    CK_CHECK(pump_until(terminals,
                        [&] { return screen_text(terminal).find("first line") != std::string::npos; }));
    client.attach(engine.snapshot(terminal.id(), terminal.session()));

    const auto pump_tick = [&] {
        terminals.drain(32 * 1024);
        for (const ckm::proto::GridDelta& delta : engine.flush(terminals).deltas) client.receive(delta);
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(4000);
    while (std::chrono::steady_clock::now() < deadline) {
        pump_tick();
        if (text_of(client.mirror).find("third") != std::string::npos) break;
        (void)::usleep(5000);
    }
    pump_tick();

    CK_CHECK(text_of(client.mirror).find("first line") != std::string::npos);
    CK_CHECK(text_of(client.mirror).find("second") != std::string::npos);
    CK_CHECK(text_of(client.mirror).find("third") != std::string::npos);
    CK_CHECK(client.gaps == 0);
    CK_CHECK(client.refusals == 0);

    // And cell for cell, not just "the words are there". The server's belief is
    // what it will diff against next tick; if it and the client have drifted,
    // every later delta is computed against a screen nobody has.
    const ckm::server::TerminalDiffer* differ = engine.differ_for(terminal.id());
    CK_CHECK(differ != nullptr);
    if (differ != nullptr) {
        GridState believed = differ->believed();
        // The server keeps no history and the client keeps all of it, by
        // design; the grids are what must agree.
        GridState held = client.mirror;
        held.scrollback.clear();
        held.scrollback_start = 0;
        held.max_scrollback_lines = 0;
        CK_CHECK(same_state(believed, held));
    }
}

CK_TEST(forty_writes_between_two_ticks_cost_one_delta) {
    // Coalescing, which is what makes a flush tick a tick rather than a
    // forwarder. The child writes many times; the server reads damage once.
    Terminals terminals(test_settings());
    DiffEngine engine;
    Terminal& terminal = terminals.open(
        spec_running("i=0; while [ $i -lt 40 ]; do printf 'chunk%d ' $i; i=$((i+1)); done; sleep 5"));
    // The whole burst has to be in the emulator before the single flush, and
    // "damage exists" cannot say that — it is set before the child starts.
    CK_CHECK(pump_until(terminals,
                        [&] { return screen_text(terminal).find("chunk39") != std::string::npos; }));
    // Give the child time to finish the burst, still without flushing.
    for (int wait = 0; wait < 40; ++wait) {
        terminals.drain(32 * 1024);
        (void)::usleep(5000);
    }

    const std::vector<ckm::proto::GridDelta> deltas = engine.flush(terminals).deltas;
    CK_CHECK(deltas.size() == 1U);
    if (deltas.size() == 1U) {
        CK_CHECK(deltas.front().seq == 1U);
        CK_CHECK(deltas.front().term == terminal.id());
    }
    // And nothing at all on a tick where the child said nothing.
    CK_CHECK(engine.flush(terminals).deltas.empty());
    CK_CHECK(engine.flush(terminals).deltas.empty());
}

CK_TEST(a_delta_is_withheld_while_a_synchronized_frame_is_open_and_sent_whole_once_it_closes) {
    // ckvision_spin — and any well-behaved redrawing program — brackets a
    // frame in DEC 2026 precisely so a host never shows it half-drawn.
    // Flushed across several ticks the way the real server loop does — a
    // drain per PTY-readable wakeup, a flush at its own fixed cadence — so a
    // frame the child wrote as one burst can still land across more than one
    // of this engine's own ticks: this is the field report ("sixel graphics
    // flickering heavily... run directly, it's not flickering") reproduced
    // at the diff-engine layer, without a PTY or real timing involved.
    ckv::term::TerminalCapabilityProfile profile = ckv::term::embedded_xterm_sixel_profile();
    profile.cells = ckv::Size{20, 4};
    ckv::term::TerminalEmulator emulator(profile);
    DiffEngine engine;
    Client client;
    client.attach(engine.snapshot(1, emulator));
    emulator.clear_damage();  // the snapshot said everything

    emulator.feed_output("\x1b[?2026h");
    CK_CHECK(!engine.flush(1, emulator).delta.has_value());
    emulator.feed_output("\x1b[1;1Hfirst");
    CK_CHECK(!engine.flush(1, emulator).delta.has_value());
    emulator.feed_output("\x1b[2;1Hsecond");
    CK_CHECK(!engine.flush(1, emulator).delta.has_value());
    CK_CHECK(text_of(client.mirror).find("first") == std::string::npos);

    // The close arrives, and everything the whole frame changed comes out as
    // the one delta the child asked for — not one per write.
    emulator.feed_output("\x1b[?2026l");
    const auto tick = engine.flush(1, emulator);
    CK_CHECK(tick.delta.has_value());
    if (tick.delta.has_value()) client.receive(*tick.delta);
    CK_CHECK(text_of(client.mirror).find("first") != std::string::npos);
    CK_CHECK(text_of(client.mirror).find("second") != std::string::npos);

    // And the hold does not outlive the frame it was for: an ordinary write
    // after the close goes out on its own tick again.
    emulator.feed_output("\x1b[3;1Hthird");
    CK_CHECK(engine.flush(1, emulator).delta.has_value());
}

CK_TEST(the_sequence_is_monotonic_with_no_gaps_however_the_terminals_behave) {
    // The adversarial half of the sequence rule: several terminals, one silent,
    // one flooding, one resized under load, one closing mid-run, plus ticks
    // where nothing happened at all — and per terminal the numbers must still be
    // 1, 2, 3 with nothing missing and nothing repeated. A client checks exactly
    // this and reconnects when it fails, so a false gap is a reattach storm and
    // a missed gap is a screen that quietly stops matching.
    Terminals terminals(test_settings());
    DiffEngine engine;
    Terminal& quiet = terminals.open(spec_running("read line"));
    Terminal& chatty = terminals.open(spec_running("while :; do printf 'tick '; sleep 0.02; done"));
    Terminal& flood = terminals.open(spec_running("yes diff-engine-flood"));
    Terminal& doomed = terminals.open(spec_running("printf 'here\\r\\n'; sleep 30"));
    const TerminalId doomed_id = doomed.id();

    std::map<TerminalId, std::uint32_t> last;
    int gaps = 0;
    int repeats = 0;
    int deltas_seen = 0;
    for (int tick = 0; tick < 60; ++tick) {
        terminals.drain(4 * 1024);
        if (tick == 20) chatty.resize(52, 10, 52 * 9, 10 * 18);
        if (tick == 30) {
            engine.forget(doomed_id);
            terminals.close(doomed_id);
        }
        for (const ckm::proto::GridDelta& delta : engine.flush(terminals).deltas) {
            ++deltas_seen;
            const std::uint32_t previous = last.count(delta.term) ? last[delta.term] : 0;
            if (delta.seq == previous) ++repeats;
            if (delta.seq != previous + 1) ++gaps;
            last[delta.term] = delta.seq;
        }
        (void)::usleep(3000);
    }

    CK_CHECK(deltas_seen > 10);
    CK_CHECK(gaps == 0);
    CK_CHECK(repeats == 0);
    // The flood produced far more than the quiet terminal: the sequence is per
    // terminal, not a shared counter that would make one child's traffic show
    // up as gaps in another's stream.
    CK_CHECK(last[flood.id()] > 0U);
    CK_CHECK(last.count(quiet.id()) == 0U || last[quiet.id()] <= last[flood.id()]);
    // A resnapshot restarts the count, so a client's continuity check works
    // from its first delta rather than having to trust it.
    (void)engine.snapshot(chatty.id(), chatty.session());
    CK_CHECK(engine.sequence_for(chatty.id()) == 0U);
    CK_CHECK(pump_until(terminals, [&] { return chatty.damage().any(); }, 2000));
    const std::vector<ckm::proto::GridDelta> after = engine.flush(terminals).deltas;
    bool saw_chatty = false;
    for (const ckm::proto::GridDelta& delta : after)
        if (delta.term == chatty.id()) {
            saw_chatty = true;
            CK_CHECK(delta.seq == 1U);
        }
    CK_CHECK(saw_chatty);
}

CK_TEST(a_resize_costs_the_whole_grid_and_the_mirror_still_matches) {
    Terminals terminals(test_settings());
    DiffEngine engine;
    Terminal& terminal = terminals.open(spec_running("printf 'before the resize\\r\\n'; sleep 5"));
    Client client;
    CK_CHECK(pump_until(
        terminals, [&] { return screen_text(terminal).find("before the resize") != std::string::npos; }));
    client.attach(engine.snapshot(terminal.id(), terminal.session()));
    for (const ckm::proto::GridDelta& delta : engine.flush(terminals).deltas) client.receive(delta);

    terminal.resize(60, 12, 60 * 9, 12 * 18);
    terminals.drain(32 * 1024);
    const std::vector<ckm::proto::GridDelta> deltas = engine.flush(terminals).deltas;
    CK_CHECK(deltas.size() == 1U);
    if (deltas.empty()) return;

    // Every row of the new size, because what a client's mirror does with its
    // own cells when it resizes is the client's business — and a server that
    // guessed would leave stale cells wherever it guessed wrong.
    int cell_ops = 0;
    for (const ckm::proto::GridOp& op : deltas.front().ops)
        if (std::holds_alternative<ckm::proto::CellsOp>(op)) ++cell_ops;
    CK_CHECK(cell_ops == 12);

    // And the size itself, stated first, before anything measured against it.
    // This test used to rebuild the mirror by hand here, with a comment saying
    // the client resizes on `TermMeta` — which was false twice over: nothing
    // sends a `TermMeta` on resize and `TermMeta` has no size field. What
    // really happened is C3: a repaint of a smaller terminal fitted inside the
    // larger mirror, so the rows and columns past the new edge kept the old
    // program's output and nothing anywhere noticed.
    const auto* resize = std::get_if<ckm::proto::ResizeOp>(&deltas.front().ops.front());
    CK_CHECK(resize != nullptr);
    if (resize != nullptr) {
        CK_CHECK(resize->columns == 60);
        CK_CHECK(resize->rows == 12);
    }

    // No hand-rebuilt mirror: the delta is applied to the mirror as it stands,
    // at the OLD size, and it comes out the new one.
    CK_CHECK((client.mirror.cells == ckv::Size{40, 8}));
    client.receive(deltas.front());
    CK_CHECK(client.refusals == 0);
    CK_CHECK((client.mirror.cells == ckv::Size{60, 12}));
    CK_CHECK(client.mirror.grid.size() == 720U);
    CK_CHECK(text_of(client.mirror).find("before the resize") != std::string::npos);

    // And the server's belief and the client's mirror still agree, cell for
    // cell — the check that says the resize was carried rather than guessed at
    // by both ends independently.
    const ckm::server::TerminalDiffer* differ = engine.differ_for(terminal.id());
    CK_CHECK(differ != nullptr);
    if (differ != nullptr) {
        GridState held = client.mirror;
        held.scrollback.clear();
        held.scrollback_start = 0;
        held.max_scrollback_lines = 0;
        CK_CHECK(same_state(differ->believed(), held));
    }
}

CK_TEST(a_tick_costs_what_changed_rather_than_what_the_terminal_remembers) {
    // WP-4b's measured criterion, and the one number that says U0-b actually
    // arrived: the same one-line mutation, diffed against a terminal holding no
    // history and one holding two thousand lines of it. The per-tick cost must
    // not grow with what the terminal remembers, because a session that gets
    // slower the longer it has been alive is the failure this path exists to
    // avoid.
    //
    // Measured against a bare emulator rather than a child on a PTY: a shell's
    // scheduling in the number would make the number about the shell. What is
    // checked is the RATIO, which stays true on a machine faster or busier than
    // this one; the absolute figures are printed for the record.
    const auto cost_per_tick = [](std::size_t history_lines, double* out_cost) {
        ckv::term::TerminalCapabilityProfile profile = ckv::term::embedded_xterm_sixel_profile();
        profile.cells = ckv::Size{120, 40};
        profile.cell_pixels = ckv::Size{9, 18};
        ckv::term::TerminalSubsessionOptions options;
        options.max_scrollback_lines = history_lines;
        options.max_output_bytes = 1U << 20U;
        options.max_parser_work_per_step = 128U << 10U;
        ckv::term::TerminalEmulator emulator(profile, options);

        // Fill the history first: the steady state is what a long-lived session
        // pays, not what its first frame costs.
        std::string chunk;
        for (std::size_t line = 0; line < history_lines + 40; ++line) {
            chunk += "history line " + std::to_string(line) + " already scrolled away\r\n";
            if (chunk.size() < (32U << 10U)) continue;
            emulator.feed_output(chunk);
            chunk.clear();
        }
        if (!chunk.empty()) emulator.feed_output(chunk);

        // Through the engine, not a bare differ: the engine is what clears the
        // damage, and a loop that never clears it re-reads everything the
        // terminal has ever done on every tick.
        ckm::server::DiffEngine engine;
        (void)engine.snapshot(1, emulator);
        emulator.clear_damage();  // the snapshot said everything

        const int ticks = 500;
        int deltas = 0;
        const auto start = std::chrono::steady_clock::now();
        for (int tick = 0; tick < ticks; ++tick) {
            emulator.feed_output("\x1b[5;1H status: frame " + std::to_string(tick) + " of a line a "
                                 "program rewrites every frame ");
            if (engine.flush(1, emulator).delta.has_value()) ++deltas;
        }
        *out_cost = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
                        .count() /
                    ticks;
        return deltas;
    };

    double bare = 0.0;
    double deep = 0.0;
    const int bare_deltas = cost_per_tick(0, &bare);
    const int deep_deltas = cost_per_tick(2000, &deep);
    std::printf("  [diff engine] 120x40, one row rewritten: %.1f us/tick with no history, "
                "%.1f us/tick with 2000 lines\n",
                bare, deep);
    // Every tick had news, so both numbers are the cost of a delta and not the
    // cost of deciding there was nothing to send.
    CK_CHECK(bare_deltas == 500);
    CK_CHECK(deep_deltas == 500);
    CK_CHECK(bare > 0.0);
    // Flat, not merely bounded: twice the cost would still be fast and would
    // still mean the history is being touched every tick.
    CK_CHECK(deep < bare * 1.5 + 5.0);

    // And the other half of the criterion: N terminals at a flush tick. Sixty
    // of them, each 120x40 with two thousand lines of history and each with a
    // row rewritten every tick — a server busier than any reader will make it.
    const int fleet = 60;
    std::vector<std::unique_ptr<ckv::term::TerminalEmulator>> terminals_under_test;
    ckv::term::TerminalCapabilityProfile profile = ckv::term::embedded_xterm_sixel_profile();
    profile.cells = ckv::Size{120, 40};
    profile.cell_pixels = ckv::Size{9, 18};
    ckv::term::TerminalSubsessionOptions options;
    options.max_scrollback_lines = 2000;
    options.max_output_bytes = 1U << 20U;
    options.max_parser_work_per_step = 128U << 10U;
    ckm::server::DiffEngine fleet_engine;
    for (int index = 0; index < fleet; ++index) {
        terminals_under_test.push_back(
            std::make_unique<ckv::term::TerminalEmulator>(profile, options));
        std::string chunk;
        for (int line = 0; line < 200; ++line)
            chunk += "terminal " + std::to_string(index) + " line " + std::to_string(line) + "\r\n";
        terminals_under_test.back()->feed_output(chunk);
        (void)fleet_engine.snapshot(static_cast<TerminalId>(index + 1), *terminals_under_test.back());
        terminals_under_test.back()->clear_damage();
    }

    const int fleet_ticks = 60;
    int fleet_deltas = 0;
    const auto fleet_start = std::chrono::steady_clock::now();
    for (int tick = 0; tick < fleet_ticks; ++tick)
        for (int index = 0; index < fleet; ++index) {
            terminals_under_test[static_cast<std::size_t>(index)]->feed_output(
                "\x1b[7;1H frame " + std::to_string(tick) + " on a busy server ");
            if (fleet_engine.flush(static_cast<TerminalId>(index + 1),
                                   *terminals_under_test[static_cast<std::size_t>(index)])
                    .delta.has_value())
                ++fleet_deltas;
        }
    const double per_tick = std::chrono::duration<double, std::micro>(
                                std::chrono::steady_clock::now() - fleet_start)
                                .count() /
                            fleet_ticks;
    std::printf("  [diff engine] %d terminals, each 120x40 with 2000 lines of history: "
                "%.0f us per flush tick (%.1f us each)\n",
                fleet, per_tick, per_tick / fleet);
    // Every terminal had news on every tick, so this is the cost of sixty
    // deltas and not of sixty terminals being found to be idle.
    CK_CHECK(fleet_deltas == fleet * fleet_ticks);
    // Linear in the number of terminals, which is the only shape that can be
    // reasoned about: each terminal's cost is its own.
    CK_CHECK(per_tick / fleet < deep * 3.0 + 20.0);
}

CK_TEST(the_history_a_child_scrolled_away_reaches_the_client_as_lines) {
    Terminals terminals(test_settings());
    DiffEngine engine;
    Terminal& terminal = terminals.open(spec_running("sleep 30"));
    Client client;
    terminal.session().feed_output("starting\r\n");
    terminals.drain(4096);
    client.attach(engine.snapshot(terminal.id(), terminal.session()));

    // Twelve lines on an eight-row screen: four leave for the history.
    std::string burst;
    for (int line = 0; line < 12; ++line) burst += "scrolled-" + std::to_string(line) + "\r\n";
    terminal.session().feed_output(burst);
    for (const ckm::proto::GridDelta& delta : engine.flush(terminals).deltas) client.receive(delta);

    CK_CHECK(client.refusals == 0);
    CK_CHECK(client.gaps == 0);
    CK_CHECK(ckm::scrollback_lines(client.mirror) != 0);
    // The oldest line the child scrolled away is in the client's own history,
    // which is what makes paging and copy mode local (the protocol spec).
    std::string history;
    for (const ckv::Cell& cell : ckm::scrollback_cells(client.mirror)) history += cell.grapheme();
    CK_CHECK(history.find("starting") != std::string::npos);
    CK_CHECK(history.find("scrolled-0") != std::string::npos);

    // And the screen still holds the newest lines, in the right place.
    CK_CHECK(text_of(client.mirror).find("scrolled-11") != std::string::npos);
}

CK_TEST(a_scrolling_child_costs_a_scroll_op_on_the_live_path_too) {
    // The op WP-4a exists for, arriving where it matters: a real terminal
    // scrolling under a real damage report. The emulator marks every row of a
    // scrolled screen as changed, so without the shift search this delta would
    // carry the whole screen.
    Terminals terminals(test_settings());
    DiffEngine engine;
    Terminal& terminal = terminals.open(spec_running("sleep 30"));
    std::string fill;
    for (int line = 0; line < 8; ++line) fill += "row " + std::to_string(line) + "\r\n";
    terminal.session().feed_output(fill);
    terminals.drain(4096);
    (void)engine.snapshot(terminal.id(), terminal.session());

    terminal.session().feed_output("one more line\r\n");
    const std::vector<ckm::proto::GridDelta> deltas = engine.flush(terminals).deltas;
    CK_CHECK(deltas.size() == 1U);
    if (deltas.empty()) return;
    int scrolls = 0;
    int cell_ops = 0;
    for (const ckm::proto::GridOp& op : deltas.front().ops) {
        if (std::holds_alternative<ckm::proto::ScrollOp>(op)) ++scrolls;
        if (std::holds_alternative<ckm::proto::CellsOp>(op)) ++cell_ops;
    }
    CK_CHECK(scrolls == 1);
    CK_CHECK(cell_ops <= 2);
}

// --- R1: a snapshot that always fits ---------------------------------------

CK_TEST(the_size_a_snapshot_will_take_is_known_before_it_is_written) {
    // The whole point of `encoded_size` is deciding what to send BEFORE the
    // bytes exist. A second encoder that had drifted from the first would
    // decide wrongly and silently, so it is pinned against what `encode`
    // actually produces, over a snapshot with something in every field that
    // varies: a title with a multi-byte character in it, a grid with a wide
    // character and its continuation half, history, images and a print job.
    ckm::proto::Attached attached;
    attached.session = 7;
    attached.snapshot.desktop_columns = 100;
    attached.snapshot.desktop_rows = 30;
    attached.snapshot.focused_term = 11;

    ckm::proto::TerminalState busy;
    busy.term = 11;
    busy.index = 2;
    busy.title = "vim plans/05-protocol.md ✓";
    busy.columns = 4;
    busy.rows = 2;
    busy.grid = ckm::proto::to_runs({cell_of("a"), cell_of("a"), cell_of("漢"),
                                     ckv::Cell::continuation(ckv::Style{}), cell_of("b"),
                                     cell_of("c"), cell_of(" "), cell_of(" ")});
    busy.scrollback = {ckm::proto::to_runs({cell_of("o"), cell_of("l"), cell_of("d")}),
                       ckm::proto::to_runs({cell_of("e"), cell_of("r")})};
    busy.images = {77, 78};
    busy.print_jobs.push_back(
        ckm::proto::PrintJobInfo{5, ckm::proto::PrintJobKind::Autoprint, 900, 30, -7});
    // And the reattach fields (R8), the diagnostic included, because it is the
    // second variable-length field a terminal state carries and an arithmetic
    // that forgot it would be wrong by exactly one complaint.
    busy.clipboard_serial = 4;
    busy.exited = 1;
    busy.exit_status = 2;
    busy.hold = 1;
    busy.diagnostic_kind = ckm::proto::DiagnosticKind::UnsupportedSequence;
    busy.diagnostic = "unsupported child OSC sequence ✗";
    attached.snapshot.terminals.push_back(std::move(busy));

    // And one with nothing at all in it, so the fixed part is checked on its
    // own rather than inside a sum where an error of a few bytes hides.
    ckm::proto::TerminalState bare;
    bare.term = 12;
    attached.snapshot.terminals.push_back(std::move(bare));

    // 89 fixed bytes plus the two counts that say the grid and the history are
    // empty. It was 71 + 8 before WP-30 put an eight-byte tile share on every
    // terminal state, 79 + 8 before the custom title's own two-byte length
    // joined it, and 81 + 8 before WP-41 added the bell and activity serials;
    // the decoder's own per-entry minimum moves by the same amount each time,
    // and the two are checked against each other by this number being written
    // down twice on purpose.
    //
    // Which is what caught WP-41: the struct and the writer grew by eight
    // bytes and `kFixed` did not, so the predictor ran short and the failure
    // surfaced here rather than anywhere near the change.
    CK_CHECK(ckm::proto::encoded_size(attached.snapshot.terminals[1]) == 97U);
    const std::string frame = ckm::proto::encode(attached);
    CK_CHECK(!frame.empty());
    CK_CHECK(frame.size() ==
             ckm::proto::kHeaderBytes + 8 + ckm::proto::encoded_size(attached.snapshot));
}

CK_TEST(a_default_configuration_attach_fits_under_the_cap) {
    // C1's reproduction, and the regression test for it: a hundred columns and
    // the default ten-thousand-line history made a 16.29 MiB `Attached`, past
    // the DECODER's own limit — so the client exited with "the server sent
    // something this build cannot read" and the session was permanently
    // unattachable. Not a slow attach: no attach at all, for good.
    constexpr int kColumns = 100;
    constexpr std::size_t kDefaultScrollback = 10000;  // [general] scrollback's default
    ckv::term::TerminalEmulator emulator = crowded_emulator(kColumns, 24, kDefaultScrollback);
    DiffEngine engine;

    // The history really is the default's worth, said here so that a shortfall
    // diagnoses itself instead of quietly weakening the measurement below.
    const std::span<const ckv::Cell> held = emulator.scrollback();
    const std::size_t held_lines = held.size() / static_cast<std::size_t>(kColumns);
    CK_CHECK(held_lines >= kDefaultScrollback - 8);

    // What the old snapshot carried, measured rather than asserted: this is
    // the number that made the frame unreadable, and if it ever stops being
    // over the cap this test has stopped testing anything.
    const std::size_t whole = whole_history_bytes(emulator, kColumns);
    std::printf("  [snapshot] %d columns x %zu lines of history: %.2f MiB of scrollback alone\n",
                kColumns, kDefaultScrollback, static_cast<double>(whole) / (1024.0 * 1024.0));
    CK_CHECK(whole > ckm::proto::kMaxSnapshotPayloadBytes);

    ckm::proto::Attached attached;
    attached.session = 1;
    attached.snapshot.desktop_columns = kColumns;
    attached.snapshot.desktop_rows = 24;
    attached.snapshot.terminals.push_back(engine.snapshot(1, emulator));
    // A screen goes whole — it is what the terminal IS — and what is left of
    // the budget after the screens is what the histories share.
    const std::size_t screens = 8 + ckm::proto::encoded_size(attached.snapshot);
    CK_CHECK(screens < ckm::proto::kSnapshotPayloadBudget);
    const std::size_t spent = engine.fill_history(1, attached.snapshot.terminals.front(), emulator,
                                                  ckm::proto::kSnapshotPayloadBudget - screens);
    CK_CHECK(spent > 0U);

    const std::string frame = ckm::proto::encode(attached);
    CK_CHECK(!frame.empty());
    CK_CHECK(frame.size() <= ckm::proto::kHeaderBytes + ckm::proto::kSnapshotPayloadBudget);
    CK_CHECK(frame.size() <= ckm::proto::kHeaderBytes + ckm::proto::kMaxSnapshotPayloadBytes);

    // And it is a bound, not a discard: the reader still gets thousands of
    // lines to page back through, and they are the NEWEST ones — the half a
    // reader actually pages into.
    const std::vector<std::vector<ckm::proto::CellRun>>& history =
        attached.snapshot.terminals.front().scrollback;
    CK_CHECK(history.size() > 1000U);
    CK_CHECK(history.size() < kDefaultScrollback);
    const std::span<const ckv::Cell> newest =
        held.subspan((held_lines - 1) * static_cast<std::size_t>(kColumns),
                     static_cast<std::size_t>(kColumns));
    const std::vector<ckv::Cell> restored = ckm::proto::from_runs(history.back());
    CK_CHECK(restored.size() == static_cast<std::size_t>(kColumns));
    CK_CHECK(restored.front().grapheme() == newest.front().grapheme());
}

CK_TEST(a_terminal_with_a_long_history_does_not_starve_its_neighbours) {
    // The fair share, which is the whole reason the history is filled in a
    // second pass: one terminal that has been running `find /` all week must
    // not spend the session's entire budget before the terminals beside it are
    // asked. Each takes what it can from an equal share, and what it does not
    // use passes to the next — so terminals with no history waste nothing.
    constexpr int kColumns = 80;
    ckv::term::TerminalEmulator hoarder = crowded_emulator(kColumns, 8, 2000);
    ckv::term::TerminalEmulator modest = crowded_emulator(kColumns, 8, 12);
    ckv::term::TerminalEmulator quiet = crowded_emulator(kColumns, 8, 4);

    DiffEngine engine;
    std::vector<ckm::proto::TerminalState> states;
    states.push_back(engine.snapshot(1, hoarder));
    states.push_back(engine.snapshot(2, modest));
    states.push_back(engine.snapshot(3, quiet));

    // A budget the first terminal alone would eat several times over, so the
    // sharing is what decides the outcome rather than there being room for
    // everyone.
    const std::size_t budget = 64u * 1024u;
    CK_CHECK(whole_history_bytes(hoarder, kColumns) > budget * 4);

    std::size_t remaining = budget;
    std::size_t left = states.size();
    const ckv::core::TerminalSubsession* sources[3] = {&hoarder, &modest, &quiet};
    for (std::size_t index = 0; index < states.size(); ++index) {
        const std::size_t share = remaining / std::max<std::size_t>(1, left);
        remaining -= engine.fill_history(static_cast<TerminalId>(index + 1), states[index],
                                         *sources[index], share);
        --left;
    }

    for (const ckm::proto::TerminalState& state : states) CK_CHECK(!state.scrollback.empty());
    // The two small ones fit whole; the greedy one took a share and no more.
    CK_CHECK(states[1].scrollback.size() == 12U);
    CK_CHECK(states[2].scrollback.size() == 4U);
    std::size_t total = 0;
    for (const ckm::proto::TerminalState& state : states)
        total += ckm::proto::encoded_size(state.scrollback);
    CK_CHECK(total <= budget + 3 * 4);  // each line count is the caller's, not the share's
}

// --- WP-16: pictures on the wire -------------------------------------------

namespace {

// An 8x12 pure-red picture at row 2, column 3 (so the anchor is not the
// origin), drawn the way a child draws one.
constexpr std::string_view kRedSixelAt23 = "\x1b[2;3H\x1bPq#0;2;100;0;0!8~-!8~\x1b\\";

ckv::term::TerminalEmulator sixel_emulator() {
    ckv::term::TerminalSubsessionOptions options;
    return ckv::term::TerminalEmulator(ckv::term::embedded_xterm_sixel_profile(), options);
}

}  // namespace

CK_TEST(a_picture_the_child_draws_travels_as_image_ops_and_only_once) {
    ckv::term::TerminalEmulator emulator = sixel_emulator();
    DiffEngine engine;
    emulator.feed_output(kRedSixelAt23);

    DiffEngine::TerminalTick tick = engine.flush(1, emulator);
    // Begin, at least one chunk, end, place — in that order.
    CK_CHECK(tick.images.size() >= 4U);
    if (tick.images.size() < 4U) return;
    const auto* begin = std::get_if<ckm::proto::ImageAddBegin>(&tick.images.front());
    CK_CHECK(begin != nullptr);
    if (begin == nullptr) return;
    CK_CHECK(begin->width == 8U);
    CK_CHECK(begin->height == 12U);

    std::size_t pixel_bytes = 0;
    bool ended = false;
    const ckm::proto::ImagePlace* place = nullptr;
    for (const ckm::proto::Message& op : tick.images) {
        if (const auto* chunk = std::get_if<ckm::proto::ImageChunk>(&op))
            pixel_bytes += chunk->bytes.size();
        if (std::holds_alternative<ckm::proto::ImageEnd>(op)) ended = true;
        if (const auto* placed = std::get_if<ckm::proto::ImagePlace>(&op)) place = placed;
    }
    CK_CHECK(ended);
    CK_CHECK(pixel_bytes == static_cast<std::size_t>(begin->width) * begin->height * 4U);
    CK_CHECK(place != nullptr);
    if (place == nullptr) return;
    CK_CHECK(place->term == 1U);
    CK_CHECK(place->id == begin->id);
    CK_CHECK(place->cells.x == 2);
    CK_CHECK(place->cells.y == 1);

    // Said once: a tick where the pictures did not change says nothing about
    // them, however often it is taken.
    CK_CHECK(engine.flush(1, emulator).images.empty());
    CK_CHECK(engine.flush(1, emulator).images.empty());
}

CK_TEST(a_picture_redrawn_in_place_keeps_its_wire_id_and_is_never_removed) {
    // An animating child — ckvision_spin is the field case — redraws its
    // picture at the same anchor every frame. That is the same PLACEMENT
    // with new pixels, and it must travel as one: pixels again under the
    // old id, then the Place that swaps them, and NO Remove. Remove-then-
    // Add-under-a-new-id was the flicker: the Remove is a few bytes and
    // crossed the socket first, the replacement is megabytes and arrived
    // over many reads, and every client repaint in between painted the
    // fallback where the picture stood — once per animation frame.
    ckv::term::TerminalEmulator emulator = sixel_emulator();
    DiffEngine engine;
    emulator.feed_output(kRedSixelAt23);
    const DiffEngine::TerminalTick first = engine.flush(1, emulator);
    const auto* begin = first.images.empty()
                            ? nullptr
                            : std::get_if<ckm::proto::ImageAddBegin>(&first.images.front());
    CK_CHECK(begin != nullptr);
    if (begin == nullptr) return;
    const std::uint64_t original_id = begin->id;

    // The next frame: same spot, same size, different pixels (green now).
    emulator.feed_output("\x1b[2;3H\x1bPq#0;2;0;100;0!8~-!8~\x1b\\");
    const DiffEngine::TerminalTick second = engine.flush(1, emulator);
    CK_CHECK(!second.images.empty());
    bool any_remove = false;
    const ckm::proto::ImageAddBegin* readd = nullptr;
    const ckm::proto::ImagePlace* replace = nullptr;
    for (const ckm::proto::Message& op : second.images) {
        if (std::holds_alternative<ckm::proto::ImageRemove>(op)) any_remove = true;
        if (const auto* b = std::get_if<ckm::proto::ImageAddBegin>(&op)) readd = b;
        if (const auto* p = std::get_if<ckm::proto::ImagePlace>(&op)) replace = p;
    }
    CK_CHECK(!any_remove);
    CK_CHECK(readd != nullptr);
    CK_CHECK(replace != nullptr);
    if (readd != nullptr) CK_CHECK(readd->id == original_id);
    if (replace != nullptr) CK_CHECK(replace->id == original_id);

    // And attach_images now serializes the CURRENT frame under that id: a
    // client attaching mid-animation is given the pixels the watching
    // clients hold, not the frame before.
    bool attach_readds_same_id = false;
    for (const ckm::proto::Message& op : engine.attach_images(1))
        if (const auto* b = std::get_if<ckm::proto::ImageAddBegin>(&op))
            attach_readds_same_id = b->id == original_id;
    CK_CHECK(attach_readds_same_id);

    // A frame where nothing was redrawn still says nothing.
    CK_CHECK(engine.flush(1, emulator).images.empty());
}

CK_TEST(clearing_the_screen_removes_the_picture_from_the_wire) {
    ckv::term::TerminalEmulator emulator = sixel_emulator();
    DiffEngine engine;
    emulator.feed_output(kRedSixelAt23);
    const DiffEngine::TerminalTick placed = engine.flush(1, emulator);
    const auto* begin = placed.images.empty()
                            ? nullptr
                            : std::get_if<ckm::proto::ImageAddBegin>(&placed.images.front());
    CK_CHECK(begin != nullptr);
    if (begin == nullptr) return;
    const std::uint64_t placed_id = begin->id;

    emulator.feed_output("\x1b[2J");
    const DiffEngine::TerminalTick cleared = engine.flush(1, emulator);
    bool removed = false;
    for (const ckm::proto::Message& op : cleared.images)
        if (const auto* remove = std::get_if<ckm::proto::ImageRemove>(&op))
            removed = remove->term == 1U && remove->id == placed_id;
    CK_CHECK(removed);
}

CK_TEST(attach_images_restates_exactly_what_the_clients_hold) {
    ckv::term::TerminalEmulator emulator = sixel_emulator();
    DiffEngine engine;
    emulator.feed_output(kRedSixelAt23);
    const DiffEngine::TerminalTick tick = engine.flush(1, emulator);
    const auto* begin = tick.images.empty()
                            ? nullptr
                            : std::get_if<ckm::proto::ImageAddBegin>(&tick.images.front());
    CK_CHECK(begin != nullptr);
    if (begin == nullptr) return;

    // A late attacher is handed the same picture under the SAME id, so the
    // clients already watching and the one arriving agree about every
    // placement without renumbering anybody.
    const std::vector<ckm::proto::Message> restated = engine.attach_images(1);
    CK_CHECK(restated.size() == tick.images.size());
    const auto* restated_begin =
        restated.empty() ? nullptr : std::get_if<ckm::proto::ImageAddBegin>(&restated.front());
    CK_CHECK(restated_begin != nullptr);
    if (restated_begin == nullptr) return;
    CK_CHECK(restated_begin->id == begin->id);
    bool placed_again = false;
    for (const ckm::proto::Message& op : restated)
        if (const auto* place = std::get_if<ckm::proto::ImagePlace>(&op))
            placed_again = place->id == begin->id && place->cells.x == 2 && place->cells.y == 1;
    CK_CHECK(placed_again);
}

CK_TEST(a_snapshot_names_the_picture_ids_its_watchers_hold) {
    // The list a healing mirror keeps its pixels by: the restatement that
    // follows a snapshot arrives under these same stable ids, so a mirror
    // that kept the rasters the list names shows the old frame until the new
    // pixels land instead of gray. An id the list omits is a picture whose
    // Remove was dropped with the rest of the backlog, and goes.
    ckv::term::TerminalEmulator emulator = sixel_emulator();
    DiffEngine engine;
    CK_CHECK(engine.snapshot(1, emulator).images.empty());
    emulator.feed_output(kRedSixelAt23);
    const DiffEngine::TerminalTick tick = engine.flush(1, emulator);
    const auto* begin = tick.images.empty()
                            ? nullptr
                            : std::get_if<ckm::proto::ImageAddBegin>(&tick.images.front());
    CK_CHECK(begin != nullptr);
    if (begin == nullptr) return;
    const std::vector<std::uint64_t> listed = engine.snapshot(1, emulator).images;
    CK_CHECK(listed.size() == 1U);
    CK_CHECK(!listed.empty() && listed.front() == begin->id);

    emulator.feed_output("\x1b[2J");
    (void)engine.flush(1, emulator);
    CK_CHECK(engine.snapshot(1, emulator).images.empty());
}

namespace {

// A terminal that hands the differ exactly the rasters a test places — the
// seam's own type, no emulator behind it — so identity-versus-content cases
// the emulator's decode cache would optimize away stay constructible.
class RasterSource final : public ckv::core::TerminalSubsession {
public:
    explicit RasterSource(ckv::Size cells)
        : cells_(static_cast<std::size_t>(cells.width) * static_cast<std::size_t>(cells.height),
                 ckv::Cell::from_grapheme(" ", ckv::Style{})),
          size_(cells) {
        profile_.cells = cells;
        damage_.rows.assign(static_cast<std::size_t>(cells.height),
                            ckv::core::TerminalDamage::RowSpan{});
    }

    void place(std::shared_ptr<const ckv::Image> image, ckv::Point anchor, ckv::Size extent) {
        rasters_.clear();
        rasters_.push_back(ckv::core::TerminalRaster{1, anchor, extent, std::move(image), "[sixel]"});
        damage_.rasters = true;
    }

    ckv::core::TerminalSnapshot snapshot() const override { return {}; }
    ckv::core::TerminalStatus status() const override {
        ckv::core::TerminalStatus status;
        status.cells = size_;
        status.state = ckv::core::TerminalSubsessionState::Running;
        return status;
    }
    const ckv::core::TerminalDamage& damage() const noexcept override { return damage_; }
    void clear_damage() noexcept override {
        damage_ = {};
        damage_.rows.assign(static_cast<std::size_t>(size_.height),
                            ckv::core::TerminalDamage::RowSpan{});
    }
    bool synchronized_output_active() const noexcept override { return false; }
    std::span<const ckv::Cell> cells() const noexcept override { return cells_; }
    std::span<const ckv::Cell> scrollback() const noexcept override { return {}; }
    std::span<const ckv::core::TerminalRaster> rasters() const noexcept override {
        return rasters_;
    }
    std::span<const ckv::core::TerminalDiagnostic> diagnostics() const noexcept override {
        return {};
    }
    const ckv::core::TerminalCapabilityProfile& profile() const noexcept override {
        return profile_;
    }
    void feed_output(std::string_view) override {}
    void resize(ckv::Size, ckv::Size) override {}
    void send_input(std::string_view) override {}
    std::string take_pending_input() override { return {}; }
    ckv::core::TerminalSubsessionState state() const noexcept override {
        return ckv::core::TerminalSubsessionState::Running;
    }

private:
    std::vector<ckv::Cell> cells_;
    std::vector<ckv::core::TerminalRaster> rasters_;
    ckv::core::TerminalDamage damage_;
    ckv::core::TerminalCapabilityProfile profile_;
    ckv::Size size_;
};

}  // namespace

CK_TEST(pixels_equal_to_the_believed_ones_do_not_travel_again) {
    // A new object whose bytes equal the believed ones is the same picture
    // decoded again — a payload past the emulator's cache cap arrives as a
    // fresh object on every redraw — and it must cost nothing on the wire: a
    // merely-busy child re-drawing an unchanged plot was shipping megabytes
    // per tick to say "unchanged", which is half of how ckgrapher saturated
    // its session (field report, 2026-08-19).
    RasterSource source(ckv::Size{20, 6});
    DiffEngine engine;
    auto first = std::make_shared<ckv::Image>(8, 12);
    for (int y = 0; y < 12; ++y)
        for (int x = 0; x < 8; ++x) first->set_pixel(x, y, ckv::Image::Rgba{200, 30, 40, 255});
    source.place(first, ckv::Point{0, 0}, ckv::Size{2, 2});
    CK_CHECK(!engine.flush(1, source).images.empty());

    // Same bytes, different object: nothing travels.
    auto identical = std::make_shared<ckv::Image>(*first);
    source.place(identical, ckv::Point{0, 0}, ckv::Size{2, 2});
    CK_CHECK(engine.flush(1, source).images.empty());

    // Genuinely different pixels still travel.
    auto changed = std::make_shared<ckv::Image>(*first);
    changed->set_pixel(0, 0, ckv::Image::Rgba{1, 2, 3, 255});
    source.place(changed, ckv::Point{0, 0}, ckv::Size{2, 2});
    CK_CHECK(!engine.flush(1, source).images.empty());
}

// --- R8: everything about a terminal that is not its grid --------------------

namespace {

// A bare emulator that may do the three things R8 transports: put text on the
// clipboard, capture print output, and complain. No PTY, because none of these
// needs a child — what is on trial is what the SERVER makes of what the
// emulator holds.
ckv::term::TerminalEmulator talkative_emulator() {
    ckv::term::TerminalCapabilityProfile profile = ckv::term::embedded_xterm_sixel_profile();
    profile.cells = ckv::Size{40, 8};
    profile.cell_pixels = ckv::Size{9, 18};
    profile.osc_policy = ckv::core::TerminalOscPolicy::StoreMetadata;
    profile.clipboard_policy = ckv::core::TerminalClipboardPolicy::AllowWrite;
    profile.printer_policy = ckv::core::TerminalPrinterPolicy::Capture;
    ckv::term::TerminalSubsessionOptions options;
    options.max_scrollback_lines = 200;
    return ckv::term::TerminalEmulator(profile, options);
}

}  // namespace

CK_TEST(the_kitty_keyboard_flags_a_child_asked_for_travel_in_the_modes_word) {
    // M-R2. The server answered the child's re-probe with the enhancements ON
    // while the client, which is what actually encodes the keys, was never
    // told — so a program that had just switched the legacy fallback off was
    // sent the legacy encoding for as long as it ran.
    ckv::term::TerminalEmulator emulator = talkative_emulator();
    DiffEngine engine;
    const ckm::proto::TerminalState before = engine.snapshot(1, emulator);
    CK_CHECK((before.modes & ckm::proto::kKeyboardFlagsMask) == 0u);
    emulator.clear_damage();

    // `CSI > 1 u`: push "disambiguate escape codes", which is what a program
    // reading keys rather than text asks for first.
    emulator.feed_output("\x1b[>1u");
    const DiffEngine::TerminalTick tick = engine.flush(1, emulator);
    CK_CHECK(tick.delta.has_value());
    bool said = false;
    if (tick.delta.has_value())
        for (const ckm::proto::GridOp& op : tick.delta->ops)
            if (const auto* modes = std::get_if<ckm::proto::ModesOp>(&op)) {
                // The changed mask as well as the value: a delta that carried
                // the bit without saying it had changed would leave a mirror
                // that already held a different set exactly as it was.
                said = (modes->changed_mask & ckm::proto::kKeyboardFlagsMask) != 0u &&
                       ((modes->values & ckm::proto::kKeyboardFlagsMask) >>
                        ckm::proto::kKeyboardFlagsShift) == 1u;
            }
    CK_CHECK(said);

    // And a client attaching now is given the whole set rather than the change.
    const ckm::proto::TerminalState after = engine.snapshot(1, emulator);
    CK_CHECK(((after.modes & ckm::proto::kKeyboardFlagsMask) >>
              ckm::proto::kKeyboardFlagsShift) == 1u);
}

CK_TEST(a_snapshot_carries_the_clipboard_watermark_and_the_newest_complaint) {
    // The two halves of what a reattaching client needs about things that
    // HAPPENED rather than things that are set. The clipboard text is
    // deliberately not among them: a write is a live act, and one replayed
    // minutes later would put a child's text over whatever its reader had
    // copied since.
    ckv::term::TerminalEmulator emulator = talkative_emulator();
    DiffEngine engine;
    emulator.feed_output("\x1b]52;c;aGVsbG8=\x07");  // "hello", allowed by the profile
    emulator.feed_output("\x1b]99;x\x07");           // an OSC this terminal does not implement

    const ckm::proto::TerminalState state = engine.snapshot(1, emulator);
    CK_CHECK(state.clipboard_serial == 1U);
    CK_CHECK(state.diagnostic_kind == ckm::proto::DiagnosticKind::UnsupportedSequence);
    CK_CHECK(!state.diagnostic.empty());
    // The newest one, not the first: the emulator keeps a ring and a view
    // paints its last entry.
    emulator.feed_output("\x1b]52;c;?\x07");  // a clipboard READ, refused under every policy
    const ckm::proto::TerminalState later = engine.snapshot(1, emulator);
    CK_CHECK(later.diagnostic != state.diagnostic);
    // A refused read is not a write, so the watermark did not move.
    CK_CHECK(later.clipboard_serial == 1U);
}

CK_TEST(a_snapshot_says_whether_the_printer_is_running) {
    // The one printer fact a reader must be shown: while the controller is on,
    // the child's output goes to the printer and NOT to the screen, so a
    // reattaching client that reported "idle" would leave them watching a
    // terminal that has apparently stopped responding.
    ckv::term::TerminalEmulator emulator = talkative_emulator();
    DiffEngine engine;
    CK_CHECK(engine.snapshot(1, emulator).printer_state == ckm::proto::PrinterState::Idle);
    emulator.feed_output("\x1b[5i");  // MC: the printer controller on
    const ckm::proto::TerminalState state = engine.snapshot(1, emulator);
    CK_CHECK(state.printer_state == ckm::proto::PrinterState::Capturing);
    CK_CHECK(emulator.damage().printer);  // and the tick has something to say about it
}
