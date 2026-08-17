# ckmux — Terminal Emulation

Plan 04 · Status: draft · 2026-08-16 · [Index](README.md)

The VT engine decides whether "any app just works". **Decision: ckmux does
not write a terminal emulator.** ckVision already ships one that matches our
architecture exactly, and the co-development rule applies in full force
here: gaps are *fixed in the library, never worked around in applications* —
by us, directly in ckVision's tree. ckmux's emulation work is therefore
(a) integration and (b) the prioritized gap list below, implemented in
ckVision as part of the ckmux milestones. Full grounding with file
references in [07-ckvision-integration.md](07-ckvision-integration.md).

## What we build on

- **`ckv::term::TerminalEmulator`** (`include/cvision/term/terminal_emulator.hpp`):
  a defensive, *off-screen* xterm-subset emulator. Fed bytes → snapshotable
  cell state. **Headless by design** — no PTY, no process, no UI, no host
  terminal required (it is fuzzed and unit-tested standalone). Runs in the
  ckmux **server**, one instance per terminal.
- **`ckv::term::PosixTerminalSubsession`**: owns the PTY (`forkpty` + `execve`
  with explicit argv/cwd/env, non-blocking master, `TIOCSWINSZ` incl. pixel
  sizes) and embeds the emulator; `drain(byte_budget)` pulls child output in
  bounded chunks; exposes `wait_handles()` (PTY fd) for our poll loop; close
  protocol SIGHUP→SIGTERM→(grace)→SIGKILL on the process *group*. The server
  uses these two classes directly — no `Application`, no `scene`, no host
  `Terminal` (the `term` layer depends only on `core`).
- **`ckv::widgets::TerminalView`**: the client-side widget. Renders a
  snapshot, encodes keyboard input (xterm modifier params, application cursor
  keys), translates mouse per the child's selected encoding (X10/SGR),
  bracketed paste, focus reports, PageUp/PageDown scrollback, Shift-drag
  selection, contained Sixel via `draw_image` with text fallback.

The client feeds `TerminalView` through a small ckmux-owned
`RemoteTerminalSubsession` (implements ckVision's 7-virtual
`core::TerminalSubsession` seam against protocol mirror state) — so the
widget is reused verbatim; see [07](07-ckvision-integration.md).

## Already supported (verified in ckVision today)

