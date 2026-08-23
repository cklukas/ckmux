// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
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
//     stops guessing and resnapshots (the protocol spec); on the server side, a
//     resnapshot restarts the count, so the very first delta after one is
//     always 1 and even a delta lost right after an attach is visible as a gap.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "common/grid_delta.hpp"
#include "common/proto.hpp"
#include "cvision/core/image.hpp"
#include "server/terminals.hpp"

namespace ckm::server {

// The mode bits the protocol carries, read off a terminal's scalars. One
// function, because a bit that means "mouse reporting" on one side of the
// socket and something else on the other is a defect nothing would report.
std::uint32_t modes_of(const ckv::term::TerminalStatus& status);

// Whether anybody is in a position to TAKE a fresh payload for this picture
// right now. Asked once per redrawn picture per tick, before the pixels are
// compared or copied.
//
// The differ knows nothing about clients and should not: what it is being told
// here is not who is watching but whether the work is worth doing. A client
// that already owes an unsent payload for this wire id has not yet been sent
// the frame BEFORE this one, and the debt supersedes on arrival — so a payload
// built for it now is compared, copied, queued and dropped, several megabytes
// at a time, at tick rate. An empty function means "no opinion", which is
// every caller that has no clients to consult, and behaves exactly as this did
// before the question existed.
using PictureReadiness = std::function<bool(TerminalId term, std::uint64_t wire_id)>;
GridCursor cursor_of(const ckv::term::TerminalStatus& status);

// Child-supplied text cut to a length the wire can say, on a character
// boundary.
//
// A child writes its title with OSC 2 and may write a megabyte of it. A `str`
// field's length is two bytes, so past 65535 the codec refuses the whole
// message — a delta or a snapshot lost to a program's choice of caption. Cut
// here instead, at the producer, far below the field's own limit: the cost is a
// long title shown short, which is what a reader would see anyway.
//
// The back-off is to a leading byte (`(b & 0xC0) != 0x80`), because a cut
// through the middle of a UTF-8 sequence produces bytes no decoder can read as
// text — and the string_view makes this a substring rather than a copy.
//
// Exposed because two producers now state a title — a delta's `Title` op and
// the `TermMeta` that carries a terminal's marks — and two clamps a byte apart
// would make a mirror flip between two captions, one per tick.
std::string_view clamp_utf8(std::string_view text, std::size_t limit);

// A diagnostic's kind, in the wire's own enumeration. One conversion, stated
// once: the protocol writes its values down rather than following the
// library's, so this is the single place the two are matched up.
proto::DiagnosticKind wire_kind(ckv::core::TerminalDiagnostic::Kind kind);

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
// megabytes, past the protocol's own payload cap for a delta (the protocol spec: 1 MiB).
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
    // may have more than one client watching it (the session model's takeover, and any
    // second attach), and a differ that cleared what it had read would leave
    // the next one told nothing about those rows.
    //
    // Takes the ckVision seam rather than ckmux's `Terminal`, because nothing
    // here needs a process: what it reads is a damage report, borrowed cells,
    // borrowed history and the scalars. That is also what lets the cost be
    // measured against a bare emulator, with no child's scheduling in the
    // number and no PTY in the way.
    std::optional<proto::GridDelta> flush(TerminalId id, const ckv::core::TerminalSubsession& source);

    // The picture ops for this tick (WP-16): what has to be said so a mirror's
    // rasters match the terminal's. Same reading discipline as flush() — a pure
    // read, damage cleared by the engine afterwards — and the same belief
    // model: what the clients hold is what this differ last emitted.
    //
    // Change detection is object identity, and it is exact BECAUSE the belief
    // retains each placed image's shared_ptr: the emulator copies-on-write into
    // any image somebody else still holds (terminal_emulator.cpp,
    // damage_rasters), so pixels that changed always arrive as a new object,
    // and an unchanged pointer really is unchanged pixels. Wire ids are minted
    // per PLACEMENT from the engine-wide counter — the proto's Add/Place/Remove
    // carry no placement id of their own, so one id must mean one placed
    // picture. The same image placed twice therefore ships its pixels twice;
    // the emulator's decode cache makes that rare, and ambiguity would be
    // worse than repetition.
    //
    // A new object standing exactly where a believed one stands is that same
    // placement REDRAWN — an animation frame — and keeps its wire id: the new
    // pixels travel under the old id and no Remove is sent, so a mirror shows
    // the previous frame until the Place that swaps in the next one. The
    // Remove-then-Add alternative blanked the picture for as long as the new
    // frame's megabytes were still crossing the socket, once per frame, which
    // a reader saw as flicker.
    //
    // `ready` is asked before a redrawn picture's pixels are examined, and a
    // no makes this leave the picture ALONE: no comparison, no payload, and —
    // the part that has to be got right — no adoption of the new pixels into
    // the belief either. The belief is what the clients are believed to hold,
    // so adopting pixels that were never sent would be the differ telling
    // itself a story: it would never emit that frame, and a child that then
    // stopped drawing would leave the reader on the frame before it forever.
    // Left unadopted, the next tick asks again, and emits the moment somebody
    // can take it.
    std::vector<proto::Message> flush_images(TerminalId id,
                                             const ckv::core::TerminalSubsession& source,
                                             std::uint64_t& next_image_id,
                                             const PictureReadiness& ready = {});

    // How many bytes of picture payload this differ has copied into messages
    // at the flush tick. The gauge for work AVOIDED — the only way to see that
    // a payload is no longer being built is to count the ones that are — and
    // the reason it counts bytes rather than calls: what was expensive was
    // never the call.
    std::size_t picture_bytes_built() const noexcept { return picture_bytes_built_; }

