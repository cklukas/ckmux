# ckmux — Work Packages

Plan 12 · Status: current · 2026-08-17 · [Index](README.md)

This file answers one question: **what is open, and what do I do next?**

[10-roadmap.md](10-roadmap.md) says what each milestone *means* and when it is
accepted. It is prose, and prose cannot be selected from — which is how a
milestone with the entire multiplexer still ahead of it came to read as though
nothing were open. This is the executable half: numbered packages, one state
each, a prerequisite column, and a selection rule. ckVision runs its corrective
work the same way ([corrective-assurance.md](../../ckvision/plans/corrective-assurance.md)),
and ckmux inherits the discipline for the same reason it inherits the rest
([11-conventions.md](11-conventions.md)).

## How to use this file

1. Open the **current execution queue** below.
2. If a row is `In progress`, continue that row.
3. Otherwise take the first row marked `Ready`.
4. Never select work from milestone prose, a plan's wish list, or a
   numerically earlier package merely because it reads as unfinished there.
5. When a package's implementation and its own evidence land, update its row,
   the [10-roadmap.md](10-roadmap.md) milestone text, and every plan whose
   behavior changed — in the same commit ([11-conventions.md](11-conventions.md):
   the plans are the spec).

**Namespace.** `WP-n` in this file is a *ckmux* package. ckVision's queue has
its own `WP-n` numbering that means something else entirely; the two never
appear in the same table. ckVision work that ckmux depends on keeps the `U-`
names it already has in [07-ckvision-integration.md](07-ckvision-integration.md),
which is the one vocabulary shared across both repos.

## States

| State | Meaning |
|---|---|
| `Ready` | Every prerequisite is satisfied. Selectable. |
| `In progress` | Being worked on now. **At most one row may hold this.** |
| `Blocked` | A prerequisite is not met. The prerequisite column always names it. |
| `Implementation complete — gate pending` | The package's own criteria hold literally, but a milestone-level gate ([09-testing.md](09-testing.md)) has not been run or has not passed. Satisfies a dependent row's prerequisite; is **not** selected as the next package. |
| `Done` | The criteria hold literally, the gates are recorded, and no known contradiction remains. |

`Done` is not "the code landed". Partial implementation, a substituted test, a
green local build, or an owner's judgement that it is good enough is not `Done`
— and a later defect reopens the owning package, exactly as ckVision's D-034
requires. M1 is the worked example: it was recorded as landed with a menu item
that did nothing, and the correction is written down rather than quietly fixed
([10-roadmap.md](10-roadmap.md), M1).

**`Blocked` means a package cannot technically be started, not that it comes
later.** Priority lives in the row order; the state column answers only "could
someone begin this today?". The first draft of this queue got that wrong — it
chained every row onto the one above it in milestone order and reported a
single `Ready` package with thirty-four `Blocked` behind it, which is a
different way of saying nothing is open. Several tracks here are genuinely
independent: the protocol codec is ckmux-local serialization, the server's
terminal ownership needs no `Application` at all
(`PosixTerminalSubsession` includes only `term/` headers), the diff algebra is
pure, and most emulator and widget U-items touch files nothing else in M2
goes near. They are `Ready` because they are, and the order below says which to
take first.

## Current execution queue

