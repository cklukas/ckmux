// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "server/diff_engine.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
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
    view.title = clamp_utf8(status.title, proto::kMaxTitleBytes);
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

std::string_view clamp_utf8(std::string_view text, std::size_t limit) {
    if (text.size() <= limit) return text;
    std::size_t cut = limit;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u) --cut;
    return text.substr(0, cut);
}

proto::DiagnosticKind wire_kind(ckv::core::TerminalDiagnostic::Kind kind) {
    using Kind = ckv::core::TerminalDiagnostic::Kind;
    switch (kind) {
        case Kind::LimitExceeded: return proto::DiagnosticKind::LimitExceeded;
        case Kind::UnsupportedSequence: return proto::DiagnosticKind::UnsupportedSequence;
        case Kind::MalformedSequence: return proto::DiagnosticKind::MalformedSequence;
        case Kind::ChildExited: return proto::DiagnosticKind::ChildExited;
    }
    // Unreachable for any value the library has; stated rather than assumed,
    // because a kind added upstream must not silently become another one.
    return proto::DiagnosticKind::MalformedSequence;
}

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
    // The tracking level, above the flags. Not the same question as
    // `MouseReporting`, which is only whether the child is listening at all: a
    // program that asked for DEC 1000 and is sent 1003's motion reports reads
    // the surplus as something else entirely, so the level travels rather than
    // being inferred at the far end (ckVision's TerminalMouseTracking).
    modes |= (static_cast<std::uint32_t>(status.mouse_tracking) << proto::kMouseTrackingShift) &
             proto::kMouseTrackingMask;
    // The kitty keyboard enhancements, above the tracking level. A mode in all
    // but its spelling, so it rides the modes word and needs no path of its
    // own: the emulator flags `damage.modes` when the child pushes or pops a
    // set, and the whole word travels on every snapshot. Without it the client
    // encoded keys the legacy way while the server told the child the
    // enhancements were on — the fallback the child had just switched off
    // (M-R2).
    modes |= (static_cast<std::uint32_t>(status.keyboard_flags) << proto::kKeyboardFlagsShift) &
             proto::kKeyboardFlagsMask;
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
    //
    // This is also the whole of the server's side of C3: `full_repaint` states
    // the new size as its first op, so the client's mirror resizes from the
    // same delta that repaints it rather than from a message nobody sends.
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
    // The view's title rather than the status's: it has been cut to something
    // the wire can say, and a snapshot that could not be encoded is a session
    // nobody can attach to.
    state.title = std::string(view.title);
    state.columns = static_cast<std::uint16_t>(std::max(0, status.cells.width));
    state.rows = static_cast<std::uint16_t>(std::max(0, status.cells.height));
    state.cursor = proto::CursorOp{view.cursor.column, view.cursor.row, view.cursor.style,
                                   view.cursor.visible, view.cursor.blink};
    state.modes = view.modes;
    state.grid = proto::to_runs(std::vector<ckv::Cell>(view.grid.begin(), view.grid.end()));
    // No history here: `fill_history` puts it in, once the caller knows what
    // is left of the budget after every terminal's screen (C1). A grid has to
    // go whole — it is what the terminal IS — and the history is what it
    // remembers, which may be short without being wrong.
    state.printer_state = status.printer_controller_active ? proto::PrinterState::Capturing
                                                           : proto::PrinterState::Idle;
    state.printer_bytes = static_cast<std::uint32_t>(
        std::min<std::size_t>(status.printer_pending_bytes, 0xFFFFFFFFu));
    // The clipboard WATERMARK and not the text (proto::TerminalState says why):
    // a write is a live act, so what a reattaching client is given is the
    // number that stops it replaying an old one — and stops the next real one
    // from colliding with a watermark it never reset.
    state.clipboard_serial = status.clipboard_serial;
    // The newest complaint, and nothing older. The ring is the emulator's; what
    // a view paints is its last entry, so that is what the wire carries.
    const std::span<const ckv::core::TerminalDiagnostic> complaints = source.diagnostics();
    if (!complaints.empty()) {
        state.diagnostic_kind = wire_kind(complaints.back().kind);
        state.diagnostic = std::string(clamp_utf8(complaints.back().message, proto::kMaxTitleBytes));
    }
    // The wire ids of the pictures this terminal's watchers hold — the ids the
    // restatement that follows a snapshot will arrive under, since they are
    // stable across snapshots. A mirror adopting this keeps showing the pixels
    // it already holds under these ids rather than blanking them: the field
    // shape this closes is a client healing under load, whose every picture
    // vanished at each heal and re-crossed the socket megabytes later, gray in
    // between. Ids the list does not name are pictures the believed set no
    // longer has (their Remove may have been dropped with the rest of the
    // backlog), and the mirror drops them on adoption.
    state.images.reserve(believed_rasters_.size());
    for (const BelievedRaster& entry : believed_rasters_) state.images.push_back(entry.wire_id);

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

