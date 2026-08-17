// Copyright (c) 2026 C. Klukas. All rights reserved.
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

std::vector<proto::GridOp> diff(const GridState& previous, const GridView& current,
                                std::span<const std::vector<ckv::Cell>> pushed,
                                std::span<const RowDamage> damaged) {
    std::vector<GridOp> ops;

    if (!pushed.empty()) {
        ScrollbackPushOp push;
        push.lines.reserve(pushed.size());
        for (const std::vector<ckv::Cell>& line : pushed) push.lines.push_back(runs_of(line));
        ops.push_back(std::move(push));
    }

    const int width = current.cells.width;
    const int height = current.cells.height;
    const std::size_t expected = static_cast<std::size_t>(std::max(0, width)) *
                                 static_cast<std::size_t>(std::max(0, height));
    const bool same_geometry = previous.cells == current.cells &&
                               previous.grid.size() == expected && current.grid.size() == expected;

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
            int first = 0;
            while (first < width && same_cell(mirror_row[static_cast<std::size_t>(first)],
                                              wanted[static_cast<std::size_t>(first)]))
                ++first;
            if (first == width) continue;
            int last = width;
            while (last > first && same_cell(mirror_row[static_cast<std::size_t>(last - 1)],
                                             wanted[static_cast<std::size_t>(last - 1)]))
                --last;
            ops.push_back(CellsOp{static_cast<std::uint16_t>(row), static_cast<std::uint16_t>(first),
                                  runs_of(wanted.subspan(static_cast<std::size_t>(first),
                                                         static_cast<std::size_t>(last - first)))});
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
    const int width = mirror.cells.width;
    const int height = mirror.cells.height;
    if (mirror.grid.size() != static_cast<std::size_t>(std::max(0, width)) *
                                  static_cast<std::size_t>(std::max(0, height)))
        return false;

    // Checked in full before anything is performed. All-or-nothing is the
    // whole contract: a mirror holding half a delta shows a program's screen
    // with a stripe of some other moment in it, and nothing downstream can
    // tell that it happened.
    for (const GridOp& op : ops) {
        if (const auto* scroll = std::get_if<ScrollOp>(&op)) {
            if (scroll->top > scroll->bottom || scroll->bottom > height) return false;
            const int region = scroll->bottom - scroll->top;
            if (std::abs(static_cast<int>(scroll->lines)) > region) return false;
            continue;
        }
        if (const auto* cells = std::get_if<CellsOp>(&op)) {
            if (cells->row >= height || cells->column > width) return false;
            if (total_length(cells->runs) > static_cast<std::size_t>(width - cells->column))
                return false;
            continue;
        }
        if (const auto* cursor = std::get_if<CursorOp>(&op)) {
            // One past the last column is allowed: that is where a terminal
            // leaves the cursor after filling a row it has not wrapped yet,
            // and refusing it would make a mirror resnapshot over a state its
            // own emulator produces routinely.
            if (cursor->row >= height || cursor->column > width) return false;
            continue;
        }
        if (const auto* push = std::get_if<ScrollbackPushOp>(&op)) {
            for (const std::vector<CellRun>& line : push->lines)
                if (total_length(line) == 0) return false;
            continue;
        }
    }

    for (const GridOp& op : ops) {
        if (const auto* push = std::get_if<ScrollbackPushOp>(&op)) {
            // A holder that keeps no history does not decode the lines at all.
            // That is the server's own belief about a client: it never diffs
            // against a history, so paying to rebuild one per terminal per tick
            // would be the copy U0-b removed, arriving from the other side.
            if (mirror.max_scrollback_lines == 0) continue;
            for (const std::vector<CellRun>& line : push->lines)
                mirror.scrollback.push_back(proto::from_runs(line));
            // Capacity is the reader's, and it is counted in lines because a
            // line is what they scroll past (plans/08 `scrollback`).
            if (mirror.scrollback.size() > mirror.max_scrollback_lines) {
                const std::size_t excess = mirror.scrollback.size() - mirror.max_scrollback_lines;
                mirror.scrollback.erase(mirror.scrollback.begin(),
                                        mirror.scrollback.begin() +
                                            static_cast<std::ptrdiff_t>(excess));
            }
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
    if (a.grid.size() != b.grid.size() || a.scrollback.size() != b.scrollback.size()) return false;
    for (std::size_t index = 0; index < a.grid.size(); ++index)
        if (!same_cell(a.grid[index], b.grid[index])) return false;
    for (std::size_t line = 0; line < a.scrollback.size(); ++line) {
        if (a.scrollback[line].size() != b.scrollback[line].size()) return false;
        for (std::size_t index = 0; index < a.scrollback[line].size(); ++index)
            if (!same_cell(a.scrollback[line][index], b.scrollback[line][index])) return false;
    }
    return true;
}

std::vector<proto::CellRun> encode_grid(const GridState& state) { return proto::to_runs(state.grid); }

bool decode_grid(std::span<const proto::CellRun> runs, ckv::Size cells, GridState& state) {
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
