# ckmux — Client⇄Server Protocol

Plan 05 · Status: draft · 2026-08-16 · [Index](README.md)

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
- Hard caps enforced on decode: `payload_len ≤ 1 MiB` for everything except
  image chunks (≤ 256 KiB payload by construction) and snapshots (≤ 16 MiB).
  Violation = protocol error = connection dropped (client then reconnects
  and re-snapshots — the universal recovery).
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

### Client → server

| Type | Payload | Notes |
|---|---|---|
| `Hello` | above | first message, mandatory |
| `ListSessions` | — | picker + `ckmux ls` |
| `NewSession` | `name str?, spawn_first u8, cmd str?` | |
| `Attach` | `session u64, cols u16, rows u16, pixel_w u16, pixel_h u16` | **always granted — latest client wins**: any prior client is detached first (`Detached{takeover}`, best-effort bounded flush; an unresponsive old client never delays the new attach). Pixel size feeds `TIOCSWINSZ` pixel fields & XTWINOPS 14/16 replies |
| `Detach` | — | also implicit on socket close |
| `ClientResize` | `cols, rows, pixel_w, pixel_h` | host terminal resized |
| `NewTerminal` | `session u64?, cmd str?, geom rect?` | |
| `CloseTerminal` / `KillTerminal` | `term u64` | graceful / SIGKILL |
| `RespawnTerminal` | `term u64` | restart exited terminal in-window |
| `MoveTerminal` | `term u64, dest_session u64` | |
| `MoveResize` | `term u64, rect` | window geometry request |
| `Raise` / `FocusTerm` | `term u64` | z-order / focus |
| `ZoomTerm` | `term u64, on u8` | maximize/restore |
| `RenameSession` / `RenameTerminal` | `id u64, name str` | |
| `KillSession` | `session u64` | |
| `KillServer` | — | |
| `Input` | `term u64, blob` | already-encoded child bytes — keys/mouse/focus are encoded **client-side** by ckVision's `TerminalView` against mirrored mode state ([04](04-terminal-emulation.md)); the server just writes to the PTY (queued) |
| `PasteChunk` | `term u64, seq u32, final u8, blob` | ≤ 64 KiB chunks, credit-paced |
| `SetPrinterPolicy` | `scope u8 (terminal\|session\|global), target u64, mode u8 (ask\|capture\|off), ask_cache u32, spool_limit u32` | WP-PRINT ([04](04-terminal-emulation.md)); the server owns the emulator, so it owns the policy. `global` also asks the server to persist it ([08](08-configuration.md)) |
| `PrintJobFetch` | `term u64, job u64` | payload is pulled only when the user previews or saves |
| `PrintJobDiscard` | `term u64, job u64 (0 = all for this terminal)` | |
| `Ping` | `nonce u64` | tests/latency probe |

### Server → client

| Type | Payload | Notes |
|---|---|---|
| `HelloAck` / `Refuse` | above | |
| `SessionList` | `n × { id, name, terminals u16, attached u8, created i64, last_attached i64 }` | |
| `Attached` | `session u64` + **Snapshot** | full state; also sent for self-healing |
| `Detached` | `reason u8 (user|takeover|session_killed|server_shutdown), text str` | |
| `LayoutDelta` | `session u64, n × { term u64, rect, z u16, zoomed u8 }, focused u64, desktop cols/rows` | |
| `TermOpened` | `term u64, session u64, index u16, rect, title str, cols, rows` | |
| `TermClosed` | `term u64, exited u8, exit_status i32, hold u8` | hold ⇒ window stays with banner |
| `TermMeta` | `term u64, index u16, title str, flags u8 (bell, activity)` | |
| `GridDelta` | below | the hot path |
| `ImageAddBegin/Chunk/End` | `id u64, w/h u16, seq, blob (raw RGBA)` | chunked ≤ 256 KiB |
| `ImagePlace` / `ImageRemove` | `term u64, id u64, cell rect, pixel offsets` | |
| `ClipboardSet` | `blob` | inner-app OSC 52 → client sets host clipboard per config |
| `PrintState` | `term u64, mode u8, state u8 (idle\|capturing\|sunk\|full), bytes u32, jobs u16` | drives the frame button's label and its live byte counter; coalesced onto the flush tick like everything else |
| `PrintJobAdded` | `term u64, job u64, kind u8 (screen\|line\|controller\|autoprint), bytes u32, lines u32, at i64` | **metadata only** — a 1 MB job never rides the hot path or the attach snapshot |
| `PrintJobData` | `term u64, job u64, seq u32, final u8, blob` | answer to `PrintJobFetch`, chunked ≤ 256 KiB |
| `SessionsChanged` | — | picker refresh hint |
| `PasteAck` | `seq u32` | the credit half of `PasteChunk` (below); it was described under flow control and missing from this table |
| `Pong` | `nonce u64` | |
| `Error` | `code u16, context str, human str` | non-fatal errors (e.g. name collision) |

