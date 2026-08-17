// Copyright (c) 2026 C. Klukas. All rights reserved.
#include "client/mirror.hpp"

#include <algorithm>
#include <variant>

namespace ckm::client {
namespace {

// A damage report the size of a grid, all of it dirty. What a fresh mirror and a
// resnapshotted one both start from: the first frame has to draw everything,
// because the view has drawn nothing.
ckv::core::TerminalDamage everything(ckv::Size cells) {
    ckv::core::TerminalDamage damage;
    damage.full = true;
    damage.cursor = true;
    damage.modes = true;
    damage.title = true;
    damage.rasters = true;
    damage.rows.assign(static_cast<std::size_t>(std::max(0, cells.height)),
                       ckv::core::TerminalDamage::RowSpan{0, std::max(0, cells.width)});
    return damage;
}

}  // namespace

void TerminalMirror::adopt(const proto::TerminalState& state) {
    const std::size_t limit = state_.max_scrollback_lines;
    state_ = blank_state(ckv::Size{state.columns, state.rows}, limit);
    (void)decode_grid(state.grid, state_.cells, state_);
    state_.scrollback.clear();
    for (const std::vector<proto::CellRun>& line : state.scrollback)
        state_.scrollback.push_back(proto::from_runs(line));
    // The reader's capacity, not the server's: a client that kept more than it
    // was configured to would hold text its reader asked it to forget.
    if (limit != 0 && state_.scrollback.size() > limit)
        state_.scrollback.erase(state_.scrollback.begin(),
                                state_.scrollback.begin() +
                                    static_cast<std::ptrdiff_t>(state_.scrollback.size() - limit));
    state_.cursor = GridCursor{state.cursor.column, state.cursor.row, state.cursor.style,
                               state.cursor.visible, state.cursor.blink};
    state_.modes = state.modes;
    state_.title = state.title;

    history_cache_valid_ = false;
    damage_ = everything(state_.cells);
    // A snapshot is what a lost sequence is cured by, so taking one is what
    // makes the mirror current again — and the count starts over, which is why
    // the next delta's seq of 1 means something (plans/05).
    sequence_ = 0;
    needs_snapshot_ = false;
    exited_ = false;
    hold_ = false;
    exit_status_.reset();
}

bool TerminalMirror::apply(const proto::GridDelta& delta) {
    if (delta.seq != sequence_ + 1) {
        // Something was lost. The protocol's answer is not to work out what:
        // the client resnapshots, because a mirror that guessed would show a
        // screen no program ever drew.
        ++gaps_;
        needs_snapshot_ = true;
        return false;
    }
    if (!apply_delta(delta.ops, state_)) {
        // The ops do not fit — the two ends disagree about the size, which a
        // TermMeta was supposed to have settled first. Same cure.
        needs_snapshot_ = true;
        return false;
    }
    sequence_ = delta.seq;

    // What the view has to repaint, read off the ops rather than by comparing
    // grids: the ops ARE the statement of what changed, and a mirror that
    // compared would be doing the diff engine's work a second time.
    for (const proto::GridOp& op : delta.ops) {
        if (const auto* cells = std::get_if<proto::CellsOp>(&op)) {
            std::size_t length = 0;
            for (const proto::CellRun& run : cells->runs) length += run.run_length;
            note_row(cells->row, cells->column,
                     cells->column + static_cast<int>(length));
            continue;
        }
        if (const auto* scroll = std::get_if<proto::ScrollOp>(&op)) {
            for (int row = scroll->top; row < scroll->bottom; ++row)
                note_row(row, 0, state_.cells.width);
            continue;
        }
        if (const auto* push = std::get_if<proto::ScrollbackPushOp>(&op)) {
            damage_.scrollback_pushed += push->lines.size();
            history_cache_valid_ = false;
            continue;
        }
        if (std::holds_alternative<proto::CursorOp>(op)) damage_.cursor = true;
        if (std::holds_alternative<proto::ModesOp>(op)) damage_.modes = true;
        if (std::holds_alternative<proto::TitleOp>(op)) damage_.title = true;
    }
    return true;
}

void TerminalMirror::apply(const proto::TermMeta& meta) {
    if (meta.title != state_.title) {
        state_.title = meta.title;
        damage_.title = true;
    }
}

void TerminalMirror::apply(const proto::TermClosed& closed) {
    exited_ = closed.exited != 0;
    hold_ = closed.hold != 0;
    exit_status_ = closed.exit_status;
    // The banner a view draws for an exited terminal is part of its picture, so
    // the change is damage like any other.
    note_every_row();
}

void TerminalMirror::set_history_limit(std::size_t lines) {
    state_.max_scrollback_lines = lines;
    if (lines == 0) {
        state_.scrollback.clear();
        history_cache_valid_ = false;
        return;
    }
    if (state_.scrollback.size() > lines) {
        state_.scrollback.erase(state_.scrollback.begin(),
                                state_.scrollback.begin() +
                                    static_cast<std::ptrdiff_t>(state_.scrollback.size() - lines));
        history_cache_valid_ = false;
    }
}

std::span<const ckv::Cell> TerminalMirror::grid() const noexcept {
    return std::span<const ckv::Cell>(state_.grid.data(), state_.grid.size());
}

std::span<const ckv::Cell> TerminalMirror::history() const noexcept {
    if (!history_cache_valid_) const_cast<TerminalMirror*>(this)->rebuild_history_cache();
    return std::span<const ckv::Cell>(history_cache_.data(), history_cache_.size());
}

void TerminalMirror::rebuild_history_cache() {
    const std::size_t width = static_cast<std::size_t>(std::max(1, state_.cells.width));
    history_cache_.clear();
    history_cache_.reserve(state_.scrollback.size() * width);
    for (const std::vector<ckv::Cell>& line : state_.scrollback) {
        // A line whose width does not match the grid is padded or cut, because
        // every reader indexes the history as rows of the CURRENT width — the
        // same rule the emulator keeps when a resize re-lays its history.
        for (std::size_t column = 0; column < width; ++column)
            history_cache_.push_back(column < line.size() ? line[column] : blank_cell());
    }
    history_cache_valid_ = true;
}

void TerminalMirror::clear_damage() noexcept {
    damage_ = ckv::core::TerminalDamage{};
    damage_.rows.assign(static_cast<std::size_t>(std::max(0, state_.cells.height)),
                        ckv::core::TerminalDamage::RowSpan{});
}

void TerminalMirror::note_row(int row, int first, int last) {
    if (row < 0 || row >= state_.cells.height) return;
    if (damage_.rows.size() != static_cast<std::size_t>(std::max(0, state_.cells.height)))
        damage_.rows.resize(static_cast<std::size_t>(std::max(0, state_.cells.height)));
    ckv::core::TerminalDamage::RowSpan& span = damage_.rows[static_cast<std::size_t>(row)];
    const int clamped_first = std::max(0, std::min(first, state_.cells.width));
    const int clamped_last = std::max(0, std::min(last, state_.cells.width));
    if (span.empty()) {
        span.first = clamped_first;
        span.last = clamped_last;
        return;
    }
    span.first = std::min(span.first, clamped_first);
    span.last = std::max(span.last, clamped_last);
}

void TerminalMirror::note_every_row() {
    for (int row = 0; row < state_.cells.height; ++row) note_row(row, 0, state_.cells.width);
}

}  // namespace ckm::client