| Order | Package | Where | Est. | State | Prerequisite |
|---:|---|---|---:|---|---|
| — | M0 — Scaffolding | ckmux | — | **Done** | — |
| — | M1 — Single-process shell | ckmux | — | **Done** (corrected 2026-08-17) | — |
| 1 | **U0-a** — attach-an-existing-subsession on `Application` | ckVision | 0.5 d | **Done** (2026-08-17) | — |
| 2 | **WP-1** — Protocol codec and framing | ckmux | 3–4 d | **Done** (2026-08-17) | — |
| 3 | **U0-b** — damage/no-copy snapshot access | ckVision | 3–5 d | **Done** (2026-08-17) | — |
| 4 | **WP-3** — Server-side terminal ownership | ckmux | 2–3 d | **Done** (2026-08-17) | — |
| 5 | **WP-4a** — Grid diff algebra | ckmux | 2–3 d | **Done** (2026-08-17) | — |
| 6 | **WP-2** — Server process, socket, and lifecycle | ckmux | 3–4 d | **Done** (2026-08-17) | WP-1 ✓ |
| 7 | **WP-4b** — The diff engine on the live snapshot path | ckmux | 2–3 d | **Done** (2026-08-17) | WP-3 ✓, WP-4a ✓ |
| 8 | **U0-c** — emulator: bell, DEC line drawing, RIS/DECALN, scrollback capacity | ckVision | 2–3 d | **Done** (2026-08-17) | — |
| 9 | **U0-d** — OSC title policy for the ckmux profile | ckVision | 0.5 d | **Done** (2026-08-17) | — |
| 10 | **WP-5** — `RemoteTerminalSubsession` and the client mirror | ckmux | 4–5 d | **Done** (2026-08-17) | WP-2 ✓, WP-4b ✓, U0-a ✓ |
| 11 | **WP-6** — Attach, detach, reattach, and self-healing | ckmux | 3–4 d | **Done** (2026-08-17) | WP-5 ✓ |
| 12 | **WP-7** — M2 acceptance: the core-promise test and the flood gate | ckmux | 2–3 d | **Implementation complete — gate pending** (2026-08-17) | WP-6 ✓, U0-c ✓, U0-d ✓ |
| 13 | **WP-12** — Config v1: the remaining keys and `bind` | ckmux | 3–4 d | **Done** (2026-08-17) | — |
| 14 | **WP-8** — Multi-session server state | ckmux | 4–6 d | **Done** (2026-08-18) | WP-7 ✓ |
| 15 | **WP-9** — Session picker and the takeover flow | ckmux | 3–4 d | **Done** (2026-08-18) | WP-8 ✓ |
| 16 | **WP-10** — Move-terminal between sessions | ckmux | 2–3 d | Blocked | WP-8 |
| 17 | **WP-11** — CLI subcommands | ckmux | 3–4 d | Blocked | WP-2, WP-8 |
| 18 | **WP-13** — Exit policy, banners, and respawn | ckmux | 2–3 d | Blocked | WP-8 |
| 19 | **WP-14** — Notifications and the complete help pages | ckmux | 2–3 d | Blocked | WP-12 |
| 20 | **WP-15** — M3 acceptance: every operation row, driven from menus alone | ckmux | 2–3 d | Blocked | WP-9, WP-10, WP-13, WP-14 |
| 21 | **U1-a** — emulator: tab stops (HTS/TBC) | ckVision | 1 d | **Done** (2026-08-17) | — |
| 22 | **U1-c** — emulator: Media Copy capture behind a policy | ckVision | 1–2 d | **Done** (2026-08-17) | — |
| 23 | **U1-d** — typed dialog fields: `Radio`, `Combo`, `Number` | ckVision | 2–3 d | **Done** (2026-08-17) | — |
| 24 | **WP-17** — Copy mode, completed | ckmux | 4–6 d | **Done** (2026-08-17) | — |
| 25 | **WP-16** — Sixel transport, placement, and advertisement | ckmux | 4–5 d | **Done** (2026-08-18) | WP-6 ✓ |
| 26 | **WP-18** — Paste credit flow | ckmux | 1–2 d | Blocked | WP-6 |
| 27 | **WP-19** — Bell and activity monitors | ckmux | 2 d | Blocked | WP-6, U0-c |
| 28 | **PRINT-1…6** — the virtual printer, client and server | ckmux | 4 d | Blocked | WP-6 |
| 29 | **WP-20** — Performance budget gates under floods | ckmux | 2–3 d | Blocked | WP-16 |
| 30 | **WP-21** — M4 acceptance: the reference-app matrix and vttest chapters | ckmux | 3–4 d | Blocked | WP-16…WP-19, WP-20, PRINT-1…6, U1-a |
| 31 | **WP-26** — License, jointly with ckVision's D-OPEN-1 | both | 1 d | **Ready** (a decision, not a build) | — |
| 32 | **WP-23** — `ckmux-256color` terminfo | ckmux | 1–2 d | Blocked | WP-21 |
| 33 | **WP-22** — Linux port: poller, paths, credentials | ckmux | 4–6 d | Blocked | WP-21 |
| 34 | **WP-24** — Man page and user guide | ckmux | 3–4 d | Blocked | WP-22 |
| 35 | **WP-25** — Packaging | ckmux | 2–3 d | Blocked | WP-24, WP-26 |
| 36 | **WP-27** — v1 acceptance audit against [01-vision.md](01-vision.md) | ckmux | 2–3 d | Blocked | WP-22…WP-26 |

Roughly **90–120 focused days**, of which ~12–18 are upstream in ckVision.
These are planning ranges, not evidence. A package may land in several
commits; its row stays `In progress` until every acceptance item is met.

**New findings** discovered while implementing a package are added *to that
package*, unless they need a new architectural decision — in which case add a
new row with an explicit prerequisite rather than deferring the work silently.

## Verification policy for every package

[09-testing.md](09-testing.md) owns the suite. This is the bar a *package*
clears before its row moves, at the layer where its own claim is observable:

- **Regression** — for a package that fixes a defect, a named test that fails
  before and passes after.
- **Unit/property** — codecs, state machines, and diff algebra tested without a
  socket, a PTY, or a wall clock.
- **Public-path** — user-visible behavior enters through the real protocol, a
  real `Application::step`, or the documented public API. Calling a handler
  directly does not stand in for a criterion about what a user can do.
