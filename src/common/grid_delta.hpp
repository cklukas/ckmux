// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// The grid diff algebra (WP-4a): two grid states in, a `GridDelta`'s ops out,
// and the same ops applied to a mirror to put it back. Pure functions over
// values — no server, no socket, no PTY, no clock anywhere near them, which is
// why this is the layer the property tests live at (the testing plan).
//
// The one claim worth stating up front: applying `diff(a, b)` to a mirror
// holding `a` must leave it holding `b` exactly — every cell, the cursor, the
// modes, the title and the history. Everything else here is about making that
// cost few bytes; the correctness is not a matter of degree.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "common/proto.hpp"
#include "cvision/core/cell.hpp"
#include "cvision/core/geometry.hpp"

namespace ckm {

// The cursor as a value. Deliberately not `proto::CursorOp`: the state a delta
// is computed from is not a message, and a type that is both invites a diff
// that compares wire encodings rather than terminals.
struct GridCursor {
    std::uint16_t column = 0;
    std::uint16_t row = 0;
    std::uint8_t style = 0;  // ckv::CursorShape
    std::uint8_t visible = 1;
    std::uint8_t blink = 0;

    friend bool operator==(const GridCursor&, const GridCursor&) = default;
};

// Everything a `GridDelta` can carry, and nothing else.
//
// This is the client's mirror and the server's last-sent state, the same type
// on both ends on purpose: a delta is only ever correct with respect to what
// the other end is believed to hold, so "what was sent" and "what is held"
// must be the same shape or the belief cannot be checked.
struct GridState {
    ckv::Size cells{0, 0};
    // Row-major, exactly cells.width * cells.height entries.
    std::vector<ckv::Cell> grid;
    // The history, oldest line first, flat — rows of `cells.width` cells —
    // with the live region beginning `scrollback_start` lines in. One buffer
    // and an offset rather than a vector of lines, because a history has one
    // hot operation: a child scrolling at full speed pushes lines for as long
    // as the reader lets it run. Appending to a flat buffer is O(width) per
    // line, keeping capacity by advancing the offset is O(1), and the dead
    // prefix is reclaimed only once it outweighs what is kept — so each cell
    // moves at most once per history's worth of lines. The previous shape
    // paid O(history) per push twice over (a front erase here, a flatten in
    // the mirror for every reader), and one `find /` made a client unusable
    // for as long as its output kept arriving. The emulator keeps its own
    // history in exactly this shape, for exactly this reason.
    std::vector<ckv::Cell> scrollback;
    std::size_t scrollback_start = 0;  // lines, not cells
    // What the mirror keeps. 0 means no history at all, which is a reader's
    // `scrollback = 0` and not an unset field.
    std::size_t max_scrollback_lines = 0;
    GridCursor cursor;
    // `proto::ModeBit` flags.
    std::uint32_t modes = 0;
    std::string title;
};

// Whether two states hold the same terminal.
//
// `ckv::Cell` has no `operator==` — what a continuation cell compares as is a
// question the library declined to answer once and for all — so this spells
// out what "the same" means for a mirror: every cell draws the same, and the
// history, the cursor, the modes and the title agree. It is public because
// "applying a delta leaves the mirror identical" has to be a claim something
// can actually evaluate, and that claim is WP-4a's whole point.
bool same_state(const GridState& a, const GridState& b);

// A grid of blanks, and the blank itself. The vacated rows of a scroll are
// filled with this, and every op that follows in the same delta overwrites
// whatever of it still differs.
ckv::Cell blank_cell();
GridState blank_state(ckv::Size cells, std::size_t max_scrollback_lines);

// The live history: how many lines it holds, and its cells — rows of
// `cells.width`, oldest first, borrowed. The dead prefix before
// `scrollback_start` is storage, not history; no reader sees it.
std::size_t scrollback_lines(const GridState& state);
std::span<const ckv::Cell> scrollback_cells(const GridState& state);

// Appends one line, padded or cut to the state's width — the width every
// reader indexes rows by — then enforces capacity. The one gate through
// which history enters a state: `apply_delta`'s push op and a snapshot
// adopt both land here, so the capacity rule cannot fork. A state with
// `max_scrollback_lines == 0` keeps nothing and copies nothing.
void push_scrollback_line(GridState& state, std::span<const ckv::Cell> line);

// Re-applies the capacity rule after it changed: the offset advances past
// what a smaller limit no longer keeps, and the dead prefix is reclaimed
// under the same once-it-outweighs-the-kept rule a push uses.
void enforce_scrollback_capacity(GridState& state);

// Changes a state's size.
//
// The grid becomes blanks: the only delta that carries a resize repaints every
// row of the new size immediately afterwards, so keeping anything would be a
// guess that the repaint overwrites. The HISTORY is kept and re-laid — padded
// or cut per row to the new width, the width every reader of it indexes by and
// the rule `push_scrollback_line` already enforces. Discarding it instead would
// throw away a reader's scrollback every time they made a window narrower,
// which is the one thing keeping the history on this side of the socket was
// for. A height-only change re-lays nothing.
//
// The CURSOR is brought inside the new grid, since a delta may legally move it
// and then resize — each op is checked against the geometry as it stood when
// that op was reached — and a state whose cursor is outside its own grid is one
// its readers have to be tolerant of rather than one that is right.
void resize_state(GridState& state, ckv::Size cells);

// The current side of a diff, borrowed rather than copied.
//
// This is the shape a live terminal can be read in without paying for a copy
// of itself: `cells()` hands out a span into the emulator's own storage and
// `status()` hands out the scalars, so a server at its flush tick copies only
// the cells it is about to send (U0-b). The `GridState` overload below is the
// same function with an owned current side, which is what the tests and the
// attach path use.
struct GridView {
    ckv::Size cells{0, 0};
    std::span<const ckv::Cell> grid;
    GridCursor cursor;
    std::uint32_t modes = 0;
    std::string_view title;
};

// One row's changed columns, as [first, last) — the shape
// `ckv::core::TerminalDamage` reports a row in.
//
// Deliberately its own type rather than that one: this file is the pure
// algebra, and its inputs are plain values with no library seam behind them.
// The conversion is a handful of integers per tick, which is nothing next to
// what the hint buys — a diff that examines the rows a child touched instead of
// the screen.
struct RowDamage {
    int first = 0;
    int last = 0;