std::size_t TerminalDiffer::fill_history(proto::TerminalState& state,
                                         const ckv::core::TerminalSubsession& source,
                                         std::size_t budget) const {
    state.scrollback.clear();
    const std::span<const ckv::Cell> history = source.scrollback();
    const std::size_t width = static_cast<std::size_t>(std::max(1, static_cast<int>(state.columns)));
    const std::size_t lines = history.size() / width;

    // Newest first, because that is the half a reader pages into: a history
    // that has to be cut is cut at its oldest end, exactly where the emulator
    // and the client's own capacity cut it. The walk stops at the first line
    // that will not fit rather than skipping it and trying the next — the
    // lines have to stay contiguous, or a client's scrollback would have a
    // hole in it that nothing on the wire could describe.
    std::size_t spent = 0;
    std::vector<std::vector<proto::CellRun>> newest_first;
    for (std::size_t index = lines; index > 0; --index) {
        const std::span<const ckv::Cell> row = history.subspan((index - 1) * width, width);
        std::vector<proto::CellRun> runs =
            proto::to_runs(std::vector<ckv::Cell>(row.begin(), row.end()));
        const std::size_t cost = proto::encoded_size(runs);
        if (spent + cost > budget) break;
        spent += cost;
        newest_first.push_back(std::move(runs));
    }

    // Reversed back to oldest-first, which is the order the wire states and
    // `TerminalMirror::adopt` pushes them in.
    std::reverse(newest_first.begin(), newest_first.end());
    state.scrollback = std::move(newest_first);
    return spent;
}

namespace {

// AddBegin, the pixel chunks, End — the whole picture on the wire. RGBA rows
// exactly as `Image` holds them (stride == width * 4, no padding), split at
// the chunk cap the codec enforces.
// What one picture costs to say, in payload bytes. Beside the builder rather
// than derived at the call sites, so the gauge cannot drift from the thing it
// is gauging.
std::size_t payload_bytes_of(const ckv::Image& image) {
    return static_cast<std::size_t>(image.stride()) * static_cast<std::size_t>(image.height());
}

void append_image_payload(std::vector<proto::Message>& ops, std::uint64_t wire_id,
                          const ckv::Image& image) {
    proto::ImageAddBegin begin;
    begin.id = wire_id;
    begin.width = static_cast<std::uint16_t>(std::max(0, image.width()));
    begin.height = static_cast<std::uint16_t>(std::max(0, image.height()));
    ops.push_back(begin);
    const std::size_t total =
        static_cast<std::size_t>(image.stride()) * static_cast<std::size_t>(image.height());
    const char* const bytes = reinterpret_cast<const char*>(image.data());
    std::uint32_t seq = 0;
    for (std::size_t offset = 0; offset < total; offset += proto::kMaxChunkPayloadBytes) {
        proto::ImageChunk chunk;
        chunk.id = wire_id;
        chunk.seq = seq++;
        chunk.bytes.assign(bytes + offset,
                           std::min<std::size_t>(proto::kMaxChunkPayloadBytes, total - offset));
        ops.push_back(std::move(chunk));
    }
    proto::ImageEnd end;
    end.id = wire_id;
    ops.push_back(end);
}

proto::ImagePlace place_of(TerminalId id, std::uint64_t wire_id, ckv::Point anchor,
                           ckv::Size cell_extent) {
    proto::ImagePlace place;
    place.term = id;
    place.id = wire_id;
    place.cells = proto::Rect{static_cast<std::int16_t>(anchor.x),
                              static_cast<std::int16_t>(anchor.y),
                              static_cast<std::uint16_t>(std::max(0, cell_extent.width)),
                              static_cast<std::uint16_t>(std::max(0, cell_extent.height))};
    return place;
}

}  // namespace