### `GridDelta` (hot path)

```
term u64, seq u32, ops[]:
  op Scroll   { top u16, bottom u16, n i16 }          # region shift, client memmoves
  op Cells    { row u16, col u16, cells[] }           # RLE runs
  op Cursor   { col u16, row u16, style u8, visible u8, blink u8 }
  op Modes    { changed_mask u32, values u32 }        # mouse/paste/alt-screen/… bits
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
(same RLE cell encoding), bounded scrollback in full, and placed images
(pixel data follows via the chunked image messages). The client's mirror is
literally a rehydrated `TerminalSnapshot` — it feeds ckVision's
`TerminalView` unchanged ([07](07-ckvision-integration.md)). Consistency
rule: snapshots are cut atomically between ticks — a client never sees half
a frame.

Also per terminal: the **printer state and job list** (effective mode, its
scope, capture state, byte count, and one metadata row per held job — never
the payloads). Print jobs are session state like scrollback, so a reattached
client finds its frame button exactly as it left it ([04](04-terminal-emulation.md)
WP-PRINT).

## Flow control

- **Server → client**: per-client outbound queue, high-water 4 MiB. Above
  high-water: stop queueing `GridDelta`s, mark `dirty_snapshot`; when the
  queue drains below low-water, send one fresh `Attached` snapshot. Slow or
  wedged clients therefore cost bounded memory and always self-heal; PTY
  reading never blocks on a client.
- **Client → server (paste)**: `PasteChunk` is credit-based: server replies
  `PasteAck { seq }` as each chunk is written to the PTY; client keeps ≤ 2
  chunks in flight. Keystrokes/mouse are unpaced (human-rate).
- No keepalives: Unix sockets deliver HUP reliably; `Ping` exists for tests.

## CLI mapping (same protocol, `client_kind = cli`)

`ckmux ls` → `ListSessions`; `ckmux new [-s name] [cmd]` → `NewSession`;
`ckmux attach <name>` → UI client with preselected session; `ckmux
kill-session/-server`, `ckmux move-terminal <id> <session>` → the obvious
messages. CLI never receives sync messages (server sends none to
`cli` clients beyond direct replies).

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

## What exists today (WP-1, 2026-08-17)

The whole catalogue above encodes and decodes, in `src/common/proto.{hpp,cpp}`
— framing, the handshake, every message, `GridDelta`'s six ops and the attach
snapshot. Nothing in it knows about a socket, which is why all of it is tested
without one ([09-testing.md](09-testing.md)'s unit layer).

Two shapes are worth knowing before writing against it. `Message` is a
`std::variant` of every message struct, and a decoder finds the alternative
whose `kType` matches the frame — so adding a message to the variant is the
whole of adding it, and one that is not in the variant cannot be named on the
wire at all. And `FrameReader` owns stream reassembly: a socket read gives
half a frame, three frames, or a frame and a half, and that problem is solved
once here rather than in the server and the client separately.

Still open: nothing in WP-1 itself. The transport that carries these bytes is
WP-2, and the first real senders are WP-3 (server terminals) and WP-4a/4b (the
diff engine that fills `GridDelta`).
