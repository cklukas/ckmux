# ckmux — Client⇄Server Protocol

Plan 05 · Status: draft · 2026-08-18 · [Index](README.md)

Local Unix-socket protocol between the ckVision client (and CLI tools) and
the server. Design goals: dumb client, authoritative server; self-healing
(any desync → fresh snapshot); zero dependencies (hand-rolled framing, no
serialization library); fuzzable decoders on both sides.

## Transport & framing

`SOCK_STREAM` Unix socket ([02-architecture.md](02-architecture.md) for path,
perms, peer checks). Every message:

```
u32 payload_len   (LE; excludes this 8-byte header)
u16 type
u16 flags         (reserved, 0)
payload           (type-specific, fixed fields + length-prefixed var fields)
```

- Integers LE; `str` = u16 len + UTF-8 bytes; `blob` = u32 len + bytes.
- Hard caps enforced **on decode**: `payload_len ≤ 1 MiB` for everything except
  image chunks (≤ 256 KiB payload by construction) and snapshots (≤ 16 MiB).
  Violation = protocol error. A desync the mirror itself notices is healed by
  re-sending `Attach` — the universal recovery below — but a frame whose
  *framing* is wrong is fatal for that connection on both sides: there is no
  way to resynchronise a stream of unknown provenance, so the server drops that
  client and the client prints one line and exits (WP-2, `run_client.cpp`).
- **The 16 MiB is the decoder's refusal; the sender holds itself to 4 MiB.**
  They are different numbers on purpose: a decoder must take whatever a
  legitimate build of this program produced, while a sender must leave itself
  room to be wrong. `payload_cap()` used to be consulted in `decode()` and
  nowhere else while `TerminalDiffer::snapshot` walked the whole history, so a
  default-config attach encoded a frame the far end had to refuse — measured
  at **16.29 MiB** for 100 columns × 10 000 lines, after which the client
  exited with "the server sent something this build cannot read" and that
  session could never be attached to again (C1 in
  [13-architecture-review.md](13-architecture-review.md)). Now `encode()`
  refuses to produce a frame past its type's cap at all, and the server builds
  a snapshot inside `kSnapshotPayloadBudget` — see Snapshot, below.
- Type ranges: `0x00xx` control, `0x01xx` session ops, `0x02xx` terminal
  I/O, `0x03xx` state sync, `0x04xx` events/errors.

## Handshake & versioning

```
client → Hello    { proto_version u32, build str, client_kind u8 (ui|cli) }
server → HelloAck { proto_version u32, build str }   or   Refuse { reason str }
```

Pre-1.0 there is **one** `proto_version` integer, bumped on *any* wire
change; mismatch → `Refuse` with both versions and the remedy in the text
("restart the server: ckmux kill-server — 2 sessions would be lost: …" vs.
"upgrade the client"). Same-binary dispatch makes mismatch rare
([02](02-architecture.md)) but upgrades leave old servers running — this
message will be seen, so it is designed, not an afterthought.

## Message catalog

**The `Today` column is the catalogue's liveness, and it is evidence rather
than intent.** A message that encodes and decodes is not a message the program
sends: WP-1 built the whole catalogue at once on purpose, so for most of these
rows the codec is the only thing that exists, and a plan listing them
undifferentiated reads as a working feature list. Each value below was derived
by grepping `src/` for that type outside `src/common/proto.*` — a construction
site is a producer, a `get_if`/`holds_alternative` arm that acts on it is a
handler — and re-derived on the date in this file's header (m-docs in
[13-architecture-review.md](13-architecture-review.md)).

| Value | Meaning |
|---|---|
| **live** | one end constructs it and the other acts on it, in shipping code |
| **encode-only** | codec only: no producer, no handler. **17 of the 48 types** are here |
| **handler only** | one end acts on it; nothing sends it |
| **producer only** | one end sends it; nothing acts on it |
| **tests only** | producer and handler exist, but the only asker is a test |

### Client → server

