# ckmux — Client⇄Server Protocol

Plan 05 · Status: draft · 2026-08-19 · [Index](README.md)

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

**The remedy the refusal names has to still be reachable.** A UI client with
a mismatched `Hello` is dropped immediately — it has nothing safe to do with
a server it cannot understand. A CLI client is given one bounded chance
(2 seconds) to send exactly `KillServer` after the refusal, and the server
honours it: no payload, no protocol to agree on, just the eight-byte frame
header. Reported live (2026-08-18) — before this, `ckmux kill-server`
against a mismatched server was refused and dropped before `KillServer`
could ever be sent, so the refusal's own remedy was the one thing it made
unreachable. Anything else sent in that window, or nothing at all, ends the
connection exactly as an ordinary mismatch does.

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
| **encode-only** | codec only: no producer, no handler. **14 of the 50 types** are here |
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
| `ClientResize` | `cols, rows, pixel_w, pixel_h` | **live** | the reader's own window, whenever it changes while attached (`ServerSession::desktop_resized`, asked of the host terminal on every pass of the client's loop and sent only when the size actually moved). The cell metric it carries is what a child's `TIOCSWINSZ` pixel fields and its XTWINOPS answers derive from, so a reader who resized their window — or changed its font — used to keep the old metric until they detached and came back (R8) |
| `NewTerminal` | `session u64?, cmd str?, geom rect?` | **live** | a zero-size rect means "place it yourself"; a non-zero one becomes the terminal's columns and rows |
| `CloseTerminal` | `term u64, force u8, grace_seconds u16` | **live** | the program is ASKED first (SIGHUP→SIGTERM through ckVision's `request_termination()`, PTY still draining); the survivors are killed at the deadline only if `force`. `force = 0` means the program keeps running and its terminal with it |
| `KillTerminal` | `term u64` | *encode-only* | SIGKILL outright; `CloseTerminal{force=1}` covers the reader-facing case today |
| `RespawnTerminal` | `term u64` | *encode-only* | restart exited terminal in-window — WP-13 |
| `MoveTerminal` | `term u64, dest_session u64, to_new_session u8` | **live** | the flag outranks the id, because `Attach` already gave 0 a different meaning; a close pending against the terminal is cancelled, since a move is the reader deciding to keep it (WP-10) |
| `MoveResize` | `term u64, rect` | **live** | one TERMINAL's own size, sent by its mirror when its view is laid out — and only that: the PTY is sized from this rect, so it is the grid inside the frame and never the window around it. Where the WINDOW is travels on `SetLayout` |
| `SetLayout` | `n × { term u64, rect, z u16, zoomed u8, tile 4×u16 }` | **live** | where this client's windows now are — every window of the session it is attached to, in one message, sent once the reader has stopped moving them (WP-29's 150 ms settle). The server records it per terminal as session state and states it back, on the attach snapshot and on `LayoutDelta` (WP-28: plans/03's "window layout is session state, owned by the server", made real). A whole arrangement rather than the one window that moved, for two things a per-window message cannot say: a `z_order` is a position in a STACK, so raising one window renumbers the ones it passed, and a window that CLOSES leaves the arrangement without being named at all. Deliberately not `MoveResize`, and not `MoveResize` with two fields bolted on — that rect is the terminal's grid and sizes a PTY, this one is the window on the desktop, and one rect meaning both is exactly the confusion the two-geometries note below exists for. No session id: a client is attached to one, and `ClientResize` names none for the same reason. `tile` is the window's share of a FILLED TILING in ten-thousandths of the content area (`x, y, width, height`), ckVision's own `Desktop::filled_tile_fractions()` answer at the moment of reporting, and zero extent when the arrangement was not one — the tag WP-28 left unadded until something wrote and read it, which is WP-30 |
| `Raise` / `FocusTerm` | `term u64` | *encode-only* | z-order and focus, both still dormant as COMMANDS: nothing sends either. Raising is reported rather than requested today — a raise changes the whole stack, and the stack travels whole on `SetLayout` (WP-28). **`Session::focused` is maintained off `Input` instead** — the terminal a reader's keys, mouse reports and focus reports go to is the terminal they are in, and it is the only statement of focus this wire carries today (R8). Two things read it: the snapshot's `focused_term`, and `TermMeta`'s activity mark, which means "output in a terminal the reader is NOT in" |
| `ZoomTerm` | `term u64, on u8` | *encode-only* | maximize/restore |
| `RenameSession` | `id u64, name str` | **live** | |
| `RenameTerminal` | `id u64, name str` | **live** | the name a READER gives one terminal, which the server keeps as session state and states back on `TermMeta` and in the attach snapshot (`TerminalState.custom_title`). It does not replace what the child claims with OSC 0/2: that goes on being recorded underneath, which is what gives "use the default title again" a CURRENT title to hand the caption back to rather than whatever it said when the reader pinned it. An **empty name clears the override** — the one place this deliberately differs from `RenameSession`, where empty means "leave it alone" because a session with no name is a row in the picker with nothing to point at. Clamped to `kMaxTitleBytes` where it is stored, on a character boundary, through the same function the child's own title goes through |
| `KillSession` | `session u64, force u8, grace_seconds u16` | **live** | ask every child first (`request_termination()`); kill the survivors at the deadline only if `force` |
| `KillServer` | — | **live** | `ckmux kill-server`, over its own connect path so running it twice does not start a server in order to kill one |
| `Input` | `term u64, blob` | **live** | already-encoded child bytes — keys/mouse/focus are encoded **client-side** by ckVision's `TerminalView` against mirrored mode state ([04](04-terminal-emulation.md)); the server just writes to the PTY (queued) |
| `PasteChunk` | `term u64, seq u32, final u8, blob` | **live** | ≤ 64 KiB chunks, credit-paced (WP-18). The client cuts a paste up front — so the clipboard is captured at the moment the reader asked, not read as it drains — and keeps `kPasteCredit` chunks unacked; the server writes each to the PTY and acks it. Sequence numbers are per CONNECTION, because `PasteAck` carries a seq and nothing else: two windows pasting at once would otherwise ack each other's chunks |
| `WatchStats` | `on u8` | **live** | whether THIS connection wants per-terminal process stats (WP-38). Per connection and deliberately not session state: the subscription is a fact about a reader's View menu, it dies with the socket, and a server nobody subscribed to does no sampling work at all — no per-second wakeup, no process-table read, proven by a pass counter a test watches stay at zero. No per-metric selection: the three numbers travel together, a few dozen bytes a second, and which of them is SHOWN is the client's business |
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
| `LayoutDelta` | `session u64, n × { term u64, rect, z u16, zoomed u8, tile 4×u16 }, focused u64, desktop cols/rows` | **live** | the session's whole arrangement, sent to its one watching client on the tick and only when a window actually moved — the same edge trigger `TermMeta` uses, and here it is also what keeps the echo from becoming a loop: the client this states a layout back to is the client that reported it, and an unchanged layout produces nothing, so the exchange stops after one. Every window rather than the ones that moved, because a `z_order` is a position among the others and a partial list is an arrangement nothing can apply. The client's handler is WP-30's, and it lays **nothing** down from one: there is at most one watcher per session and it is normally the client that reported this very arrangement, so a statement here is its own report coming home. Restoring from it moved windows and stole focus out from under a reader mid-gesture — a terminal opened after the snapshot has its place first stated by a delta a fraction of a second into the session, which buried the close-terminal dialog behind the terminal it was asking about. Windows are placed from the attach snapshot and from nothing else |
| `TermOpened` | `term u64, session u64, index u16, rect, title str, cols, rows` | **live** | `rect` and `title` are left at their defaults today; the client places and names the window itself |
| `TermClosed` | `term u64, exited u8, exit_status i32, hold u8` | **live** | `exited = 0` is a leaving, not a death — that is how a moved terminal's old watcher is told. A child that ends on its own produces one off the lifecycle damage flag, with `hold` set from `[general] on-exit` (R2); the same three facts are on the attach snapshot now, so a client that was not listening when it happened still learns them (R8) |
| `TermMeta` | `term u64, index u16, title str, flags u8 (bell, activity)` | **live** | what a reader who is not in this terminal has MISSED. `Bell` is set when the child rings; `Activity` when it writes to a terminal that is not the focused one — the tmux meaning, and the only one that makes a window marker worth looking at. Both are cleared when that terminal becomes the focused one. Marks rather than counts, and edge-triggered: the server sends one only when the pair changes, so a terminal printing steadily costs one message and not one per tick. The title travels with them, through the same clamp the `Title` op uses, because a message that STATES a title must not state an empty one (R8) |
| `GridDelta` | below | **live** | the hot path |
| `ImageAddBegin/Chunk/End` | `id u64, w u16, h u16` · `id u64, seq u32, blob (raw RGBA)` · `id u64` | **live** (WP-16) | chunked ≤ 256 KiB |
| `ImagePlace` / `ImageRemove` | `term u64, id u64, cell rect, pixel offsets` · `term u64, id u64` | **live** (WP-16) | the believed set is re-sent whole after an attach, so every mirror agrees until the next tick moves them together |
| `ClipboardSet` | `term u64, blob` | **live** | inner-app OSC 52 → the client's clipboard targets, per `[terminal] clipboard`. Sent off the emulator's clipboard damage flag and gated on `[terminal] osc52` at the server as well as at the child's door. The terminal id travels with the text although a reader has one clipboard: the client routes the write into that terminal's mirror, and `TerminalView` then delivers it exactly as it would for a terminal in this process — serial moves, host is handed the text (R8). **The snapshot carries the serial and NOT the text**: see Snapshot, below |
| `TermDiagnostic` | `term u64, kind u8, text str` | **live** | what the terminal last had to complain about — an unimplemented sequence, a malformed one, a limit a child ran into. The NEWEST one, one message at a time, off the emulator's diagnostics damage flag: the ring is bounded and the view paints its last entry, so shipping the ring whole would re-send what a client already holds every time one arrived. The snapshot carries the same single entry (R8) |
| `PrintState` | `term u64, mode u8, state u8 (idle\|capturing\|sunk\|full), bytes u32, jobs u16` | **live** | sent off the emulator's printer damage flag, so a client knows the one printer fact a reader must be shown: while the controller is on, the child's output goes to the PRINTER and not to the screen, and a terminal that did not say so reads as one that has stopped responding. A message rather than a `GridDelta` op, because the printer is not the grid (R8). It will also drive the frame button's label and its live byte counter — PRINT-1…6 |
| `PrintJobAdded` | `term u64, job u64, kind u8 (screen\|line\|controller\|autoprint), bytes u32, lines u32, at i64` | *encode-only* | **metadata only** — a 1 MB job never rides the hot path or the attach snapshot |
| `PrintJobData` | `term u64, job u64, seq u32, final u8, blob` | *encode-only* | answer to `PrintJobFetch`, chunked ≤ 256 KiB |
| `TermStats` | `term u64, cpu_permille u32, rss_bytes u64, real_bytes u64, flags u8 (has_real, alive)` | **live** | one terminal's process-tree cost, about once a second, only to connections that asked (`WatchStats`) — WP-38. `cpu_permille` is percent of one core in tenths, the `top` convention, derived by the server from two consecutive cumulative samples (0 on the first, which has no interval to differ over); `rss_bytes` is the tree's resident sum, honest about overcounting shared pages, and `real_bytes` is the platform's own answer to what it actually costs (macOS `phys_footprint`; PSS when WP-22 fills Linux in), meaningful only under `has_real`. A final message with `alive` clear is sent ONCE for a terminal whose child is gone, so the watcher clears the readout rather than freezing it. Never in the snapshot and never stored: a subscriber has current numbers within a sample period, and a number restored from state would be a second old and wrong |
| `SessionsChanged` | — | *encode-only* | picker refresh hint, superseded before it was ever sent: every change answers with a fresh `SessionList`, which carries the answer instead of asking for another round trip |
| `PasteAck` | `seq u32` | **live** | the credit half of `PasteChunk` (below) — WP-18. Sent as the chunk is WRITTEN to the PTY, which is the whole of the pacing: an ack on receipt would pace against the server's own socket buffer and let a paste through at once. Sent even when the terminal has gone, because the credit is about the client's queue rather than the terminal's existence, and a client waiting on an ack that is never coming holds the rest of that paste and every later one behind it |
| `Pong` | `nonce u64` | *tests only* | accepted and dropped by the client; see `Ping` |
| `Error` | `code u16, context str, human str` | **live** | non-fatal errors (e.g. name collision). Codes are `proto::ErrorCode`: `NotImplemented` (1) — understood, and this build does not do it yet — then `NoSuchTerminal`, `NoSuchSession`, `NameTaken`, and `LimitReached` (5) — named something real and the answer is still no, which is a different sentence and a different remedy from "no such" (WP-13's `[general] max-terminals`). A request the server cannot honour is always **answered**: a client waiting for a reply that never comes is indistinguishable, to a reader, from a server that has hung (WP-2). `ServerSession` claims it and `ClientApp` puts it in front of the reader who asked, code and context included, because those two are what a bug report is written from (R8) |

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
                                                      # and the KITTY KEYBOARD flags in
                                                      # bits 9–13 — modes in all but
                                                      # their spelling, so they ride the
                                                      # word a mode already travels in
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

**Three of them since WP-28**, and the third is why layout got a message of its
own rather than two more fields on `MoveResize`. `SetLayout`, `LayoutDelta` and
`TerminalState.rect` carry the WINDOW: where it sits on the desktop, frame
included, and where it stands in the stack. Nothing is sized from that rect —
a window whose corner hangs off an edge is an arrangement a reader made rather
than a number to clamp — and one message carrying both meanings would have made
"the rect" ambiguous at exactly the seam WP-7's bug lived on.

A `GridDelta` states the terminal's own grid too, whenever it changes: the
`Resize` op leads any delta that repaints every row, and the ops after it are
validated against the size the mirror is about to be rather than the one it
was. Without it a repaint of a *smaller* terminal passed every bounds check
against a larger mirror, and the rows and columns past the new edge kept the
last program's output for good (C3, reproduced).

**A caption is two facts, and the wire carries both.** `TerminalState.title`
and `TermMeta.title` are what the CHILD claims; `custom_title` on each is what
the READER named it, empty for none. They are separate fields rather than one
string the last writer wins, because both keep changing and both keep being
asked: a program renames itself at every prompt underneath a reader's name, and
the client resolves the two into one caption (reader's name, else the child's,
else `Terminal N`). Session state for the reason the layout is — a name that
vanished when its reader detached is the one thing a multiplexer must not do
with a name — so it rides the snapshot as well as the live message, and is
therefore right before any `TermMeta` could arrive rather than corrected a tick
after the window appears.

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

Per terminal it also carries the three things that HAPPENED rather than
things that are set, and each is shaped by what a reattaching reader needs
rather than by what the server holds (R8):

- **the clipboard watermark, and never the clipboard text.** A child's OSC 52
  is a live act — "put this on the reader's clipboard now" — so the text
  travels only on `ClipboardSet`. A snapshot that carried it would, on every
  reattach, put a child's text over whatever its reader had copied since,
  minutes later, with nobody asking. What the snapshot restores is the SERIAL,
  because the consumer of a clipboard write compares it against the one it last
  acted on: a mirror that started over at zero would meet the next real write
  with a number its watcher had already seen and drop it in silence. Monotonic,
  never reset, never backwards.
- **`exited` / `exit_status` / `hold`.** The child's own life, which no delta
  can carry because it is not a fact about a screen. A terminal whose child has
  ended and which is still in a session is a held one by construction, which is
  what `hold` reports. An exit is a one-way door: a snapshot that states one is
  adopted whole, and one that states none leaves what a `TermClosed` already
  said, since ids are never reused and only a `TermOpened` says a terminal is
  new.
- **the newest diagnostic**, and nothing older — the same single entry
  `TermDiagnostic` carries.

**What of that is filled in today**: the grid, the scrollback, the modes
(the kitty keyboard flags among them), the cursor, the size, the title, the
placed images, the printer state, the clipboard watermark, the exit and the
last complaint — everything a reattaching reader sees.

The **layout** fields (`rect`, `z_order`, `zoomed`, `tile` per terminal) are
written by the server as of WP-28, off the arrangement its client last reported
through `SetLayout` — they are session state like the history above, held per
terminal and outliving every client that watched it, which is what plans/03
decided and what M-R3 found nothing had implemented. A zero-size rect means no
client has ever placed that window, the same sentinel `NewTerminal.rect`'s zero
is; a terminal moved into another session arrives with one, because a z-order is
a place among windows it has never been beside. A zero-extent `tile` means the
window was not part of a filled tiling, which is the ordinary floating case.

And a client now READS them (WP-30), which is what M-R3 was about: a reattaching
reader's windows go back where they were rather than being cascaded afresh. The
policy is stated once, in `ClientApp::apply_layout` — maximized stays maximized
against the desktop as it is now; a window that was part of a filled tiling
comes back at the same PROPORTION of it, which is why `tile` travels as a
fraction rather than the absolute rect beside it; and an ordinary floating
window keeps its size and is moved up and left as far as it takes, with a
shrink-to-fit second step that `[general] resize-windows-to-fit` leaves off.

**It runs from the attach snapshot and from nothing else**, which is the part
that had to be learnt rather than designed. A `LayoutDelta` is the server's
record moving while the reader watches, and the reader is who moved it; a
terminal opened *after* the snapshot — every terminal of a freshly made session
— therefore has its place first stated by a delta a fraction of a second in, and
restoring from that ran while somebody was part-way through a menu. Even from a
snapshot the restore lays down geometry only: a heal arrives on the same message
with every window already placed (hence "once per attachment"), and neither the
z-order nor the focus is touched while a dialog is in front, because a dialog is
above the arrangement whatever the arrangement covers.
M-R3 in [13-architecture-review.md](13-architecture-review.md) is the finding
all three packages answer, and it called the policy "a design question, not a
transport one", which is why the transport landed on its own first.

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
the payloads). The mode, the capture state and the byte count are written now
(R8), so a reattached client reports a running printer as running rather than
as a terminal that has stopped responding; the *job list* stays shaped and
written by nobody until PRINT-1…6. Print jobs are session state like
scrollback, so a reattached client will find its frame button exactly as it
left it ([04](04-terminal-emulation.md) WP-PRINT).

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
- **A picture debt holds the current picture, not a history of frames.** A
  client healed after falling behind is *owed* its pictures rather than having
  them queued whole, and the debt is dripped as its queue has room. What a
  reader is owed is a statement about a picture **now**: when a child redraws
  one — an animation frame, arriving under the same wire id — the newest
  payload replaces the one still waiting to go out, and only a payload already
  part-way across the socket is allowed to finish, because a client assembling
  by sequence number cannot be left holding a picture whose end never comes.
  Appending instead was unbounded in exactly the case a multiplexer must
  survive: `ckvision_spin` drawing at 16fps into a pane whose host terminal
  draws Sixel more slowly made every frame a permanent entry, measured at
  **354 MiB owed to one client after 300 frames** — and the reader watched it
  play back, a cube still turning where its window had been ten seconds
  earlier, going on for a minute or more after the child had exited (field
  report, 2026-08-19). Superseded, the same run owes **1.8 frames**.
- **And a payload nobody can take is not built.** Supersession makes the debt
  bounded; it does not stop the work that fills it. The differ compares a
  redrawn picture's pixels against the believed ones and copies the whole
  frame into chunk messages — a megabyte of each, per tick, per picture — for
  a payload the debt then drops. So it asks first: when every client that
  would be sent this picture already owes an unsent payload for the same wire
  id (or there is nobody to send it to), the picture is left alone. The part
  that has to be got right is the **belief**, which is what the clients are
  believed to hold: pixels that were not sent are not adopted into it, or the
  differ would never emit that frame and a child that stopped drawing would
  leave the reader on the frame before it forever. Left unadopted, the next
  tick asks again and sends what is current *then*. Measured over the same 300
  frames: **356 MiB built before, 3.6 MiB after**.
- **Client → server (paste)**: `PasteChunk` is credit-based: server replies
  `PasteAck { seq }` as each chunk is written to the PTY; client keeps ≤ 2
  chunks in flight. Keystrokes/mouse are unpaced (human-rate). *Live since
  WP-18 (2026-08-20).* The client drops a paste in progress when it stops
  watching the session — a takeover routes nothing differently, since the
  server dispatches a chunk by terminal id and asks no questions about who is
  attached, so finishing would type the rest of one reader's clipboard into a
  window that now belongs to another.
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

## What exists today (2026-08-19)

The whole catalogue above encodes and decodes, in `src/common/proto.{hpp,cpp}`
— framing, the handshake, every message, `GridDelta`'s six ops and the attach
snapshot. Nothing in it knows about a socket, which is why all of it is tested
without one ([09-testing.md](09-testing.md)'s unit layer).

**Encoding is not sending, and the `Today` column above is the difference.**
Of the fifty types in `proto::Message`, **thirty-four are live** end to
end: the handshake, sessions, attach and detach, the terminal operations a
reader can reach from a menu, `Input`, `GridDelta`, the five image messages
(WP-16), — since R8 — `ClipboardSet`, `PrintState`, `TermDiagnostic` and
the three that used to be wired at one end only, and — since WP-29/WP-30 —
`SetLayout` and `LayoutDelta`. **Fourteen are codec-only**, and `Ping`/`Pong`
are asked for by nothing but a test.

**Nothing is wired at one end any more, and two rows were on purpose.**
`SetLayout` had a server handler and no sender, and `LayoutDelta` a server
producer and no reader, for exactly as long as WP-28 stood alone: the server is
the authority for window layout (plans/03), so making it store and state one is
a package that can be finished and tested by itself, and WP-29's reporting
client and WP-30's restoring one were then written against a wire that already
answered. Three other rows were once in this state by accident, and each read as
working until somebody asked which end: `ClientResize` had a server handler and
no sender, `TermMeta` had a client handler and no sender, and `Error` had eight
senders and no reader. That was the shape WP-1 chose deliberately — build the
wire once, wire the ends as the packages arrive — and R8 was the package that
arrived for all three. The column is honest only while it is re-derived, which
is what m-docs asks for and what the header date says was last done.

**A message can be live and still carry a dead FIELD, and the catalogue test
cannot see it.** `Attach.share` shipped with WP-44 declared, encoded, decoded,
handled by the server, and round-tripping in `test_proto`'s catalogue — while
no line in the client ever set it, so the opt-in it existed for was unreachable
from a shell. The guard was working exactly as designed and could not have
caught it: "every alternative of the variant round-trips" is a claim about the
ENCODER. It says a value survives the wire, not that anything in the program
ever produces one. The same blind spot swallowed `TermMeta`'s `bell_serial`
until a suite was written that asserted a non-zero value, and `SetDesktopSize`
would have joined them had its own message not been new enough to fail the
catalogue outright.

So the rule the message-level column states — nothing wired at one end — reads
down to the field: **every field needs a producer somewhere in `src/`, and a
test that names it.** A `grep` for the field's own name returning only its
declaration is the cheap version and would have caught all three. The
expensive-but-honest version is a case that sets the thing through the API a
reader actually reaches and asserts the byte on the wire, which is what
`tests/test_attach_flags.cpp` does for `share` — deliberately in two layers,
because a suite that only proved the flag PARSED would have passed on the day
WP-44 shipped it unreachable.

Two shapes are worth knowing before writing against it. `Message` is a
`std::variant` of every message struct, and a decoder finds the alternative
whose `kType` matches the frame — so adding a message to the variant is the
whole of adding it, and one that is not in the variant cannot be named on the
wire at all. And `FrameReader` owns stream reassembly: a socket read gives
half a frame, three frames, or a frame and a half, and that problem is solved
once here rather than in the server and the client separately.

Still open in WP-1 itself: nothing. The transport (WP-2), the server's
terminals (WP-3), the diff engine that fills `GridDelta` (WP-4a/4b), the
reattach fields (R8) and the window layout (WP-28/29/30) have all landed, which
is what moved thirty-four rows into the **live** column. The layout is whole
now: the server stores a session's arrangement and states it on every snapshot
and on `LayoutDelta`, the client reports where the reader put its windows, and a
reattaching client lays them back down — M-R3, answered end to end
([13-architecture-review.md](13-architecture-review.md)).

**Version 3 covers all of it.** R1's `ResizeOp`, tracking level and close/move
fields shipped nowhere before R8's additions were made on top of them — the
keyboard-flag bits, `TerminalState`'s clipboard watermark, exit state and
diagnostic, `ClipboardSet`'s terminal id, and `TermDiagnostic` — so both sets
are v3. WP-28's `SetLayout` is v3 too, and more easily: it is a new type rather
than a change to an existing one, so nothing already on this wire changed shape
for it, and a build that did not know it would refuse it as an unknown type,
which is precisely what the one version integer is for. WP-30's `tile` field on
`LayoutEntry` and `TerminalState` is v3 as well, and this one *does* change the
shape of something already on the wire — for the same reason the first list
gives: the version integer exists so that two builds can tell whether they can
talk; two numbers for a wire nobody has ever spoken would say the same thing
twice (`proto.hpp` records the same list). WP-38's `WatchStats` and `TermStats`
are v3 by the easy route `SetLayout` took: two new types, nothing already on
this wire changed shape for them, and a build that does not know them refuses
them as unknown types. The field arrives with its producer
and its consumer in one change, because a field nothing writes is precisely the
shaped-and-dead state M-R3 named and WP-28 declined to add another of.

`custom_title` on `TerminalState` and `TermMeta` (WP-36) is v3 by the same
reasoning and under the same pairing rule. It also retires the last
*encode-only* row that had shipped a message with nobody to receive it:
`RenameTerminal` had a codec and no handler, which is the shaped-and-dead state
one message further along, and it is answered here rather than left standing.