- **Wire-level** — anything in [03-session-model.md](03-session-model.md)'s
  operation table is proven over the socket, not against the server's own
  in-process state.
- **Adversarial** — malformed frames, truncated reads, a wedged peer, a
  destroyed window mid-callback, sanitizer lanes.
- **Reachability** — *a package that adds a command, menu entry, or key is not
  done while anything it advertises is unhandled.* `tests/test_command_reachability.cpp`
  enforces this globally; it exists because Window List shipped as a live-looking
  menu item that did nothing, for a whole milestone, and the suite never asked.

## The packages

### M2 — the split

The promise: terminals outlive the client. Nothing in M2 is user-visible
polish; all of it is the machinery that makes `kill -9` on a client harmless.
[02-architecture.md](02-architecture.md) and [05-protocol.md](05-protocol.md)
are the specs.

**U0-a — attach-an-existing-subsession.** *Done, 2026-08-17
(ckVision 2355bc3; [07](07-ckvision-integration.md) L-43/L-44).*
`adopt_terminal_subsession()` puts a caller's session into the same owning
vector, so `step()` drains it, notifies it and folds its `wait_handles()` into
the poll identically to a launched one — `launch` is now a wrapper over it.
Proven with a fake subsession, and the suite is green apart from one failure
that predates it and belongs to another package's file
(`posix_terminal_withdraws_runtime_sixel_geometry…`, red at ckVision b73290e).

**Added to the package while implementing it:** `release_terminal_subsession()`.
Adoption without a way out is a one-way door, and a mirror whose far end
closed would be drained on every step for the life of the client — which was
already true of every terminal ckmux closed. It is safe to call from inside a
change notification, since that is where a host learns a session ended.

**U0-b — damage/no-copy snapshots.** *Done, 2026-08-17 (ckVision `c4f96d2`;
[07](07-ckvision-integration.md) L-45…L-47).* Damage as a column span per row
with flags beside it, `snapshot(TerminalSnapshotOptions)` for a caller that
declines the history, and borrowed `cells()`/`scrollback()` forwarded through
`PosixTerminalSubsession`. Measured before and after on the pinned machine:
574 µs → 126 µs → 57 µs per read, and the last two do not grow with the
history. R1's mitigation is in the library, which is where it had to be — WP-4b
now has nothing to work around.

**WP-1 — Protocol codec and framing.** *Done, 2026-08-17.* Framing, the
handshake, the version integer, and every message in
[05-protocol.md](05-protocol.md)'s catalogue in `src/common/proto.{hpp,cpp}`,
with `FrameReader` owning stream reassembly so the server and the client do not
each solve it. Every criterion has its case: a round trip per message type
(with a count pinned to the variant's own size, so a message added and
forgotten fails rather than drifts), random valid messages, random bytes,
one-byte-at-a-time reads, coalesced frames, a frame and a half, and each cap.

**Added while implementing it**, all recorded in plan 05: `PasteAck` was
described under flow control and missing from the catalogue; the cell's colour
is three-kinded rather than two, because indexed colour survives end to end
upstream now and flattening it here would make ckmux decide what a reader's
palette means; a payload with bytes left over is an error rather than a
remainder to ignore; and a length is checked against its cap *before* its bytes
are waited for, since refusing a claimed gigabyte after buffering one is not
refusing it.

**WP-2 — Server process, socket, and lifecycle.** The detached server: Unix
socket, single-threaded `poll()`, lazy start, the bind race between two
starters, stale-socket recovery, version-mismatch refusal, `kill-server`. No
UI, no threads, links only ckVision's `core`+`term`.
*Done when*: each lifecycle case above has a wire-level test against a real
server process on a private `CKMUX_SOCKET`.
**Done** (2026-08-17). `src/platform/{socket,poller,process}.{hpp,cpp}`,
`src/server/server.{hpp,cpp}`, `src/client/server_connection.{hpp,cpp}`, and
argv dispatch in `src/main.cpp` (`ckmux`, `ckmux --server <socket>
[--foreground]`, `ckmux kill-server`). Nine wire-level cases against real
processes plus six in-process loop cases on a `ManualClock`.

What is deliberately NOT here: the UI client still runs its terminals in its own
process. `connect_to_server` is the real lazy-start path and is exercised by the
tests and by `kill-server`, but wiring the client's windows to a server's
terminals is WP-5 and WP-6. Nobody should read "WP-2 done" as "ckmux is
detachable yet".