| Type | Payload | Today | Notes |
|---|---|---|---|
| `Hello` | above | **live** | first message, mandatory |
| `ListSessions` | — | **live** | picker + `ckmux ls` |
| `NewSession` | `name str?, spawn_first u8, cmd str?` | **live** | empty name means the server names it (`session-N`); answered with `SessionList` to every greeted client — creating a session stales every picker |
| `Attach` | `session u64, cols u16, rows u16, pixel_w u16, pixel_h u16, host_sixel u8` | **live** | session 0 = "the default one": the session already there, or a new one on a server that has none — a cold `ckmux` start is one round trip. **Always granted — latest client wins**: any prior client is detached first (`Detached{takeover}`, best-effort bounded flush; an unresponsive old client never delays the new attach). Pixel size feeds `TIOCSWINSZ` pixel fields & XTWINOPS 14/16 replies; `host_sixel` is what the client's OUTER terminal advertised, which is what a child's own advertisement must reflect (WP-16) |
| `Detach` | — | **live** | also implicit on socket close |
| `ClientResize` | `cols, rows, pixel_w, pixel_h` | *handler only* | the server applies it; no client sends one. A host terminal resized while attached therefore reaches the server only at the next `Attach` — the desktop size travels once, at attach time (`server_session.cpp`) |
| `NewTerminal` | `session u64?, cmd str?, geom rect?` | **live** | a zero-size rect means "place it yourself"; a non-zero one becomes the terminal's columns and rows |
| `CloseTerminal` | `term u64, force u8, grace_seconds u16` | **live** | the program is ASKED first (SIGHUP→SIGTERM through ckVision's `request_termination()`, PTY still draining); the survivors are killed at the deadline only if `force`. `force = 0` means the program keeps running and its terminal with it |
| `KillTerminal` | `term u64` | *encode-only* | SIGKILL outright; `CloseTerminal{force=1}` covers the reader-facing case today |
| `RespawnTerminal` | `term u64` | *encode-only* | restart exited terminal in-window — WP-13 |
| `MoveTerminal` | `term u64, dest_session u64, to_new_session u8` | **live** | the flag outranks the id, because `Attach` already gave 0 a different meaning; a close pending against the terminal is cancelled, since a move is the reader deciding to keep it (WP-10) |
| `MoveResize` | `term u64, rect` | **live** | one TERMINAL's own size, sent by its mirror when its view is laid out |
| `Raise` / `FocusTerm` | `term u64` | *encode-only* | z-order / focus; the client draws its own z-order, and the server's `Session::focused` is set where a terminal is opened and never moved after, so the snapshot's `focused_term` is "the last one created" rather than "the one the reader is in" |
| `ZoomTerm` | `term u64, on u8` | *encode-only* | maximize/restore |
| `RenameSession` | `id u64, name str` | **live** | |
| `RenameTerminal` | `id u64, name str` | *encode-only* | M3 |
| `KillSession` | `session u64, force u8, grace_seconds u16` | **live** | ask every child first (`request_termination()`); kill the survivors at the deadline only if `force` |
| `KillServer` | — | **live** | `ckmux kill-server`, over its own connect path so running it twice does not start a server in order to kill one |
| `Input` | `term u64, blob` | **live** | already-encoded child bytes — keys/mouse/focus are encoded **client-side** by ckVision's `TerminalView` against mirrored mode state ([04](04-terminal-emulation.md)); the server just writes to the PTY (queued) |
| `PasteChunk` | `term u64, seq u32, final u8, blob` | *encode-only* | ≤ 64 KiB chunks, credit-paced — WP-18 |
| `SetPrinterPolicy` | `scope u8 (terminal\|session\|global), target u64, mode u8 (ask\|capture\|off), ask_cache u32, spool_limit u32` | *encode-only* | WP-PRINT ([04](04-terminal-emulation.md)); the server owns the emulator, so it owns the policy. `global` also asks the server to persist it ([08](08-configuration.md)) |
| `PrintJobFetch` | `term u64, job u64` | *encode-only* | payload is pulled only when the user previews or saves |
| `PrintJobDiscard` | `term u64, job u64 (0 = all for this terminal)` | *encode-only* | |
| `Ping` | `nonce u64` | *tests only* | the server answers every one; nothing in `src/` asks. It exists to be the latency probe under a flood ([09](09-testing.md)), which is a thing only a test needs |

### Server → client

| Type | Payload | Today | Notes |
|---|---|---|---|
| `HelloAck` / `Refuse` | above | **live** | |
| `SessionList` | `n × { id, name, terminals u16, attached u8, created i64, last_attached i64 }` | **live** | sent to every greeted client whenever the set changes, which is why there is no separate refresh hint |
| `Attached` | `session u64` + **Snapshot** | **live** | full state; also sent for self-healing |
| `Detached` | `reason u8 (user\|takeover\|session_killed\|server_shutdown), text str` | **live** | |
| `LayoutDelta` | `session u64, n × { term u64, rect, z u16, zoomed u8 }, focused u64, desktop cols/rows` | *encode-only* | no producer, and the snapshot's matching `rect`/`z_order`/`zoomed` fields are written by nobody and read by nobody — a reattach cascades default windows. M-R3 → R8 in [13](13-architecture-review.md) |
| `TermOpened` | `term u64, session u64, index u16, rect, title str, cols, rows` | **live** | `rect` and `title` are left at their defaults today; the client places and names the window itself |
| `TermClosed` | `term u64, exited u8, exit_status i32, hold u8` | **live** | `exited = 0` is a leaving, not a death — that is how a moved terminal's old watcher is told. `hold` is read by the mirror and never set by the server (WP-13), and a child that exits on its own produces no `TermClosed` at all (m-exit-msg → R2) |
| `TermMeta` | `term u64, index u16, title str, flags u8 (bell, activity)` | *handler only* | the client applies the title; the server never sends one, because a title change travels as `GridDelta`'s `Title` op. The bell and activity flags have no producer anywhere — WP-19 |
| `GridDelta` | below | **live** | the hot path |
| `ImageAddBegin/Chunk/End` | `id u64, w u16, h u16` · `id u64, seq u32, blob (raw RGBA)` · `id u64` | **live** (WP-16) | chunked ≤ 256 KiB |
| `ImagePlace` / `ImageRemove` | `term u64, id u64, cell rect, pixel offsets` · `term u64, id u64` | **live** (WP-16) | the believed set is re-sent whole after an attach, so every mirror agrees until the next tick moves them together |
| `ClipboardSet` | `blob` | *encode-only* | inner-app OSC 52 → client sets host clipboard per config. The policy is enabled toward the child and the write is then dropped: no producer, no handler, no rehydrated serial. M-R1 → R8; [08](08-configuration.md)'s `osc52` claim reads ahead of this |
| `PrintState` | `term u64, mode u8, state u8 (idle\|capturing\|sunk\|full), bytes u32, jobs u16` | *encode-only* | drives the frame button's label and its live byte counter; coalesced onto the flush tick like everything else |
| `PrintJobAdded` | `term u64, job u64, kind u8 (screen\|line\|controller\|autoprint), bytes u32, lines u32, at i64` | *encode-only* | **metadata only** — a 1 MB job never rides the hot path or the attach snapshot |
| `PrintJobData` | `term u64, job u64, seq u32, final u8, blob` | *encode-only* | answer to `PrintJobFetch`, chunked ≤ 256 KiB |
| `SessionsChanged` | — | *encode-only* | picker refresh hint, superseded before it was ever sent: every change answers with a fresh `SessionList`, which carries the answer instead of asking for another round trip |
| `PasteAck` | `seq u32` | *encode-only* | the credit half of `PasteChunk` (below); it was described under flow control and missing from this table — WP-18 |
| `Pong` | `nonce u64` | *tests only* | accepted and dropped by the client; see `Ping` |
| `Error` | `code u16, context str, human str` | *producer only* | non-fatal errors (e.g. name collision). Codes are `proto::ErrorCode`: `NotImplemented` (1) — understood, and this build does not do it yet — then `NoSuchTerminal`, `NoSuchSession`, `NameTaken`. A request the server cannot honour is always **answered**: a client waiting for a reply that never comes is indistinguishable, to a reader, from a server that has hung (WP-2). The answer currently stops at the client's front door — `ServerSession::handle` does not claim it and `run_client` discards the result, so a reader is told nothing |

### `GridDelta` (hot path)

```
term u64, seq u32, ops[]:
  op Resize   { cols u16, rows u16 }                  # the size the ops after
                                                      # it describe; first in
                                                      # any delta that changes it
  op Scroll   { top u16, bottom u16, n i16 }          # region shift, client memmoves
  op Cells    { row u16, col u16, cells[] }           # RLE runs
  op Cursor   { col u16, row u16, style u8, visible u8, blink u8 }
  op Modes    { changed_mask u32, values u32 }        # mouse/paste/alt-screen/… bits,
                                                      # plus the mouse TRACKING LEVEL
                                                      # in bits 7–8 (DEC 1000/1002/1003
                                                      # are three levels of one facility)
  op Title    { title str }
  op SbPush   { n × line (RLE cells) }                # lines entering scrollback —
                                                      # the client mirror keeps the
                                                      # full (bounded) scrollback, so
                                                      # paging/copy-mode is local
cells[] entry: run_len u16, cell:
  { text str≤15 (grapheme cluster, UTF-8), width u8,
    fg u32, bg u32,          # tagged: default | indexed | rgb  (matches ckv::Color)
    attrs u8,                # Bold|Dim|Italic|Underline|Reverse|Strike (ckv::Attr)
    reserved u16 }           # future: underline styles/color, hyperlink id (plan 04 U1/U2)
```

**Three kinds of colour, not two.** This table said `default | rgb` when it
was written, before indexed colour survived end to end upstream (L-22 in
[07-ckvision-integration.md](07-ckvision-integration.md)). Flattening a
palette index to RGB on the wire would make ckmux the thing that decides what
a reader's palette means: their own terminal theme would stop applying to a
child's `SGR 31` the moment ckmux was in the way, and per-terminal re-theming
would be impossible for good. The tag is one byte of the u32 and carries all
three.

**`width` and `text` have to agree.** Width 0 with no text is the far half of
a wide character and must survive as such — rebuilt as a space it turns every
wide character on the screen into two, which is the classic multiplexer bug
the shared width authority exists to prevent ([04](04-terminal-emulation.md)).
Any other combination of the two is refused rather than drawn.

Deltas are emitted at the flush tick (`max-fps`), coalesced per terminal;
`Scroll` ops make full-screen scrolling cheap (no cell resend). `seq` is
per-terminal and monotonic — a gap observed by the client (should not
happen) triggers reconnect-and-resnapshot.

**Two geometries, and they are not the same number.** `Attach` and
`ClientResize` carry the CLIENT's desktop and, through their pixel fields, the
size of one cell of it. `MoveResize` carries one TERMINAL's own size — the grid
its view has, which is smaller than the desktop by whatever frame the client
draws around it. Sizing a terminal to the desktop puts a shell's prompt above
the top of its own window, where a reader cannot see it, while every layer
underneath works perfectly (WP-7, reported from a running ckmux).

A `GridDelta` states the terminal's own grid too, whenever it changes: the
`Resize` op leads any delta that repaints every row, and the ops after it are
validated against the size the mirror is about to be rather than the one it
was. Without it a repaint of a *smaller* terminal passed every bounds check
against a larger mirror, and the rows and columns past the new edge kept the
last program's output for good (C3, reproduced).

**A resnapshot is an `Attach`, answered with `Attached`.** There is no separate
"send it again" message: a client that observes a gap, or that a server told it
had fallen behind, re-sends `Attach` and is given every terminal whole. One
message, one code path, and nothing that can be implemented on one side only
(WP-6).

**A snapshot restarts the count**, so the first delta after an attach or a
resnapshot is always `seq = 1`. The snapshot carries no sequence of its own,
because that would have to be the number of a delta nobody sent; restarting is
what lets a client check continuity from its *first* delta instead of having to
take that one on trust. Numbering therefore means "monotonic between
snapshots", and a client that resnapshots resets what it expects (WP-4b).

### Snapshot (inside `Attached`)

Desktop size + layout + per terminal: ids/index/title/flags, size, cursor
(+ style/visibility), the full mode set (mouse reporting + encoding,
bracketed paste, application cursor keys, focus reporting, alternate
buffer — mirroring ckVision's `TerminalSnapshot` fields 1:1), complete grid
(same RLE cell encoding), as much of the bounded scrollback as fits a byte
budget, newest lines first, and placed images
(pixel data follows via the chunked image messages). The client's mirror is
literally a rehydrated `TerminalSnapshot` — it feeds ckVision's
`TerminalView` unchanged ([07](07-ckvision-integration.md)). Consistency
rule: snapshots are cut atomically between ticks — a client never sees half
a frame.

**What of that is filled in today**: the grid, the scrollback, the modes, the
cursor, the size, the title and the placed images — everything a reattaching
reader sees. The **layout** fields (`rect`, `z_order`, `zoomed` per terminal)
are shaped and written by nobody, so window geometry is not restored and each
window is placed by the client's own cascade; that is M-R3, remediation topic
R8 in [13-architecture-review.md](13-architecture-review.md), and it is stated
the same way in [12-work-packages.md](12-work-packages.md)'s M3 section.

**The history is the one part of a snapshot that may be short without the
snapshot being wrong.** The screen is what a terminal *is* and goes whole; the
history is what it remembers. The server measures every terminal's screen
first, then shares what is left of a **4 MiB** budget between their histories —
an equal share each, with whatever a terminal does not use passing to the next,
so one terminal with a week of `find /` behind it cannot starve the windows
beside it. A client given less history than the server holds can page back less
far; a client given a frame past the codec's cap gets no screen at all, which
is what this exists to prevent (C1). The client discards history past its own
`[general] scrollback` in any case.

Also per terminal: the **printer state and job list** (effective mode, its
scope, capture state, byte count, and one metadata row per held job — never
the payloads) — *shaped, and written by nobody until PRINT-1…6*. Print jobs
are session state like scrollback, so a reattached
client finds its frame button exactly as it left it ([04](04-terminal-emulation.md)
WP-PRINT).

## Flow control

- **Two marks, not one.** The 4 MiB high-water below is where a stream is in
  trouble; **256 KiB** is where sending more *screen* stops being worth doing.
  A delta describes a moment, and one still queued when the next tick comes
  round describes a moment that has passed — while everything else on the
  connection waits behind it. Measured under a `yes` flood with only the 4 MiB
  mark in place, a `Ping` took **2.8 seconds** to come back; with the 256 KiB
  delta mark it takes **20 ms** (WP-7).
- **A snapshot carries at most 4 MiB, newest history first.** The same trade as
  the bullet below, one level up: at attach time the screens are what a reader
  is looking at and the history is what they may page into. Bounding it is what
  makes the 16 MiB decoder cap an invariant instead of a hope — raising the cap
  would move the number and leave the identical permanently-unattachable
  session one config line away, since `scrollback = 100000` is a legal setting.
  Chunking would break the one-message rule that recovery is built on: a
  resnapshot is an `Attach` answered with `Attached`, and a client reassembling
  a multi-part snapshot when the high-water mark trips mid-way holds state that
  is neither the old mirror nor the new one.
- **A delta carries at most 256 lines of history.** A flooding child pushes
  thousands per tick, and a delta carrying them all is past the 1 MiB payload
  cap — a refused frame, not a slow one. Past that the screen still goes and the
  history does not: the client's scrollback falls behind while its screen stays
  live, and one snapshot reconciles it when the flood stops. Answering the flood
  with a snapshot *instead* was measured at 2.8 MB per tick, which is what made
  the `Ping` above take three seconds.
- **Server → client**: per-client outbound queue, high-water 4 MiB. Above
  high-water: stop queueing `GridDelta`s, mark `dirty_snapshot`; when the
  queue drains below low-water, send one fresh `Attached` snapshot. PTY
  reading never blocks on a client.
  **The mark is advisory, and "bounded memory" is not true yet**: a frame past
  high-water is still queued, because dropping half of one would desynchronise
  the stream, so a wedged peer that never reads grows the queue without limit.
  Defining the disconnect-the-laggard policy that makes the sentence true — and
  fixing the heal gate that re-snapshots forever against it — is R2's, M-S1 and
  M-S5 in [13-architecture-review.md](13-architecture-review.md).
- **Client → server (paste)**: `PasteChunk` is credit-based: server replies
  `PasteAck { seq }` as each chunk is written to the PTY; client keeps ≤ 2
  chunks in flight. Keystrokes/mouse are unpaced (human-rate). *Designed, and
  neither message has a producer or a handler yet — WP-18.*
- No keepalives: Unix sockets deliver HUP reliably; `Ping` exists for tests.

## CLI mapping (same protocol, `client_kind = cli`)

`ckmux ls` → `ListSessions`; `ckmux new [-s name] [cmd]` → `NewSession`;
`ckmux attach <name>` → UI client with preselected session; `ckmux
kill-session/-server`, `ckmux move-terminal <id> <session>` → the obvious
messages. CLI never receives sync messages (server sends none to
`cli` clients beyond direct replies).

**Today** only `ckmux kill-server` speaks this way — it greets with
`client_kind = cli` and sends `KillServer` — and the flush tick is what honours
the rule, skipping every client whose kind is not `Ui` (`server.cpp`). The
other subcommands are WP-11 and are answered by `main()`'s
unknown-argument refusal until it lands
([12-work-packages.md](12-work-packages.md)).

## Invariants & hygiene

1. Client mirrors are **write-only from the wire**: no client-side
   prediction, ever (except local echo… which we do not do — latency is
   local-socket, not worth divergence).
2. Every decoder path is bounds-checked and fuzzed ([09-testing.md](09-testing.md));
   a malformed frame kills the *connection*, never the server.
3. All ids are u64 and never recycled within a server lifetime.
4. Unknown message types: connection error pre-1.0 (single version number
   means they cannot legitimately occur); reserved-flags must be zero.
5. Wire structs live in `src/common/proto.{hpp,cpp}` with encode/decode pairs
   and round-trip property tests — one definition, used by both sides.
6. **A payload with bytes left over is a protocol error.** Every message has a
   fixed shape, so a remainder means the sender and the decoder disagree about
   what the type means — exactly the desync that must not be papered over. It
   is cheaper to drop the connection and re-snapshot than to carry a mirror
   that is subtly wrong.
7. **A length is checked against its cap before it is waited for.** A peer
   claiming a gigabyte is refused on the strength of the claim; refusing it
   after buffering a gigabyte is the difference between a bounded error and a
   denial of service. The same bound applies to the stream reassembler, so a
   peer that declares a frame and then dribbles cannot grow it either.

## What exists today (2026-08-18)

The whole catalogue above encodes and decodes, in `src/common/proto.{hpp,cpp}`
— framing, the handshake, every message, `GridDelta`'s six ops and the attach
snapshot. Nothing in it knows about a socket, which is why all of it is tested
without one ([09-testing.md](09-testing.md)'s unit layer).

**Encoding is not sending, and the `Today` column above is the difference.**
Of the forty-eight types in `proto::Message`, **twenty-six are live** end to
end: the handshake, sessions, attach and detach, the terminal operations a
reader can reach from a menu, `Input`, `GridDelta`, and — since WP-16 — the
five image messages. **Seventeen are codec-only**, and `Ping`/`Pong` are asked
for by nothing but a test. The remaining three are wired at one end only, and
are the interesting ones, because each reads as working until somebody asks
which end:
`ClientResize` has a server handler and no sender, `TermMeta` has a client
handler and no sender, and `Error` has eight senders and no reader. That is
the shape WP-1 chose deliberately — build the wire once, wire the ends as the
packages arrive — and it is honest only while this column says so.

Two shapes are worth knowing before writing against it. `Message` is a
`std::variant` of every message struct, and a decoder finds the alternative
whose `kType` matches the frame — so adding a message to the variant is the
whole of adding it, and one that is not in the variant cannot be named on the
wire at all. And `FrameReader` owns stream reassembly: a socket read gives
half a frame, three frames, or a frame and a half, and that problem is solved
once here rather than in the server and the client separately.

Still open in WP-1 itself: nothing. The transport (WP-2), the server's
terminals (WP-3) and the diff engine that fills `GridDelta` (WP-4a/4b) have
all landed, which is what moved twenty-six rows into the **live** column.
What is open *about* this wire is R1 in
[13-architecture-review.md](13-architecture-review.md) — the encode-side size
bound, `ScrollbackPush` run-length validation, `Reader::count()`'s reservation,
and `Writer::str`'s silent truncation — and R3, which needs a geometry op so a
shrinking terminal cannot diverge a mirror that never hears about the new
shape.
