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
| 1 | **U0-a** — attach-an-existing-subsession on `Application` | ckVision | 0.5 d | **Ready** | — |
| 2 | **WP-1** — Protocol codec and framing | ckmux | 3–4 d | **Ready** | — |
| 3 | **U0-b** — damage/no-copy snapshot access | ckVision | 3–5 d | **Ready** | — |
| 4 | **WP-3** — Server-side terminal ownership | ckmux | 2–3 d | **Ready** | — |
| 5 | **WP-4a** — Grid diff algebra | ckmux | 2–3 d | **Ready** | — |
| 6 | **WP-2** — Server process, socket, and lifecycle | ckmux | 3–4 d | Blocked | WP-1 |
| 7 | **WP-4b** — The diff engine on the live snapshot path | ckmux | 2–3 d | Blocked | WP-3, WP-4a, U0-b |
| 8 | **U0-c** — emulator: bell, DEC line drawing, RIS/DECALN, scrollback capacity | ckVision | 2–3 d | **Done** (2026-08-17) | — |
| 9 | **U0-d** — OSC title policy for the ckmux profile | ckVision | 0.5 d | **Ready** | — |
| 10 | **WP-5** — `RemoteTerminalSubsession` and the client mirror | ckmux | 4–5 d | Blocked | WP-2, WP-4b, U0-a |
| 11 | **WP-6** — Attach, detach, reattach, and self-healing | ckmux | 3–4 d | Blocked | WP-5 |
| 12 | **WP-7** — M2 acceptance: the core-promise test and the flood gate | ckmux | 2–3 d | Blocked | WP-6, U0-c, U0-d |
| 13 | **WP-12** — Config v1: the remaining keys and `bind` | ckmux | 3–4 d | **Done** (2026-08-17) | — |
| 14 | **WP-8** — Multi-session server state | ckmux | 4–6 d | Blocked | WP-7 |
| 15 | **WP-9** — Session picker and the takeover flow | ckmux | 3–4 d | Blocked | WP-8 |
| 16 | **WP-10** — Move-terminal between sessions | ckmux | 2–3 d | Blocked | WP-8 |
| 17 | **WP-11** — CLI subcommands | ckmux | 3–4 d | Blocked | WP-2, WP-8 |
| 18 | **WP-13** — Exit policy, banners, and respawn | ckmux | 2–3 d | Blocked | WP-8 |
| 19 | **WP-14** — Notifications and the complete help pages | ckmux | 2–3 d | Blocked | WP-12 |
| 20 | **WP-15** — M3 acceptance: every operation row, driven from menus alone | ckmux | 2–3 d | Blocked | WP-9, WP-10, WP-13, WP-14 |
| 21 | **U1-a** — emulator: tab stops (HTS/TBC) | ckVision | 1 d | **Ready** | — |
| 22 | **U1-c** — emulator: Media Copy capture behind a policy | ckVision | 1–2 d | **Ready** | — |
| 23 | **U1-d** — typed dialog fields: `Radio`, `Combo`, `Number` | ckVision | 2–3 d | **Ready** | — |
| 24 | **WP-17** — Copy mode, completed | ckmux | 4–6 d | **Ready** | — |
| 25 | **WP-16** — Sixel transport, placement, and advertisement | ckmux | 4–5 d | Blocked | WP-6 |
| 26 | **WP-18** — Paste credit flow | ckmux | 1–2 d | Blocked | WP-6 |
| 27 | **WP-19** — Bell and activity monitors | ckmux | 2 d | Blocked | WP-6, U0-c |
| 28 | **PRINT-1…6** — the virtual printer, client and server | ckmux | 4 d | Blocked | U1-c, U1-d, WP-6 |
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

**U0-a — attach-an-existing-subsession.** `Application` can only
`launch_terminal_subsession(spec)`, which constructs and owns the session
itself. Its loop already drains every subsession, calls
`notify_terminal_subsession_changed`, and folds each one's `wait_handles()`
into the poll — a ckmux mirror needs exactly that and cannot get in. Add an
adopt entry point over the same owning vector.
*Done when*: a custom `TerminalSubsession` handed to `Application` is drained,
polled, and notified identically to a launched one, proven by a test using a
fake subsession, with the whole ckVision suite green.

**U0-b — damage/no-copy snapshots.** `snapshot()` returns `TerminalSnapshot`
by value — grid, rasters, and up to `max_scrollback_lines` of scrollback, deep
copied on every call. At tick rate × N terminals that is the R1 risk in
[10-roadmap.md](10-roadmap.md). Wanted: dirty spans or a borrowed view, plus
snapshot-without-scrollback.
*Done when*: the server can read a terminal's changed cells without copying its
scrollback; a benchmark records the before/after on the pinned machine; existing
callers are unchanged or updated in the same commit.
*Sequencing*: land this **before** WP-4 hardens the hot path — R1's mitigation
is that the fix goes in the library first, never a ckmux workaround.