Five decisions:

  * **Binding is not a lock, and that is the whole race.** A stale socket file
    has to be unlinked before a bind can succeed, so two starters that each
    unlink and bind BOTH succeed — and the one that went first is left listening
    on a socket file nobody will ever reach again. So there is a sibling lock
    file, `flock`ed exclusively and held for the server's whole life: a starter
    takes the lock, and only then probes for a live server, unlinks and binds. A
    starter that cannot take the lock has lost and goes back to connecting. Eight
    concurrent starters is a test, not a thought experiment.
  * **The hangup a shell acts on is a signal, and the reads that come with it.**
    (WP-3's finding, applied here: the server's shutdown closes children before
    it unlinks its socket, so there is no window in which a new server can start
    while the old one still owns running programs.)
  * **A signal is delivered to the loop, not acted on in the handler.** A handler
    that shut the server down would be closing sockets and killing children on a
    stack it does not own; it writes one byte down a self-pipe instead, which is
    what makes `poll()` return and lets the loop — which owns everything — decide.
  * **Unimplemented messages are answered.** A client waiting for a reply that
    never comes is indistinguishable, to a reader, from a server that has hung.
    Sessions and attach are `Error{NotImplemented}` with a sentence saying which
    package brings them, and the connection survives.
  * **A decode error is fatal for one connection and nothing else.** There is no
    way to resynchronise a stream whose framing is wrong, and guessing means
    acting on bytes of unknown provenance — so that client goes and the others
    are untouched, which is a test of its own.

Three mutations were checked to fail the suite, because a lifecycle test that has
never been seen to fail is a claim about itself: removing the start lock (the
eight-way race then leaves two servers alive), leaving the socket file behind on
shutdown, and skipping the stale-socket unlink. A fourth finding came from
stressing the suite thirty times: the race test sampled process states once and
read a loser mid-teardown as a second server, about one run in twenty. It waits
for the count to settle now.

**WP-3 — Server-side terminal ownership.** One `PosixTerminalSubsession` per
terminal: spawn, drain under a byte budget, resize with pixel fields, the
SIGHUP→SIGTERM→SIGKILL close policy.
*Done when*: PTY-level tests cover exit status, the grace path, and resize;
children never outlive an explicit close.
**Done** (2026-08-17). `src/server/terminals.{hpp,cpp}`: ids that are never
recycled, the collection, per-terminal drain budgets, `TIOCSWINSZ` with the
cell metric derived from the client's text area, exit status read where the
child's exit is actually observed, and close on the process group. Eleven
PTY-level tests, all with real children. Three findings worth carrying:
the protocol carries the **window** pixel size while ckVision wants the size
of **one cell**, and confusing them is silent (every Sixel comes out wrong,
because XTWINOPS 14/16 derive from it) — the arithmetic is pinned by reading
`TIOCGWINSZ` back out of the pty; a drain budget has to be measured in bytes
against a clock, not in iterations; and two library defects fell out of it,
recorded as L-48 and L-50 in [07-ckvision-integration.md](07-ckvision-integration.md).

**WP-4a — Grid diff algebra.** Two grids → `GridDelta` and back: scroll ops,
RLE cell runs, cursor, modes, title, scrollback pushes. Pure functions over
values, with no server, socket, PTY or clock anywhere near them — which is why
this is startable today and why [09-testing.md](09-testing.md) puts it in the
unit layer.
*Done when*: property test — random edit scripts, scroll ops included, applied
to a mirror yield a byte-identical grid; a full-screen scroll costs a `Scroll`
op rather than a cell resend; a decoder fed random bytes errors and never
crashes.
**Done** (2026-08-17). `src/common/grid_delta.{hpp,cpp}`: `GridState` (the same
type on both ends, because a delta is only correct with respect to what the
other end is believed to hold), `diff`, `apply`, and `same_state` — which is
public because "the mirror is byte-identical" has to be a claim something can
evaluate. Measured on a 120x40 screen scrolling one line: **1 477 bytes**
against **30 712** for the same screen re-sent row by row, and most of what
remains is the history line and the new row, which no encoding avoids.

Four decisions worth knowing before WP-4b builds on it:

  * **The lines that entered the history are an argument, not a derivation.**
    Two histories cannot say what moved between them — with capacity in play
    lines leave the front while lines arrive at the back, so the alignment is
    ambiguous and costs O(history) on a path that runs at the flush tick. The
    emulator already says it exactly (`TerminalDamage::scrollback_pushed`).
  * **The shift search is a byte question, never a correctness one.** Rows are
    compared by hash while a shift is being looked for (quadratic in the
    changed band, so it must be cheap) and by real cells once one is chosen: a
    wrong guess costs a repaint the mirror was going to get anyway. That is
    what lets the search score candidate shifts and pick the best rather than
    demand a perfect match — which is how the commonest scroll a multiplexer
    carries, a pager scrolling everything above its status line, stays a scroll.
  * **`apply` is all-or-nothing.** A mirror holding half a delta shows a stripe
    of some other moment and nothing downstream can tell; the ops are checked
    against the mirror's geometry before any of them are performed, and a
    refusal is the client's cue to resnapshot (plans/05's gap rule).
  * **A size change is not a delta.** `GridDelta` carries no geometry on
    purpose, so `TermMeta` goes first and a mirror that has not taken it
    refuses the delta instead of applying it to the wrong shape.

