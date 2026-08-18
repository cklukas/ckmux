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
    // Through the same gate a delta's push uses, so the reader's capacity —
    // not the server's — bounds what is kept: a client that kept more than it
    // was configured to would hold text its reader asked it to forget.
    for (const std::vector<proto::CellRun>& line : state.scrollback)
        push_scrollback_line(state_, proto::from_runs(line));
    state_.cursor = GridCursor{state.cursor.column, state.cursor.row, state.cursor.style,
                               state.cursor.visible, state.cursor.blink};
    state_.modes = state.modes;
    state_.title = state.title;

    damage_ = everything(state_.cells);
    // The pictures are restated after every snapshot (the server's
    // attach_images follows the Attached), so whatever was held goes the same
    // way the grid did.
    rasters_.clear();
    raster_wire_ids_.clear();
    damage_.rasters = true;
    // A snapshot is what a lost sequence is cured by, so taking one is what
    // makes the mirror current again — and the count starts over, which is why
    // the next delta's seq of 1 means something (plans/05).
    sequence_ = 0;
    needs_snapshot_ = false;
    // The exit state deliberately survives.
    //
    // A snapshot replaces the SCREEN. Whether the child is still running is a
    // fact about a process, and `proto::TerminalState` carries no field for it
    // (common/proto.hpp) — so a snapshot cannot restate what a `TermClosed`
    // already said, and clearing it here brought a dead shell's window back
    // alive-looking after every reattach: no banner, no exit code, and a
    // terminal a reader could type into with nothing on the other end.
    //
    // `open()` is the seam for a terminal that genuinely starts again: it
    // clears all three. That is sound because the server mints terminal ids
    // monotonically and never reuses one (server/terminals.hpp), so a
    // `TermOpened` for an id is the only statement that a terminal by that id
    // is new — and a mirror never outlives the connection it was filled over
    // (`ServerSession::connection_lost` drops them all). When the snapshot
    // grows an exit field, `adopt` takes the state from `state` and this
    // becomes the fallback for a server that did not send one.
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
            continue;
        }
        if (std::holds_alternative<proto::CursorOp>(op)) damage_.cursor = true;
        if (std::holds_alternative<proto::ModesOp>(op)) damage_.modes = true;
        if (std::holds_alternative<proto::TitleOp>(op)) damage_.title = true;
    }
    return true;
}

void TerminalMirror::open(ckv::Size cells) {
    const std::size_t limit = state_.max_scrollback_lines;
    state_ = blank_state(cells, limit);
    damage_ = everything(state_.cells);
    sequence_ = 0;
    needs_snapshot_ = false;
    // The one place a mirror goes from dead back to alive, and the reason
    // `adopt` may leave the exit state alone: a `TermOpened` is the server
    // announcing a terminal, ids are never reused, so this really is a
    // different program behind the same mirror.
    exited_ = false;
    hold_ = false;
    exit_status_.reset();
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
    // A status only when something exited. `exited == 0` is the server saying
    // the terminal left this session without dying — a move (server.cpp) — and
    // it sends 0 in the status field because there is nothing to report. Stored
    // anyway, that zero becomes `status().exit_code == 0` and a view draws "the
    // program finished" over a program that is still running somewhere else.
    if (exited_)
        exit_status_ = closed.exit_status;
    else
        exit_status_.reset();
    // The banner a view draws for an exited terminal is part of its picture, so
    // the change is damage like any other.
    note_every_row();
}

void TerminalMirror::set_history_limit(std::size_t lines) {
    state_.max_scrollback_lines = lines;
    enforce_scrollback_capacity(state_);
}

std::span<const ckv::Cell> TerminalMirror::grid() const noexcept {
    return std::span<const ckv::Cell>(state_.grid.data(), state_.grid.size());
}

std::span<const ckv::Cell> TerminalMirror::history() const noexcept {
    return scrollback_cells(state_);
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

void TerminalMirror::place_image(std::uint64_t id, std::shared_ptr<const ckv::Image> image,
                                 ckv::Point anchor, ckv::Size cell_extent) {
    for (std::size_t index = 0; index < raster_wire_ids_.size(); ++index) {
        if (raster_wire_ids_[index] != id) continue;
        rasters_[index].anchor = anchor;
        rasters_[index].cell_extent = cell_extent;
        if (image != nullptr) rasters_[index].image = std::move(image);
        damage_.rasters = true;
        return;
    }
    // A bare move for a picture this mirror never received: ignored, because
    // the snapshot that heals whatever gap caused it restates the pictures.
    if (image == nullptr) return;
    ckv::core::TerminalRaster raster;
    raster.id = raster_identity_;
    raster.anchor = anchor;
    raster.cell_extent = cell_extent;
    raster.image = std::move(image);
    raster.fallback = "[sixel]";
    rasters_.push_back(std::move(raster));
    raster_wire_ids_.push_back(id);
    damage_.rasters = true;
}

void TerminalMirror::remove_image(std::uint64_t id) {
    for (std::size_t index = 0; index < raster_wire_ids_.size(); ++index) {
        if (raster_wire_ids_[index] != id) continue;
        rasters_.erase(rasters_.begin() + static_cast<std::ptrdiff_t>(index));
        raster_wire_ids_.erase(raster_wire_ids_.begin() + static_cast<std::ptrdiff_t>(index));
        damage_.rasters = true;
        return;
    }
}

void TerminalMirror::set_raster_identity(int identity) noexcept {
    raster_identity_ = identity;
    for (ckv::core::TerminalRaster& raster : rasters_) raster.id = identity;
}

}  // namespace ckm::client
