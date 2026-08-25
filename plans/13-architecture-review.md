# 13 — Architecture Review Findings (2026-08-18)

Status: remediation complete — R1–R10 done · source: multi-agent code/architecture review
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

## Field defects after remediation (2026-08-18, night)

Reported live from a running ckmux + cksetup session, after the remediation
above landed: cksetup's own Terminal Report, opened inside a ckmux terminal
window, showed `Mouse events seen: 0` (the outer host had decoded 869) and
`SIXEL graphics: no` (DA1 with no parameter 4) despite the outer host
supporting both.

- **SIXEL — confirmed, fixed.** `run_client.cpp:65` read
  `terminal.capabilities().sixel_graphics` and called
  `session.set_host_sixel(...)` exactly once, at the top of
  `run_attached_client`, before the event loop had processed a single
  iteration. `baseline_capabilities().sixel_graphics` is `false`
  (`capabilities.hpp:228`), and the DA1/XTSMGRAPHICS probe that would refine
  it resolves asynchronously through the terminal's own `poll()` — so
  `host_sixel_` was cached `false` for the life of the client, and every
  `Attach` this client ever sent carried `host_sixel = 0`, regardless of what
  the reader's real terminal offered (`terminals.cpp:75`:
  `profile.sixel = settings.sixel == Auto && spec.host_sixel`). Fixed at the
  right layer: ckVision's `Application` already swallowed the resolving
  `CapabilityChangedEvent` internally (a repaint, `application.cpp:744`) with
  no way for a host to observe it — added
  `Application::set_capability_changed_handler(std::function<void()>)`,
  fired in the same branch, after the repaint. `run_client.cpp` installs a
  handler that re-reads `terminal.capabilities().sixel_graphics` and calls
  `session.set_host_sixel(...)` again whenever it fires, so a terminal opened
  after the probe resolves — which is the overwhelming majority of them —
  advertises correctly. No test existed for either half; none is added here
  beyond the fix landing (a `HeadlessTerminal`-based host has no probe race
  to reproduce against — the bug is a real-terminal-only timing hazard).
