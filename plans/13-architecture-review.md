# 13 — Architecture Review Findings (2026-08-18)

Status: waves 1–2 complete — R1–R7, R9, R10 done, R8 ongoing · source: multi-agent code/architecture review
of 2026-08-18 (7 specialist reviewers over the whole tree, 65 claims
adversarially verified by independent agents — several by probe programs
compiled against the built library — 0 findings refuted). Reviewed at
`530b0f3` plus the working tree of that morning; the reattach-replay audit ran
at the later, clean HEAD.

This document is both the record of the findings and the remediation tracker.
Each topic below carries one state: **open** (nobody is on it), **ongoing**
(a session or agent is working on it), **done** (fixed, centrally tested, and
committed). Update the state in the same commit as the fix — this file is a
plan, and plans lie unless they are edited with the code (11-conventions).

The review's overall verdict, for the record: the design-level engineering is
strong (the layer rule genuinely holds; the codec, diff algebra, and server
loop structure are sound; the testing culture is real). The weaknesses cluster
in one pattern — invariants asserted in comments and plans but not enforced by
code — plus four criticals at the M2 seams. Full narrative with per-finding
verification evidence: the "ckmux Architecture Review" artifact of 2026-08-18.

## The queue

| # | Topic | Findings | State |
|---|-------|----------|-------|
| R1 | Protocol size bounds | C1, M-P1, M-P2, m-str | **done** — encode refuses past-cap frames; snapshots budget-bounded at 4 MiB with fair-shared history; count() takes per-entry minimums; `ScrollbackPush`/`decode_grid`/`from_runs` geometry-guarded; protocol v3 (`ResizeOp`, tracking level, the close/move fields) |
| R2 | Server fairness & loop health | C2, M-S1…M-S5, m-exit-msg | **done** — session-filtered snapshot scan; three-mark queue policy (256 KiB screen gate shared by heal and delta paths, 4 MiB high-water, 32 MiB drop-the-laggard; a repair does not re-mark what it repaired); accept-pause on exhaustion + poll-outcome three-strikes; ask→watch→kill→watch escalation on ckVision `request_kill()` (`c0995f8`); self-exit `TermClosed` off the lifecycle damage flag; stated working directory; geometry ceiling |
| R3 | Mirror geometry & resize | C3 | **done** — `ResizeOp` leads every geometry-changing delta, validation runs against the geometry each op will see, history re-laid at the new width, cursor clamped in; pinned end-to-end over a real socket both directions |
| R4 | Platform fd & signal hygiene | M-F1…M-F5, m-plat | **done** — Debug + ASan suites green; the server-side adoption of `AcceptResult`/`Poller::outcome` is R2's row; the PTY half of the SIGPIPE fix is R7's |
| R5 | Client lifetimes & reattach windows | C4, M-L1…M-L3, M-R4, m-client | **done** — reattach reconciles windows with mirrors (`announced_`/`windows_forgotten()`, heals announce nothing); teardown order provable; single-flight heal; lifetime tokens on every window held across a dialog; Send Prefix delivers any chord via ckVision `send_key`; profile from reader settings; clipboard failures surfaced with the reason |
| R6 | Config & keymap truthfulness | M-T1, m-conf | **done** — config half in `a63c819`; chord spelling unified on plans/06's `^B` with `chord_spelling()` the single authority (`prefix_label` reduced to a forwarder, pinned equal by test), both spellings parse, Ctrl+letter case-folds (`C-B` previously parsed into a chord no terminal can send) |
| R7 | ckVision upstream fixes | U1…U3 | **done** — ckVision `8ddcd53`: five named damage flags + keyboard-flags-as-modes (U1, widened to printer/diagnostics), DECPAM refusal D-053 (U2), mouse-tracking levels D-054 (U3), `TerminalView::send_key`, PTY signal reset, sixel refusal un-deadened; both suites green. ckmux-side adoption of the new flags is R2/R8's work |
| R8 | Reattach feature gaps | M-R1…M-R3, m-replay | **done** (M-R3 carved out) — M-R1, M-R2, M-R4 and m-replay fixed, Debug + ASan + fuzz-corpus green; **M-R3 (layout) deliberately not** — see the note below. Extends protocol v3 rather than bumping it (v3 had shipped nowhere): kitty keyboard flags as `ModesOp` bits 9–13; `TerminalState` gains the clipboard watermark, `exited`/`exit_status`/`hold` and the newest diagnostic; `ClipboardSet` gains its terminal id; `TermDiagnostic` is new. Producers and consumers both ends, with `ClientResize`, `Error` and `Session::focused` picked up from R10's handoffs |
| R9 | Build & enforcement | m-build, layer lint, fuzz targets | **done** — layer rule mechanically enforced (`tools/check_layers.py`, ctest `layer_check` + self-test); install ships `bin/ckmux` alone; `CKMUX_SANITIZE` forwards; source checkout outranks an installed package; `ckmux_platform` is a real target; `-Wconversion` opt-in (`CKMUX_WCONVERSION`); honest label taxonomy (unit/e2e/pty/golden/script/architecture/fuzz-corpus); three libFuzzer targets with 21 checked-in seeds, corpus-gated in ctest |
| R10 | Documentation truth | m-docs, plans/12 queue states | **done** — plans/12 queue re-derived (ten rows corrected, "a Blocked row whose prerequisites are all done is a bug in this file" now stated); plans/05 catalog carries per-row liveness; plans/10 status/risks current, three open questions resolved; plans/02 describes the mechanisms that exist; plans/07 records L-57…L-62 and the real consumption precedence; README done earlier (`a357512`) |

