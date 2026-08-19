# ckmux — Work Packages

Plan 12 · Status: current · 2026-08-18 · [Index](README.md)

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

**Remediation outranks the queue while it is open.**
[13-architecture-review.md](13-architecture-review.md) carries its own list —
R1…R10, one state each, same discipline — and four of its findings are
criticals in machinery that packages here already call `Done`. A `Ready` row
below is genuinely startable; it is not necessarily the *next* thing to do.
Take an open R-topic first, and come back here when 13's queue is clear.

**Namespace.** `WP-n` in this file is a *ckmux* package. ckVision's queue has
its own `WP-n` numbering that means something else entirely; the two never
appear in the same table. ckVision work that ckmux depends on keeps the `U-`
names it already has in [07-ckvision-integration.md](07-ckvision-integration.md),
which is the one vocabulary shared across both repos.

## States

| State | Meaning |
|---|---|
| `Ready` | Every prerequisite is satisfied. Selectable. |
| `In progress` | Being worked on now, and **by whom**: the row names the session holding it, e.g. `In progress (ckmux-63)`. **At most one row per session** — the rule was written when one agent worked this queue and read as "at most one row at all", which with several sessions live is unsatisfiable and was therefore not being written down at all. A row nobody has marked reads `Ready` to every other session, which is precisely how two of them start the same package; claim the row before the first edit, and clear it when the package lands. A row left `In progress` by a session that is no longer active reverts to `Ready` — left standing it blocks the queue instead of merely misreporting it. |
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
| 16 | **WP-10** — Move-terminal between sessions | ckmux | 2–3 d | **Done** (2026-08-18) | WP-8 ✓ |
| 17 | **WP-28** — Window layout: wire and session-state storage | ckmux | 2 d | **Done** (2026-08-19) | WP-8 ✓ |
| 18 | **WP-29** — Client-side layout capture and reporting | ckmux | 2 d | **Done** (2026-08-19) | WP-28 ✓ |
| 19 | **U4-a** — Window Switcher Bar (new ckVision widget) | ckVision | 3–4 d | **Done** (2026-08-19, ckVision `fae8f97`) | — |
| 20 | **U4-b** — Tile Horizontally/Vertically/Grid + filled-layout query | ckVision | 2–3 d | **Done** (2026-08-19, ckVision `0c06c3f`) | — |
| 21 | **U4-c** — Maximize-follows-on-open capability | ckVision | 1 d | **Done** (2026-08-19, ckVision `0c06c3f`) | — |
| 22 | **WP-30** — Reattach layout restoration (move, resize-if-opted-in, maximize, tiled-fraction) | ckmux | 3–4 d | **Done** (2026-08-19) | WP-28 ✓, WP-29 ✓, U4-b ✓ |
| 23 | **WP-31** — Tile Horizontally / Vertically / Grid commands in ckmux | ckmux | 0.5–1 d | **Done** (2026-08-19) | U4-b ✓ |
| 24 | **WP-32** — Window Switcher Bar integration in ckmux | ckmux | 1–2 d | **Done** (2026-08-19) | U4-a ✓ |
| 25 | **WP-33** — Maximize-follows integration in ckmux | ckmux | 0.5 d | **Done** (2026-08-19) | U4-c ✓ |
| 25a | **U4-g** — A dialog too tall for its screen scrolls, buttons pinned | ckVision | 1–2 d | **Done** (2026-08-19, ckVision `6d7172b`) | — |
| 25b | **U4-i** — Minimize: window state, the `_` control, and what a hidden window does | ckVision | 2–3 d | **Done** (2026-08-19, ckVision `720e0eb`) | — |
| 25c | **U4-j** — Switcher bar: status icons and taskbar click semantics | ckVision | 1–2 d | **Done** (2026-08-19) | U4-i ✓ |
| 25c2 | **U4-o** — A standard `ckv.window.minimize` command and its `Desktop` handler | ckVision | 0.5 d | **Done** (2026-08-19) | U4-i ✓ |
| 25d | **U4-k** — The minimize animation | ckVision | 1–2 d | **Ready** | U4-i |
| 25e | **WP-34** — Minimize in ckmux: the bar appears whenever something is hidden | ckmux | 0.5–1 d | **Done** (2026-08-19) | U4-i ✓, U4-j ✓, U4-o ✓ |
| 25f | **U4-l** — A paged item strip, and the switcher bar as its first consumer | ckVision | 2–3 d | **Done** (2026-08-19, ckVision `51eaced`) | — |
| 25g | **WP-35** — Collapsing the chrome in ckmux: bar to the last row, footer away | ckmux | 0.5 d | **Done** (2026-08-19) | U4-l ✓ |
| 25h | **U4-n** — A resize re-lays the arrangement a reader stated | ckVision | 2 d | **Done** (2026-08-19, ckVision `0e7c013`) | — |
| 26 | **WP-11** — CLI subcommands | ckmux | 3–4 d | **Ready** (head start landed) | WP-2 ✓, WP-8 ✓ |
| 27 | **WP-13** — Exit policy, banners, and respawn | ckmux | 2–3 d | **Ready** | WP-8 ✓ |
| 28 | **WP-14** — Notifications and the complete help pages | ckmux | 2–3 d | **Ready** | WP-12 ✓ |
| 29 | **WP-15** — M3 acceptance: every operation row, driven from menus alone | ckmux | 2–3 d | Blocked | WP-13, WP-14, WP-31 (WP-9 ✓, WP-10 ✓) |
| 30 | **U1-a** — emulator: tab stops (HTS/TBC) | ckVision | 1 d | **Done** (2026-08-17) | — |
| 31 | **U1-c** — emulator: Media Copy capture behind a policy | ckVision | 1–2 d | **Done** (2026-08-17) | — |
| 32 | **U1-d** — typed dialog fields: `Radio`, `Combo`, `Number` | ckVision | 2–3 d | **Done** (2026-08-17) | — |
| 33 | **WP-17** — Copy mode, completed | ckmux | 4–6 d | **Done** (2026-08-17) | — |
| 34 | **WP-16** — Sixel transport, placement, and advertisement | ckmux | 4–5 d | **Done** (2026-08-18) | WP-6 ✓ |
| 35 | **WP-18** — Paste credit flow | ckmux | 1–2 d | **Ready** | WP-6 ✓ |
| 36 | **WP-19** — Bell and activity monitors | ckmux | 2 d | **Ready** (wire half landed as R8's side effect — `TermMeta` carries both flags; nothing in the UI reads them yet) | WP-6 ✓, U0-c ✓ |
| 37 | **PRINT-1…6** — the virtual printer, client and server | ckmux | 4 d | **Ready** | WP-6 ✓, U1-c ✓, U1-d ✓ |
| 38 | **WP-20** — Performance budget gates under floods | ckmux | 2–3 d | **Ready** | WP-16 ✓ |
| 39 | **WP-21** — M4 acceptance: the reference-app matrix and vttest chapters | ckmux | 3–4 d | Blocked | WP-18, WP-19, WP-20, PRINT-1…6 (WP-16 ✓, WP-17 ✓, U1-a ✓) |
| 40 | **WP-26** — License, jointly with ckVision's D-OPEN-1 | both | 1 d | **Ready** (a decision, not a build) | — |
| 41 | **WP-23** — `ckmux-256color` terminfo | ckmux | 1–2 d | Blocked | WP-21 |
| 42 | **WP-22** — Linux port: poller, paths, credentials | ckmux | 4–6 d | Blocked | WP-21 |
| 43 | **WP-24** — Man page and user guide | ckmux | 3–4 d | Blocked | WP-22 |
| 44 | **WP-25** — Packaging | ckmux | 2–3 d | Blocked | WP-24, WP-26 |
| 45 | **WP-27** — v1 acceptance audit against [01-vision.md](01-vision.md) | ckmux | 2–3 d | Blocked | WP-22…WP-26 |
| 46 | **U4-m** — Damped switcher-bar entry widths (with **U4-n**, `PagedStrip::press_in_flight()`, landed as ckVision `850711f`) | ckVision | 0.5–1 d | **Done** (2026-08-19) | U4-a ✓, U4-l ✓ |
| 47 | **WP-36** — Rename Terminal: a custom title that overrides the child's, and a bar that does not twitch | ckmux | 1–2 d | **Done** (2026-08-19) | WP-6 ✓, WP-32 ✓, U4-m ✓ |
| 48 | **U5-a** — `process_id()` on `PosixTerminalSubsession`: a pid for observation | ckVision | 0.5 d | **Done** (2026-08-19, ckVision `da11baa`) | — |
| 49 | **WP-37** — Process-stats seam: the tree and what it costs, macOS first | ckmux | 2–3 d | **Done** (2026-08-19) | — |
| 50 | **WP-38** — Stats on the server tick and on the wire | ckmux | 2–3 d | **Implementation complete — gate pending** (2026-08-19, ckmux-53; central gate requested) | WP-37 ✓, U5-a ✓ |
| 51 | **WP-39** — The View menu, the config keys, and the frame readout | ckmux | 2–3 d | **Ready** (starts after WP-38's central gate) | WP-38 impl ✓ |

Roughly **114–147 focused days**, of which ~20–26 are upstream in ckVision.
These are planning ranges, not evidence. A package may land in several
commits; its row stays `In progress` until every acceptance item is met.

**New findings** discovered while implementing a package are added *to that
package*, unless they need a new architectural decision — in which case add a
new row with an explicit prerequisite rather than deferring the work silently.

**The state column is re-derived, not inherited (2026-08-18).** Eight rows sat
`Blocked` on prerequisites that had every one of them reached `Done` — a queue
reporting one selectable package while eight were open, which is the first
draft's failure in a slower form. A row goes `Blocked` when a prerequisite is
unmet and comes back the moment it is met, and nothing performs that second
half but somebody re-reading the column. So every row above was re-derived from
this file's own definitions on the date in the header: WP-10 landed with the
close-dialog rework and is `Done`; WP-11, WP-13, WP-14, WP-18, WP-19 and
PRINT-1…6 all had their named prerequisites satisfied; WP-16's landing freed
WP-20; and WP-15 and WP-21, which are genuinely still blocked, now name only
what is actually missing. **A `Blocked` row whose prerequisite column shows
nothing but ✓ marks is a bug in this file** — that is the check, and it takes a
minute.

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
say why. Still open here: the terminal limit, which
waits on WP-13's exit policy. `kill-empty-session` landed 2026-08-18 for the
explicit paths — a session emptied by a close or a move ends, and the server
with its last session; the child-exits-by-itself path still waits on WP-13.
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
**The close dialog now tells the whole truth (2026-08-18, reader-directed).**
`^B x` asked "Close it anyway?" — yes or no, about a program it then always
killed. It is now the dialog plans/06 sketches: closing ASKS the program to
quit (SIGHUP then SIGTERM, ckVision's `request_termination`, PTY still
draining), kills after `kill-grace-seconds` only when the ticked-by-default
box says so, and offers "Move instead…" — the `^B .` picker — so the program
can keep running in another or a new session. `CloseTerminal` grew
`{force, grace_seconds}`; the server watches a `PendingClose` per terminal on
the tick, exactly as sessions are ended, and the client's window falls only
when `TermClosed` reports the terminal gone — `exited=0` being a move, not a
death. A serverless client shows the same dialog without the untickable
choices and says what closing really does there. Wire, loop, dialog and both
end-to-end walks are pinned in `test_proto`, `test_server_loop`,
`test_client_smoke` and `test_menu_commands`.
**WP-10 — Move-terminal between sessions.** *Done, 2026-08-18.* The pointer
move D-02 exists for: PTY, emulator, scrollback, and id all unchanged; both
sessions' clients see the layout update; the child never notices. Landed with
the close-dialog rework: `MoveTerminal{term, destination, to_new_session}`
reparents on the server — the source's watcher gets `TermClosed{exited=0}` (a
leaving, not a death), the target's watcher is healed by a fresh snapshot, and
the flag creates the destination so "a new session" is always a workable
answer. A close pending against the moved terminal is cancelled: a move is
the reader deciding to keep it.
**WP-11 — CLI subcommands.** *Ready.* `ls`, `new`, `attach`, `kill-session`,
`check-config`, over the same protocol with `client_kind = cli`.
`--help` and `--version` landed ahead of the package (2026-08-18, `a696435`):
both are answered in `main()` without touching a socket, the help page is the
CLI's reference (typeset for 80 columns, measured by `test_cli.cpp`), and the
unknown-argument refusal points at `--help` instead of listing subcommands
in a second place. `kill-server` is WP-2's and has been real since then —
deliberately over its own connect path, so running it twice does not start a
server in order to kill it. Everything else in that list still arrives at the
unknown-argument refusal, which is what `main()`'s dispatch says today
(`src/main.cpp`): five subcommands to write, not six.
**WP-12 — Config v1 remainder.** *Done, 2026-08-17.* Every key in
[08-configuration.md](08-configuration.md)'s table is read, validated and
defaulted; `bind`/`unbind` work and a rebinding reaches dispatch, the footer,
the which-key popup and the help pages together, because all four read one
`Keymap`. A key the plan documents but no package has wired up yet is
*neither* warned about as unknown *nor* silently accepted: it is read, and
`keys_not_honoured_yet()` names it with the package it waits for. Only
`check-config`'s CLI entry point still waits for WP-11, and both halves of
its output already exist as return values.
**WP-13 — Exit policy, banners, respawn.** *Ready.* `on-exit` = close/hold/
hold-on-error, the exit banner, and respawn-in-window. The wire is shaped for
all three and nothing on the server fills it in: `TermClosed.hold` is read by
the client (`mirror.cpp`, `server_session.cpp`) and never set, `RespawnTerminal`
has neither producer nor handler, and `[general] on-exit` is the first line
`keys_not_honoured_yet()` prints. **Carried over from WP-8**: the per-session
terminal limit ([03](03-session-model.md): "config, default 64"), which has no
key in [08](08-configuration.md) yet and belongs with the policy that decides
what an over-limit request is told. **Depends on R2**
([13](13-architecture-review.md), m-exit-msg): a child that exits by itself
produces no `TermClosed` at all today — the server observes the exit and says
nothing — so there is no event for a banner to hang on until that lands.
**WP-14 — Notifications and help.** *Ready* — WP-12 is `Done`, and nothing
here needs a server. `NotificationCenter` toasts; help pages
complete, with their key tables generated from the same registry that
dispatches the keys. `[general] clock` no longer waits here: the menu-bar
clock and its calendar landed on their own (2026-08-17), being one trailing
view on a bar that already existed and needing nothing from the server, the
notifications or the help pages.
**WP-15 — M3 acceptance.** Every operation-table row green at the wire; a
newcomer drives everything from menus alone (checklist walkthrough); the
author's tmux muscle-memory set is covered.

### Window layout and management (2026-08-19)

M-R3 in [13-architecture-review.md](13-architecture-review.md) named the gap
and deliberately left it unfixed: everything about a terminal survives
reattach — content, history, modes, clipboard watermark, printer state, exit
status — except its window's position, size and z-order, which M-R3 called "a
design question, not a transport one." The design arrived (reader-directed,
2026-08-19) with three related feature requests attached, so this is one
cluster of nine packages rather than one. plans/03 already settles the
ownership question these build on: "window layout is session state, owned by
the server."

