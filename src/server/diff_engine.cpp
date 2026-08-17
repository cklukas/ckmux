// Copyright (c) 2026 C. Klukas. All rights reserved.
#include "server/diff_engine.hpp"

#include <algorithm>
#include <utility>

namespace ckm::server {
namespace {

using ckv::term::TerminalStatus;

// The view a live terminal is diffed through: borrowed cells, and the scalars
// beside them. Nothing here copies a grid, which is the whole of U0-b arriving
// where it was aimed.
GridView view_of(const ckv::core::TerminalSubsession& source, const TerminalStatus& status) {
    GridView view;
    view.cells = status.cells;
    view.grid = source.cells();
    view.cursor = cursor_of(status);
    view.modes = modes_of(status);
    view.title = status.title;
    return view;
}

// The damage report, in the algebra's own vocabulary. A handful of integers per
// tick; what it buys is a diff that reads the rows a child touched rather than
// the screen.
std::vector<RowDamage> row_hints(const ckv::term::TerminalDamage& damage, int height) {
    std::vector<RowDamage> hints;
    hints.reserve(static_cast<std::size_t>(std::max(0, height)));
    for (std::size_t row = 0; row < damage.rows.size() && row < static_cast<std::size_t>(std::max(0, height));
         ++row)
        hints.push_back(RowDamage{damage.rows[row].first, damage.rows[row].last});
    return hints;
}

GridState state_of(const GridView& view) {
    GridState state;
    state.cells = view.cells;
    state.grid.assign(view.grid.begin(), view.grid.end());
    state.max_scrollback_lines = 0;  // the server keeps no mirror history
    state.cursor = view.cursor;
    state.modes = view.modes;
    state.title = std::string(view.title);
    return state;
}

}  // namespace

std::uint32_t modes_of(const TerminalStatus& status) {
    std::uint32_t modes = 0;
    const auto set = [&modes](proto::ModeBit bit, bool on) {
        if (on) modes |= static_cast<std::uint32_t>(bit);
    };
    set(proto::ModeBit::MouseReporting, status.mouse_reporting_enabled);
    // The encoding is a second bit rather than a second field: it is only ever
    // asked about while reporting is on, and X10 is the absence of SGR.
    set(proto::ModeBit::MouseEncodingSgr,
        status.mouse_encoding == ckv::term::TerminalMouseEncoding::Sgr);
    set(proto::ModeBit::BracketedPaste, status.bracketed_paste_enabled);
    set(proto::ModeBit::ApplicationCursorKeys, status.application_cursor_keys);
    set(proto::ModeBit::FocusReporting, status.focus_reporting_enabled);
    set(proto::ModeBit::AlternateBuffer, status.alternate_buffer);
    set(proto::ModeBit::AlternateScroll, status.alternate_scroll_enabled);
    return modes;
}

GridCursor cursor_of(const TerminalStatus& status) {
    GridCursor cursor;
    cursor.column = static_cast<std::uint16_t>(std::max(0, status.cursor.position.x));
    cursor.row = static_cast<std::uint16_t>(std::max(0, status.cursor.position.y));
    cursor.style = static_cast<std::uint8_t>(status.cursor.shape);
    cursor.visible = status.cursor.visible ? 1 : 0;
    cursor.blink = status.cursor.blink ? 1 : 0;
    return cursor;
}

std::vector<std::vector<ckv::Cell>> newest_history_lines(const ckv::core::TerminalSubsession& source,
                                                         int columns, std::size_t count) {
    std::vector<std::vector<ckv::Cell>> lines;
    if (count == 0) return lines;
    const std::span<const ckv::Cell> history = source.scrollback();
    const std::size_t width = static_cast<std::size_t>(std::max(1, columns));
    const std::size_t available = history.size() / width;
    // A burst larger than the terminal's capacity has already dropped its own
    // oldest lines; sending what is left is not a loss, because a client with
    // the same capacity would have dropped them in the same order.
    const std::size_t take = std::min(count, available);
    lines.reserve(take);
    for (std::size_t line = available - take; line < available; ++line) {
        const std::span<const ckv::Cell> row = history.subspan(line * width, width);
        lines.emplace_back(row.begin(), row.end());
    }
    return lines;
}

std::optional<proto::GridDelta> TerminalDiffer::flush(TerminalId id,
                                                      const ckv::core::TerminalSubsession& source) {
    const ckv::term::TerminalDamage& damage = source.damage();
    if (!damage.any()) return std::nullopt;

    const TerminalStatus status = source.status();
    const GridView view = view_of(source, status);

    // More scrolled away in one tick than a delta may carry: the screen goes and
    // the history waits. Answering this with a snapshot — which does carry the
    // history — is what a first version did, and it cost 2.8 MB per tick under
    // `yes`, with every other answer on the connection queued behind it.
    const bool flooded = damage.scrollback_pushed > kMaxHistoryLinesPerDelta;
    if (flooded) history_diverged_ = true;
    // And when it calms down, one snapshot puts the history back. Once, not per
    // tick: `history_diverged_` is cleared by taking it.
    if (!flooded && history_diverged_) {
        needs_snapshot_ = true;
        return std::nullopt;
    }

    const std::vector<std::vector<ckv::Cell>> pushed =
        flooded ? std::vector<std::vector<ckv::Cell>>{}
                : newest_history_lines(source, status.cells.width, damage.scrollback_pushed);

    proto::GridDelta delta;
    delta.term = id;

    // A size change costs the whole grid, and that is not an optimisation to be
    // clever about: what a client's mirror does with its own cells when it
    // resizes is the client's business, and a server that assumed one answer
    // would leave stale cells on a reader's screen wherever it assumed wrong.
    // An empty belief — a terminal nobody has been told about yet — is the same
    // case, which is why the belief starts at a size of nothing.
    //
    // `damage.full` is NOT that case. It means "assume every row changed", which
    // for a diff is a statement about which rows to examine and not an
    // instruction to re-send them: a terminal that has just been snapshotted
    // still has its full flag set from construction, and re-sending everything
    // on the first tick after every attach would undo the snapshot's whole
    // purpose.
    if (believed_.cells != status.cells) {
        delta.ops = full_repaint(view, pushed);
        believed_ = state_of(view);
    } else {
        // No hints when the terminal says everything changed: an empty span
        // means "examine every row", which is exactly what full asks for.
        const std::vector<RowDamage> hints =
            damage.full ? std::vector<RowDamage>{} : row_hints(damage, status.cells.height);
        delta.ops = diff(believed_, view, pushed, hints);
        // The belief is advanced by APPLYING what is being sent, not by copying
        // the terminal: if the ops do not reproduce the terminal, the server's
        // belief diverges exactly as the client's mirror would, and the tests
        // that compare the two catch it. Copying the terminal here would hide
        // precisely the bug worth finding.
        if (!apply_delta(delta.ops, believed_)) {
            // Cannot happen — the ops were built against this belief — and if
            // it ever does, the honest answer is the whole grid rather than a
            // mirror nobody can reason about.
            delta.ops = full_repaint(view, pushed);
            believed_ = state_of(view);
        }
    }

    if (delta.ops.empty()) return std::nullopt;
    delta.seq = ++sequence_;
    return delta;
}

proto::TerminalState TerminalDiffer::snapshot(TerminalId id,
                                              const ckv::core::TerminalSubsession& source) {
    const TerminalStatus status = source.status();
    const GridView view = view_of(source, status);

    proto::TerminalState state;
    state.term = id;
    state.title = status.title;
    state.columns = static_cast<std::uint16_t>(std::max(0, status.cells.width));
    state.rows = static_cast<std::uint16_t>(std::max(0, status.cells.height));
    state.cursor = proto::CursorOp{view.cursor.column, view.cursor.row, view.cursor.style,
                                   view.cursor.visible, view.cursor.blink};
    state.modes = view.modes;
    state.grid = proto::to_runs(std::vector<ckv::Cell>(view.grid.begin(), view.grid.end()));
    // The history in full, in lines: a reattached client pages back through
    // everything the server still holds without a round trip (plans/05).
    const std::span<const ckv::Cell> history = source.scrollback();
    const std::size_t width = static_cast<std::size_t>(std::max(1, status.cells.width));
    for (std::size_t line = 0; line + width <= history.size(); line += width) {
        const std::span<const ckv::Cell> row = history.subspan(line, width);
        state.scrollback.push_back(proto::to_runs(std::vector<ckv::Cell>(row.begin(), row.end())));
    }
    state.printer_state = status.printer_controller_active ? proto::PrinterState::Capturing
                                                           : proto::PrinterState::Idle;
    state.printer_bytes = static_cast<std::uint32_t>(
        std::min<std::size_t>(status.printer_pending_bytes, 0xFFFFFFFFu));

    // What the client has just been given is what it holds, and the deltas that
    // follow start again at 1. A snapshot carries no sequence of its own — that
    // would have to be the number of a delta nobody sent — so restarting the
    // count is what lets a client check continuity from its FIRST delta instead
    // of having to take that one on trust.
    believed_ = state_of(view);
    sequence_ = 0;
    needs_snapshot_ = false;
    history_diverged_ = false;
    return state;
}

std::optional<proto::GridDelta> DiffEngine::flush(TerminalId id,
                                                 ckv::core::TerminalSubsession& source) {
    std::optional<proto::GridDelta> delta = differs_[id].flush(id, source);
    // Sent: the host saying it has caught up. Here rather than inside the
    // differ, so that a terminal with several clients watching it has its
    // damage cleared once, after all of them have read — not by whichever one
    // looked first.
    source.clear_damage();
    return delta;
}

std::vector<proto::GridDelta> DiffEngine::flush(Terminals& terminals) {
    std::vector<proto::GridDelta> deltas;
    for (const TerminalId id : terminals.ids()) {
        Terminal* terminal = terminals.find(id);
        if (terminal == nullptr) continue;
        std::optional<proto::GridDelta> delta = flush(id, terminal->session());
        if (delta.has_value()) deltas.push_back(std::move(*delta));
    }
    return deltas;
}

proto::TerminalState DiffEngine::snapshot(TerminalId id,
                                          const ckv::core::TerminalSubsession& source) {
    return differs_[id].snapshot(id, source);
}

void DiffEngine::forget(TerminalId id) { differs_.erase(id); }

std::uint32_t DiffEngine::sequence_for(TerminalId id) const {
    const auto found = differs_.find(id);
    return found == differs_.end() ? 0 : found->second.sequence();
}

const TerminalDiffer* DiffEngine::differ_for(TerminalId id) const {
    const auto found = differs_.find(id);
    return found == differs_.end() ? nullptr : &found->second;
}

}  // namespace ckm::server