## Critical findings

- **C1 — encode has no size cap; a default-config attach snapshot exceeds the
  decoder's own 16 MiB limit.** `payload_cap()` is decode-only
  (`proto.cpp:997`), `TerminalDiffer::snapshot` walks the whole history
  (`diff_engine.cpp:182`). Measured: 100 cols × 10,000-line scrollback →
  16.29 MiB `Attached` → the client exits with "the server sent something this
  build cannot read"; the session is permanently unattachable. Fix: enforce
  the cap at encode/send and split or bound snapshots (plans/05 already
  asserts the bound; make it true). → R1
- **C2 — the `needs_snapshot` scan broadcasts to every client with no session
  filter** (`server.cpp:786`), and the latch is sticky. One flooding terminal
  in one session converts delta delivery to full-snapshot-per-tick for all
  clients, forever. → R2
- **C3 — a shrinking terminal silently diverges the mirror.** Deltas carry no
  geometry (`mirror.cpp:66`); a smaller repaint passes all bounds checks
  against the larger mirror. Reproduced; no counter fires. Fix: carry geometry
  on full-repaint (or a resize op) and let the mirror resize. → R3
- **C4 — reattach with held mirrors opens no windows.** `ensure_terminal`
  fires `on_terminal_opened` only on mirror creation
  (`server_session.cpp:35`); after takeover-detach the mirrors survive and the
  reattached client shows an empty desktop. → R5

## Major findings

Protocol (R1): **M-P1** `ScrollbackPush` run lengths unvalidated — 449 bytes
expands to 62.9 MB in the mirror (`grid_delta.cpp:337`). **M-P2**
`Reader::count()` reserves count×entry-size — ~738 MB reservation from a 4 MiB
frame, reachable at the server, `bad_alloc` out of a "never throws" decode
(`proto.cpp:175`). **m-str** `Writer::str` silently truncates >65535
(`proto.cpp:41`; assert per AGENTS.md).

Server (R2): **M-S1** send queue unbounded, high-water mark advisory
(`server.cpp:726`; define a disconnect-the-laggard policy). **M-S2**
`accept()` hard failure (EMFILE) spins the loop at 100 % CPU
(`server.cpp:233` with `socket.cpp:270`). **M-S3** "non-blocking" session kill
blocks up to ~3 s per terminal (`server.cpp:595`; use ckVision
`request_termination()` + tick-driven reap). **M-S4** no FD_CLOEXEC on any
socket — every spawned shell inherits the control plane (`socket.cpp:154/236/270`;
fix lives in R4, consumed here). **M-S5** `send_snapshot` clears
`dirty_snapshot` its own overflow just set, and the 256 KiB delta gate vs
`queued()==0` heal gate re-snapshots forever (`server.cpp:722`).
**m-exit-msg** a child that exits on its own produces no `TermClosed` at all
(`server.cpp:466`; pairs with M-R4).