**WP-1 — Protocol codec and framing.** Framing, handshake, version integer,
and every message in [05-protocol.md](05-protocol.md)'s catalog, encode and
decode.
*Done when*: round-trip for every message type; property tests (random valid
messages survive; random bytes error and never crash); split reads, coalesced
frames, and cap violations each have a named test.

**WP-2 — Server process, socket, and lifecycle.** The detached server: Unix
socket, single-threaded `poll()`, lazy start, the bind race between two
starters, stale-socket recovery, version-mismatch refusal, `kill-server`. No
UI, no threads, links only ckVision's `core`+`term`.
*Done when*: each lifecycle case above has a wire-level test against a real
server process on a private `CKMUX_SOCKET`.

**WP-3 — Server-side terminal ownership.** One `PosixTerminalSubsession` per
terminal: spawn, drain under a byte budget, resize with pixel fields, the
SIGHUP→SIGTERM→SIGKILL close policy.
*Done when*: PTY-level tests cover exit status, the grace path, and resize;
children never outlive an explicit close.

**WP-4a — Grid diff algebra.** Two grids → `GridDelta` and back: scroll ops,
RLE cell runs, cursor, modes, title, scrollback pushes. Pure functions over
values, with no server, socket, PTY or clock anywhere near them — which is why
this is startable today and why [09-testing.md](09-testing.md) puts it in the
unit layer.
*Done when*: property test — random edit scripts, scroll ops included, applied
to a mirror yield a byte-identical grid; a full-screen scroll costs a `Scroll`
op rather than a cell resend; a decoder fed random bytes errors and never
crashes.

**WP-4b — The diff engine on the live snapshot path.** WP-4a wired to real
terminals: read changed cells through U0-b rather than a deep copy, coalesce
per terminal at the flush tick, monotonic per-terminal `seq`, and the gap rule
(a client observing a gap reconnects and resnapshots).
*Done when*: N terminals diffed at tick rate without copying scrollback, with
the cost measured rather than asserted; `seq` monotonicity has an adversarial
test.

**WP-5 — `RemoteTerminalSubsession` and the client mirror.** The ckmux class
implementing `core::TerminalSubsession` over mirror state, adopted through
U0-a, so ckVision's `TerminalView` is reused verbatim — its input encoding,
mouse translation, selection, scrollback paging, and Sixel drawing all
unchanged. This is the junction where the four independent tracks above meet,
and the first row that genuinely cannot start early.
*Done when*: the M1 UI drives remote terminals with no change to its own
window, footer, or prefix code; input encoded client-side arrives as opaque
bytes.

**WP-6 — Attach, detach, reattach, and self-healing.** `Hello`/`Attach`/
`Detached`, the full snapshot, detach as a closed socket, and the
backpressure rule: on high-water or any observed desync, resnapshot rather
than repair.
*Done when*: an artificially slow client hits high-water and is healed by
snapshot-on-drain, asserted at the wire.

**WP-7 — M2 acceptance.** The core-promise test — spawn a counter, attach,
`kill -9` the client, wait, reattach, golden-compare the rehydrated frame and
see the counter advanced — plus the flood budget gate at protocol level, with
`Ping` round-trip latency asserted *during* a `yes` flood.
*Done when*: both are green in CI, and the author starts daily use.

### M3 — sessions & management

The multiplexer. [03-session-model.md](03-session-model.md)'s operation table
is the specification, and every row of it becomes at least one wire-level test.

**WP-8 — Multi-session server state.** Multiple sessions; create/rename/kill;
id, index, and name rules; `kill-empty-session`; the terminal limit.
**WP-9 — Session picker and takeover.** Startup picker, and the
latest-client-wins transfer: the old client informed best-effort, dropped to
its picker, never able to delay or veto.
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
dispatches the keys.
**WP-15 — M3 acceptance.** Every operation-table row green at the wire; a
newcomer drives everything from menus alone (checklist walkthrough); the
author's tmux muscle-memory set is covered.

### M4 — fidelity

Any app, correctly. [04-terminal-emulation.md](04-terminal-emulation.md) is the
spec and holds the reference-app matrix.

Three of its ckVision items are startable today and none of them waits on M2:
**U1-a** tab stops, **U1-c** Media Copy capture behind a policy defaulting to
deny, and **U1-d** the rest of the typed dialog fields — `Radio`, `Combo`,
`Number` (`Check` and `Note` landed for `Settings ▸ General…`, L-29/L-30). They
are emulator and widget work in files nothing in M2 touches; they sit late in
the order because M2 is the promise, not because anything stops them.

**WP-17 copy mode** — search, vi keys, rectangular selection, clipboard targets
— is likewise startable now. It lives above the `TerminalSubsession` seam, over
a `TerminalView`'s scrollback, so it is written the same way against M1's
in-process terminals as against M2's mirrors; nothing about it changes when the
terminal moves behind the socket. That is worth knowing on a day when the
server is not the thing to work on.

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