    // The pictures a client attaching NOW must be given: the believed set,
    // exactly as the clients already watching hold it — not the terminal's
    // current set, which everyone learns together at the next tick. Emitted
    // per attach; ids stay stable across snapshots, so a second client's
    // attach re-sends ids the first already holds, which the mirror treats as
    // the restatement it is.
    std::vector<proto::Message> attach_images(TerminalId id) const;

    // Everything a client attaching now has to be told about this terminal,
    // EXCEPT its history, and the reset of the sequence that goes with it: the
    // deltas that follow start again at 1, so a client's continuity check
    // works from its first delta rather than from its second.
    proto::TerminalState snapshot(TerminalId id, const ckv::core::TerminalSubsession& source);

    // Fills `state.scrollback` with the newest history lines that fit `budget`
    // bytes of payload, oldest of those first, and returns what it spent.
    //
    // Separate from `snapshot` because the budget is not known until every
    // terminal's mandatory part has been measured — and because the history is
    // the one part of a snapshot that may be SHORT without the snapshot being
    // WRONG. The screen is what a terminal is; the history is what it
    // remembers. A client given less of it than the server holds can page back
    // less far; a client given a frame past the codec's cap gets no screen at
    // all, which is what this exists to prevent (C1).
    std::size_t fill_history(proto::TerminalState& state,
                             const ckv::core::TerminalSubsession& source,
                             std::size_t budget) const;

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
    // One placed picture as the clients are believed to hold it. The retained
    // shared_ptr is load-bearing twice over: it is the change detector (see
    // flush_images), and it is what attach_images serializes — which is the
    // pixels the watching clients were sent, not whatever the emulator has
    // erased into since.
    struct BelievedRaster {
        std::shared_ptr<const ckv::Image> image;
        std::uint64_t wire_id = 0;
        ckv::Point anchor;
        ckv::Size cell_extent;
    };

    // The server's belief carries no history: it never diffs against one (the
    // lines that entered it come from the damage report), and holding a copy
    // per terminal would be the memory U0-b was about. The client keeps the
    // history, because the client is what a reader pages through.
    GridState believed_;
    std::vector<BelievedRaster> believed_rasters_;
    std::size_t picture_bytes_built_ = 0;
    std::uint32_t sequence_ = 0;
    bool needs_snapshot_ = false;
    bool history_diverged_ = false;
};

// Every terminal the server owns, diffed at the flush tick.
class DiffEngine {
public:
    // A picture op with the terminal it belongs to. Add/Chunk/End name only a
    // global image id, so the terminal has to travel beside them for the
    // server's session filter to know whose client may see the pixels.
    struct ImageOp {
        TerminalId term = 0;
        proto::Message message;
    };
    // One terminal's tick: the grid delta, if the grid had news, and the
    // picture ops, if the pictures did. Either can be empty; both read the
    // same damage, which the engine clears once after both have read.
    struct TerminalTick {
        std::optional<proto::GridDelta> delta;
        std::vector<proto::Message> images;
    };
    // One tick over every terminal.
    struct Tick {
        std::vector<proto::GridDelta> deltas;
        std::vector<ImageOp> images;
    };

    // One tick. Deltas and image ops per terminal that had news, in
    // terminal-id order so the sequence a client sees is the order the server
    // produced.
    Tick flush(Terminals& terminals, const PictureReadiness& ready = {});

    // One terminal, for a caller holding the seam directly rather than a
    // collection — and the reason this exists at all: the differ's `flush` is a
    // pure read, so somebody has to say "sent". Making that somebody the engine
    // in both overloads means no caller has to remember, which is a footgun
    // this file walked into on its first day (a differ read in a loop with
    // nothing clearing damage re-copied the whole history every tick, and the
    // measurement was forty times slower than it should have been).
    TerminalTick flush(TerminalId id, ckv::core::TerminalSubsession& source,
                       const PictureReadiness& ready = {});

    // The attach path, per terminal — the screen, and then as much of the
    // history as the caller says it can afford. Two calls because the second
    // number is not known until the first has been made for every terminal in
    // the session (`TerminalDiffer::fill_history`).
    proto::TerminalState snapshot(TerminalId id, const ckv::core::TerminalSubsession& source);
    std::size_t fill_history(TerminalId id, proto::TerminalState& state,
                             const ckv::core::TerminalSubsession& source, std::size_t budget) const;
    // The believed pictures for the same path, sent right after the snapshot.
    std::vector<proto::Message> attach_images(TerminalId id) const;

    // A terminal that has closed. Its id is never reused (WP-3), so forgetting
    // it cannot be confused with a new terminal arriving.
    void forget(TerminalId id);

    // Every differ's payload building, added up. What a test watches to see
    // that frames nobody could take were never built.
    std::size_t picture_bytes_built() const noexcept;

    std::uint32_t sequence_for(TerminalId id) const;
    const TerminalDiffer* differ_for(TerminalId id) const;

private:
    // Ordered, so a tick's deltas come out in a defined order rather than
    // whatever a hash happened to do. A client cannot tell, but a test can,
    // and a test that cannot tell is a test that cannot check the sequence.
    std::map<TerminalId, TerminalDiffer> differs_;
    // Engine-wide, never reused, never zero: a wire image id names one placed
    // picture for the whole connection's life, whichever terminal placed it.
    std::uint64_t next_image_id_ = 1;
};

}  // namespace ckm::server