- **Mouse — investigated exhaustively, not reproduced against current HEAD.**
  Two independent tests were written and both pass on the tree as it stands:
  `a_click_the_child_asked_for_reaches_it_over_the_wire`
  (`tests/test_remote_terminal.cpp`) drives the encode/status chain directly
  (child enables SGR 1000+1006 → wire modes → mirror → `status()` →
  `TerminalView::on_mouse` → `send_input`), and
  `a_click_inside_a_remote_terminal_window_reaches_the_child_over_the_wire`
  drives the same scenario through a real `Application`/`Desktop`/`Window`
  dispatch and `topmost_view_at` hit-test — the layer the first test
  bypasses. Both confirm bytes reach `proto::Input` correctly. Ruled out
  along the way: bit-shift mismatch between `modes_of()` (producer) and
  `status()` (consumer) for the new tracking-level bits — none, bits 7–8
  consistently on both ends; a config-parsing regression zeroing
  `[terminal] mouse` — the key still parses correctly and defaults `true`;
  `TerminalView::on_mouse` reading `profile()` as a gate — it does not, only
  `status()`; `Window::on_mouse` or the new D-051 scrollbar frame overlay
  stealing the click — `Window::on_mouse` returns `false` for anything that
  is not its own chrome, and the scrollbar is a frame overlay, not a content
  sibling. Given this session's remediation bumped the protocol to v3 and
  landed the mouse-tracking-level and kitty-flag work only hours before this
  report arrived, the leading explanation is that the running client and/or
  server predate this session's commits. **Action for whoever reproduces
  this next: rebuild both binaries from current HEAD, `ckmux kill-server`,
  and restart before retesting** — if the symptom survives a clean
  rebuild+restart, that rules out staleness and the next step is a real
  two-process reproduction (`test_core_promise.cpp`'s pattern: a forked
  server, a real socket, a real host terminal or a recorded one) rather than
  the synthetic harnesses used here.
  - **Follow-up (2026-08-18, later that night): staleness checked directly,
    the named real two-process reproduction written, still not reproduced.**
    Reported live again from a running `build/ckmux` + `cksetup` session
    (`Terminal 1`, grid 119×41 inside it): both Terminal Reports now showed
    "Mouse events seen" == "Mouse reports decoded" on **both** sides (338/338
    outer, 71/71 inner) — the specific 0-vs-decoded symptom above was absent
    — yet the reader reported clicks still doing nothing. Checked before
    writing any new test: `build/ckmux`'s binary mtime (19:48:54) is 5 minutes
    behind HEAD, but the only commit it lacks is `a2f044c` (the CLI
    kill-server fix, unrelated to mouse); ckVision's checkout is clean at
    `3f11135`; `cksetup`'s binary (a separate tree,
    `~/git/cworks_dir/cworks`, which resolves ckVision via the same
    `/Volumes/PRO-BLADE/git/ckvision` checkout — see its `CMakeLists.txt`'s
    search order) was last built at 19:06:42, after `3f11135` landed. All
    three are current; staleness is not the explanation this time. Wrote the
    two-process reproduction the previous note asked for, as two new
    `Reader`-driven (`PosixTerminalSubsession`, real PTY, real decoded grid)
    cases in `tests/test_menu_commands.cpp`:
    `a_mouse_click_on_the_terminal_report_close_button_closes_it` (ckmux's
    own Terminal Report, opened by keyboard, closed by a real SGR click on
    "Close") and `a_mouse_click_reaches_a_real_child_apps_own_button` (a
    *second*, separate real process — ckVision's own `hello` example —
    launched inside the `Terminal 1` pane's shell, its greeting box opened
    with its own Alt+G chord, its OK button closed with a real SGR click
    forwarded across the wire exactly as `TerminalView::encode_mouse` would
    encode it). **Both pass.** This closes the gap the note above named — a
    click now has been driven through a real PTY, a real rendered screen, a
    real cross-process forward, and a real second `Application`'s own
    hit-test, coordinates read from the decoded cell grid rather than
    guessed — and still does not reproduce the field report through a plain
    click.
  - **A confirmed mechanism that reproduces the same symptom: Shift+click
    into a pane is reserved for host-side selection and never reaches the
    child, by design.** `TerminalView::on_mouse`'s first branch claims
    Shift+Left unconditionally — before `mouse_reporting_enabled` is even
    read — for the reader's own copy-by-selection gesture (plans/06's mouse
    rules: "Shift + mouse gestures: ckmux-local selection & context menu
    override"; intentional, and correctly so — it is how a reader selects
    text out of `vim` or `htop` without either program's own mouse capture
    getting in the way). Two more `Reader`-driven cases pin the asymmetry:
    `a_shift_click_on_ckmuxs_own_button_still_activates_it` (ckmux's own
    `widgets::Button` has no Shift branch of its own — Shift+click on the
    Terminal Report's Close still closes it) against
    `a_shift_click_into_a_pane_is_reserved_for_selection_not_forwarded` (the
    identical click at the `hello` example's OK button, forwarded through a
    `TerminalView`, does **not** close it — the box is provably still up).
    From a reader's side this is indistinguishable from "I clicked and
    nothing happened": the outer host still dispatches and counts the event
    (it is real input, just claimed for selection), so "Mouse events seen"
    keeps climbing exactly as reported, while a click aimed at a *pane's
    content* — a dialog a child program drew, unlike ckmux's own chrome —
    silently never reaches it. This is not a bug in the traditional sense
    (it is documented, deliberate, and consistent with every other terminal
    multiplexer's Shift-to-select convention), but it is a complete,
    mechanistic explanation for the exact symptom shape reported, and it
    does not require staleness, a resize race, or anything `cksetup`-
    specific to produce. **Action for whoever is chasing a live repro of
    this next: ask whether Shift was held (deliberately, by habit from a
    different terminal's convention, or by a stuck key/trackpad/input-path
    quirk that adds it unasked) before assuming a code-level regression.**
  - Remaining, lower-priority standing hypotheses if a plain, unmodified
    click is confirmed and still fails: (1) a resize race between the
    pane's rendered/hit-test bounds and the child's actual PTY size, in the
    spirit of C3 and `f0e4e60` — try clicking immediately after a
    resize/tile/zoom rather than on an untouched layout; (2) something
    specific to `cksetup`'s own integration rather than the generic
    ckmux/ckVision plumbing exercised here (`~/git/cworks_dir/cworks/csetup`)
    — the tests above use a plain ckVision example and ckmux's own dialog,
    neither of which is `cksetup`; (3) a genuine quirk of the reporter's
    actual outer host terminal: the *same* live report showed "Cell from
    text area" (11×24, from CSI 14 t / grid) at exactly half "Cell from
    window"/"Effective cell" (22×48, from TIOCGWINSZ / CSI 16 t) on the
    outer side only — a real, reproducible host-reporting disagreement,
    most likely a HiDPI CSI-14-t-vs-TIOCGWINSZ mismatch
    (`terminal-emulator-source-analysis` territory) — not obviously
    load-bearing for click routing since SGR mouse reports carry cell
    coordinates directly and `effective_cell_text()` already prefers the
    directly-reported `cell_pixels` over the derived-from-text-area one, but
    unconfirmed and worth checking against the reporter's actual terminal
    app rather than assumed away.
- **`ckmux kill-server` could not reach a mismatched server — confirmed,
  fixed.** Chasing the staleness theory above surfaced this directly: the
  running server was `./build/ckmux` last built at 12:05, hours before the
  protocol v3 bump; `ckmux kill-server` against it printed the refusal
  ("end the running server with `ckmux kill-server`") and then exited —
  the fix the message named was the one thing it made unreachable.
  `main.cpp`'s `kill_server()` already had a comment saying "a server of
  another version can still be killed — that is usually exactly why
  somebody is running this," but the code `return 1;`'d right after
  printing the refusal instead of falling through to send `KillServer`,
  and even if it had, `server.cpp` called `drop(client, ...)` immediately
  after sending `Refuse`, closing the connection before any further message
  could arrive. Fixed on both ends: the server gives a CLI-kind client
  (never a UI client) one bounded 2-second window after a version-mismatch
  refusal to send exactly `KillServer` — no payload, nothing to agree on —
  and honours it; the CLI's own `kill_server()` now sends `KillServer`
  regardless of whether it was refused, instead of giving up. Two tests in
  `tests/test_server_lifecycle.cpp`: the successful kill, and that a
  mismatched CLI connection that asks for anything else (or nothing) is
  still dropped within the window rather than held open indefinitely.

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
(`clipboard.cpp:49` in the review's tree at `530b0f3`; fixed since, so no
current line matches). **M-F3** `write_to_command` blocks the single-threaded
client with no timeout (`clipboard.cpp:70`, same review-time tree). **M-F4**
no fork/exec site resets
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
  **The design decision arrived (reader-directed, 2026-08-19)**, along with
  three related window-management requests it pulled in: a reattaching
  window is moved (never resized, by default) to stay in view, a maximized
  window is restored maximized, a filled-tile layout is restored
  proportionally, resize-if-still-needed is an opt-in setting, plus a new
  Window Switcher Bar, Tile Horizontally/Vertically, and maximize-follows.
  Nine work packages now carry it — [12-work-packages.md](12-work-packages.md)
  WP-28 through WP-33 and U4-a/b/c — replacing this paragraph as the record
  of what M-R3 needs; this note stays as the finding's provenance.

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