**WP-4b — The diff engine on the live snapshot path.** WP-4a wired to real
terminals: read changed cells through U0-b rather than a deep copy, coalesce
per terminal at the flush tick, monotonic per-terminal `seq`, and the gap rule
(a client observing a gap reconnects and resnapshots).
*Done when*: N terminals diffed at tick rate without copying scrollback, with
the cost measured rather than asserted; `seq` monotonicity has an adversarial
test.
**Done** (2026-08-17). `src/server/diff_engine.{hpp,cpp}`: one `TerminalDiffer`
per terminal holding what its client is believed to hold and the sequence that
belief is at, and a `DiffEngine` that flushes them.

Measured, Release, 120×40 with a row rewritten every tick: **15.6 µs/tick**
with no history and **15.3 µs/tick** with two thousand lines of it — flat, which
is the property. **Sixty terminals**, each 120×40 and each holding two thousand
lines, each with news every tick: **598 µs per flush tick**, 10 µs apiece, which
is under two per cent of a 33 ms frame at 30 fps.

Five decisions, three of them found by the tests rather than by thinking:

  * **The engine takes ckVision's seam, not ckmux's `Terminal`.** Nothing here
    needs a process — a damage report, borrowed cells, borrowed history and the
    scalars is the whole input — and that is also what lets the cost be measured
    against a bare emulator with no shell's scheduling in the number. It needed
    U0-b's reads promoted to the seam upstream (plans/07 L-52); they had been
    reachable only on the concrete classes.
  * **`status()`, so a tick does not copy a grid to find the cursor.** Damage
    flags the cursor, the modes and the title independently of any cell, and
    `snapshot()` was the only way to read them (L-51).
  * **`damage.full` is not an instruction to re-send.** It says "assume every row
    changed", which for a diff means "examine every row". Treating it as "send
    everything" made the first tick after every attach a full repaint, undoing
    the snapshot that had just been sent. A size change is the one wholesale
    case, and an empty belief is that same case.
  * **Clearing damage belongs to the engine, once per tick.** A differ that
    cleared what it had read would leave a second client watching the same
    terminal told nothing about those rows. Both `flush` overloads therefore
    clear, because the first thing that went wrong when only one of them did was
    a measurement forty times too slow: a loop reading a differ directly, with
    nothing clearing, re-copied the whole history every tick.
  * **The belief advances by applying what was sent, not by copying the
    terminal.** If the ops do not reproduce the terminal, the server's belief
    diverges exactly as the client's mirror would — and the test that builds a
    mirror from nothing but the deltas catches it. Copying the terminal there
    would hide precisely the bug worth finding.

**WP-5 — `RemoteTerminalSubsession` and the client mirror.** The ckmux class
implementing `core::TerminalSubsession` over mirror state, adopted through
U0-a, so ckVision's `TerminalView` is reused verbatim — its input encoding,
mouse translation, selection, scrollback paging, and Sixel drawing all
unchanged. This is the junction where the four independent tracks above meet,
and the first row that genuinely cannot start early.
*Done when*: the M1 UI drives remote terminals with no change to its own
window, footer, or prefix code; input encoded client-side arrives as opaque
bytes.
**Done** (2026-08-17). `src/client/mirror.{hpp,cpp}` (one terminal's client-side
state: a snapshot, the deltas after it, a history the reader pages through
locally, and a damage report of its own) and
`src/client/remote_terminal.{hpp,cpp}` (`RemoteTerminalSubsession`, the seam over
that state). `ClientOptions::terminal_source` is the substitution point, and it
is one line inside `open_terminal`: everything after it — the window and its
frame, the close confirmation, the exited banner, the footer, the prefix and its
which-key popup, copy mode — is the same code reading the same seam. That the
substitution is one line is the acceptance criterion, and it is only possible
because M1 was written against `core::TerminalSubsession` rather than against a
PTY.

Ten tests, the last of which drives the real `ClientApp` with terminals that live
in another process and reads the result off a `VirtualDisplay`, including `^B c`
opening a second one without a byte of that chord reaching the child.

Four things worth knowing:

  * **The client's history is the reader's, not the server's.** Capacity comes
    from `[general] scrollback`; a client that kept whatever a server sent would
    hold text its reader asked it to forget, and would spend the memory too.
  * **Damage is read off the ops, not by comparing grids.** The ops are the
    statement of what changed; a mirror that compared would be doing the diff
    engine's work a second time.
  * **A gap is not repaired, it is reported.** A delta whose sequence does not
    follow, or ops that do not fit, set `needs_snapshot()` — and the mirror
    applies nothing. WP-6 owns the reconnect.
  * **`feed_output` on a mirror is a routing mistake, and is counted.** A
    terminal whose output silently went nowhere is the hardest fault to find, so
    a test can now say it never happens.