Platform (R4): **M-F1** no CLOEXEC (see M-S4). **M-F2** clipboard helper
inherits the TUI's stdout/stderr — diagnostics land on the drawn frame
(`clipboard.cpp:49`). **M-F3** `write_to_command` blocks the single-threaded
client with no timeout (`clipboard.cpp:70`). **M-F4** no fork/exec site resets
signal dispositions — children inherit SIGPIPE ignored (`process.cpp:58`).
**M-F5** `accept_one` collapses EAGAIN and hard errors (`socket.cpp:270`;
expose the distinction for R2). **m-plat**: `connect()` EINTR; socket dir
umask/symlink check; `Poller::wait` allocates against its own header and
cannot distinguish poll failure from timeout; `Stream::send` discards
`flush()`'s failure; `daemonize` contract comment wrong; XDG absolute-path
rule; `_NSGetExecutablePath` retry; `waitpid` ECHILD handling; lock-file
removal contract.

Client (R5): **M-L1** raw `Window*` across the close-confirmation dialog
(`client_app.cpp:867`; use `View::lifetime_token()`). **M-L2** teardown order
destroys mirrors before the views borrowing them (`run_client.cpp:374`, again
at `:70`). **M-L3** `heal_if_needed` re-sends `Attach` every poll pass with no
in-flight guard (`server_session.cpp:141`). **M-R4** `adopt()` erases learned
exit state (`mirror.cpp:52`) — with m-exit-msg, a dead shell's window comes
back alive-looking after reattach. **m-client**: copy mode holds an uncleared
`Window*`; `close_window_for_terminal` skips copy-mode teardown; silent
status-0 exit on write/framing failure (`run_client.cpp:339`) while read
failure is handled gracefully; `options.client` read after move
(`run_client.cpp:89`); discarded `TerminalRequest`; unguarded `front()`;
Send Prefix is a silent no-op for non-Ctrl-letter prefixes yet advertises a
chord (`client_app.cpp:274`); prefix displayed `C-b` where plans/06 writes
`^B`; the `^B ^B` test asserts nothing (`test_prefix.cpp:157`); `pending_dialogs_`
never pruned; digit-selection order vs caption divergence.

Config & keymap (R6): **M-T1** `kill-grace-seconds` written to `[general]`,
parsed only under `[terminal]` — the Settings dialog cannot save it and the
next start blames the reader's file (`config.cpp:252`; plans/08 says
`[general]`, so the parser moves). **m-conf**: `keys_not_honoured_yet()` lies
about two honoured keys (test pins the lie); duplicate key in a section defeats
`save_setting`; unreadable-but-existing config treated as absent; `#` inside
`exec:` truncates with no quoting; unknown sections unvalidated; warnings name
only the basename; fixed temp name and non-durable rename in `save_setting`;
`apply_directive` indexes `words[0]` unchecked.

ckVision upstream (R7) — per the co-development rule these are made in
ckVision, then consumed here: **U1** three state changes set no damage flag
(Kitty keyboard flags `terminal_emulator.cpp:1265`, `clipboard_serial_`
`:360`, `mark_exited`/`mark_failed` `:2173`) — a damage-gated delta transport
misses all three while attached. **U2** DECPAM/DECPNM application keypad is
unhandled (raises "unsupported"; the numpad is unconditionally numeric) —
decide: implement or record as a refusal. **U3** mouse granularity 1000/1002/
1003 collapses to one bool (the view acts as 1003) — record the refusal or
track it; ckmux's wire is lossless either way.

Reattach gaps (R8): **M-R1** OSC 52 enabled toward the child, then dropped —
no `ClipboardSet` producer, client never rehydrates the serial, no
`on_clipboard_write` handler; plans/08 claims it honoured. **M-R2** Kitty
keyboard flags not transported while `DeclaredProfile` answers the child's
re-probe with ON — legacy fallback disabled, legacy encoding delivered.
**M-R3** layout (`rect`/`z_order`/`zoomed`) snapshot-shaped, never written,
never read; `LayoutDelta` has no producer; reattach cascades default windows.
**m-replay**: bell/activity flags declared and produced by nobody; printer
state has no client-side field; diagnostics ring not transported; no test
drives any mode through snapshot → `adopt()`.

