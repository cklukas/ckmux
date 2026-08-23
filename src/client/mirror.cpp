// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
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

// A complaint's kind, back from the wire's own enumeration into the library's.
// The protocol writes its values down rather than following ckVision's
// (proto::DiagnosticKind), so the two are matched up here and in the server's
// `wire_kind` and nowhere else.
ckv::core::TerminalDiagnostic::Kind kind_of(proto::DiagnosticKind kind) {
    using Kind = ckv::core::TerminalDiagnostic::Kind;
    switch (kind) {
        case proto::DiagnosticKind::LimitExceeded: return Kind::LimitExceeded;
        case proto::DiagnosticKind::UnsupportedSequence: return Kind::UnsupportedSequence;
        case proto::DiagnosticKind::MalformedSequence: return Kind::MalformedSequence;
        case proto::DiagnosticKind::ChildExited: return Kind::ChildExited;
    }
    return Kind::MalformedSequence;
}

}  // namespace

bool TerminalMirror::adopt(const proto::TerminalState& state) {
    // The geometry is refused before anything is sized by it. `blank_state`
    // below allocates columns × rows cells on the strength of two `u16`s the
    // peer chose, so a snapshot claiming 65535 by 65535 — four billion cells
    // out of a kilobyte on the wire — has to be stopped here rather than
    // discovered by the allocator (the protocol spec, invariant 2). Refused rather than
    // clamped: a mirror of a size nobody sent would draw a terminal that does
    // not exist, and the protocol's answer to a snapshot it cannot take is the
    // one it already has — ask for another.
    if (state.columns > proto::kMaxGridColumns || state.rows > proto::kMaxGridRows) {
        needs_snapshot_ = true;
        return false;
    }
    const std::size_t limit = state_.max_scrollback_lines;
    state_ = blank_state(ckv::Size{state.columns, state.rows}, limit);
    (void)decode_grid(state.grid, state_.cells, state_);
    // Through the same gate a delta's push uses, so the reader's capacity —
    // not the server's — bounds what is kept: a client that kept more than it
    // was configured to would hold text its reader asked it to forget.
    for (const std::vector<proto::CellRun>& line : state.scrollback) {
        // The same rule a delta's push is held to, on the path that does not
        // go through `apply_delta`: a snapshot line wider than the terminal it
        // claims to belong to is refused rather than materialised (M-P1's
        // other half — 62.9 MB out of 449 bytes, reached through attach).
        std::size_t length = 0;
        for (const proto::CellRun& run : line) length += run.run_length;
        if (length == 0 || length > static_cast<std::size_t>(std::max(0, state_.cells.width)))
            continue;
        push_scrollback_line(state_, proto::from_runs(line));
    }
    state_.cursor = GridCursor{state.cursor.column, state.cursor.row, state.cursor.style,
                               state.cursor.visible, state.cursor.blink};
    state_.modes = state.modes;
    state_.title = state.title;
    custom_title_ = state.custom_title;
    bell_marked_ = (state.flags & static_cast<std::uint8_t>(proto::TermMetaFlag::Bell)) != 0;
    activity_marked_ = (state.flags & static_cast<std::uint8_t>(proto::TermMetaFlag::Activity)) != 0;
    // And the counts, from the snapshot as from a meta — the same rule either
    // way, which is what stops "a reader who has just arrived" being a special
    // case (WP-41).
    bell_serial_ = state.bell_serial;
    activity_serial_ = state.activity_serial;
    printer_active_ = state.printer_state == proto::PrinterState::Capturing;
    printer_state_ = state.printer_state;
    printer_mode_ = state.printer_mode;
    printer_scope_ = state.printer_scope;
    printer_bytes_ = state.printer_bytes;
    printer_jobs_ = state.print_jobs.size();
    // The jobs a reattaching reader finds waiting. Metadata only — the text
    // stays on the server until somebody opens a preview.
    print_jobs_ = state.print_jobs;
    // Any fetch that was in flight when this snapshot arrived was against a
    // terminal whose state has just been restated wholesale. Its chunks may be
    // for jobs that are no longer there, and half a document assembled across
    // a resnapshot is worse than none.
    fetching_.clear();
    fetched_.clear();
    // The clipboard WATERMARK, and no text with it — the server sends none in a
    // snapshot on purpose (proto::TerminalState). A reattaching client that was
    // handed the last text would put it on its reader's system clipboard over
    // whatever they had copied since, minutes after the child asked; one that
    // took no watermark would meet the NEXT write with a number a watcher had
    // already seen and drop it. So: the number, never the text.
    //
    // Never backwards, because a consumer's comparison is a high-water mark and
    // a serial that went down could collide with one. The server's own is
    // monotonic per terminal and this mirror only ever advances by one per
    // write, so the max is the same number in every real case and an invariant
    // rather than an argument in every case.
    clipboard_serial_ = std::max(clipboard_serial_, state.clipboard_serial);
    clipboard_text_.clear();
    diagnostics_.clear();
    if (!state.diagnostic.empty())
        diagnostics_.push_back(
            ckv::core::TerminalDiagnostic{kind_of(state.diagnostic_kind), state.diagnostic});
    // An exit is a one-way door, and the snapshot now states which side of it
    // this terminal is on. A snapshot that says the child ended is adopted
    // whole — status and hold with it — and one that says nothing leaves what a
    // `TermClosed` already said, because ids are never reused and a terminal
    // cannot come back to life under the same one (`open()` is the seam for a
    // terminal that genuinely starts again). Before the field existed, `adopt`
    // cleared the exit state and a dead shell's window came back alive-looking
    // after every reattach: no banner, no exit code, and a terminal a reader
    // could type into with nothing on the other end (M-R4).
    if (state.exited != 0) {
        exited_ = true;
        hold_ = state.hold != 0;
        exit_status_ = state.exit_status;
    }

    damage_ = everything(state_.cells);
    // The pictures the snapshot says this terminal's watchers hold are KEPT:
    // their wire ids are stable across snapshots, the restatement that
    // follows will arrive under these same ids, and place_image treats a
    // restated id as the swap it is. Clearing them all — the first answer
    // here — meant every heal blanked every picture until its pixels had
    // re-crossed the socket, which under load is exactly when they are
    // megabytes behind: a reader watching a busy child saw gray with the
    // picture surfacing for a moment per heal (field report, 2026-08-19,
    // ckgrapher). What the snapshot does NOT name goes, because a picture
    // missing from the believed set had its Remove dropped with the rest of
    // the backlog this snapshot is healing.
    for (std::size_t index = raster_wire_ids_.size(); index-- > 0;) {
        const std::uint64_t held = raster_wire_ids_[index];
        if (std::find(state.images.begin(), state.images.end(), held) != state.images.end())
            continue;
        rasters_.erase(rasters_.begin() + static_cast<std::ptrdiff_t>(index));
        raster_wire_ids_.erase(raster_wire_ids_.begin() + static_cast<std::ptrdiff_t>(index));
    }
    damage_.rasters = true;
    // A snapshot is what a lost sequence is cured by, so taking one is what
    // makes the mirror current again — and the count starts over, which is why
    // the next delta's seq of 1 means something (the protocol spec).
    sequence_ = 0;
    needs_snapshot_ = false;
    // A snapshot that states no exit leaves the exit state alone, above.
    //
    // `open()` is the seam for a terminal that genuinely starts again: it
    // clears all three. That is sound because the server mints terminal ids
    // monotonically and never reuses one (server/terminals.hpp), so a
    // `TermOpened` for an id is the only statement that a terminal by that id
    // is new — and a mirror never outlives the connection it was filled over
    // (`ServerSession::connection_lost` drops them all).
    return true;
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
        // The ops do not fit. A delta that changes the size says so itself
        // now, so this is either a peer asking for a geometry no grid may have
        // or a mirror that has genuinely lost track — and the cure for both is
        // the one the protocol already defines. `adopt` sizes itself from the
        // snapshot, so the recovery needs nothing else.
        needs_snapshot_ = true;
        return false;
    }
    sequence_ = delta.seq;

    // What the view has to repaint, read off the ops rather than by comparing
    // grids: the ops ARE the statement of what changed, and a mirror that
    // compared would be doing the diff engine's work a second time.
    for (const proto::GridOp& op : delta.ops) {
        if (std::holds_alternative<proto::ResizeOp>(op)) {
            // Everything, at the new size — read off `state_`, which
            // `apply_delta` has already resized. Not `everything()` wholesale,
            // because that would discard the pushed-line count this same walk
            // may go on to accumulate: a delta states its size and then pushes
            // the history that scrolled away at it, and a view told about only
            // half of that draws half a frame.
            damage_.full = true;
            damage_.cursor = true;
            damage_.modes = true;
            damage_.title = true;
            damage_.rasters = true;
            damage_.rows.assign(static_cast<std::size_t>(std::max(0, state_.cells.height)),
                                ckv::core::TerminalDamage::RowSpan{0, std::max(0, state_.cells.width)});
            continue;
        }
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
    // And nothing a reader has missed, because there was nothing here to miss:
    // this terminal is a different program behind the same mirror.
    bell_marked_ = false;
    activity_marked_ = false;
    printer_active_ = false;
    printer_bytes_ = 0;
    printer_jobs_ = 0;
    diagnostics_.clear();
}