It needed one upstream change (ckVision 2108568): `TerminalView` was taking a
whole snapshot eleven times — every repaint and several times per keystroke —
and on a mirror that means rebuilding a flattened history each time. It now reads
`status()`, `cells()`, `scrollback()` and the two new borrows `rasters()` and
`diagnostics()`, with a snapshot only for the clipboard text and only when the
serial says it changed.

**WP-6 — Attach, detach, reattach, and self-healing.** `Hello`/`Attach`/
`Detached`, the full snapshot, detach as a closed socket, and the
backpressure rule: on high-water or any observed desync, resnapshot rather
than repair.
*Done when*: an artificially slow client hits high-water and is healed by
snapshot-on-drain, asserted at the wire.
**Done** (2026-08-17). Server: `Attach` (granted immediately, taking the session
from whoever held it), `Detached` with the reason in words, detach as a closed
socket, `ClientResize`, `NewTerminal`, `Input`, `CloseTerminal`, and the healing
rule. Client: `src/client/server_session.{hpp,cpp}` — the mirrors, the routing,
and the one thing a client must know about losing track, which is to ask for the
terminal whole rather than to repair it.

Measured at the wire: **258 ticks** of full-screen output (about nine seconds at
30 fps) fill a stopped client's queue to the 4 MiB mark, the queue then stops
growing however much the child says, and the client is healed with exactly **two**
attachments in total — the one it asked for and the one it was given.

Three decisions:

  * **Healing is a resnapshot, never a repair.** The deltas a client missed are
    gone: the diff engine's belief moved on without it, and there is nothing to
    replay. So past the high-water mark deltas stop being queued, and when the
    queue drains the client is sent every terminal whole. A snapshot restarts
    that terminal's sequence, so the client's own continuity check works from its
    first delta afterwards (WP-4b).
  * **A resnapshot has no message of its own.** A client that has lost track
    sends `Attach` again and the server answers with the terminals whole — which
    is the same code path, the same message, and one less thing that can be
    implemented on one side only.
  * **One session, for now.** Attach, detach and takeover are real; what they
    attach TO is a single implicit session until WP-8 makes them plural, named
    and listable. `NewTerminal` is answered here rather than in WP-8, because an
    attach with no way to get a terminal cannot be tested end to end.

WP-2's interim rule — deltas to any greeted UI client — is narrowed here to the
attached client, as it said it would be, and the loop test that pinned it now
attaches first.

**WP-7 — M2 acceptance.** The core-promise test — spawn a counter, attach,
`kill -9` the client, wait, reattach, golden-compare the rehydrated frame and
see the counter advanced — plus the flood budget gate at protocol level, with
`Ping` round-trip latency asserted *during* a `yes` flood.
*Done when*: both are green in CI, and the author starts daily use.
**Implementation complete** (2026-08-17); the row stays short of `Done` because
the second half of that sentence is the author's to give. Both gates are green:

  * **The core promise.** A real server process, a real client, a child counting
    in a real PTY, and a real client death. Measured: the first client saw
    `count 3`; after 500 ms with nobody watching, the reattaching client was
    handed `count 11` — and it kept counting. The rehydrated frame is compared
    against the same terminal a moment later, cell for cell and style for style.
  * **The flood gate.** `Ping` round trip while a child runs `yes`: **32 ms
    worst, 24 ms mean** (Release).

