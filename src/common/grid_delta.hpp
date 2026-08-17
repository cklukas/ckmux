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
std::vector<proto::GridOp> diff(const GridState& previous, const GridState& current,
                                std::span<const std::vector<ckv::Cell>> pushed = {});

// Applies `ops` to `mirror`, or refuses them and leaves it untouched.
//
// All-or-nothing on purpose. A mirror that has taken half a delta is a mirror
// that lies about what a program has on screen, and the protocol's answer to
// an impossible delta is already written down: the client resnapshots
// (plans/05, the gap rule). So the ops are checked against the mirror's own
// geometry before any of them are performed.
bool apply(std::span<const proto::GridOp> ops, GridState& mirror);

// The grid as the wire carries it — run-length encoded, whole rows in order —
// and back. The attach snapshot uses this; a delta does not, because a delta
// sends spans of rows rather than the grid.
std::vector<proto::CellRun> encode_grid(const GridState& state);
bool decode_grid(std::span<const proto::CellRun> runs, ckv::Size cells, GridState& state);

}  // namespace ckm