std::vector<proto::Message> TerminalDiffer::flush_images(TerminalId id,
                                                         const ckv::core::TerminalSubsession& source,
                                                         std::uint64_t& next_image_id,
                                                         const PictureReadiness& ready) {
    const std::span<const ckv::core::TerminalRaster> current = source.rasters();
    if (current.empty() && believed_rasters_.empty()) return {};

    // Match every current raster to a believed placement by pixel-object
    // identity, one to one. The pass is written to say the removes first, so
    // a client's picture memory shrinks before it grows.
    std::vector<bool> kept(believed_rasters_.size(), false);
    std::vector<std::size_t> matched(current.size(), SIZE_MAX);
    std::vector<bool> replaced(current.size(), false);
    for (std::size_t index = 0; index < current.size(); ++index) {
        if (current[index].image == nullptr) continue;
        for (std::size_t believed = 0; believed < believed_rasters_.size(); ++believed) {
            if (kept[believed]) continue;
            if (believed_rasters_[believed].image.get() == current[index].image.get()) {
                kept[believed] = true;
                matched[index] = believed;
                break;
            }
        }
    }
    // Second pass: a picture standing exactly where a believed one stands is
    // the same PLACEMENT with new pixels — an animation frame — not a
    // departure and an arrival. It keeps its wire id and its pixels travel
    // again under it; crucially, NO Remove is sent. Remove-then-Add was how
    // every animation frame put a blank where the picture stood: the new
    // pixels are megabytes and cross the socket in many reads, the Remove is
    // a few bytes and crossed first, and every client repaint between the
    // two painted the fallback into the hole — a reader watching
    // ckvision_spin in a pane saw the picture blink off at every frame
    // (field report, 2026-08-19). Under one id the mirror keeps showing the
    // old pixels until the Place that swaps them, which is the atomicity the
    // child's own synchronized-output bracket already asked for.
    for (std::size_t index = 0; index < current.size(); ++index) {
        if (matched[index] != SIZE_MAX || current[index].image == nullptr) continue;
        for (std::size_t believed = 0; believed < believed_rasters_.size(); ++believed) {
            if (kept[believed]) continue;
            const BelievedRaster& entry = believed_rasters_[believed];
            if (entry.anchor.x == current[index].anchor.x &&
                entry.anchor.y == current[index].anchor.y &&
                entry.cell_extent.width == current[index].cell_extent.width &&
                entry.cell_extent.height == current[index].cell_extent.height) {
                kept[believed] = true;
                matched[index] = believed;
                replaced[index] = true;
                break;
            }
        }
    }

    std::vector<proto::Message> ops;
    for (std::size_t believed = 0; believed < believed_rasters_.size(); ++believed) {
        if (kept[believed]) continue;
        proto::ImageRemove remove;
        remove.term = id;
        remove.id = believed_rasters_[believed].wire_id;
        ops.push_back(remove);
    }

    std::vector<BelievedRaster> next;
    next.reserve(current.size());
    for (std::size_t index = 0; index < current.size(); ++index) {
        const ckv::core::TerminalRaster& raster = current[index];
        if (raster.image == nullptr) continue;
        if (matched[index] != SIZE_MAX) {
            BelievedRaster entry = believed_rasters_[matched[index]];
            if (replaced[index]) {
                // Nobody can take this frame, so nobody pays for it. Asked
                // BEFORE the comparison below, because the comparison is the
                // expensive half: a full-screen picture is a megabyte of
                // memcmp and then, when it differs, a megabyte of copying into
                // chunk messages — per tick, per terminal, for a payload the
                // debt supersedes the instant it is queued. The belief is left
                // exactly as it was, which is what makes this safe: what the
                // clients hold has not changed, and the next tick that finds
                // somebody ready sends the pixels that are current THEN rather
                // than these.
                if (ready && !ready(id, entry.wire_id)) {
                    next.push_back(std::move(entry));
                    continue;
                }
                // A new object whose pixels EQUAL the believed ones is not a
                // change at all — it is the same picture decoded again (a
                // payload past the emulator's decode-cache cap arrives as a
                // fresh object every redraw) or re-punched into the same
                // holes — and resending megabytes to say "unchanged" is what
                // let a merely-busy child saturate the wire. The pointer is
                // adopted so the next tick's identity check is cheap again;
                // nothing travels.
                if (entry.image->width() == raster.image->width() &&
                    entry.image->height() == raster.image->height() &&
                    std::memcmp(entry.image->data(), raster.image->data(),
                                static_cast<std::size_t>(raster.image->width()) *
                                    static_cast<std::size_t>(raster.image->height()) * 4U) == 0) {
                    entry.image = raster.image;
                    next.push_back(std::move(entry));
                    continue;
                }
                // New pixels under the old id: the whole payload again, then
                // the Place that makes them current. The believed image is
                // swapped too — it is what attach_images serializes, and a
                // client attaching now must be sent the pixels the watching
                // clients will hold after this tick, not the frame before.
                entry.image = raster.image;
                append_image_payload(ops, entry.wire_id, *entry.image);
                picture_bytes_built_ += payload_bytes_of(*entry.image);
                ops.push_back(place_of(id, entry.wire_id, entry.anchor, entry.cell_extent));
                next.push_back(std::move(entry));
                continue;
            }
            const bool moved = entry.anchor.x != raster.anchor.x ||
                               entry.anchor.y != raster.anchor.y ||
                               entry.cell_extent.width != raster.cell_extent.width ||
                               entry.cell_extent.height != raster.cell_extent.height;
            if (moved) {
                entry.anchor = raster.anchor;
                entry.cell_extent = raster.cell_extent;
                ops.push_back(place_of(id, entry.wire_id, entry.anchor, entry.cell_extent));
            }
            next.push_back(std::move(entry));
            continue;
        }
        BelievedRaster entry;
        entry.image = raster.image;
        entry.wire_id = next_image_id++;
        entry.anchor = raster.anchor;
        entry.cell_extent = raster.cell_extent;
        append_image_payload(ops, entry.wire_id, *entry.image);
        picture_bytes_built_ += payload_bytes_of(*entry.image);
        ops.push_back(place_of(id, entry.wire_id, entry.anchor, entry.cell_extent));
        next.push_back(std::move(entry));
    }
    believed_rasters_ = std::move(next);
    return ops;
}