void TerminalMirror::apply(const proto::TermMeta& meta) {
    if (meta.title != state_.title) {
        state_.title = meta.title;
        damage_.title = true;
    }
    // Assigned, like the marks below and for the same reason: this message
    // states the name as it now stands, and the message that says a reader
    // handed it back is the same message with an empty string in it.
    //
    // No title damage raised for this one. `damage_.title` is what tells a
    // TerminalView its child renamed itself; a custom title is not the child's
    // and is read by the window layer on its own poll, so raising it here
    // would be telling the view about something it does not draw.
    custom_title_ = meta.custom_title;
    // The marks are assigned, not accumulated: the message states them as they
    // now stand, and the one that says a reader has caught up is the same
    // message with the bits off.
    bell_marked_ = (meta.flags & static_cast<std::uint8_t>(proto::TermMetaFlag::Bell)) != 0;
    activity_marked_ = (meta.flags & static_cast<std::uint8_t>(proto::TermMetaFlag::Activity)) != 0;
    // The counts are assigned rather than accumulated for the same reason the
    // marks are: the message states them as they now stand. They only ever go
    // up on the server, but a mirror must not assume that — a reattach states
    // whatever is current, and a mirror that took a maximum would be holding a
    // number the server does not have.
    bell_serial_ = meta.bell_serial;
    activity_serial_ = meta.activity_serial;
}