    bool empty() const noexcept { return first >= last; }
};

// The ops that turn `previous` into `current`.
//
// `pushed` are the lines that entered the history since `previous` was
// current, oldest first. It is an argument rather than something derived from
// the two histories, because two histories cannot say what moved between
// them: with capacity in play, lines leave the front while lines arrive at the
// back, so the comparison is both ambiguous (identical lines align several
// ways) and O(history) on a path that runs at the flush tick. The emulator
// already says exactly this — `TerminalDamage::scrollback_pushed` with the
// lines themselves — so the caller has it (WP-4b).
//
// If the sizes differ, the delta STATES the new one and then emits every row
// of `current`. It used to say the size was somebody else's to send — that the
// server sent a `TermMeta` first and the caller resized its mirror — and that
// was false twice over: nothing sends a `TermMeta` on resize, and `TermMeta`
// has no size field. What actually happened is that a repaint of a smaller
// terminal passed every bounds check against a larger mirror, so the rows and
// columns past the new edge kept whatever the last program had drawn there and
// no counter anywhere fired (13-architecture-review C3, reproduced).
//
// The op order is fixed and is the apply order: the size, then the history
// push, then the scroll, then the cells, then cursor, modes and title. The size
// comes first because everything after it is measured against the grid it
// names — including the pushed lines, which are rows of the new width. A
// reader's history grows before the screen moves off it.
// `damaged` is what the caller already knows changed, one entry per row. Rows
// it does not name are not examined at all, which is the difference between a
// tick that costs the changed rows and one that costs the screen. An empty span
// means "no information", and then every row is examined — which is what the
// two-state overload passes, because a caller holding two values has no damage
// report to offer.
//
// Changed cells in one row are partitioned by exact wire cost. An unchanged
// gap stays inside a CellsOp when its RLE representation is cheaper than
// another operation header, and is split around when carrying it costs more.
// Thus sparse process-monitor updates do not restate the text between fields,
// while two changes inside one repeated-cell run retain that compression.
//
// A hint that is WRONG costs correctness, unlike the scroll search below: rows
// left out of it are rows the mirror will not be told about. It is a report
// from the emulator, not a guess.
std::vector<proto::GridOp> diff(const GridState& previous, const GridView& current,
                                std::span<const std::vector<ckv::Cell>> pushed = {},
                                std::span<const RowDamage> damaged = {});

std::vector<proto::GridOp> diff(const GridState& previous, const GridState& current,
                                std::span<const std::vector<ckv::Cell>> pushed = {});

// The size, then every row of a view, whatever it holds, plus the cursor,
// modes and title — the one delta that assumes nothing at all about what the
// other end has, its own geometry included.
//
// What an attach and a resize cost. A diff against a blank state would be
// cheaper and would be wrong: it would emit only the rows that are not blank,
// and the rows it skipped are exactly the ones where a client's mirror could
// still be holding something else.
std::vector<proto::GridOp> full_repaint(const GridView& current,
                                        std::span<const std::vector<ckv::Cell>> pushed = {});

// Applies `ops` to `mirror`, or refuses them and leaves it untouched.
//
// Named `apply_delta` and not `apply` because `std::apply` exists: an
// unqualified call from inside a namespace that has a `std::vector` in the
// argument list resolves to that one through ADL, and the error it produces
// names `std::tuple_size` rather than anything a reader wrote.
//
// All-or-nothing on purpose. A mirror that has taken half a delta is a mirror
// that lies about what a program has on screen, and the protocol's answer to
// an impossible delta is already written down: the client resnapshots
// (the protocol spec, the gap rule). So the ops are checked before any of them are
// performed — against the geometry as it will be when each one is reached,
// since a `Resize` earlier in the same delta changes what "fits" means for
// every op after it.
bool apply_delta(std::span<const proto::GridOp> ops, GridState& mirror);

// The grid as the wire carries it — run-length encoded, whole rows in order —
// and back. The attach snapshot uses this; a delta does not, because a delta
// sends spans of rows rather than the grid.
std::vector<proto::CellRun> encode_grid(const GridState& state);
bool decode_grid(std::span<const proto::CellRun> runs, ckv::Size cells, GridState& state);

}  // namespace ckm