std::vector<proto::Message> TerminalDiffer::attach_images(TerminalId id) const {
    std::vector<proto::Message> ops;
    for (const BelievedRaster& entry : believed_rasters_) {
        append_image_payload(ops, entry.wire_id, *entry.image);
        ops.push_back(place_of(id, entry.wire_id, entry.anchor, entry.cell_extent));
    }
    return ops;
}

DiffEngine::TerminalTick DiffEngine::flush(TerminalId id, ckv::core::TerminalSubsession& source,
                                          const PictureReadiness& ready) {
    // The child asked to hold this until it says the frame is whole (DEC
    // 2026): an empty tick, and damage left untouched rather than cleared, so
    // the tick after the matching reset reads everything the frame changed —
    // the grid AND the pictures, both read below — and sends it as the one
    // update the child asked for. The same coalescing an ordinary quiet spell
    // already gives a child that never uses the mode at all; this only widens
    // which spells count as quiet.
    //
    // Held for as long as the child holds it, with no separate timeout: a
    // child that opens a frame and then truly stops — crashed, hung — freezes
    // this terminal's display either way, with or without one open, and a
    // child that is still alive closes every frame it opens (2026 is meaningless
    // otherwise) or reopens on its very next redraw, which reads as one
    // slightly late update rather than a stuck one.
    if (source.synchronized_output_active()) return {};
    TerminalDiffer& differ = differs_[id];
    TerminalTick tick;
    tick.delta = differ.flush(id, source);
    tick.images = differ.flush_images(id, source, next_image_id_, ready);
    // Sent: the host saying it has caught up. Here rather than inside the
    // differ, so that a terminal with several clients watching it has its
    // damage cleared once, after all of them have read — not by whichever one
    // looked first.
    source.clear_damage();
    return tick;
}

DiffEngine::Tick DiffEngine::flush(Terminals& terminals, const PictureReadiness& ready) {
    Tick tick;
    for (const TerminalId id : terminals.ids()) {
        Terminal* terminal = terminals.find(id);
        if (terminal == nullptr) continue;
        TerminalTick per_terminal = flush(id, terminal->session(), ready);
        if (per_terminal.delta.has_value()) tick.deltas.push_back(std::move(*per_terminal.delta));
        for (proto::Message& message : per_terminal.images)
            tick.images.push_back(ImageOp{id, std::move(message)});
    }
    return tick;
}

std::vector<proto::Message> DiffEngine::attach_images(TerminalId id) const {
    const TerminalDiffer* differ = differ_for(id);
    return differ == nullptr ? std::vector<proto::Message>{} : differ->attach_images(id);
}

proto::TerminalState DiffEngine::snapshot(TerminalId id,
                                          const ckv::core::TerminalSubsession& source) {
    return differs_[id].snapshot(id, source);
}

std::size_t DiffEngine::fill_history(TerminalId id, proto::TerminalState& state,
                                     const ckv::core::TerminalSubsession& source,
                                     std::size_t budget) const {
    const TerminalDiffer* const differ = differ_for(id);
    // A terminal with no differ has never been snapshotted, so there is no
    // state of the caller's for a history to belong to.
    return differ == nullptr ? 0 : differ->fill_history(state, source, budget);
}

std::size_t DiffEngine::picture_bytes_built() const noexcept {
    std::size_t built = 0;
    for (const auto& [id, differ] : differs_) {
        (void)id;
        built += differ.picture_bytes_built();
    }
    return built;
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
