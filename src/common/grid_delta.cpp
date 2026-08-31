// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "common/grid_delta.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <variant>

namespace ckm {
namespace {

using proto::CellRun;
using proto::CellsOp;
using proto::CursorOp;
using proto::GridOp;
using proto::ModesOp;
using proto::ScrollbackPushOp;
using proto::ScrollOp;
using proto::TitleOp;

// A cell equals another when it would draw the same. Cell has no operator==,
// and giving it one upstream would have to answer what a continuation cell
// compares as; here the question is narrower — what the wire carries is the
// grapheme, the width and the style, so those three are what a delta may not
// lose.
bool same_cell(const ckv::Cell& a, const ckv::Cell& b) {
    return a.grapheme() == b.grapheme() && a.width() == b.width() && a.style() == b.style();
}

void mix(std::uint64_t& hash, std::uint8_t byte) {
    hash ^= byte;
    hash *= 0x100000001B3ull;  // FNV-1a
}

void mix_color(std::uint64_t& hash, const ckv::Color& color) {
    if (color.is_rgb()) {
        mix(hash, 2);
        mix(hash, color.r());
        mix(hash, color.g());
        mix(hash, color.b());
        return;
    }
    if (color.is_indexed()) {
        mix(hash, 1);
        mix(hash, color.index());
        return;
    }
    mix(hash, 0);
}

// Rows are compared by hash while a scroll is being looked for, and by cells
// once one has been chosen. That split is deliberate: the search is quadratic
// in the height of the changed band, so it must be cheap, and a collision can
// only ever cost bytes — the repaint pass that follows compares real cells and
// re-sends whatever the shift did not actually move.
std::uint64_t row_hash(std::span<const ckv::Cell> row) {
    std::uint64_t hash = 0xCBF29CE484222325ull;
    for (const ckv::Cell& cell : row) {
        for (const char byte : cell.grapheme()) mix(hash, static_cast<std::uint8_t>(byte));
        mix(hash, 0xFF);  // a separator, so "ab"+"c" and "a"+"bc" differ
        mix(hash, static_cast<std::uint8_t>(cell.width()));
        mix_color(hash, cell.style().fg);
        mix_color(hash, cell.style().bg);
        mix(hash, static_cast<std::uint8_t>(cell.style().attrs));
        mix(hash, static_cast<std::uint8_t>(cell.style().underline));
        mix_color(hash, cell.style().underline_color);
    }
    return hash;
}

std::span<const ckv::Cell> row_of(std::span<const ckv::Cell> grid, int width, int row) {
    const std::size_t offset = static_cast<std::size_t>(row) * static_cast<std::size_t>(width);
    return grid.subspan(offset, static_cast<std::size_t>(width));
}

// A region shift, performed the way the mirror will perform it — this is the
// same code path the server uses to predict what the client now holds, so
// there is exactly one definition of what a `Scroll` op means.
void shift_rows(std::vector<ckv::Cell>& grid, int width, int top, int bottom, int lines) {
    const int height = bottom - top;
    if (height <= 0 || lines == 0) return;
    const ckv::Cell blank = blank_cell();
    const auto row_begin = [&](int row) {
        return grid.begin() + static_cast<std::ptrdiff_t>(row) * static_cast<std::ptrdiff_t>(width);
    };
    if (std::abs(lines) >= height) {
        std::fill(row_begin(top), row_begin(bottom), blank);
        return;
    }
    if (lines > 0) {
        std::move(row_begin(top + lines), row_begin(bottom), row_begin(top));
        std::fill(row_begin(bottom - lines), row_begin(bottom), blank);
        return;
    }
    const int down = -lines;
    std::move_backward(row_begin(top), row_begin(bottom - down), row_begin(bottom));
    std::fill(row_begin(top), row_begin(top + down), blank);
}

std::vector<CellRun> runs_of(std::span<const ckv::Cell> cells) {
    return proto::to_runs(std::vector<ckv::Cell>(cells.begin(), cells.end()));
}

std::size_t total_length(const std::vector<CellRun>& runs) {
    std::size_t total = 0;
    for (const CellRun& run : runs) total += run.run_length;
    return total;
}

// Emits the byte-cheapest partition of one changed row. A CellsOp may span an
// unchanged gap when the wanted cells there belong to a run already being
// paid for (turning two isolated cells into one repeated run can be cheaper
// than two op headers). Across whole unchanged runs, the choice is exact:
// split when their encoded bytes cost more than another CellsOp's fixed
// overhead, keep them when they cost the same or less.
void emit_changed_row(std::uint16_t row, std::span<const ckv::Cell> mirror,
                      std::span<const ckv::Cell> wanted, std::vector<GridOp>& ops) {
    if (mirror.size() != wanted.size() || wanted.empty()) return;

    const std::size_t cells_op_overhead = proto::encoded_size(CellsOp{});
    bool have_span = false;
    std::size_t span_first = 0;
    std::size_t span_last = 0;  // exclusive
    std::size_t gap_bytes = 0;

    const auto emit_span = [&] {
        ops.push_back(CellsOp{row, static_cast<std::uint16_t>(span_first),
                              runs_of(wanted.subspan(span_first, span_last - span_first))});
    };

    for (std::size_t run_first = 0; run_first < wanted.size();) {
        std::size_t run_last = run_first + 1;
        while (run_last < wanted.size() && same_cell(wanted[run_first], wanted[run_last]))
            ++run_last;

        std::size_t first_changed = run_last;
        std::size_t last_changed = run_first;
        for (std::size_t column = run_first; column < run_last; ++column) {
            if (same_cell(mirror[column], wanted[column])) continue;
            first_changed = std::min(first_changed, column);
            last_changed = column + 1;
        }

        if (first_changed == run_last) {
            if (have_span) {
                const std::size_t run_length = run_last - run_first;
                gap_bytes += proto::encoded_size(
                    CellRun{static_cast<std::uint16_t>(run_length), wanted[run_first]});
            }
            run_first = run_last;
            continue;
        }

        if (!have_span) {
            have_span = true;
            span_first = first_changed;
            span_last = last_changed;
        } else if (gap_bytes > cells_op_overhead) {
            emit_span();
            span_first = first_changed;
            span_last = last_changed;
        } else {
            span_last = last_changed;
        }
        gap_bytes = 0;
        run_first = run_last;
    }

    if (have_span) emit_span();
}

// Every row of `current`, as its own op. What a resize, an attach-and-diff or
// a mirror of the wrong size comes down to.
void emit_every_row(const GridView& current, std::vector<GridOp>& ops) {
    const std::size_t expected = static_cast<std::size_t>(std::max(0, current.cells.width)) *
                                 static_cast<std::size_t>(std::max(0, current.cells.height));
    if (current.grid.size() != expected) return;  // a view that disagrees with itself
    for (int row = 0; row < current.cells.height; ++row)
        ops.push_back(CellsOp{static_cast<std::uint16_t>(row), 0,
                              runs_of(row_of(current.grid, current.cells.width, row))});
}

}  // namespace

ckv::Cell blank_cell() { return ckv::Cell::from_grapheme(" ", ckv::Style{}); }

GridState blank_state(ckv::Size cells, std::size_t max_scrollback_lines) {
    GridState state;
    state.cells = cells;
    state.grid.assign(static_cast<std::size_t>(std::max(0, cells.width)) *
                          static_cast<std::size_t>(std::max(0, cells.height)),
                      blank_cell());
    state.max_scrollback_lines = max_scrollback_lines;
    return state;
}

namespace {

std::size_t scrollback_width(const GridState& state) {
    return static_cast<std::size_t>(std::max(1, state.cells.width));
}

}  // namespace

std::size_t scrollback_lines(const GridState& state) {
    const std::size_t total = state.scrollback.size() / scrollback_width(state);
    // An offset past the end reads as empty rather than as an underflow: a
    // caller that cleared the buffer by hand owes no matching offset reset.
    return total > state.scrollback_start ? total - state.scrollback_start : 0;
}

std::span<const ckv::Cell> scrollback_cells(const GridState& state) {
    const std::size_t lines = scrollback_lines(state);
    if (lines == 0) return {};
    const std::size_t width = scrollback_width(state);
    return std::span<const ckv::Cell>(state.scrollback.data() + state.scrollback_start * width,
                                      lines * width);
}

void enforce_scrollback_capacity(GridState& state) {
    if (state.max_scrollback_lines == 0) {
        state.scrollback.clear();
        state.scrollback_start = 0;
        return;
    }
    const std::size_t width = scrollback_width(state);
    const std::size_t live = scrollback_lines(state);
    if (live > state.max_scrollback_lines)
        state.scrollback_start += live - state.max_scrollback_lines;
    // The dead prefix is reclaimed once it is as large as what is kept: one
    // erase per history's worth of lines, so each cell moves at most once —
    // never per push, which is where the previous shape paid O(history) for
    // every line a flooding child scrolled.
    if (state.scrollback_start >= scrollback_lines(state) && state.scrollback_start > 0) {
        state.scrollback.erase(state.scrollback.begin(),
                               state.scrollback.begin() +
                                   static_cast<std::ptrdiff_t>(state.scrollback_start * width));
        state.scrollback_start = 0;
    }
}

void resize_state(GridState& state, ckv::Size cells) {
    if (state.cells == cells) return;
    const bool same_width = state.cells.width == cells.width;

    // Taken out before the width changes, because the buffer is flat and the
    // width is the only thing that says where its rows begin: reinterpreting it
    // at the new width would slice every line in the wrong place.
    std::vector<ckv::Cell> history;
    std::size_t old_width = 0;
    if (!same_width) {
        old_width = scrollback_width(state);
        const std::span<const ckv::Cell> live = scrollback_cells(state);
        history.assign(live.begin(), live.end());
        state.scrollback.clear();
        state.scrollback_start = 0;
    }

    state.cells = cells;
    state.grid.assign(static_cast<std::size_t>(std::max(0, cells.width)) *
                          static_cast<std::size_t>(std::max(0, cells.height)),
                      blank_cell());

    // The cursor comes with it. Each op in a delta is checked against the
    // geometry as it stood when that op was reached, so "move the cursor to
    // row 20, then become five rows tall" is a well-formed sequence — and a
    // state that kept row 20 would be holding a position outside its own grid
    // for a view to index by. Clamped here rather than left to the renderer:
    // ckVision tolerates it at draw time, but a mirror that relies on its
    // consumer's tolerance is a mirror whose invariant is somebody else's.
    // One past the last COLUMN survives, because that is where a terminal
    // leaves the cursor on a row it has filled and not yet wrapped.
    const int last_column = std::max(0, state.cells.width);
    const int last_row = std::max(0, state.cells.height - 1);
    if (state.cursor.column > last_column)
        state.cursor.column = static_cast<std::uint16_t>(last_column);
    if (state.cursor.row > last_row) state.cursor.row = static_cast<std::uint16_t>(last_row);

    if (same_width) {
        enforce_scrollback_capacity(state);
        return;
    }

    // Through the one gate history enters a state by, so the padding and the
    // capacity rule are the same ones a push obeys — and so a re-lay cannot
    // grow the history past what the reader asked to keep.
    for (std::size_t line = 0; old_width > 0 && line + old_width <= history.size();
         line += old_width)
        push_scrollback_line(state, std::span<const ckv::Cell>(history.data() + line, old_width));
}

void push_scrollback_line(GridState& state, std::span<const ckv::Cell> line) {
    if (state.max_scrollback_lines == 0) return;
    const std::size_t width = scrollback_width(state);
    // Padded or cut to the state's width, because every reader indexes the
    // history as rows of the CURRENT width — the same rule the emulator keeps
    // when a resize re-lays its history.
    for (std::size_t column = 0; column < width; ++column)
        state.scrollback.push_back(column < line.size() ? line[column] : blank_cell());
    enforce_scrollback_capacity(state);
}

std::vector<proto::GridOp> diff(const GridState& previous, const GridView& current,
                                std::span<const std::vector<ckv::Cell>> pushed,
                                std::span<const RowDamage> damaged) {
    std::vector<GridOp> ops;

    const int width = current.cells.width;
    const int height = current.cells.height;
    const std::size_t expected = static_cast<std::size_t>(std::max(0, width)) *
                                 static_cast<std::size_t>(std::max(0, height));
    const bool same_geometry = previous.cells == current.cells &&
                               previous.grid.size() == expected && current.grid.size() == expected;

    // One rule, stated once: an op sequence that repaints every row states the
    // size first. It has to come before the history push below, because the
    // pushed lines are rows of the new width and the mirror files them against
    // its own.
    if (!same_geometry)
        ops.push_back(proto::ResizeOp{static_cast<std::uint16_t>(std::max(0, width)),
                                      static_cast<std::uint16_t>(std::max(0, height))});

    if (!pushed.empty()) {
        ScrollbackPushOp push;
        push.lines.reserve(pushed.size());
        for (const std::vector<ckv::Cell>& line : pushed) push.lines.push_back(runs_of(line));
        ops.push_back(std::move(push));
    }

    if (!same_geometry) {
        emit_every_row(current, ops);
    } else if (width > 0 && height > 0) {
        const std::span<const ckv::Cell> held(previous.grid);
        // Which rows are looked at. A damage report names them, and rows it does
        // not name are not read at all — that is the whole difference between a
        // tick that costs what the child touched and one that costs the screen.
        // With no report, everything is looked at, which is what a caller
        // holding two plain values has to mean.
        std::vector<char> examined(static_cast<std::size_t>(height), damaged.empty() ? 1 : 0);
        for (std::size_t row = 0; row < damaged.size() && row < examined.size(); ++row)
            if (!damaged[row].empty()) examined[row] = 1;

        // Hashes are used for two things only: finding the band a shift might
        // explain, and scoring candidate shifts. Neither is load-bearing — the
        // repaint pass below compares real cells over every row it was told to
        // look at, so a collision costs bytes and never a missed update.
        std::vector<std::uint64_t> before(static_cast<std::size_t>(height), 0);
        std::vector<std::uint64_t> after(static_cast<std::size_t>(height), 0);
        std::vector<char> hashed(static_cast<std::size_t>(height), 0);
        const auto hash_row = [&](int row) {
            if (hashed[static_cast<std::size_t>(row)]) return;
            before[static_cast<std::size_t>(row)] = row_hash(row_of(held, width, row));
            after[static_cast<std::size_t>(row)] = row_hash(row_of(current.grid, width, row));
            hashed[static_cast<std::size_t>(row)] = 1;
        };
        const auto row_differs = [&](int row) {
            hash_row(row);
            return before[static_cast<std::size_t>(row)] != after[static_cast<std::size_t>(row)];
        };

        int top = 0;
        while (top < height && (!examined[static_cast<std::size_t>(top)] || !row_differs(top))) ++top;
        int bottom = height;
        while (bottom > top &&
               (!examined[static_cast<std::size_t>(bottom - 1)] || !row_differs(bottom - 1)))
            --bottom;

        // Borrowed until a shift is chosen. Copying the previous grid on every
        // tick would be a full grid copy on the one path whose whole point is
        // not to have one — and the copy is only needed to predict what a
        // shift leaves behind.
        std::vector<ckv::Cell> shifted;
        std::span<const ckv::Cell> mirror_grid = held;
        if (bottom - top >= 2) {
            for (int row = top; row < bottom; ++row) hash_row(row);
            // The blank row a shift leaves behind, so a vacated row that the
            // child really did blank counts as moved rather than as a repaint.
            const std::vector<ckv::Cell> blank_row(static_cast<std::size_t>(width), blank_cell());
            const std::uint64_t blank = row_hash(blank_row);
            const auto score_for = [&](int lines) {
                int matches = 0;
                for (int row = top; row < bottom; ++row) {
                    const int source = row + lines;
                    const std::uint64_t moved = (source >= top && source < bottom)
                                                    ? before[static_cast<std::size_t>(source)]
                                                    : blank;
                    if (moved == after[static_cast<std::size_t>(row)]) ++matches;
                }
                return matches;
            };
            const int band = bottom - top;
            const int standing = score_for(0);
            int best_lines = 0;
            int best_score = standing;
            for (int lines = -(band - 1); lines <= band - 1; ++lines) {
                if (lines == 0) continue;
                const int score = score_for(lines);
                if (score > best_score) {
                    best_score = score;
                    best_lines = lines;
                }
            }
            // Two rows is the threshold, not one: a `Scroll` op costs bytes of
            // its own, and a shift that saves a single row has only moved which
            // row gets re-sent.
            if (best_lines != 0 && best_score - standing >= 2) {
                ops.push_back(ScrollOp{static_cast<std::uint16_t>(top),
                                       static_cast<std::uint16_t>(bottom),
                                       static_cast<std::int16_t>(best_lines)});
                shifted.assign(held.begin(), held.end());
                shift_rows(shifted, width, top, bottom, best_lines);
                mirror_grid = shifted;
                // Every row the shift touched now has to be checked against
                // what is really there, whether or not damage named it.
                for (int row = top; row < bottom; ++row) examined[static_cast<std::size_t>(row)] = 1;
            }
        }

        // Whatever is not already right is sent as cells. Real cells, not
        // hashes: this pass is what makes the shift above a question of bytes
        // rather than of correctness, and it is where a damage hint stops the
        // tick from touching rows nothing happened to.
        for (int row = 0; row < height; ++row) {
            if (!examined[static_cast<std::size_t>(row)]) continue;
            const std::span<const ckv::Cell> mirror_row = row_of(mirror_grid, width, row);
            const std::span<const ckv::Cell> wanted = row_of(current.grid, width, row);
            emit_changed_row(static_cast<std::uint16_t>(row), mirror_row, wanted, ops);
        }
    }

    if (!(previous.cursor == current.cursor))
        ops.push_back(CursorOp{current.cursor.column, current.cursor.row, current.cursor.style,
                               current.cursor.visible, current.cursor.blink});
    if (previous.modes != current.modes)
        ops.push_back(ModesOp{previous.modes ^ current.modes, current.modes});
    if (previous.title != current.title) ops.push_back(TitleOp{std::string(current.title)});
    return ops;
}

std::vector<proto::GridOp> full_repaint(const GridView& current,
                                        std::span<const std::vector<ckv::Cell>> pushed) {
    std::vector<GridOp> ops;
    // The size, before anything that depends on it. A repaint that assumes
    // nothing cannot assume the geometry either — and the pushed lines below
    // are rows of the NEW width, which `push_scrollback_line` pads and cuts
    // against the state's own, so a mirror that had not resized yet would file
    // them at the wrong width.
    ops.push_back(proto::ResizeOp{static_cast<std::uint16_t>(std::max(0, current.cells.width)),
                                  static_cast<std::uint16_t>(std::max(0, current.cells.height))});
    if (!pushed.empty()) {
        ScrollbackPushOp push;
        push.lines.reserve(pushed.size());
        for (const std::vector<ckv::Cell>& line : pushed) push.lines.push_back(runs_of(line));
        ops.push_back(std::move(push));
    }
    emit_every_row(current, ops);
    // Stated rather than compared: a repaint that assumes nothing cannot assume
    // the cursor either.
    ops.push_back(CursorOp{current.cursor.column, current.cursor.row, current.cursor.style,
                           current.cursor.visible, current.cursor.blink});
    ops.push_back(ModesOp{0xFFFFFFFFu, current.modes});
    ops.push_back(TitleOp{std::string(current.title)});
    return ops;
}

std::vector<proto::GridOp> diff(const GridState& previous, const GridState& current,
                                std::span<const std::vector<ckv::Cell>> pushed) {
    // No damage report, so every row is examined: two values cannot say what
    // moved between them, only what differs.
    return diff(previous,
                GridView{current.cells, std::span<const ckv::Cell>(current.grid), current.cursor,
                         current.modes, current.title},
                pushed, {});
}

bool apply_delta(std::span<const proto::GridOp> ops, GridState& mirror) {
    int width = mirror.cells.width;
    const int height = mirror.cells.height;
    if (mirror.grid.size() != static_cast<std::size_t>(std::max(0, width)) *
                                  static_cast<std::size_t>(std::max(0, height)))
        return false;

    // Checked in full before anything is performed, and against the geometry as
    // it will be WHEN EACH OP IS REACHED. All-or-nothing is the whole contract:
    // a mirror holding half a delta shows a program's screen with a stripe of
    // some other moment in it, and nothing downstream can tell that it
    // happened. The running geometry is the other half — a repaint of a
    // SMALLER terminal used to pass every bounds check against a larger mirror,
    // so the rows and columns past the new edge kept whatever the last program
    // had drawn there and no counter anywhere fired (13-architecture-review C3).
    int check_width = width;
    int check_height = height;
    for (const GridOp& op : ops) {
        if (const auto* resize = std::get_if<proto::ResizeOp>(&op)) {
            // A size of nothing is not a size, and one past what a grid may
            // ever be is a mirror asked to allocate on a peer's say-so. The
            // bound is the protocol's own, so both ends refuse the same number.
            if (resize->columns == 0 || resize->rows == 0) return false;
            if (resize->columns > proto::kMaxGridColumns) return false;
            if (resize->rows > proto::kMaxGridRows) return false;
            check_width = resize->columns;
            check_height = resize->rows;
            continue;
        }
        if (const auto* scroll = std::get_if<ScrollOp>(&op)) {
            if (scroll->top > scroll->bottom || scroll->bottom > check_height) return false;
            const int region = scroll->bottom - scroll->top;
            if (std::abs(static_cast<int>(scroll->lines)) > region) return false;
            continue;
        }
        if (const auto* cells = std::get_if<CellsOp>(&op)) {
            if (cells->row >= check_height || cells->column > check_width) return false;
            if (total_length(cells->runs) > static_cast<std::size_t>(check_width - cells->column))
                return false;
            continue;
        }
        if (const auto* cursor = std::get_if<CursorOp>(&op)) {
            // One past the last column is allowed: that is where a terminal
            // leaves the cursor after filling a row it has not wrapped yet,
            // and refusing it would make a mirror resnapshot over a state its
            // own emulator produces routinely.
            if (cursor->row >= check_height || cursor->column > check_width) return false;
            continue;
        }
        if (const auto* push = std::get_if<ScrollbackPushOp>(&op)) {
            for (const std::vector<CellRun>& line : push->lines) {
                const std::size_t length = total_length(line);
                // A history line is a row of the grid. Shorter is padded by
                // `push_scrollback_line`; longer is a peer claiming a row
                // wider than the terminal it belongs to, which is a mirror
                // asked to allocate on somebody else's say-so — measured at
                // 62.9 MB out of 449 bytes on the wire
                // (13-architecture-review, M-P1). The width is the RUNNING
                // one, so a push that follows a resize is measured against the
                // terminal it is about to belong to.
                if (length == 0 || length > static_cast<std::size_t>(std::max(0, check_width)))
                    return false;
            }
            continue;
        }
    }

    for (const GridOp& op : ops) {
        if (const auto* resize = std::get_if<proto::ResizeOp>(&op)) {
            resize_state(mirror, ckv::Size{resize->columns, resize->rows});
            // The ops after this one index by the NEW width — the shift and
            // the cell copies below both take it as an argument, and taking
            // the old one would write every row at the wrong offset.
            width = mirror.cells.width;
            continue;
        }
        if (const auto* push = std::get_if<ScrollbackPushOp>(&op)) {
            // A holder that keeps no history does not decode the lines at all.
            // That is the server's own belief about a client: it never diffs
            // against a history, so paying to rebuild one per terminal per tick
            // would be the copy U0-b removed, arriving from the other side.
            if (mirror.max_scrollback_lines == 0) continue;
            // Capacity is the reader's, counted in lines because a line is
            // what they scroll past (the configuration spec `scrollback`); the push helper
            // enforces it at O(width) per line, whatever the history's size.
            for (const std::vector<CellRun>& line : push->lines)
                push_scrollback_line(mirror, proto::from_runs(line));
            continue;
        }
        if (const auto* scroll = std::get_if<ScrollOp>(&op)) {
            shift_rows(mirror.grid, width, scroll->top, scroll->bottom, scroll->lines);
            continue;
        }
        if (const auto* cells = std::get_if<CellsOp>(&op)) {
            const std::vector<ckv::Cell> incoming = proto::from_runs(cells->runs);
            std::copy(incoming.begin(), incoming.end(),
                      mirror.grid.begin() +
                          static_cast<std::ptrdiff_t>(cells->row) * static_cast<std::ptrdiff_t>(width) +
                          static_cast<std::ptrdiff_t>(cells->column));
            continue;
        }
        if (const auto* cursor = std::get_if<CursorOp>(&op)) {
            mirror.cursor = GridCursor{cursor->column, cursor->row, cursor->style, cursor->visible,
                                       cursor->blink};
            continue;
        }
        if (const auto* modes = std::get_if<ModesOp>(&op)) {
            // The mask says which bits this op speaks for. Assigning the whole
            // word instead would let a delta that mentions one mode silently
            // restate every other, which is exactly what a mask is for.
            mirror.modes = (mirror.modes & ~modes->changed_mask) | (modes->values & modes->changed_mask);
            continue;
        }
        if (const auto* title = std::get_if<TitleOp>(&op)) {
            mirror.title = title->title;
            continue;
        }
    }
    return true;
}

bool same_state(const GridState& a, const GridState& b) {
    if (!(a.cells == b.cells) || a.cursor != b.cursor || a.modes != b.modes || a.title != b.title)
        return false;
    if (a.grid.size() != b.grid.size()) return false;
    for (std::size_t index = 0; index < a.grid.size(); ++index)
        if (!same_cell(a.grid[index], b.grid[index])) return false;
    // The LIVE histories, compared cell for cell. Where each buffer's dead
    // prefix currently ends is a storage fact, not a terminal fact: two
    // mirrors that reclaimed at different moments still hold the same
    // history.
    const std::span<const ckv::Cell> history_a = scrollback_cells(a);
    const std::span<const ckv::Cell> history_b = scrollback_cells(b);
    if (history_a.size() != history_b.size()) return false;
    for (std::size_t index = 0; index < history_a.size(); ++index)
        if (!same_cell(history_a[index], history_b[index])) return false;
    return true;
}

std::vector<proto::CellRun> encode_grid(const GridState& state) { return proto::to_runs(state.grid); }

bool decode_grid(std::span<const proto::CellRun> runs, ckv::Size cells, GridState& state) {
    // A geometry past what this protocol will carry is refused, not clamped.
    // The size is the peer's, and it is what sizes the allocation: a snapshot
    // declaring 65535 by 65535, with run lengths that sum to match, is a
    // kilobyte of frame asking a client to materialise four billion cells —
    // which is a hostile peer costing the process rather than the connection
    // (the protocol spec, invariant 2; found by the fuzz lane).
    if (cells.width < 0 || cells.height < 0) return false;
    if (cells.width > static_cast<int>(proto::kMaxGridColumns) ||
        cells.height > static_cast<int>(proto::kMaxGridRows))
        return false;
    const std::size_t expected = static_cast<std::size_t>(std::max(0, cells.width)) *
                                 static_cast<std::size_t>(std::max(0, cells.height));
    std::size_t total = 0;
    for (const CellRun& run : runs) total += run.run_length;
    if (total != expected) return false;
    state.cells = cells;
    state.grid = proto::from_runs(std::vector<CellRun>(runs.begin(), runs.end()));
    return true;
}

}  // namespace ckm
