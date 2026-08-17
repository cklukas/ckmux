// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// The grid diff algebra (WP-4a): two grid states in, a `GridDelta`'s ops out,
// and the same ops applied to a mirror to put it back. Pure functions over
// values — no server, no socket, no PTY, no clock anywhere near them, which is
// why this is the layer the property tests live at (plans/09).
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
    // Oldest line first. Lines rather than a flat buffer, because a line is
    // the unit capacity is counted in and the unit `SbPush` carries — the
    // emulator keeps the same invariant for the same reason.
    std::vector<std::vector<ckv::Cell>> scrollback;
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
// If the sizes differ, every row of `current` is emitted: a `GridDelta`
// carries no geometry, because a mirror that resized itself from a delta would
// be guessing. The server sends `TermMeta` first and the caller resizes its
// mirror; `apply` refuses the delta if it did not.
//
// The op order is fixed and is the apply order: the history push, then the
// scroll, then the cells, then cursor, modes and title. A reader's history
// grows before the screen moves off it.
// `damaged` is what the caller already knows changed, one entry per row. Rows
// it does not name are not examined at all, which is the difference between a
// tick that costs the changed rows and one that costs the screen. An empty span
// means "no information", and then every row is examined — which is what the
// two-state overload passes, because a caller holding two values has no damage
// report to offer.
//
// A hint that is WRONG costs correctness, unlike the scroll search below: rows
// left out of it are rows the mirror will not be told about. It is a report
// from the emulator, not a guess.
std::vector<proto::GridOp> diff(const GridState& previous, const GridView& current,
                                std::span<const std::vector<ckv::Cell>> pushed = {},
                                std::span<const RowDamage> damaged = {});

std::vector<proto::GridOp> diff(const GridState& previous, const GridState& current,
                                std::span<const std::vector<ckv::Cell>> pushed = {});

// Every row of a view, whatever it holds, plus the cursor, modes and title —
// the one delta that assumes nothing at all about what the other end has.
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
// (plans/05, the gap rule). So the ops are checked against the mirror's own
// geometry before any of them are performed.
bool apply_delta(std::span<const proto::GridOp> ops, GridState& mirror);

// The grid as the wire carries it — run-length encoded, whole rows in order —
// and back. The attach snapshot uses this; a delta does not, because a delta
// sends spans of rows rather than the grid.
std::vector<proto::CellRun> encode_grid(const GridState& state);
bool decode_grid(std::span<const proto::CellRun> runs, ckv::Size cells, GridState& state);

}  // namespace ckm