What the fix decided, where a decision was needed:

- **The clipboard travels as a live act, and a snapshot carries only its
  watermark.** A child's OSC 52 says "put this on the reader's clipboard now".
  Carrying the text on the snapshot would replay it on every reattach, over
  whatever the reader had copied since; carrying no watermark would let the
  next real write land on a serial the watching view had already acted on and
  be dropped in silence. So the serial goes, monotonic and never reset, and the
  text goes only on `ClipboardSet` — which now names its terminal, so the write
  reaches the reader through ckVision's own path (the view sees the serial move
  and hands the text to its host) rather than through a second one invented
  here.
- **Activity means output in a terminal the reader is not in**, which is the
  tmux meaning and the only one that makes a window marker worth looking at.
  Bell and activity are both marks — "since the reader was last here" — cleared
  when that terminal becomes the focused one, and sent only when the pair
  changes. A resize the client itself asked for damages every row and does not
  count, or every window a reattaching client lays out would mark itself the
  moment it appeared.
- **The printer and the diagnostics are messages, not `GridDelta` ops**, since
  neither is the grid: `PrintState` already existed for exactly this, and
  `TermDiagnostic` carries the newest complaint rather than the ring, because
  the view paints the last entry and shipping the ring would re-send what a
  client already holds.
- **M-R3 (layout) is not fixed and is not folded into "done" quietly.** It is
  the one gap here that is a FEATURE rather than a correctness hole — a
  reattaching reader sees every terminal, with its screen, its history, its
  modes, its clipboard watermark, its printer and its exit; what they do not
  see is the windows where they left them. It needs a producer for
  `LayoutDelta`, a client that reports its own window geometry as it changes,
  and a decision about what happens when a session is reattached on a smaller
  desktop than it was laid out for — which is a design question, not a
  transport one. plans/05 and plans/12 both say so where it belongs.

Build & enforcement (R9): `cmake --install` ships 119 ckVision SDK files;
`CKMUX_SANITIZE` does not reach ckVision; the "build enforces the layer rule"
comment is false and the promised include-lint does not exist (port ckVision's
`check_layers.py`); all suites labelled `unit` vs plans/09's promised layers;
no fuzz targets; `-Wconversion` absent while geometry narrows unchecked;
`platform` documented as a module but compiled into `ckmux_common`; duplicate
`value_of`; vestigial `if(UNIX)`/MSVC branches; find_package precedence
contradicts the co-development comment beside it.

Documentation truth (R10): README/plans/10 describe the pre-M2 product; stale
header comments disclaim implemented features (`server.hpp:9`,
`server.cpp:483`, `client_app.hpp:6`); eight plans/12 rows Blocked with all
prerequisites Done; plans/05 catalog liveness unmarked (17 dormant message
types); plans/05 layout claim vs no producer; plans/08 `osc52` claim vs M-R1.

R10 handed three of its findings on rather than editing a plan to match a gap,
and R8 picked all three up because each is one end of a message this topic was
already wiring: `ClientResize` now has a sender (the client's loop watches the
host terminal's size and sends one when it moves), `Error` now has a reader
(`ServerSession` routes it and `ClientApp` shows it with its code and its
context), and `Session::focused` is maintained off `Input` — the only statement
of focus this protocol carries, since `Raise` and `FocusTerm` remain dormant.
plans/08's `osc52` claim is true as of M-R1's fix, so nothing there had to
change.

## Working agreement for this remediation

- Sub-agents fix; the coordinating session tests centrally (build + ctest,
  ASan where relevant) and feeds failures back. Sub-agents do not run the
  test suite.
- Proper fixes over workarounds; when the right layer is ckVision, the change
  is made there first (AGENTS.md litmus), then consumed.
- One topic, one commit set; this file's state column updates in the same
  commit.