void TerminalMirror::apply(const proto::PrintJobAdded& added) {
    // Restated rather than appended blindly: the server announces a job once,
    // but a reattach can restate one this mirror already holds, and a reader
    // must not see the same capture listed twice.
    for (proto::PrintJobInfo& held : print_jobs_) {
        if (held.job != added.job.job) continue;
        held = added.job;
        printer_jobs_ = print_jobs_.size();
        return;
    }
    print_jobs_.push_back(added.job);
    printer_jobs_ = print_jobs_.size();
}

bool TerminalMirror::apply(const proto::PrintJobData& chunk) {
    // Assembled per job. Two previews open at once is a thing a reader can do,
    // the wire allows their chunks to interleave, and one buffer would splice
    // two documents into each other without either looking wrong.
    std::string& assembling = fetching_[chunk.job];
    assembling += chunk.bytes;
    if (chunk.final_chunk == 0) return false;
    fetched_[chunk.job] = std::move(assembling);
    fetching_.erase(chunk.job);
    return true;
}

const std::string* TerminalMirror::print_job_text(std::uint64_t job) const {
    const auto found = fetched_.find(job);
    return found == fetched_.end() ? nullptr : &found->second;
}

void TerminalMirror::forget_print_jobs(std::uint64_t job) {
    if (job == 0) {
        print_jobs_.clear();
        fetching_.clear();
        fetched_.clear();
    } else {
        std::erase_if(print_jobs_, [job](const proto::PrintJobInfo& held) {
            return held.job == job;
        });
        fetching_.erase(job);
        fetched_.erase(job);
    }
    printer_jobs_ = print_jobs_.size();
}

void TerminalMirror::apply(const proto::ClipboardSet& clipboard) {
    clipboard_text_ = clipboard.text;
    // Forward, always. What the number IS does not matter to anything here —
    // only that it differs from the one a consumer last acted on, and that it
    // never repeats one (mirror.hpp).
    ++clipboard_serial_;
}

void TerminalMirror::apply(const proto::PrintState& printing) {
    printer_active_ = printing.state == proto::PrinterState::Capturing;
    printer_state_ = printing.state;
    printer_mode_ = printing.mode;
    printer_bytes_ = printing.bytes;
    // The COUNT is the server's, and the list this mirror holds may be shorter
    // — a job announced while this client was away is counted here and
    // described only by the next snapshot. Trusting the list's length instead
    // would make the button say a capture had gone when it had not.
    printer_jobs_ = printing.jobs;
}

void TerminalMirror::apply(const proto::TermDiagnostic& said) {
    diagnostics_.clear();
    diagnostics_.push_back(ckv::core::TerminalDiagnostic{kind_of(said.kind), said.text});
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
    // raster_identity_ alone names this terminal, not this picture: a wire
    // id already distinguishes pictures on the connection, but two rasters
    // sharing one Surface still need distinct scene ids (surface.cpp's own
    // add_raster_region contract), which a second picture arriving before
    // the first is removed — ordinary; the snapshot restates whatever a
    // reconnect lost — would otherwise violate exactly like the local
    // TerminalEmulator's place_raster did.
    raster.id = raster_identity_ == 0
                    ? 0
                    : raster_identity_ + ckv::core::allocate_local_raster_slot(rasters_, raster_identity_);
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
    // Reassigning an already-live mirror (a reattach mints a fresh identity
    // for what may still hold rasters from before the gap) has to carry each
    // one's own local slot across the change, not just the shared base —
    // collapsing every raster back onto the bare identity would recreate the
    // same id collision place_image's own slot allocation exists to avoid.
    const int old_identity = raster_identity_;
    raster_identity_ = identity;
    for (ckv::core::TerminalRaster& raster : rasters_) {
        const int slot = old_identity == 0 ? 0 : raster.id - old_identity;
        raster.id = identity == 0 ? 0 : identity + slot;
    }
}

}  // namespace ckm::client
