// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// WP-4b: the diff algebra on the live path. Real PTYs, real children, and a
// mirror built only out of the deltas the server produced — because the claim
// is not "the server thinks it sent the right thing", it is "a client that saw
// only these bytes holds what the terminal holds".
#include <chrono>
#include <memory>
#include <cstdio>
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
        for (const std::vector<ckm::proto::CellRun>& line : state.scrollback)
            mirror.scrollback.push_back(ckm::proto::from_runs(line));
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

    // The client resizes on TermMeta (WP-6); here that is the mirror being
    // rebuilt at the new size before the delta lands.
    client.mirror = ckm::blank_state(ckv::Size{60, 12}, 200);
    client.receive(deltas.front());
    CK_CHECK(client.refusals == 0);
    CK_CHECK(text_of(client.mirror).find("before the resize") != std::string::npos);
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
    CK_CHECK(!client.mirror.scrollback.empty());
    // The oldest line the child scrolled away is in the client's own history,
    // which is what makes paging and copy mode local (plans/05).
    std::string history;
    for (const std::vector<ckv::Cell>& line : client.mirror.scrollback)
        for (const ckv::Cell& cell : line) history += cell.grapheme();
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