`ckmux` is now a client: `src/client/run_client.{hpp,cpp}` starts a server if
none is listening, attaches, and runs ckVision's loop with the socket folded in
(plans/02's D-021 pattern). Windows follow the server's terminals, which is what
makes a reattach draw what is already running.

Four defects, every one found by running it rather than by reading it:

  * **No configured shell meant no shell at all.** `shell_launch("")` set an
    empty executable, so with no configuration file *every* terminal failed to
    launch — in M1 and in the server. `resolve_shell()` existed and was correct;
    nothing called it. Every test had named a shell explicitly, which is exactly
    how a default goes untested.
  * **A delta could carry unbounded history.** A flooding child pushes thousands
    of lines per tick; the delta carrying them was megabytes, past the
    protocol's own 1 MiB cap — so the frame was refused and the client stopped
    decoding anything. A delta now carries at most 256 lines.
  * **Answering a flood with a snapshot cost 2.8 MB per tick.** The first fix
    for the above was "ask for a snapshot instead", and the snapshot carries the
    whole scrollback: measured at **426 KB/s of snapshots, five messages a
    second, and a `Ping` that took three seconds**. Now a flood sends the SCREEN
    and defers the history, with one snapshot to reconcile when it calms down:
    **10 KB/s, thirty-three messages a second, `Ping` in 17–22 ms.** Forty-two
    times fewer bytes.
  * **The drain budget was per terminal, not per pass.** Parsing 64 KiB of flood
    is 22 ms of real work — two thirds of a frame — so N flooding children cost
    N × 22 ms and the loop stopped answering. It is now shared across the pass
    with a floor per terminal.

And one in ckmux's own socket layer: `Stream::receive` read until the socket ran
dry, which under a flood is never — the client hung. Bounded now, the same rule
the server applies to a PTY.

A fifth arrived from a reader running it: **the terminal was empty**. Everything
underneath was right — the mirror held the prompt, the sequence was unbroken,
the window was drawn — and the terminal had been created at the size of the
client's whole DESKTOP while the window showing it is smaller by its frame. A
terminal view shows the bottom of its grid, as a terminal should, so a prompt
printed at the top sat above the view. The desktop and the cell metric now travel
with `Attach`/`ClientResize`; a terminal's own size travels per terminal with
`MoveResize`. `tests/test_core_promise.cpp` pins it end to end: the real client
in a PTY, a reader's keystrokes, and their output on the screen — checked by
putting the bug back.

### M3 — sessions & management

The multiplexer. [03-session-model.md](03-session-model.md)'s operation table
is the specification, and every row of it becomes at least one wire-level test.

**Found by a reader running it (2026-08-17), and fixed in the same day.** None of
these were protocol faults; every layer underneath was working, which is exactly
why the tests underneath said so:

  * **Quitting asked the wrong question.** With `htop` running, quitting ckmux
    asked "Terminal is still running a program. Close it anyway?" — a question
    about closing a program, when what is closing is a view of it. An attached
    client's Quit now detaches. `^B d` does too, and both leave everything
    running.
  * **`Sessions…` was a greyed menu item.** The server now answers
    `ListSessions` with what is really there, and the dialog names the session,
    counts its terminals and marks the one this client is watching.
  * **Every remote window was called "Terminal" and landed on the same spot.**
    The numbering lived in `new_terminal()`, which an attached client never
    reaches, so two terminals looked like one.
  * **Restarting gained a terminal every time.** The client opened one at
    startup the way a single-process application would; it now asks for one only
    if the session it joined turns out to be empty.
  * **A dialog presented with `exec_message_box` from a command handler never
    appeared at all** — a nested loop inside `Application::step`, which
    ckVision's own documentation warns against.

What is NOT restored on reattach, and is honest to say so: **window geometry**.
The terminals come back with their content, their sizes and their titles, but
each window is placed by the client's own cascade rather than where the reader
left it. Layout is server state in plans/03 and travels in `TerminalState.rect`;
storing and returning it belongs with WP-8's session state, which is the next
package.

**WP-8 — Multi-session server state.** *Done, 2026-08-18.* Sessions are plural,
named and listable: `NewSession` (answered with the new `SessionList`, since
every other client's picker is now stale), `RenameSession`, and `KillSession
{force, grace_seconds}` — the kill asks first through ckVision's
`request_termination()` (SIGHUP+SIGTERM to each child's process group, PTY
still draining) and closes the survivors only when the grace expires and the
reader ticked "kill anything still running". Attach with session 0 means "the
default one" and CREATES a session on a server that has none, which is what
lets `ckmux` at a cold start be one round trip. Two lifecycle rules landed with
it: **the server exits when its last session ends** (a server with nothing to
serve is a process holding a socket for nobody), and the client **outlives the
server** — a lost connection empties its desktop, greys every session-needing
command, and the next request that needs a server starts one. That required
suppressing SIGPIPE at the one `Stream::flush` that can raise it: a client
writing to a server that just exited died mid-frame with nothing on screen to
say why. Still open here: `kill-empty-session` and the terminal limit, which
wait on WP-13's exit policy.
**WP-9 — Session picker and takeover.** *Done, 2026-08-18.* The startup rule:
nothing running → create and attach; exactly one session and nobody watching →
attach, no questions; anything else — the only session in use, or several to
choose from — is a reader's choice, and the picker offers Attach (labelled
"(in use — attaching takes it over)" where that is what it means), New
session… (name pre-filled `session-N`, one past the largest occupied number),
and Without a session. Switching IS `Attach`: the windows of the session being
left are removed — removed, never closed, because closing ends programs the
reader merely stopped watching — and the takeover transfer is
latest-client-wins, the old client dropped to an empty desktop with the picker
offered, never able to delay or veto. Driving this from a PTY found three
library defects, fixed upstream: a dialog presented before the desktop's first
layout was a zero-rect invisible modal that ate the keyboard; a radio group's
keyboard cursor did not follow its programmatic selection, so Up from "row N
selected" wrapped from row 0 and re-selected the row being left; and an
Accept-role button ignored the mouse entirely — the accept path lived only on
Enter's `accept_request` — which shipped as "clicking Create session does
nothing".
**WP-10 — Move-terminal between sessions.** The pointer move D-02 exists for:
PTY, emulator, scrollback, and id all unchanged; both sessions' clients see the
layout update; the child never notices.
**WP-11 — CLI subcommands.** `ls`, `new`, `attach`, `kill-session`,
`kill-server`, `check-config`, over the same protocol with `client_kind = cli`.
**WP-12 — Config v1 remainder.** *Done, 2026-08-17.* Every key in
[08-configuration.md](08-configuration.md)'s table is read, validated and
defaulted; `bind`/`unbind` work and a rebinding reaches dispatch, the footer,
the which-key popup and the help pages together, because all four read one
`Keymap`. A key the plan documents but no package has wired up yet is
*neither* warned about as unknown *nor* silently accepted: it is read, and
`keys_not_honoured_yet()` names it with the package it waits for. Only
`check-config`'s CLI entry point still waits for WP-11, and both halves of
its output already exist as return values.
**WP-13 — Exit policy, banners, respawn.** `on-exit` = close/hold/
hold-on-error, the exit banner, and respawn-in-window.
**WP-14 — Notifications and help.** `NotificationCenter` toasts; help pages
complete, with their key tables generated from the same registry that
dispatches the keys. `[general] clock` no longer waits here: the menu-bar
clock and its calendar landed on their own (2026-08-17), being one trailing
view on a bar that already existed and needing nothing from the server, the
notifications or the help pages.
**WP-15 — M3 acceptance.** Every operation-table row green at the wire; a
newcomer drives everything from menus alone (checklist walkthrough); the
author's tmux muscle-memory set is covered.

### M4 — fidelity

Any app, correctly. [04-terminal-emulation.md](04-terminal-emulation.md) is the
spec and holds the reference-app matrix.

Three of its ckVision items were startable without M2 and are now **done**
(2026-08-17): **U1-a** tab stops, **U1-c** Media Copy capture behind a policy
defaulting to deny, and **U1-d** the rest of the typed dialog fields —
`Radio`, `Combo`, `Number`, beside the `Check` and `Note` that landed for
`Settings ▸ General…`. They were emulator and widget work in files nothing in
M2 touches, which is exactly why they could be taken while the server was not
the thing to work on. PRINT-1…6 now waits only on the wire.

**WP-17 copy mode** — *done, 2026-08-17.* Search (with smartcase and
wrapping), vi keys beside the arrow keys, character, line and rectangular
selection, the clipboard targets of [08](08-configuration.md) in the order
that file lists them, and `^B ]` paste bracketed only for a program that
asked for it. It reads a snapshot and nothing else, which is what "above the
`TerminalSubsession` seam" buys: the same code serves M1's in-process
terminals and M2's mirrors, and nothing in it changes when the terminal moves
behind the socket.

Two decisions in it are worth stating. The history is **frozen on entry** — a
program that keeps printing must not move the line a reader is about to
select out from under them. And copy mode's own keys are **not rebindable in
v1**: they are modal motions rather than commands, so making them
configurable would mean an action name per motion. `[keys]`'s `copy-mode`
context therefore still refuses bindings out loud, which is the honest state
rather than a silent one.

The rest of M4 does wait on the wire: **WP-16** Sixel over it (chunked image
add, place, remove, and the DA1 advertisement end to end), **WP-18** the paste
credit flow, **WP-19** bell and activity monitors, and **PRINT-1…6** the
virtual printer, whose six pieces are
already itemized in [10-roadmap.md](10-roadmap.md) and behave per
[04](04-terminal-emulation.md)/[06](06-ui-design.md)/[08](08-configuration.md).
**WP-20** the performance budget gates under floods. **WP-21** M4 acceptance:
the matrix passes on WezTerm, iTerm2 and Terminal.app; `tmux` runs correctly
*inside* ckmux; a program printing in a ckmux terminal produces a saved file
without ckmux opening a device or running a command.

### M5 — portability & release

**WP-22** the Linux port across the poller, paths and credentials seams, with
the container CI lane becoming blocking. **WP-23** `ckmux-256color` shipped and
the default flipped. **WP-24** man page and user guide with a generated key
appendix. **WP-25** packaging — `make install` at minimum. **WP-26** the
license, resolved jointly with ckVision's D-OPEN-1; it blocks publishing, not
development. **WP-27** the v1 acceptance audit against
[01-vision.md](01-vision.md)'s success criteria.

## Not in this queue

The parking lot in [10-roadmap.md](10-roadmap.md) — simultaneous multi-client
attach, tiled pane mode, control mode, resurrect, Windows/ConPTY, kitty
keyboard passthrough and graphics, hyperlink activation, scrollback reflow,
per-terminal palette re-theming, session groups, PDF export (PRINT-7). Nothing
there gets a row until a plan edit moves it, because R5 binds: an addition
edits the plan first.
