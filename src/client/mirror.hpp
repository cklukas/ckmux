// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
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
// The gap rule lives here too (the protocol spec). A delta whose sequence does not follow
// the last one means something was lost, and the answer is not to guess: the
// mirror says so, and the client resnapshots (WP-6 owns the reconnect).
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
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
    //
    // False means the snapshot was refused and the mirror is untouched: its
    // geometry is past what the protocol will carry, which is a peer trying to
    // size an allocation here rather than a terminal anybody has. The mirror's
    // own answer is the protocol's — `needs_snapshot()` goes true and another
    // is asked for — and a caller that owns the connection may end it instead.
    bool adopt(const proto::TerminalState& state);

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
    // The child put text on the clipboard (OSC 52), and the server let it.
    //
    // The serial is bumped rather than carried: what a consumer does with a
    // clipboard write is compare the serial against the one it last acted on
    // (ckVision's `TerminalView` does exactly that), so what has to be true is
    // that the number MOVES and never goes backwards. The snapshot restores the
    // server's own watermark, and every write after it advances by one from
    // there — so a write that arrives after a reattach can never land on a
    // number a watcher has already seen, which is how one would be dropped in
    // silence.
    void apply(const proto::ClipboardSet& clipboard);
    // The printer went on or off, or its byte count moved. Not part of the
    // grid — while the controller is on, the child's output is going to the
    // printer and NOT to the screen, which is the fact a reader has to be shown
    // rather than left watching a terminal that has apparently stopped.
    void apply(const proto::PrintState& printing);
    // The terminal's newest complaint. One entry, replacing whatever was there:
    // the emulator keeps the ring, and what a view paints is the last of it.
    void apply(const proto::TermDiagnostic& said);

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
    // Whether the terminal is being kept although its child has ended — the
    // window with a banner over the last screen the program drew (the session model
    // on-exit). Read by a test rather than by the view, which asks the seam
    // whether the terminal has exited and draws the banner from that; what this
    // says is that the SERVER means to keep it.
    bool held() const noexcept { return hold_; }
    // The name the READER gave this terminal, or empty for none — as the
    // server last stated it, which is the only authority on it: a custom title
    // is session state (the session model), so this mirror holds it exactly as it holds
    // the grid, and never decides it.
    //
    // Beside `title()` rather than folded into it, because they are two facts
    // and both keep changing. The child goes on renaming itself underneath an
    // override — that is what makes it an override — and the window layer is
    // where the two are resolved into one caption.
    const std::string& custom_title() const noexcept { return custom_title_; }
    // What a reader who is not in this terminal has missed (the protocol spec's
    // `TermMeta` flags), as the server last stated it.
    bool bell_marked() const noexcept { return bell_marked_; }
    bool activity_marked() const noexcept { return activity_marked_; }
    // How many times this terminal has rung, and how many times it has
    // written, counted by the server from the moment it opened and never
    // reset (WP-41). The LEVEL above says a mark is up and the server decides
    // when to put it down — which cannot be right for two readers at once,
    // because "a terminal you are not in" is a sentence about one reader, and
    // cannot say "rang AGAIN" even to one, because a bit that is already set
    // cannot be set harder. A serial says both, so a client that remembers the
    // number it last answered can tell a second bell from the first.
    std::uint32_t bell_serial() const noexcept { return bell_serial_; }
    std::uint32_t activity_serial() const noexcept { return activity_serial_; }
    // The clipboard, as the child last asked for it. The serial is what a
    // consumer compares; the text is what it forwards when the serial moved. A
    // mirror that has just adopted a snapshot holds the server's serial and NO
    // text, which is the pair that makes a reattach forward nothing.
    std::uint64_t clipboard_serial() const noexcept { return clipboard_serial_; }
    const std::string& clipboard_text() const noexcept { return clipboard_text_; }
    // The printer, as the snapshot or the last `PrintState` said.
    bool printer_active() const noexcept { return printer_active_; }
    std::size_t printer_bytes() const noexcept { return printer_bytes_; }
    std::size_t printer_jobs() const noexcept { return printer_jobs_; }
    // Which of the four things the frame button has to be able to say is true
    // (the interface spec): idle, capturing, sunk after an over-limit job, or holding a
    // full spool. A count and a bool cannot spell four states, and the two
    // that matter most to a reader — "a document is being kept" and "a
    // document is being thrown away" — are the two a bool cannot tell apart.
    proto::PrinterState printer_state() const noexcept { return printer_state_; }
    proto::PrinterMode printer_mode() const noexcept { return printer_mode_; }
    // Which scope the mode in force came from, so Printer Settings can say
    // "(from: session)" without asking the server a second question.
    proto::PrinterScope printer_scope() const noexcept { return printer_scope_; }
    // The jobs this terminal is holding, newest last, as metadata. The text is
    // not here and never arrives unasked: a reader who never opens the preview
    // should not have paid to ship a megabyte they did not look at.
    std::span<const proto::PrintJobInfo> print_jobs() const noexcept { return print_jobs_; }
    // One job's text, once a `PrintJobData` run has completed for it. Empty
    // for a job never fetched and for one that overflowed — those are
    // different facts, and `PrintJobInfo::bytes` is what tells them apart.
    const std::string* print_job_text(std::uint64_t job) const;
    // Feeds one chunk in. Answers whether THIS chunk completed a job, so a
    // caller knows the moment a preview has something whole to show rather
    // than polling for it.
    bool apply(const proto::PrintJobData& chunk);
    void apply(const proto::PrintJobAdded& added);
    // Forgets one job, or every one with id 0 — the same rule the wire's
    // discard follows.
    void forget_print_jobs(std::uint64_t job);
    // The terminal's newest complaint, or nothing. At most one entry: the ring
    // is the emulator's, and a client is given its last line.
    std::span<const ckv::core::TerminalDiagnostic> diagnostics() const noexcept {
        return diagnostics_;
    }
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
    // At most one entry, so that `diagnostics()` can hand out the seam's own
    // span type without the mirror keeping a ring the wire never sends.
    std::vector<ckv::core::TerminalDiagnostic> diagnostics_;
    ckv::core::TerminalDamage damage_;
    std::uint32_t sequence_ = 0;
    std::uint64_t gaps_ = 0;
    std::uint64_t clipboard_serial_ = 0;
    std::string clipboard_text_;
    std::size_t printer_bytes_ = 0;
    std::size_t printer_jobs_ = 0;
    bool printer_active_ = false;
    proto::PrinterState printer_state_ = proto::PrinterState::Idle;
    proto::PrinterMode printer_mode_ = proto::PrinterMode::Ask;
    proto::PrinterScope printer_scope_ = proto::PrinterScope::Global;
    std::vector<proto::PrintJobInfo> print_jobs_;
    // Chunks arriving for a job whose run is not finished. Keyed by job so two
    // previews opened at once cannot interleave into one another's text — the
    // wire allows it and a single buffer would silently splice them.
    std::unordered_map<std::uint64_t, std::string> fetching_;
    std::unordered_map<std::uint64_t, std::string> fetched_;
    bool needs_snapshot_ = false;
    bool exited_ = false;
    bool hold_ = false;
    std::string custom_title_;
    bool bell_marked_ = false;
    bool activity_marked_ = false;
    std::uint32_t bell_serial_ = 0;
    std::uint32_t activity_serial_ = 0;
    std::optional<int> exit_status_;
};

}  // namespace ckm::client
