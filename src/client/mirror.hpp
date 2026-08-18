// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// The client's mirror of one terminal (WP-5).
//
// Everything a reader sees from M2 onwards is this: the server owns the program
// and the emulator, and what arrives here is a snapshot at attach time and a
// stream of deltas afterwards. So the mirror's only real claim is the one WP-4a
// pinned — apply what arrives and hold exactly what the server holds — plus the
// two things a client needs that a server does not: a **history it can page
// through locally**, and a **damage report of its own**, so a view can repaint
// the rows that changed rather than the screen.
//
// The gap rule lives here too (plans/05). A delta whose sequence does not follow
// the last one means something was lost, and the answer is not to guess: the
// mirror says so, and the client resnapshots (WP-6 owns the reconnect).
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/grid_delta.hpp"
#include "common/proto.hpp"
#include "cvision/core/image.hpp"
#include "cvision/core/terminal_subsession.hpp"

namespace ckm::client {

class TerminalMirror {
public:
    // The size the mirror starts at, before a snapshot has said otherwise. Not
    // zero: a view that measures a terminal before the first message arrives
    // should find a plausible screen rather than nothing at all.
    TerminalMirror() = default;

    // Everything the server said about this terminal at attach time. Replaces
    // the whole screen — a snapshot is the server's answer to "we have lost
    // track", so believing any of the old picture would defeat it — and
    // restarts the sequence, which is what makes the next delta's `seq == 1`
    // meaningful.
    //
    // What it does NOT replace is whether the child is still running: the
    // snapshot has no field for that, so a client's only source is the
    // `TermClosed` it was already sent, and a reattach that forgot it showed a
    // dead shell as a live terminal (mirror.cpp says the rest). `open()` is
    // the seam that clears it, for a terminal that really is a new one.
    void adopt(const proto::TerminalState& state);

    // Applies one delta. Returns false when it was refused, which happens for
    // exactly two reasons: the sequence did not follow (something was lost), or
    // the ops do not fit the mirror (the server and the client disagree about
    // the size). Both are answered the same way and by the same code as the
    // protocol says — resnapshot — so both set `needs_snapshot()`.
    bool apply(const proto::GridDelta& delta);

    // A terminal the server has just announced. `TermOpened` says how big it is
    // but not what is on it, so the mirror starts blank at that size — and the
    // size is the part that matters: the first delta for a new terminal is a
    // full repaint, and a repaint cannot be applied to a grid of no size. A
    // mirror left at nothing would refuse it, ask for a snapshot, and only then
    // catch up, which is a round trip for something already known.
    void open(ckv::Size cells);

    void apply(const proto::TermMeta& meta);
    void apply(const proto::TermClosed& closed);

    // Whether the mirror knows it is out of date. Cleared by `adopt`, because a
    // snapshot is the cure.
    bool needs_snapshot() const noexcept { return needs_snapshot_; }
    std::uint32_t sequence() const noexcept { return sequence_; }
    // How many deltas were refused for a gap, ever. A counter rather than a
    // flag: it is the number a bug report needs, and "it happened once during
    // startup" and "it happens every second" are different faults.
    std::uint64_t gaps() const noexcept { return gaps_; }

    const GridState& state() const noexcept { return state_; }
    const std::string& title() const noexcept { return state_.title; }
    ckv::Size cells() const noexcept { return state_.cells; }
    std::optional<int> exit_status() const noexcept { return exit_status_; }
    bool exited() const noexcept { return exited_; }
    // What the reader's `[general] scrollback` says this mirror keeps. Set once,
    // by the client, from configuration: the server's own capacity is its
    // business, and a client that kept more than it was configured to would be
    // holding text the reader asked it to forget.
    void set_history_limit(std::size_t lines);

    // The grid and the history, as a view reads them: borrowed, never copied.
    // The history is flat — rows of `cells().width`, oldest first — because that
    // is the shape ckVision's seam hands out and the shape a view indexes.
    std::span<const ckv::Cell> grid() const noexcept;
    std::span<const ckv::Cell> history() const noexcept;

    // The pictures (WP-16), placed by wire id. `place_image` with pixels adds
    // or restates the placement for `id`; with `image == nullptr` it moves a
    // placement already held — the server sends a bare Place for a move — and
    // a move for a picture this mirror never received is ignored, because the
    // snapshot that heals any gap restates the pictures too.
    void place_image(std::uint64_t id, std::shared_ptr<const ckv::Image> image,
                     ckv::Point anchor, ckv::Size cell_extent);
    void remove_image(std::uint64_t id);
    std::span<const ckv::core::TerminalRaster> rasters() const noexcept { return rasters_; }
    // The scene identity every raster of this terminal carries. TerminalView
    // drops rasters whose id is 0 — an unadopted mirror would show no picture
    // at all — and the compositor keys placements by it, so the client mints
    // one per remote terminal (ServerSession::ensure_terminal).
    void set_raster_identity(int identity) noexcept;

    // What changed since the view last said it had caught up. The same contract
    // as the emulator's: reading never clears, `clear_damage()` is the reader's,
    // and it starts full so the first frame after an attach draws everything.
    const ckv::core::TerminalDamage& damage() const noexcept { return damage_; }
    void clear_damage() noexcept;

private:
    void note_row(int row, int first, int last);
    void note_every_row();

    // The history lives flat inside `state_` (GridState's own shape), so
    // `history()` is a borrow of the live region and nothing more. It was a
    // cache once, rebuilt whole whenever a line scrolled away — O(history)
    // per push, which a flooding child turns into seconds of flattening per
    // batch and a reader experiences as a client that stopped answering.
    GridState state_;
    // The placed pictures and, parallel to them, the wire id each arrived
    // under. Two vectors rather than a struct because the raster vector is
    // exactly what `rasters()` hands the view — the seam's own type, borrowed,
    // never copied.
    std::vector<ckv::core::TerminalRaster> rasters_;
    std::vector<std::uint64_t> raster_wire_ids_;
    int raster_identity_ = 0;
    ckv::core::TerminalDamage damage_;
    std::uint32_t sequence_ = 0;
    std::uint64_t gaps_ = 0;
    bool needs_snapshot_ = false;
    bool exited_ = false;
    bool hold_ = false;
    std::optional<int> exit_status_;
};

}  // namespace ckm::client