UTF-8 with UAX #29 grapheme clustering + wide-char continuation cells
(Unicode 15.1 pinned tables — emulator and renderer share one width
authority, eliminating a classic multiplexer bug class); cursor addressing
(CUU/CUD/CUF/CUB/CNL/CPL/CHA/CUP/HVP/VPA); editing
(ICH/DCH/IL/DL/ECH, ED 0-3, EL 0-2); DECSTBM scroll regions + SU/SD +
IND/RI/NEL; DECSC/DECRC; SGR 0,1,2,3,4,7,9,22-24,27,29, 16/256/truecolor
(38/48;5 and ;2); DECSET/DECRST 1 (DECCKM), 6 (DECOM), 25 (DECTCEM),
47/1047/1048/**1049** alternate screen, **1000/1002/1003/1004/1006** mouse +
focus, **2004** bracketed paste; DECSCUSR cursor styles; DA1, DSR 5/6,
XTWINOPS 14/16 (pixel geometry — Sixel apps ask); OSC 0 title
(policy-gated); **DCS Sixel decode** (4 MiB payload / 4 Mpx caps,
auto-cropped to visible cells); bounded scrollback (primary screen only);
per-oddity `TerminalDiagnostic` instead of crashes, everything fuzzed.

Snapshot state exposed to us includes exactly what a multiplexer must know:
`mouse_reporting_enabled`, `mouse_encoding`, `bracketed_paste_enabled`,
`application_cursor_keys`, `focus_reporting_enabled`, `alternate_buffer`,
cursor, title, rasters.

## Gap plan (the ckmux-driven upstream list)

Priorities: **U0** = needed for ckmux M2 (daily-drivable), **U1** = fidelity
round (ckmux M4), **U2** = when a real app demands it. Each lands upstream
per ckVision's own verification rules; ckmux tracks them in
[10-roadmap.md](10-roadmap.md).

| Pri | Gap (today's behavior) | Why ckmux needs it |
|---|---|---|
| U0 | **No damage/dirty reporting; `snapshot()` deep-copies grid + scrollback + rasters per call** | Server diffing at tick rate × N terminals must not copy ~MB per frame. Need damage spans or a borrowed/zero-copy view, and a snapshot-without-scrollback option. The single biggest perf risk in the design. |
| U0 | **BEL ignored entirely** | Bell flag per terminal (footer/title badges, [06](06-ui-design.md)). Tiny: latch a flag in the snapshot. |
| U0 | **Charset designation `ESC ( 0` unsupported** (fixed diagnostics) | ncurses ACS line-drawing (mc, dialog, old tools) draws `qqq` instead of `───` under xterm-flavored `$TERM`. |
| U0 | **Scrollback capacity fixed at 2 000 lines** | Config `scrollback` ([08](08-configuration.md)) needs a ctor option; also the "0 = off" case. |
| U0 | **RIS (`ESC c`) not implemented** | `reset`(1) inside a terminal must actually reset it. DECALN too (test tooling relies on it). |
| U1 | **OSC 4/10/11 palette/fg/bg queries dropped** | vim/nvim probe OSC 11 to pick light/dark defaults. Needs reply plumbing (emulator can already queue replies via `take_pending_input`). |
| U1 | **Child OSC 52 denied by policy, no allow path** | tmux-style `set-clipboard` ([08](08-configuration.md) `osc52`): needs an opt-in policy + snapshot/event surface, capped. |
| U1 | **Tab stops fixed at 8 (HTS/TBC ignored); REP ignored** | Correctness for the long tail (and vttest). |
| U1 | **Underline styles (SGR 4:x), SGR 21/53/58, colored underline** | nvim undercurl; degrades gracefully meanwhile. |
| U1 | **Mode 1007 alternateScroll untracked** | Wheel→arrow-keys on alternate screen (less/vim wheel scrolling) — needs mode state so the client can translate. |
| U1 | **Media Copy (MC) dropped with a diagnostic** — `CSI Ps i`, `CSI ? Ps i`, DECPFF/DECPEX, printer DSR | **WP-PRINT** (below). `xterm-256color` advertises `mc0`/`mc4`/`mc5`, so a program that prints today gets silence and the user gets no clue why. Needs a printer policy on the profile, a printer-controller parse state, autoprint, honest `CSI ? 15 n` replies, a bounded spool and an out-of-band drain. |
| U2 | **Kitty keyboard protocol (child side)** | Progressive-enhancement apps re-probe; legacy encoding is a correct fallback meanwhile. |
| U2 | **256/truecolor flattened to RGB at emulation time** (indexed identity lost) | Pass-through rendering is unaffected (why it's U2), but palette re-theming of running apps is impossible until indexed colors survive. |
| U2 | XTGETTCAP, DECRQSS, rectangular ops, DECCOLM-as-resize, OSC 8 hyperlinks | Graveyard list; implement when an actual app breaks. |

**Never** (matching upstream policy): Tektronix/ReGIS, DRCS soft fonts,
answerback strings, kitty *graphics* protocol (Sixel is the one image path
for now). Unknown sequences must keep parsing in frame and emit bounded
diagnostics — already the upstream contract.

## WP-PRINT — the virtual printer (printer control, MC)

Printer control was on the never-list, and the reason given upstream — *"a
child must not be able to write to a printer"* — is right about the danger
it names. But that danger is the **sink**, not the sequence: xterm
implements MC by piping the captured stream to a shell command
(`printerCommand`) and ships that empty precisely because `cat`-ing a
hostile file must not reach a device. Replace the sink and the hazard is
gone. **Decision: ckmux captures print output into a bounded in-memory
spool and shows it to the user. No device is ever opened, no command is
ever executed, and nothing is written to disk without an explicit user
click and a user-chosen path.**

What MC does, and what we implement for each form:

| Sequence | Meaning | Our behavior |
|---|---|---|
| `CSI 5 i` / `CSI 4 i` | printer controller **on/off**: every byte goes to the printer, **not to the screen**, until `4 i` | new parse state; bytes appended to the spool verbatim; terminator matching must survive a split across `feed_output()` calls |
| `CSI 0 i` | print screen (scrolling region only, per DECPEX) | render the grid into a job |
| `CSI ? 1 i` | print the cursor's line | render one line into a job |
| `CSI ? 5 i` / `CSI ? 4 i` | **autoprint** on/off: each line prints as the cursor leaves it | flag consulted by `index()` and the autowrap path; consecutive lines coalesce into one job |
| `CSI ? 18 h/l` (DECPFF), `CSI ? 19 h/l` (DECPEX) | form feed after print; print extent | mode state, honored by the two render forms |
| `CSI ? 15 n` | *is there a printer?* | **honest**: `CSI ? 10 n` ready · `CSI ? 11 n` not ready (capture stopped) · `CSI ? 13 n` no printer (mode `off`) |
| `CSI 10 i` / `CSI 11 i` (xterm HTML/SVG dumps), `CSI ? 10 i` / `? 11 i` | screen dumps, all pages | **not implemented** — diagnostic |

Three modes, set per terminal / per session / globally through the Printer
Settings dialog ([06](06-ui-design.md), [08](08-configuration.md)); the
default is **ask**:

| Mode | Told to the program | Captured | User sees |
|---|---|---|---|
| **ask** (default) | printer **ready** | yes, into the ask-cache (256 KB) | frame button `[ PRINT? · 12.4 KB ]` until answered |
| **capture** | printer **ready** | yes, into the spool (1 MB) | frame button `[ PRINT · 3 · 1.2 MB ]` |
| **off** | **no printer** | no | nothing (today's behavior, one setting away) |

`ask` never blocks or fails the program — a program must not wait on a
human. It gets a working printer immediately; the *user* is asked
asynchronously what to do with the result.

**Overflow is where the design earns its safety**, and the two cases differ
because consent differs:

- **ask-cache full** (nothing has been consented to yet): the whole cache is
  **freed**, the emulator goes to a **sink** — still in printer-controller
  mode, still scanning for `CSI 4 i`, still not writing to the grid, simply
  storing nothing — probes answer *not ready*, and the frame button becomes
  **`[ Print settings ]`**. The user adjusts the policy or the limit and
  re-runs the print themselves. Worst-case memory for an unanswered
  terminal is the ask cache, and zero after it.
- **spool full** in `capture` (the user already said yes): only the
  **in-flight** job is dropped — a truncated job is a lie, not a partial win
  — while completed jobs are kept, because the user asked for them.

Leaving controller mode on overflow would be the tempting shortcut and is
wrong: the program's raw print stream would land on the visible screen.
Sinking, not resuming, is what keeps D-13 true. The three ways out of
controller mode stay `CSI 4 i`, RIS, and child exit.

Emulator surface: the payload leaves through a `take_printer_jobs()` drain,
**not** through `snapshot()` — the snapshot carries three scalars (mode,
capturing, bytes) for the button and nothing more, so the U0 deep-copy
problem is not made worse. Everything above lands upstream in ckVision
under a policy that defaults to *deny*, so no other ckVision application
changes behavior ([07](07-ckvision-integration.md) U1-c).

## Sixel pipeline (exists end-to-end; ckmux adds the transport)

```
child app ──DCS q──▶ TerminalEmulator (server)     decode → RGBA Image,
                                                    crop to visible cells,
                                                    publish TerminalRaster
        ── ckmux protocol: ImageAdd/Place (plan 05) ──▶
client mirror ──▶ TerminalView.draw_image ──▶ ckVision Compositor
   (occlusion-sliced against overlapping windows/menus)
        ──▶ Presenter: re-encodes each visible slice as Sixel to the host
             (encoding cache keyed by image fingerprint), or emits the text
             fallback cells when the host lacks Sixel — zero raster bytes.
```

Window overlap, dragging, z-order, and no-Sixel hosts are therefore already
correct by construction (this is ckVision's D-042 "no child bytes ever reach
the host terminal" pipeline — the same property gives us
sanitization-by-construction, [02-architecture.md](02-architecture.md)).
ckmux decides `sixel = auto|off` per host capabilities and reflects the
choice in the child's capability profile; the emulator carries it through to
what a probing child is told — DA1 parameter 4 and the XTSMGRAPHICS limits
(register count, and the child window as the maximum geometry). Without that
advertisement a graphics-drawing program never emits a picture at all, so the
pipeline below has nothing to composite however capable the host is.

## Input path (decision revised from the first draft)

**The client encodes input, not the server** — because `TerminalView`
already implements the full encoder (keys incl. modifier params and
DECCKM-awareness, mouse translation honoring the child's active
protocol/encoding from mirrored mode state, bracketed paste, focus reports)
and we reuse it verbatim. The wire carries opaque
`Input{term, bytes}` ([05-protocol.md](05-protocol.md)); the server owns the
PTY write queue and backpressure. Mode state used for encoding is the
mirrored snapshot state — the same information a standalone terminal
emulator has, with tick-latency staleness that is harmless in practice
(same race exists between any app and any terminal).

Paste: `TerminalView`'s bracketed-paste handling is kept, but large pastes
are intercepted by the client and sent as credit-based `PasteChunk`s so the
server can pace PTY writes ([05](05-protocol.md)). Paste payload
sanitization (strip ESC/C0 except `\n`/`\t`) follows ckVision's
already-sanitized paste events.

## `$TERM` & child environment

- v1 default `TERM=xterm-256color` — entries exist everywhere; our
  supported set plus the U0 gaps covers what programs actually use. Honest
  DA/DECRQM replies keep probing apps truthful. Revisit after U0 lands:
  validate mc/dialog line-drawing (the acsc/`ESC ( 0` case) — if the U0
  charset gap slips, ship an interim `TERM` entry without `acsc` instead.
- M5: ship a `ckmux-256color` terminfo describing exactly what we do, then
  flip the default (config `term` overrides either way).
- Environment set per terminal: `COLORTERM=truecolor`, `TERM_PROGRAM=ckmux`,
  `TERM_PROGRAM_VERSION`, `CKMUX=<socket>,<session-id>` (nesting
  detection); inherited `TMUX`/`STY` scrubbed. ckVision's launch spec takes
  explicit env — nothing ambient leaks in, which suits us.

## Semantics to pin down with tests (not assumptions)

Golden-test these against the upstream emulator early — they are the classic
multiplexer bugs, and where behavior differs from xterm we file it upstream
rather than patching around:

1. Pending-wrap (autowrap at last column) exactly per xterm.
2. Resize: cursor clamp, scroll-region reset, shrink pushing bottom lines to
   scrollback, **no reflow in v1** (reflow is a recorded later feature).
3. ED 2 keeps scrollback; ED 3 clears it.
4. Alternate screen: no scrollback capture; 1049 save/restore pairing.
5. Wide char at last column; combining marks at wrap; zero-width joiners.
6. Mouse encoding transitions mid-drag (app flips 1002→1003).
7. Title with adversarial bytes (already sanitized upstream — regression
   coverage from *our* side).

## Reference apps (the compatibility bar)

Exercised per milestone, scripted where possible ([09-testing.md](09-testing.md)):
`vim`/`nvim` (truecolor, mouse, bg-probe → U1, undercurl → U1), `htop`/`btop`,
`mc` (line-drawing → U0 charset, mouse menus, subshell), `less`, `fzf`,
`emacs -nw`, `lazygit`, `ssh` with remote tmux, **`tmux` inside ckmux** (we
are just a terminal to it — great conformance oracle), `img2sixel`/`chafa`/
`lsix` (they probe DA1 and XTSMGRAPHICS before drawing), `yes`/`cat bigfile`
floods, vttest chapters 1-3/8/11 manually per milestone.

## Performance budget

- Flood (`yes` 10 s): server < 1 core, memory flat, client interactive —
  relies on `drain(byte_budget)` fairness + tick-coalesced deltas
  ([05](05-protocol.md)) + the U0 damage/no-copy snapshot work.
- `cat 100MB` ≤ 2× a plain terminal's wall time.
- Steady-state attached idle: 0 wakeups beyond poll timeouts (no polling
  loops, no timers firing when nothing changes).
- Budgets enforced as deterministic gates in benchmarks, ckVision-style
  (`bench_terminal.cpp` upstream is the model — counts, not wall-clock).
