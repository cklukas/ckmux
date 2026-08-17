// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// WP-4b: the grid diff algebra (WP-4a) wired to live terminals.
//
// One `TerminalDiffer` per terminal holds what its client is believed to hold
// and the sequence number that belief is at. At each flush tick it reads the
// terminal through U0-b — a damage report, borrowed cells, borrowed history,
// and the scalar `TerminalStatus` — and produces at most one `GridDelta`. What
// it never does is copy a terminal in order to look at it: the only cells that
// are copied are the cells being sent.
//
// Two properties this layer owns, both of which a reader would notice
// immediately if they broke:
//
//   * **Coalescing.** A child that wrote forty times between two ticks costs
//     one delta, because damage accumulates in the emulator and the tick is
//     what reads it. Clearing that damage IS the server saying it has sent
//     everything, which is why nothing else in ckmux may clear it.
//   * **A monotonic sequence per terminal.** Deltas for one terminal are
//     numbered 1, 2, 3 with no gaps and no repeats. A client that sees a gap
//     stops guessing and resnapshots (plans/05); on the server side, a
//     resnapshot restarts the count, so the very first delta after one is
//     always 1 and even a delta lost right after an attach is visible as a gap.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "common/grid_delta.hpp"
#include "common/proto.hpp"
#include "server/terminals.hpp"

namespace ckm::server {

// The mode bits the protocol carries, read off a terminal's scalars. One
// function, because a bit that means "mouse reporting" on one side of the
// socket and something else on the other is a defect nothing would report.
std::uint32_t modes_of(const ckv::term::TerminalStatus& status);
GridCursor cursor_of(const ckv::term::TerminalStatus& status);

// The history as lines, read out of the borrowed flat span the emulator keeps
// it in. `count` lines from the end — which is what "the lines that just
// entered the history" means, and it is clamped to what is actually still
// there: a burst larger than the terminal's capacity has already dropped its
// own oldest lines, and the client's capacity would have dropped them too.
std::vector<std::vector<ckv::Cell>> newest_history_lines(const ckv::core::TerminalSubsession& source,
                                                         int width, std::size_t count);

// The most history one delta may carry.
//
// A flooding child pushes thousands of lines per tick — `yes` against a 64 KiB
// drain budget is nearly four thousand — and a delta carrying them all is
// megabytes, past the protocol's own payload cap for a delta (plans/05: 1 MiB).
// A frame over the cap is not merely large: it is refused, and a stream whose
// framing cannot be trusted is a connection that has to end.
//
// Past this many lines the SCREEN still goes and the history does not. That is
// the trade this package had to make, and it was made against a measurement:
// answering a flood with a snapshot instead — the obvious first answer, since a
// snapshot carries the history whole — meant a **2.8 MB snapshot every tick**,
// which buried every other answer on the connection behind it and made a `Ping`
// take three seconds to come back. A reader watching a flooding terminal is
// watching the screen; the history they will page into afterwards is reconciled
// with one snapshot when the flood stops.
//
// Two hundred and fifty six is several screens' worth: an ordinary program
// scrolling briskly stays well under it, and what goes over is a flood.
inline constexpr std::size_t kMaxHistoryLinesPerDelta = 256;

class TerminalDiffer {
public:
    // The belief starts empty at a size of nothing, so the first flush of a
    // terminal is a full repaint. A differ that assumed a blank screen of the
    // right size would be assuming the client had one.
    TerminalDiffer() = default;

    // The delta for this tick, or nothing at all when there is nothing to say.
    //
    // A pure read: it does NOT clear the terminal's damage. Clearing is the
    // engine's, once per tick, after every differ has read — because a terminal
    // may have more than one client watching it (plans/03's takeover, and any
    // second attach), and a differ that cleared what it had read would leave
    // the next one told nothing about those rows.
    //
    // Takes the ckVision seam rather than ckmux's `Terminal`, because nothing
    // here needs a process: what it reads is a damage report, borrowed cells,
    // borrowed history and the scalars. That is also what lets the cost be
    // measured against a bare emulator, with no child's scheduling in the
    // number and no PTY in the way.
    std::optional<proto::GridDelta> flush(TerminalId id, const ckv::core::TerminalSubsession& source);

    // Everything a client attaching now has to be told about this terminal,
    // and the reset of the sequence that goes with it: the deltas that follow
    // start again at 1, so a client's continuity check works from its first
    // delta rather than from its second.
    proto::TerminalState snapshot(TerminalId id, const ckv::core::TerminalSubsession& source);

    std::uint32_t sequence() const noexcept { return sequence_; }
    // Whether the last flush gave up on saying it incrementally. The server
    // reads this and has the client resnapshotted; taking a snapshot clears it.
    bool needs_snapshot() const noexcept { return needs_snapshot_; }
    // Whether a flood has been outrunning what a delta can carry, so that the
    // client's history is behind while its screen is not.
    bool history_diverged() const noexcept { return history_diverged_; }
    // What the client is believed to hold. Public because the tests compare it
    // against a mirror built only from the deltas — which is the one check that
    // says the belief is not a story the server tells itself.
    const GridState& believed() const noexcept { return believed_; }

private:
    // The server's belief carries no history: it never diffs against one (the
    // lines that entered it come from the damage report), and holding a copy
    // per terminal would be the memory U0-b was about. The client keeps the
    // history, because the client is what a reader pages through.
    GridState believed_;
    std::uint32_t sequence_ = 0;
    bool needs_snapshot_ = false;
    bool history_diverged_ = false;
};

// Every terminal the server owns, diffed at the flush tick.
class DiffEngine {
public:
    // One tick. Returns a delta per terminal that had news, in terminal-id
    // order so the sequence a client sees is the order the server produced.
    std::vector<proto::GridDelta> flush(Terminals& terminals);

    // One terminal, for a caller holding the seam directly rather than a
    // collection — and the reason this exists at all: the differ's `flush` is a
    // pure read, so somebody has to say "sent". Making that somebody the engine
    // in both overloads means no caller has to remember, which is a footgun
    // this file walked into on its first day (a differ read in a loop with
    // nothing clearing damage re-copied the whole history every tick, and the
    // measurement was forty times slower than it should have been).
    std::optional<proto::GridDelta> flush(TerminalId id, ckv::core::TerminalSubsession& source);

    // The attach path, per terminal.
    proto::TerminalState snapshot(TerminalId id, const ckv::core::TerminalSubsession& source);

    // A terminal that has closed. Its id is never reused (WP-3), so forgetting
    // it cannot be confused with a new terminal arriving.
    void forget(TerminalId id);

    std::uint32_t sequence_for(TerminalId id) const;
    const TerminalDiffer* differ_for(TerminalId id) const;

private:
    // Ordered, so a tick's deltas come out in a defined order rather than
    // whatever a hash happened to do. A client cannot tell, but a test can,
    // and a test that cannot tell is a test that cannot check the sequence.
    std::map<TerminalId, TerminalDiffer> differs_;
};

}  // namespace ckm::server