**WP-28 — Window layout: wire and session-state storage.** Revives the
fields WP-1's catalogue already shaped and nothing has written or read since:
`TerminalState.rect`/`z_order`/`zoomed`, and `LayoutDelta`. The server stores
the last-reported geometry per terminal as session state and includes it on
every snapshot; a producer sends the delta when the client reports a change
(WP-29). *Done when*: a layout value round-trips through the server — set,
snapshot, decode — with no client attached to read it back, proving the
storage is real independent of any restoration policy.

**WP-29 — Client-side layout capture and reporting.** The client watches
each window — move, resize, zoom, close, raise — and reports the change
through WP-28's message, debounced rather than sent per intermediate drag
frame (WP-7 already found what an unthrottled per-frame channel costs). It
also tags whether the arrangement AT THE MOMENT OF REPORTING is a
ckVision-recognized filled, non-overlapping tile (U4-b's query) — the
signal WP-30's proportional restoration needs. That half can land after
U4-b; position/size/zoom capture does not need it and can ship first.
*Done when*: dragging, resizing, zooming and closing a window each produce
exactly one layout report at the wire, against a real `Desktop`.

**WP-30 — Reattach layout restoration.** The actual policy, stated exactly
as specified:

  * A **zoomed/maximized** terminal is restored maximized against the
    current desktop.
  * A terminal that was part of a **filled tile** at the moment it was last
    reported (WP-29's tag, via U4-b) is restored at the same *proportional*
    fraction of the new desktop — a 50/50 split stays 50/50 whatever size
    the reader reattaches at, rather than reusing the old absolute rect.
  * An ordinary **floating** window is restored at its stored size, first
    *moved* — shifted up and/or left, never down or right, only as far as
    needed — to bring it maximally into the new desktop's bounds. Only if it
    still does not fully fit after that move, and only if the reader has
    opted in through a new Settings checkbox (default **off**), is it THEN
    resized down to the largest size that fits — a distinct second
    operation, always after the move, never instead of it. With the
    checkbox off, a window too large for the new desktop is moved as far as
    it can be and left at its real size, which may still hang off the edge
    — automatic resizing is deliberately not the default; a reader's window
    stays the size they set it to unless they asked otherwise.

  New config key in `[general]` (name and wording to settle in
  [08-configuration.md](08-configuration.md) when this lands — something
  like `resize-windows-to-fit`), surfaced as a checkbox in
  `Settings ▸ General…`, default off.

  *Done when*: three regression scenarios are pinned — a window dragged to
  the bottom-right on a large desktop reattaches shifted up-and-left, fully
  visible, unresized, checkbox off; the same scenario with the checkbox on
  and the window still oversized after the shift is then resized down as
  the second step; a maximized window and a 50/50 tiled pair both reattach
  correctly proportioned on a differently sized desktop.

  **Settled when it was implemented (2026-08-19): independently.** Each
  floating window finds its own nearest fit; there is no group shift. A
  group shift is bounded by the WORST window in it, so one oversized window
  would drag every other one off the place its reader put it — turning one
  window's problem into everybody's. And the arrangement where relative
  offsets genuinely carry meaning is the deliberate one, which is precisely
  the filled tiling: that is restored as a whole and proportionally, and
  never reaches this rule. What is left is windows a reader placed one at a
  time, which is how they are best put back.

**U4-a — Window Switcher Bar** *(ckVision)*. A new dockable widget: every
open window listed by title, click to activate and raise, right-click for a
host-supplied context menu — "like Windows taskbar," in the reader's own
words. Docked at the bottom, stacked immediately above whatever a host
already docks there (ckmux's own footer).

**Established before implementation (2026-08-19), so nobody has to guess:**
`Desktop::dock_bottom` holds exactly ONE view per edge — its own header says
docking a second "replaces the first as far as auto-positioning goes, but
does NOT remove it". So the bar is **not** a second dock; the host composes
`ui::Column{ WindowSwitcherBar, StatusLine }` and docks that Column. This is
the right answer rather than the convenient one: `Column`'s vertical hint
sums its children, so the reserved height is correct automatically and
`content_area()` — and therefore a maximized window's bounds — already
excludes the whole stack, which is precisely the "never covered"
requirement. It also needs **no change to the dock API**, which matters
beyond elegance: another session is building the CK Office launcher against
`Desktop`/`Window` right now and was promised these packages are additive.
The bar therefore ships as a widget that works anywhere a view can be
placed, not as a privileged second dock slot. Customizable, per the request: the
window set listed, the context menu's contents, and what "activate" does are
all host-supplied (a title/window provider and a menu-builder callback), not
hardcoded to ckmux — ckmux is its first consumer, deliberately not its only
one.

  **Open for whoever implements**: command dispatch today runs against the
  application's own focused view. Right-clicking a background window's row
  and choosing Close must act on THAT window, not whichever one currently
  has focus — check whether the existing per-window frame-overlay controls
  (the print button, the close control) already solve "run this against a
  specific window" and reuse that seam rather than inventing a second one.

  *Done when*: three open windows produce three entries; clicking each
  activates and raises the matching window; right-click shows a menu whose
  contents the host supplied; a maximized window's bounds visibly exclude
  the bar's row.

**U4-b — Tile Horizontally / Vertically / Grid, and a filled-layout query**
*(ckVision)*. Three new standard window commands beside the existing `Tile`
and `Cascade`. Tile Horizontally arranges every window in full-width
horizontal bands stacked top to bottom; Tile Vertically arranges them in
full-height vertical bands side by side — the conventional meaning from
desktop taskbars' own "Tile Windows" commands, stated explicitly here
because the two words are used inconsistently across platforms. **Tile
Grid** arranges them near-square: four windows become two columns by two
rows.

The grid is specified as an exact cover, which is a correctness
requirement rather than a neatness one — the filled-layout query below and
the shadow suppression that rides on it both have to recognise a grid as
filled, and a ragged last row would make them wrong. So: `cols =
ceil(sqrt(n))`, `rows = ceil(n / cols)`, each row's height `area.height /
rows` with the last row absorbing the remainder, and within a row of `k`
windows each width `area.width / k` with the last absorbing that
remainder — so a short final row stretches across the full width instead
of leaving desktop showing. n=3 is the case that proves it: two windows
above, one full-width below.

**Checked against the code before implementation, and it changed the
package (2026-08-19):** the existing `Desktop::tile()` is *already* the
vertical-bands arrangement — it slices `content_area().width` by the window
count and gives each window the full height (`desktop.cpp:589`). Both this
file and plans/06 described it as "a near-square grid", which it has never
been. So `Tile Vertically` names the arrangement ckVision already produces
rather than adding a second one, `tile()`'s own behaviour is left exactly
as it is (`kTile` is a standard command other applications call, and a
downstream consumer was promised these packages are additive), and
`Tile Horizontally` and `Tile Grid` are the genuinely new geometry. The
grid the plans had claimed all along is thus finally built rather than
merely described — which is the whole reason to read the code before
writing the package. `Tile Vertically` and the bare `Tile` deliberately
produce the same arrangement; ckmux offers the three explicit names in its
Window menu and leaves the unqualified one to applications that already
call it. Both produce an even, non-overlapping, desktop-filling
arrangement, which is also what the rest of this package detects: **checked
against the current code, and this does not exist yet** —
`Desktop::child_casts_shadow` reads only each window's own `casts_shadow()`
flag; there is no "is the desktop currently fully tiled" state anywhere in
`desktop.cpp` today, so this is new work, not a reuse of something already
there. A public query (`Desktop::filled_tile_fractions()` or similar)
returns, when the arrangement IS such a filled tiling, every window's rect
as a fraction of the desktop; empty otherwise. WP-29 tags a layout report
with it; WP-30 restores proportionally from it. Shadow suppression while
filled — the reader's own observation, and a sound one: a shadow is a
windows-on-windows depth cue, and a full tile has no visible desktop
between windows for one to fall on — is a visual consequence of the same
detection, wired through `child_casts_shadow`, not a second mechanism.
*Done when*: both commands produce the described arrangement for 2, 3 and 4
windows; the fraction query is correct immediately after either runs and
empty again once a window is dragged off-grid; shadows are absent while
filled and return the moment a window moves.

**U4-c — Maximize-follows-on-open capability** *(ckVision)*. An opt-in
`Desktop` behavior: when a host enables it, a newly created window opens
already maximized if the window active at the moment of creation was
itself maximized. Opt-in because not every ckVision application wants this
as a default — it is new behavior ckmux is choosing, not a correction of a
wrong default elsewhere. *Done when*: with the capability enabled, opening a
window while the active one is maximized places the new one maximized too;
disabled (ckVision's own default), behavior is unchanged.

**WP-31 — Tile Horizontally / Vertically / Grid in ckmux.** Menu entries
under `Window ▸`, beside the existing Cascade; keybindings, which-key
rows and help-page entries through the one `Keymap` every other command
already goes through (R6 made `chord_spelling()`/`parse_chord()` the single
spelling authority — new bindings use it, not a table of their own). The
menu offers the three explicitly named tilings and leaves the unqualified
`Tile` out of ckmux's own surface: it produces the same arrangement as
Tile Vertically, and a menu listing both would be offering one behaviour
twice under two names. Depends on U4-b. *Done when*:
`tests/test_command_reachability.cpp` — the suite that exists because a
live-looking menu item once had no handler — passes with all three new
commands included, each reachable by its own chord.

**U4-g — A dialog too tall for its screen scrolls, and its buttons stay
put** *(ckVision)*. Reader-directed, 2026-08-19, out of a real measurement:
`Settings ▸ General…` is drawn for 22 rows, and on an 80×24 terminal with
the switcher bar and two terminals open the desktop is 21 — so the dialog
clips, and what a reader loses is the bottom, which is where Save and
Cancel are. A dialog that cannot show its buttons is not merely cramped;
it is unusable, and the reader cannot tell whether it is broken or they
are.

The rule, settled by the requester on 2026-08-19 and simpler than the
first draft of this entry: **every dialog's content lives in a scroll
area, always, with the vertical bar on `ScrollbarPolicy::Auto`, and the
button row attached to the dialog's bottom border.** No conditional
wrapping and no second tree shape — one code path, and the bar appears
only when there is genuinely more to see, which is what makes always
wrapping invisible.

The dialog carries a **recommended height** — its natural content height
— and takes that, clamped to what is available. So in the ordinary case
the dialog is exactly the size it is today, the content exactly fills the
viewport, the bar stays hidden, and **there is no observable difference on
screen**. That is the acceptance bar for a fitting dialog: not "close
enough" but "nothing changed".

**Sizing is decided at layout time, and re-decided on every size change.**
Shrinking the terminal degrades into scrolling; growing it back lets the
dialog grow toward its recommended height again. A dialog that was only
correct at the size it was born at would be a thin fix — the reader
resizes, and so does the terminal.

Checked before writing this, so the package is composition rather than
invention:
- `widgets::ScrollViewport` already scrolls an arbitrary content view and
  installs its own `Scrollbar` children (`scroll_viewport.hpp`).
- `materialize_dialog` already builds the buttons as their own `Row`
  appended last to the field `Column` (`dialog.cpp`), and already has an
  `anchor_buttons_to_bottom` path with a spacer — so the split this needs
  is one the layout half-expresses today.
So the shape is: fields into the viewport, button `Row` outside and below
it, viewport gets the remaining height.

**A limitation to carry honestly rather than discover later**:
`ScrollViewport`'s own header records that a mouse wheel over the CONTENT
does not reach the viewport — `Application` dispatch is deepest-hit-wins
with no bubbling for mouse events, so the wheel reaches the content
widget instead. Keyboard scrolling and the scrollbar work. That is
acceptable for a dialog (a reader is on the keyboard, and the bar is
clickable), and fixing it properly means mouse bubbling in `Application`,
which is its own package and must not be smuggled into this one. Say so
in the docs rather than leaving a reader to find it.

*Done when*: a dialog whose content exceeds the available height shows a
scrollbar, scrolls its fields by keyboard and by the bar, and shows its
buttons at all times — asserted with numbers, the way ckVision's own
dock tests assert; a dialog that fits is byte-identical to today,
including no scrollbar; and focus traversal still reaches every field,
scrolling it into view rather than leaving the cursor off-screen.

**U4-n — A resize re-lays the arrangement a reader stated** *(ckVision)*.
Found while designing WP-34, and not a consequence of it: tiling two windows
and resizing the terminal one row down and back left them **overlapping**,
permanently, with no dock, no bar and no minimize anywhere near it. Verified
by a standalone probe against the built library before any code was written
— `[A 0,0 80x12][B 0,12 80x12]` becomes `[A 0,0 80x12][B 0,11 80x12]` and
stays there. `Window::reposition_within` clamps each window independently,
which cannot preserve a partition even on the way down, and only ever
shrinks, so growing back never repairs it. The reader loses the filled-tiling
verdict, the shadow suppression riding on it, and the proportional-restore
tag ckmux puts on the wire as `TileShare` — for the rest of the session.

This blocked WP-34 rather than the other way round: making the switcher bar
appear on minimize changes the dock height, which is a content-area change,
which would have driven this defect on every minimize and restore. The naive
one-line fix to the reader's reported bug would have corrupted their layout.

*Landed*: the desktop remembers the cells as stated and the area they were
stated in, and maps from that **original** every pass — rounding to nearest
is not an involution, so re-deriving at each intermediate size walks a seam
and never walks it back. Validated against what the last mapping wrote, so a
reader's own drag invalidates the arrangement rather than being snapped back
into it. Zoomed and grow-policy windows veto the capture, because
`filled_tile_fractions()` has no "more than one" gate and an ordinary
maximized window is a one-cell partition that would otherwise be captured
and taken out of reach of its own policy. See ckVision D-058.

**U4-l — A paged item strip, and the switcher bar as its first consumer**
*(ckVision)*. Reader-directed, 2026-08-19, and reader-framed: *"this could
also be a paging status bar in ckVision, perhaps we need that more often, or
the window switching also in future apps."* That is the litmus test answering
itself, so the paging is **not** bolted into `WindowSwitcherBar` — it is a
general one-row strip of variable-width items that pages when they do not
fit, and the bar becomes its first consumer exactly as ckmux was the bar's.
The same reasoning that put the bar in ckVision rather than ckmux puts the
strip under the bar rather than inside it.

What the strip owns, left to right:
- **A collapse toggle at the far left**: `▼` when expanded. The strip does
  **not** hide anything itself — it reports the toggle and exposes the state,
  and the host decides what collapsing means, because the thing being
  collapsed is the host's other chrome, not the strip's. In ckmux that is
  WP-35: hide the footer, at which point the docked `Column` is one row and
  the bar is on the last row by arithmetic that already exists. Expanded
  again, the glyph is `▲` and the footer returns.
- **A left page control** (`◁`) when there is a previous page, immediately
  after the collapse toggle.
- **A page index** — `1/3`, `2/3` — after the page control. Shown only while
  there is more than one page: an index that always reads `1/1` is a column
  spent saying nothing.
- **The items**, as many as fit whole.
- **A right page control** (`▷`) at the right edge when items remain.

Interactions with what the bar already does, which the implementer must
settle rather than discover: today the bar **elides** labels when the row is
narrow. Paging changes the default — items should take their natural width
and overflow to a page — but elision cannot simply be deleted, because a
single item wider than the whole strip still has to be drawn somehow. State
the rule.

**Revalidation is the part that will bite.** The item set changes under the
strip: in ckmux a terminal is opened or closed at any time. So page count is
recomputed on every change, and **if the current page is emptied by a removal
the strip falls back to the previous page** rather than showing a blank row
with a `3/2` index. The reader asked for this explicitly; it is also the case
that is easy to leave until a bug report. Removing an item on an earlier page
must not silently jump the reader elsewhere either.

Glyph widths matter here as in U4-j: `◁`/`▷` (U+25C1/25B7) and `▲`/`▼`
(U+25B2/25BC) must be checked against ckVision's own width authority before
use — several geometric shapes are East Asian **Ambiguous**, and a
double-width triangle would shear the row it is supposed to be steering.
Verify, and fall back to ASCII if the tables disagree.

*Done when*: a strip too narrow for its items pages rather than truncating;
the index reads correctly and is absent at one page; the controls appear only
when they can do something; adding and removing items re-pages, with the
emptied-last-page case landing on the previous page; the collapse toggle
reports its state without the strip touching anything outside itself; and
`WindowSwitcherBar` behaves exactly as it does today on a strip wide enough
for its windows.

**WP-35 — Collapsing the chrome in ckmux** *(ckmux)*. Consume U4-l's toggle:
hide the footer, leaving the docked `Column` one row so the bar sits on the
last row; restore both together. The mechanism is already in place — WP-32
hides the bar itself and calls `Desktop::on_child_size_hint_changed` by hand
because `View::set_visible` does not report a hint change (U4-f), and this is
the same move on the other child. *Done when*: collapsing hides the footer and
puts the bar on the last row, expanding restores both, and a maximized
terminal grows into the freed row and gives it back.

*Landed* (2026-08-19), with one addition the package did not ask for and needs:
a **command** beside the strip's toggle — `Window ▸ Status Bar`, `^B b`. The
toggle alone would have been a trap, because the bar is not always on screen:
collapse the chrome, close down to one terminal, and the `▼` goes with the bar
while the footer stays hidden, leaving no way back to it. Both routes set one
state (`ClientApp::set_chrome_collapsed`), so the glyph and the menu cannot
disagree, and the state survives the bar coming and going — the reader's
decision about ckmux's chrome is not undone by a fact about their windows.
Re-entrancy is safe in both directions: `PagedStrip::set_collapsed` assigns
before it reports, so the call that comes back finds nothing left to change.

**U4-i — Minimize: window state, the `_` control, and what a hidden window
does** *(ckVision)*. Reader-directed, 2026-08-19: a third frame control, `_`,
immediately LEFT of the maximize/restore control, which **hides** the window.
Checked before writing: ckVision has no notion of a minimized window anywhere
— this is new state, not a flag being surfaced. `Window` already carries a
`Control` enum with `point_in_close_control`/`point_in_zoom_control`, so the
hit-testing follows an existing pattern rather than inventing one.

Minimized is **not** merely `set_visible(false)`. A hidden window must still
be *listed* (that is how it comes back), so the state has to be legible to a
switcher bar while being skipped by everything that arranges or cycles
windows: the three tilings and the grid must not give a hidden window a band,
`activate_next`/`previous` must step over it, `filled_tile_fractions()` must
not count it, and `tile()`'s exact-cover arithmetic must divide by the number
of *visible* windows or it will leave a gap. Each of those is a place the new
state can be forgotten, which is what the tests are for.

Restoring must put the window back exactly where it was, including its
z-order and its zoom: a window minimized while maximized comes back maximized.
*Done when*: minimizing hides the window and leaves it listed; every tiling,
the grid, the cycle order and the filled query behave as though it is not
there; restore returns bounds, stack position and zoom; and a desktop whose
windows are *all* minimized is still a working desktop rather than one with
an activation pointing at nothing.

**U4-j — Switcher bar: status icons and taskbar click semantics**
*(ckVision)*. Each entry gains a status icon, one space left of the title:
**minimized**, **visible but not active**, and **active**. The reader asked
for rectangles; the implementer picks the exact glyphs and must check them
against ckVision's own width authority first — `□`/`■` (U+25A1/25A0) are East
Asian **Ambiguous**, so a host that renders them double-width would silently
shear every entry's layout. `▯`/`▮` (U+25AF/25AE) and `▄` (U+2584) are
Neutral and safer; verify rather than assume, and fall back to ASCII if the
width tables disagree. The glyphs belong in one table beside the roles, not
inline at the draw site.

Click semantics become the taskbar's, which is what the reader described and
what the two halves of their request resolve to together: clicking the entry
of the **active** window minimizes it; of a **visible but inactive** window,
activates and raises it; of a **minimized** window, restores it. This
replaces today's always-activate. *Done when*: each of the three transitions
is driven through a real click and asserted, the icon matches the state after
each one, and a right-click still opens the menu without changing any state.

*Landed* (2026-08-19). The glyphs are `▮`/`▯`/`▄` (U+25AE/25AF/2584) in one
`status_glyph(Status)` table the widget exposes, so a legend or a host's own
listing names the same shapes the bar draws. The width check was run against
ckVision's own tables before a glyph was chosen and it confirmed the warning
above: `■`/`□` are East Asian Ambiguous, while all three chosen glyphs are
neither Wide nor Extended_Pictographic. All three are also **one cell**, which
is what keeps U4-m's damping honest — a window minimizing and restoring does
not change its button's length, so the row cannot re-flow on a state change.

The click policy went into the widget rather than into each host, and the seam
that made that possible is two verbs instead of one: `set_activate_action`
(unchanged) and a new `set_minimize_action`. The bar decides which of the
three transitions a click is; a host that installs its own activate action —
ckmux does, to move the keyboard with the window — keeps the semantics for
free. A minimized row and a visible-but-behind row both go through activate,
because `Desktop::activate` restores a hidden window on its way to the front:
one verb, not two, for "bring me that one". State comes from a
`set_minimized_provider` defaulting to `Window::minimized()`, so a host whose
windows are put away by a model of its own has the same seam the label and the
active window already had; where a host's providers answer both, **minimized
wins**, because a row drawn as the reader's current window while that window
is nowhere on screen is the one lie a taskbar must not tell. A window whose
`minimizable()` is false is never minimized from its row. ckVision D-059.

**U4-o — a standard `ckv.window.minimize` command** *(ckVision)*, found while
building WP-34 and landed with U4-j. U4-i gave every window a `_` control, and
that control is the one a reader cannot conveniently aim at for the window
they are already working in — a menu item and a key are how they reach it, and
every ckVision application with a Window menu wants that row. So it is the
library's command, declared LAST in `CommandRegistry` so no existing id moves,
with a `Desktop::minimize_active_window()` default handler beside `kZoom`'s
that refuses a window whose `minimizable()` is false.

**U4-k — The minimize animation** *(ckVision)*. The frame shrinks and flies to
the entry it will live in, Windows-95 style, and back on restore. ckVision has
no animation machinery at all today — `Application::start_timer` is the whole
mechanism — so this package is as much about deciding what animation *is* in
this toolkit as about one effect.

Constraints that are not negotiable, because a TUI is not a game loop: it must
never block the event loop, must be interruptible (a keystroke or a second
minimize during the flight resolves immediately to the end state), must be
skippable/disable-able, and must degrade honestly on a slow host — if frames
cannot be delivered on time the window simply arrives. The end state must be
reached exactly once whether or not the animation ran, so nothing downstream
can depend on the frames. *Done when*: the end state is identical with the
animation on and off; interrupting mid-flight leaves a coherent desktop; and
a host that never gets a timer tick still ends with the window hidden.

**WP-34 — Minimize in ckmux** *(ckmux)*. Consume the three above — two of
them, as it turned out: **U4-k, the minimize animation, is not a dependency
and remains open.** It is the flight between the window and its row, and
everything here is about where the window and the row ARE, which the end
state answers whether or not a frame of the flight was ever drawn (which is
U4-k's own first constraint). The bar's
visibility rule gains a second clause, reader-directed: it is on screen when
more than one terminal is open **or when any terminal is minimized** — with
one window hidden and none visible, the bar is the only way back, so hiding it
would strand the reader with an empty desktop and no route to their program.
*Done when*: minimizing the only terminal makes the bar appear and the
terminal restorable from it; the ckmux menu offers minimize beside zoom; and
`^B` chords for it, if any, go through the one `Keymap` like everything else.

*Landed* (2026-08-19). `sync_window_switcher()` now reads
`listed.size() > 1U || any_minimized`, asked of the windows themselves rather
than tracked beside them — the desktop already knows, and a second record
could disagree with it. The block U4-n was carrying is real and is gone: the
bar appearing changes the dock's height, which is a content-area change, which
before ckVision `0e7c013` corrupted a tiling on every minimize and restore.
The one-line fix really was not one line, and it was not this line.

Three routes reach the verb, all through the one `Keymap`: `Window ▸ Minimize`
(U4-o's standard command, beside Zoom, which is where the window's own frame
puts the two controls), `^B _` (named for the `_` control the reader can see),
and the row's own context menu, where it sits at the top and reads **&Show**
for a window that is already away — a second "&Restore" would collide with the
un-maximize row below it. Clicking the row itself is U4-j's, and ckmux adds
the two side effects its activate action already had: the keyboard follows the
window, and the footer is re-read.

Two things this package deliberately does not do, both verified rather than
assumed. **A minimized terminal does not survive a detach and reattach**:
`WindowPlacement` on the wire carries `zoomed` and a rect but nothing about
hiddenness (plans/05), so `apply_layout` brings every terminal back on screen.
That is a wire change plus a session-state field, which is WP-28/WP-30's
territory rather than this one's, and it is recorded here so the next person to
open that file knows the field is missing on purpose. **Reaching a hidden
terminal by number already works**: `^B 3` goes through `Desktop::activate`,
which restores on the way to the front, and `^B n`/`^B p` step over hidden
windows, both of which are U4-i's doing and neither of which needed anything
here.

Verified end to end as well as headlessly: `tests/test_minimize_chrome.cpp`
drives the real client, and `tests/test_menu_commands.cpp` runs the real
binary in a real terminal, presses `^B _`, and reads the screen — the window
leaves it, the bar arrives with `▄ Terminal 1` on it, and a click on that row
brings the terminal back and takes the bar away again.

**WP-32 — Window Switcher Bar integration in ckmux.** Wires ckmux's
terminal windows into U4-a's bar: the title provider (the same
numbering/caption logic `open_terminal` already assigns), the right-click
menu's contents (Maximize, Close, Move to session…, and whatever else the
standard Window menu already offers per-window), and activation (click →
focus and raise that terminal's window). Positioned above the existing
footer, as specified. Depends on U4-a. *Done when*: the bar lists every open
terminal window with its real caption, stays correct across open, close,
rename and move-to-session, and a maximized terminal's bounds visibly leave
the bar's row uncovered.

**WP-33 — Maximize-follows integration in ckmux.** Enables U4-c for
ckmux's terminal windows: a new terminal opened while another is maximized
opens maximized too, instead of at its usual cascade position. Depends on
U4-c. *Done when*: with a terminal maximized, `^B c` opens the new one
maximized; with none maximized, cascade placement is unchanged.

### M4 — fidelity

Any app, correctly. [04-terminal-emulation.md](04-terminal-emulation.md) is the
spec and holds the reference-app matrix.

Three of its ckVision items were startable without M2 and are now **done**
(2026-08-17): **U1-a** tab stops, **U1-c** Media Copy capture behind a policy
defaulting to deny, and **U1-d** the rest of the typed dialog fields —
`Radio`, `Combo`, `Number`, beside the `Check` and `Note` that landed for
`Settings ▸ General…`. They were emulator and widget work in files nothing in
M2 touches, which is exactly why they could be taken while the server was not
the thing to work on. PRINT-1…6 waited only on the wire, and the wire has been
real since WP-6: it is `Ready`, with PRINT-2's messages already encoding and
decoding and nothing yet producing or handling one ([05](05-protocol.md)).

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

**WP-16 Sixel over the wire** — *done, 2026-08-18 (`5e930a6`)*: chunked image
add, place and remove all have a producer in `diff_engine.cpp` and a handler in
`server_session.cpp`, the client's outer-host capability travels on `Attach`
(`host_sixel`) so the advertisement a child is given reflects the terminal the
picture will actually be drawn on, and a mirror's rasters carry a scene
identity of their own because a mirror is never adopted by the `Application`.

The rest of M4 is `Ready` rather than waiting: **WP-18** the paste
credit flow, **WP-19** bell and activity monitors, and **PRINT-1…6** the
virtual printer, whose six pieces are
already itemized in [10-roadmap.md](10-roadmap.md) and behave per
[04](04-terminal-emulation.md)/[06](06-ui-design.md)/[08](08-configuration.md).
**WP-20** the performance budget gates under floods, freed by WP-16.
**WP-21** M4 acceptance:
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

### Added after the M3 queue was drawn

**U4-m — Damped switcher-bar entry widths** *(ckVision)*. Reader-directed,
2026-08-19. `WindowSwitcherBar::set_width_damping(grow, shrink)`: a floor under
how often one entry's box may change width, measured on the injected `Clock`
from that entry's own last change in either direction. Off by default. See
[07](07-ckvision-integration.md) U4-m for the design and for why the damped
number is the LAYOUT width rather than a drawn one. *Done when*: a caption
rewritten sixty times over the grow delay costs the layout one width step;
a deferred change arrives with no further title change to carry it; a window
the bar has not measured takes its width at once; turning damping off releases
every held width; and a bar destroyed before its wake-up fires leaves nothing
to fire into (ASan).

**U4-n — `PagedStrip::press_in_flight()`** *(ckVision)*. The protected
predicate U4-m needs, landed as ckVision `850711f` by the strip's own author.
The strip resolves a click by index, so a relayout under a held pointer either
loses the click or spends it on a window the reader never pointed at. Recorded
in [07](07-ckvision-integration.md) with the note that this is the **third**
ckmux consumer to want a fact the library keeps private (see U4-e).

**WP-36 — Rename Terminal** *(ckmux)*. Reader-directed, 2026-08-19. A custom
title a reader gives one terminal, reached from `Terminal ▸ Rename Terminal…`,
from `^B ,`, and from `Rename…` on the window bar's context menu — where, like
every other entry there, it acts on the row that was clicked rather than on the
window in front.

The design turns on one thing: the name is an **override**, not a second writer
of the caption. The child goes on claiming titles with OSC 0/2 underneath it
and is simply not shown, so the dialog's third button, *Use Default Title*,
hands the caption back to what the program is calling itself **now** rather
than to whatever it said when the reader pinned it. An empty name asks for the
same thing, so a reader who clears the field is answered rather than refused —
the one place this deliberately differs from `rename-session`, where empty
means "leave it alone".

Session state, so it lives on the server ([03](03-session-model.md)): the
long-dormant `RenameTerminal` message gets its handler, `TerminalState` and
`TermMeta` each carry a `custom_title` beside the child's title, and the name
therefore survives a detach and reaches the other client watching that session.
An M1 client with no server keeps its own, through the same `ClientOptions`
seam every other session operation uses.

It also consumes U4-m: the window bar's buttons are damped so a caption that a
shell rewrites at every prompt stops re-flowing the row — one widening a second
at most, one narrowing every thirty — while a name the READER gave takes effect
at once, because damping is for absorbing what a program does and a rename that
visibly worked half a minute later reads as a command that did not work.

*Done when*: a pinned caption survives the child renaming itself repeatedly;
*Use Default Title* restores the child's CURRENT title, and `Terminal N` when
the child has handed its own title back; an empty field does the same; the
prompt starts from the caption the reader can see; Cancel changes nothing; the
command is unavailable with no terminal to name; the bar's context-menu entry
names the row that was clicked; a name given over the wire comes back on
`TermMeta`, is untouched by the child renaming itself, and is in the snapshot
the next client attaches with; and naming a terminal that is gone is answered
with an `Error` rather than ignored.

### Per-terminal process activity: CPU and memory (2026-08-19)

Reader-directed, 2026-08-19. Every terminal has a root child (the shell) and,
under it, whatever the reader is actually running — and the question "what is
this terminal costing me?" has an answer the operating system already keeps:
the CPU time and resident memory of that process tree, sampled about once a
second and shown on the terminal's own frame. The reader asked for a `View`
menu with checkable items — Show CPU Usage, Show Memory Usage — whose state
persists in the configuration file like every other setting and whose toggling
applies at once to **every open terminal window in the session**, not to the
active one alone.

**Checked against both trees before these packages were written, which is what
sized them** (the same discipline that reshaped U4-b): ckVision's menus already
carry checkable items (`MenuMark::Checked`/`Unchecked`, `with_mark` —
`widgets/menu.hpp` even uses a display toggle as its own example), and
`widgets::Window` already owns the display slot: `set_footer()` puts a
left-aligned line on the BOTTOM border, elides it on a narrow window before it
can collide with the resize grip, and its header describes its purpose as
"what this window is currently costing, counting, or waiting for" — which is
this feature, described before it existed. Nothing in ckmux writes that footer
today. So there is **no new widget work anywhere in this cluster**; what is
genuinely new is one pid accessor upstream, a platform sampling seam, a
protocol message, and the client glue. (Terminology, because ckmux overloads
the word: "the footer" in every other package is the bottom `StatusLine`;
this cluster writes the **window frame's footer**, `Window::set_footer`, and
says so at each use.)

Four decisions, settled here so the packages stay mechanical:

  * **The server samples, because the server owns the pids.** The client may
    be on the other end of a socket from the processes it is asking about, and
    a detached session has no client at all. So sampling is server work, the
    numbers travel as a protocol message, and the client only formats and
    displays. The serverless M1 client samples locally through the same
    platform seam — the display code cannot tell where its numbers came from,
    which is the same seam discipline `terminal_source` bought WP-5.
  * **Nobody watching, nobody sampling.** Stats exist for a reader who turned
    a checkbox on. The server samples only while at least one attached client
    has asked, and stops when the last one detaches or unsubscribes — a
    detached server must not spend a wakeup per second measuring numbers
    nobody will see. For the same reason stats are **never session state**:
    not stored, not in snapshots, gone a second after they stopped being true.
    A client that subscribes has current numbers within one sample period,
    which is all "current" can mean.
  * **Two memory numbers, because one of them lies when summed.** Summing RSS
    across a tree counts every shared page once per process that maps it — a
    shell and its child share libc, and naive addition bills it twice. So the
    seam reports both `rss_bytes` (the number every tool shows, comparable
    across platforms, honest about its overcount) and `real_bytes` — the
    platform's own "what does this actually cost" metric: `phys_footprint` on
    macOS (what Activity Monitor calls Memory), PSS on Linux (when WP-22
    fills that seam in), private working set on Windows (parking lot). The
    menu word is **Real**, not PSS, because PSS is Linux's *implementation*
    of the idea rather than the idea; the help page says which metric each
    platform provides.
  * **CPU is a rate the consumer derives, not a number the seam invents.**
    The seam returns cumulative CPU time per sample; CPU% is the delta of two
    samples over the wall time between them — percent **of one core**, the
    `top` convention, so a build fanning out over eight cores reads 800%
    rather than being flattened to a fraction that hides it. Keeping the
    clock out of the seam is what makes every rate computation testable
    without waiting a second per assertion.

Not in this cluster, deliberately: an aggregate per-session readout in the
session picker or the switcher bar. It falls out of the same wire message the
day someone wants it, and a package that exists is not padded with one that
might.

**U5-a — `process_id()` on `PosixTerminalSubsession`: a pid for observation**
*(ckVision)*. The child pid is private today, and its header says why: hosts
signal through `request_termination()`/`terminate_now()`, and "a host reaching
for the pid is exactly what this class exists to own." That reasoning is about
**control**, and it stands unchanged — nothing in this cluster sends a signal.
Observation is a different act: a host that wants to *measure* a process tree
needs to know where the tree is rooted, and there is no wrapper that could
answer for it, because enumeration and sampling are platform work the library
does not do. So: a public `process_id()` accessor (−1 once the child is
gone), documented as for observation, with the signalling rule restated where
it is declared. *Done when*: the accessor returns the spawned child's pid
while it runs and −1 after `close()`, and the class's own comment still tells
a host how to end a child without it.

**WP-37 — Process-stats seam: the tree and what it costs, macOS first.**
`src/platform/process_stats.{hpp,cpp}`, beside the poller and paths seams the
Linux port already expects to fill. The interface is two calls: snapshot the
process table once (pid and parent pid for every live process), and sample
one pid's counters (cumulative user+system CPU nanoseconds, `rss_bytes`,
`real_bytes` with a flag saying whether the platform can). Tree resolution —
descendants of a terminal's root child — is a pure walk over the one
snapshot, so **N terminals cost one table walk per second, not N**, and the
walk is testable with a hand-built table. macOS first: `proc_listallpids` +
`proc_pidinfo(PROC_PIDTBSDINFO)` for the table, `proc_pid_rusage
(RUSAGE_INFO_V4)` for the counters (`ri_user_time`/`ri_system_time`/
`ri_resident_size`/`ri_phys_footprint`) — all of it answerable for the
server's own children with no entitlement, because they are the same uid.
The Linux implementation (`/proc` scan, `smaps_rollup` for PSS — noting that
PSS is the one expensive read, which is another reason nobody-watching does
no work) is **added to WP-22's scope by this package**; Windows goes with
the parking lot's ConPTY entry. Two honest edges, stated in the header: a
pid may die between the table and the sample — it is skipped, never an
error, and a tree whose root is gone reports empty; and a process that
daemonized has reparented out of the tree and is deliberately no longer
counted, because it is no longer this terminal's cost.
*Done when*: with real children — a spinner's CPU time advances between two
samples; a child that allocates and touches tens of megabytes shows them in
`rss_bytes`; a grandchild through an intermediate shell is counted in the
tree; a dead root reports empty; `real_bytes` on macOS is nonzero and below
the naive RSS sum for a tree sharing libraries — and the cost of one full
pass over this machine's own process table is **measured**, recorded here,
and small enough for a once-per-second tick to carry.

**WP-38 — Stats on the server tick and on the wire.** Two messages, into
[05-protocol.md](05-protocol.md)'s catalogue when this lands (WP-1's pinned
variant count will insist): `WatchStats{on}` from an attached client, and
`TermStats{term, cpu_permille, rss_bytes, real_bytes, flags}` per terminal
per sample to each client that asked — `cpu_permille` of one core so the
integer carries 0.1% steps, `flags` saying whether `real_bytes` means
anything and whether the child is still alive to be measured. The sampler
rides the loop's existing tick machinery (`flush_tick`'s pattern, driven by
the injected `Clock`) at a fixed 1 Hz — the interval joins plan 08's
"explicitly not configurable in v1" list at landing — and holds the previous
sample per terminal to derive the rate. It runs **only** while someone
watches: subscription is per connection, dies with it, and the first sample
*Done* (2026-08-19). `src/platform/process_stats.{hpp,cpp}`; nine cases in
`tests/test_process_stats.cpp`, all with real children where a child is the
claim (bare `fork`s, no exec and no PTY, so the suite stays in ctest's fast
lane). Measured: a table of **281 processes snapshotted and one tree summed
in 96 µs** — four orders of magnitude inside the one-second tick; and the
spinner read **302 ms of CPU across 300 ms of spinning**, which is the
mach-timebase arithmetic being right rather than lucky (a conversion wrong
by the Apple-silicon 125/3 lands orders of magnitude outside the test's
bounds). The grandchild, dead-root and pid-loop cases are as specified. Two
notes carried forward: `real_bytes` is asserted nonzero and page-accounted
rather than "below the naive RSS sum" — for a child that dirties its pages
footprint and RSS agree, and the overcount claim is about *shared* pages,
which a test would have to stage carefully to pin without flaking; and the
first version of the allocator child was deleted wholesale by `-O3` (a
`memset` nothing reads is a dead store — the child paused with 960 KB
resident and the test failed honestly), so the child now dirties one byte
per page through a `volatile` pointer, which is not deletable.

after a subscription starts from a fresh baseline rather than a stale one.
A terminal whose child exited stops being reported, with one final
not-alive stats message so the client clears rather than freezes the
readout.
*Done when*: at the wire — a subscribed client receives `TermStats` at
sample cadence with a genuinely spinning child reading nonzero permille and
an idle shell reading ~0; a `sleep` and a deliberate allocator differ in
`rss_bytes` the right way; unsubscribing mid-connection stops the flow while
the connection lives on; with no watcher anywhere, an instrumented seam
proves the sampler is **never invoked**; and a `ManualClock` loop test pins
the cadence without a wall-clock second anywhere in the suite.

**WP-39 — The View menu, the config keys, and the frame readout.** The
user-facing half, all client. A new top-level **View** menu
([06-ui-design.md](06-ui-design.md)'s menu table gains it at landing) with
three checkable entries through the existing `MenuMark` machinery: **Show
CPU Usage**, **Show Memory Usage (RSS)**, **Show Memory Usage (Real)** —
independent, any combination. Three `[general]` keys, default off —
`show-cpu`, `show-memory-rss`, `show-memory-real` (wording settled against
[08-configuration.md](08-configuration.md)'s table at landing, where they
become three rows) — read at startup like every key WP-12 covered and
written back through the same `save_setting()` the Settings dialog's
checkboxes already use, so the preference survives the session because the
reader set it, not because they remembered to edit a file. The commands go
*Implementation complete, 2026-08-19 (ckmux-53); the central gate is pending
per the owner's testing directive.* Five wire cases in
`tests/test_stats_wire.cpp` (in-process `Server` on a `ManualClock` over a
real socket, real PTY children where a child is the claim). Measured: a
spinner read **564‰ of one core** against an idle shell's **2‰**, from 400 ms
of real spinning inside one injected second; 2.5 virtual seconds after the
subscription's own first report carried **exactly two** more passes — the
cadence is the clock's, not the loop's; `Server::stats_passes()` stayed at
**zero** across three subscriber-less virtual seconds and froze again at
unsubscribe while the same connection still answered `Ping`; and a held
(`on-exit`) terminal whose child exited was announced not-alive **once**,
with silence for it across three further sampled seconds. The allocator
half of the memory claim lives in WP-37's suite, where it was pinned against
the sampler directly; the wire case asserts live `rss_bytes` and flags
instead of re-staging the allocation behind a PTY. One finding worth its
sentence: the baseline's "does one exist yet" test was `at_nanos > 0`, and a
`ManualClock` starts at zero — so a baseline taken before the first advance
read as absent and a spinner reported 0‰ forever in exactly one test; it is
an explicit `primed` flag now, because a sentinel that collides with a
legitimate value is not a sentinel.

through the one `Keymap` (rebindable; **no default chord** — a display
toggle is not a muscle-memory operation, and the prefix table is a scarce
resource), and the reachability suite covers the menu entries as it covers
every other live-looking item.

Flipping a toggle applies **immediately to every open terminal window in
the session**: one footer composer per client owns the frame footer of
every terminal window and rewrites it from (toggle state × latest stats) —
on toggle, on every `TermStats`, on terminal open, and to the empty string
when all three are off, which restores today's frame byte for byte. The
composed line reads like `CPU 37% · RSS 210 MB · Real 96 MB`, dropping the
parts not enabled; the window elides it when narrow by its own existing
rule. Bytes are auto-scaled — KB/MB/GB/TB, 1024-based, at most three
significant digits (`999 KB`, `1.2 GB`) — by a pure formatter whose unit
boundaries are pinned in a test, because `1024` vs `1000` and `1.0 MB` vs
`1024 KB` are exactly the disagreements a test settles once. CPU above one
core displays as it is (`340%`), per the decision above. A terminal whose
child exited shows no readout — the exit banner owns that moment. Remote
terminals subscribe with `WatchStats` when the first toggle goes on and
unsubscribe when the last goes off; the serverless M1 client samples
*Done, 2026-08-19 (ckVision `da11baa`).* Pinned by a test whose child prints
its own `$$`: the accessor and the child agree on who the child is while it
runs, and the accessor answers −1 once the exit has been observed. ckVision's
full suite green around it, 144/144 with examples ON.

through WP-37's seam on the application timer at the same cadence, and the
composer cannot tell the difference.
*Done when*: `tests/test_command_reachability.cpp` passes with the three
new commands; toggling CPU on puts a readout on **every** open terminal
window within one sample period, is observable as a config-file write, and
a restarted client comes up with the checkbox still ticked and the readout
already live; all-off restores a frame identical to today's; the formatter
boundaries are pinned; and the end-to-end PTY walk asserts the footer's
shape by pattern rather than its numbers, because the numbers are real.

## Not in this queue

The parking lot in [10-roadmap.md](10-roadmap.md) — simultaneous multi-client
attach, tiled pane mode, control mode, resurrect, Windows/ConPTY, kitty
keyboard passthrough and graphics, hyperlink activation, scrollback reflow,
per-terminal palette re-theming, session groups, PDF export (PRINT-7). Nothing
there gets a row until a plan edit moves it, because R5 binds: an addition
edits the plan first.
