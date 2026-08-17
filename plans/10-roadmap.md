# ckmux — Roadmap, Risks & Open Questions

Plan 10 · Status: draft · 2026-08-16 · [Index](README.md)

Milestones are acceptance-based (ckVision's discipline): a milestone is done
when its criteria hold with test evidence per [09-testing.md](09-testing.md),
not when its code lands. Order matters; dates don't (iterative solo
development). The ckVision work items (U0/U1/U2, defined in
[07-ckvision-integration.md](07-ckvision-integration.md)) are **part of
these milestones**: we implement them ourselves, directly in the ckVision
tree under its gates, and a ckmux milestone is not done while its U-items
are open. Expect the U-list to grow as ckmux exercises the library — that
is the point of being its proving ground.

**Status: M0 and M1 are implemented** (2026-08-16; M1's record corrected
2026-08-17, below). What that means in practice, and what it deliberately does
not yet mean, is recorded at the end of each section below.

**This file is not the queue.** A milestone here is a paragraph of intent and a
set of acceptance criteria — it cannot be selected from, and reading M2–M5 as
"what is open" is what let the entire multiplexer look like one line of prose.
The selectable list of packages, each with a state and a prerequisite, is
[12-work-packages.md](12-work-packages.md). Start there; come here to find out
what a milestone must satisfy before it may be called done.

## M0 — Scaffolding ✅

Repo skeleton per [02-architecture.md](02-architecture.md) layout; CMake
consuming ckVision (`find_package`, pinned commit note); CI with
zero-warning + ASan/UBSan lanes; conventions in place
([11-conventions.md](11-conventions.md), AGENTS.md for the repo, copyright
headers, include-layer lint); a smoke app that opens a ckVision desktop and
exits headlessly under the test harness.

**Done when**: clean build + smoke test on macOS in CI; plans reviewed and
open questions below answered (or consciously deferred).

**Landed**: `CMakeLists.txt` consuming ckVision as an installed package or
from a sibling checkout; `cmake/CkmuxSettings.cmake` (warnings-as-errors,
sanitizer switch); `AGENTS.md` carrying the co-development rule and the
provenance rule; `README.md`; the `src/{client,common,server,platform}`
layout with `client` populated; a cktest suite wired one CTest entry per
file; GitHub Actions lanes for macOS, macOS+ASan/UBSan, and Linux (advisory
until M5). The open questions below are still open — they are all
answer-and-edit, none blocked M1.

## M1 — Single-process shell (the look, no persistence) ✅

Start from the pattern of ckVision's `examples/terminal` and build the real
ckmux client UI with **in-process** terminals (no server yet): menu tree,
`StatusLine` footer with context hints, prefix + which-key popup via the
`parent_escape` hook, window management (move/resize/zoom/tile/cascade/
window list/`^B 1-9`), F1 help skeleton, Dark/Light/Mono themes,
confirmations. This de-risks the entire UX and the `TerminalView`
integration before any distributed complexity exists.

**Done when**: vim/htop/mc run in floating windows with mouse + Sixel demo;
every [06-ui-design.md](06-ui-design.md) context has its footer hints and a
golden; prefix chords work with which-key; UI goldens + interaction scripts
green.

**Landed**: the six-menu bar; the permanent footer, regenerated per context
from the keymap table; the prefix on ckVision's parent-escape hook with the
delayed two-column which-key popup; window management (new/close/next/
previous/`^B 1-9`/zoom/tile/cascade/move-resize/window list); close
confirmation through the vetoable close protocol; Dark/Light/Mono themes;
context help generated from the same table that dispatches the keys; window
captions that follow the running program's own title (OSC 0/2) and fall back
to the terminal's ckmux name when it stops claiming one. Tests across six
suites, green under ASan/UBSan, plus PTY runs of the real binary. ckVision
gaps found and fixed upstream
([07-ckvision-integration.md](07-ckvision-integration.md) "Landed").

**Corrected 2026-08-17.** "Window list" stood in that sentence for a whole
milestone without working. The menu item referenced ckVision's `kWindowList` —
the one window command whose default handler `Desktop` did not install — and an
unhandled ckVision command still reports itself *available*, so the item drew
live, accepted its click, and did nothing. It was reported from the running
client rather than caught by a test, which is the part that mattered: nothing
in either suite asked whether a key a surface advertises reaches anything at
all. Fixed in ckVision (L-32 in
[07-ckvision-integration.md](07-ckvision-integration.md)) and pinned here by
`tests/test_command_reachability.cpp`, which walks the whole keymap table and
fails on any binding that is neither handled nor deliberately disabled. The
rest of that sentence was then audited the same way and holds: Window List was
the only dead entry, and `Detach`/`Sessions…`/`^B 1-9` are dark on purpose, by
predicate, as recorded below. Read under D-034's status model, M1's acceptance
did not hold when it was claimed. It holds now, with the gate that proves it.

**Pulled forward into M1** (R5 says an addition needs a plan edit first —
this is that edit): terminals start as **login shells**, resolved the way
tmux resolves them ($SHELL → `getpwuid` → `/bin/sh`, started with a dashed
`argv[0]`), because the alternative was a terminal whose `PATH` differs from
every other terminal on the machine. That is a choice, so it is settable —
which brought with it `Settings ▸ General…` and the first slice of config v1:
the file format, its location precedence, warnings, and the key-targeted
write-back, carrying exactly one key ([08-configuration.md](08-configuration.md)
"What exists today"). The rest of config v1 — the remaining keys and the
`bind` directives — landed on 2026-08-17 as WP-12, ahead of the milestone it
belongs to, because file parsing and a keymap table do not know whether a
server exists. `check-config` is what is left of it, and waits for WP-11's
CLI.

**Deliberately not yet true**: terminals live in the client process, so
quitting kills them — that is precisely what M2 changes. `Detach` and
`Sessions…` are registered, visible, and *disabled*, so the interface shows
its real shape and says plainly that the server does not exist yet rather
than hiding or pretending. Copy mode, rename, and new-terminal-with-command
are M3/M4 and are absent, not stubbed.

## M2 — The split (the promise)

Server process + protocol v1 (`Hello`/`Attach`/`Input`/`GridDelta`/
snapshot/self-healing backpressure) — **the codec and framing of it are done**
(WP-1, 2026-08-17: every message encodes and decodes, fuzzed and
stream-reassembled, with no socket anywhere near it) — one auto-created
session,
`RemoteTerminalSubsession` mirror wired into the M1 UI, lazy server start,
detach/quit/reattach, stale-socket + version-mismatch handling.
**U0-b** (damage/no-copy snapshots) is done too — 2026-08-17, ckVision
`c4f96d2`, measured at 574 µs → 57 µs per terminal read — so R1's mitigation is
in the library rather than pending. **U0-a** — hosting a subsession
the application did not launch, with the release path that makes it two-way —
is done (2026-08-17, ckVision `2355bc3`). **U0-c** — bell, charset, DA1, RIS/DECALN and the scrollback
capacity option — and **U0-d** — the title policy, its bound and the
window-title stack — are both done (2026-08-17, ckVision `08c3dea` and
`2a7d78d`; [07](07-ckvision-integration.md), L-33…L-38).

**Done when**: the core-promise test is green (spawn → attach → `kill -9`
client → reattach → advanced state, golden-verified); author starts daily
use; flood budget gate passes at protocol level.

## M3 — Sessions & management (the multiplexer)

Multiple sessions; startup session picker; new/rename/kill with
confirmations; **move-terminal between sessions**; CLI subcommands
(`ls`/`new`/`attach`/`kill-session`/`kill-server`/`check-config`); of config
v1 ([08-configuration.md](08-configuration.md)) only `check-config` is left —
the keys, the `bind` directives and the rebinding path landed as WP-12 on
2026-08-17; help pages
complete (registry-generated key tables); exit banners + respawn;
`NotificationCenter` toasts; takeover flow polished.

**Done when**: every [03-session-model.md](03-session-model.md) operation
table row has a green wire-level test; a newcomer can drive everything from
menus alone (checklist walkthrough); author's tmux muscle-memory set covered.

## M4 — Fidelity (any app)

U1 emulator round (OSC 4/10/11 replies, child OSC 52 policy, tab stops,
REP, mode 1007, underline styles); Sixel transport + placement + DA1
advertisement end-to-end; copy mode completed (search, vi keys, rectangular,
clipboard targets); paste credit flow; bell/activity monitors; performance
budget gates green under floods; reference-app matrix
([04-terminal-emulation.md](04-terminal-emulation.md)) passes; vttest
chapters recorded. Plus **WP-PRINT**, below.

### WP-PRINT — the virtual printer

Media Copy captured into a bounded spool instead of a device, surfaced on
the window frame, governed by a settings dialog
([04](04-terminal-emulation.md) has the behavior,
[06](06-ui-design.md) the UI, [08](08-configuration.md) the scopes).

| ID | Where | Item | Size |
|---|---|---|---|
| U1-c | ckVision | Emulator MC capture behind a profile policy defaulting to deny ([07](07-ckvision-integration.md)) | 1–2 d |
| U1-d | ckVision | Typed dialog fields (radio/check/combo/number) — today descriptors materialize text inputs only | 2–3 d |
| PRINT-1 | server | `PrintSpool` as session state; job boundaries; three-scope policy resolution | 1 d |
| PRINT-2 | protocol | `SetPrinterPolicy`, `PrintState`, `PrintJobAdded/Fetch/Data/Discard`, snapshot fields ([05](05-protocol.md)) | 0.5 d |
| PRINT-3 | client | Frame button: four states, live counter, unit formatter | 1 d |
| PRINT-4 | client | Ask popup and answer scopes | 0.5 d |
| PRINT-5 | client | Printer Settings dialog + key-targeted config write-back | 1 d |
| PRINT-6 | client | Print Output window; `.txt` and `.ansi` writers; save via `FileDialog` | 1 d |

≈ 8–9 days, 3–5 of them upstream. PDF export (PRINT-7) is parked below.

**Done when**: the matrix table in the release notes shows pass for every
reference app on WezTerm + iTerm2 + Terminal.app (fallback), `tmux`
runs correctly *inside* ckmux, and a program printing in a ckmux terminal
produces a saved file without ckmux ever opening a device or running a
command.

## M5 — Portability & release (v1)

Linux port (poller/paths/creds seams; container CI lane becomes blocking);
`ckmux-256color` terminfo shipped and default flipped; man page + user
guide (generated key appendix); packaging (at minimum `make install`, brew
formula if licensing settled); license resolved jointly with ckVision's
D-OPEN-1; version 1.0 acceptance audit against
[01-vision.md](01-vision.md) success criteria.

## Parking lot (post-v1, explicitly not now)

Simultaneous multi-client attach (protocol is ready; would be opt-in via an
explicit flag — automatic takeover / latest-client-wins is the specified
contract); tmux-style tiled pane mode inside a window; control mode /
scripting API; resurrect (relaunch
layout+commands after server death); Windows/ConPTY (after upstream WP-50);
kitty keyboard passthrough (U2) and kitty graphics; hyperlink activation;
scrollback reflow; per-terminal palette re-theming (needs upstream indexed
color, U2); session groups; **PRINT-7** — PDF export of captured print
output via a dependency-free base-14 `Courier` writer (~200 lines, no font
embedding). Deliberately never an external converter: spawning a command
over a child-controlled stream is the exact hazard WP-PRINT removes.

## Risks

| # | Risk | Mitigation |
|---|---|---|
| ~~R1~~ | ~~**Snapshot deep-copy cost** (U0-b) makes tick-rate diffing expensive~~ — **closed 2026-08-17**: U0-b landed in ckVision (`c4f96d2`) before any ckmux hot path existed to work around it. Measured on the pinned machine: 574 µs/op for a full snapshot, 126 µs declining the history, 57 µs for damage plus borrowed cells. The last two do not grow with the history, which was the risk. | The mitigation was carried out as written — the fix went in the library first, and `scene_budget_gate` now pins the steady-state tick. |
| R2 | **ckVision is pre-1.0 and in an acceptance-audit phase**; behavior may shift under us — and our U-items add churn of our own | Pin by commit; conformance-pin goldens ([09](09-testing.md) §2) catch drift at bump time; U-items are small, well-scoped, and land under ckVision's full gates — we develop both repos, so sequencing is ours to schedule |
| R3 | Emulator fidelity long tail (some app misbehaves) | Per-terminal diagnostics surfaced in UI (`Terminal → Info…` shows unknown-sequence log); reference matrix per milestone; upstream fix loop per adopter policy |
| R4 | Protocol/perf under floods or huge scrollbacks | Scroll ops + RLE + coalescing designed in ([05](05-protocol.md)); budget gates from M2 on; backpressure self-healing tested |
| R5 | UX scope creep (menus and options multiply) | v1 non-goals ([01](01-vision.md)) + config minimalism ([08](08-configuration.md)) are binding; additions need a plan edit first |
| R6 | Single server = single blast radius | Server stays small, UI-free, thread-free, sanitizer-clean, crash-only; the honest-failure table in [03](03-session-model.md) is user-visible truth |
| R7 | No license upstream yet (D-OPEN-1) | Blocks *publishing*, not development; resolve jointly at ckVision 0.1 |

## Open questions (answers wanted at M0 review)

1. **Prefix default**: `C-b` (recommended; tmux muscle memory, avoids
   readline `C-a`) — or `C-a`/F-key?
2. **F10 direct menu** while a terminal is focused: default off
   (recommended — mc conflict) — agree?
3. **Startup**: always show the picker (recommended) vs auto-attach when
   exactly one session exists?
4. **Session auto-names**: numeric `1, 2, …` (tmux-like, recommended) or
   generated word names?
5. **Terminal-window scrollbars**: default off (recommended)?
6. **Naming**: binary `ckmux`, C++ namespace `ckm::` — confirm.
7. **License** target for both projects (ckVision D-OPEN-1).
8. v1 **packaging** ambition: `make install` only, or brew formula too?

Recommendations are already baked into the plan docs; a different answer
just edits the named doc — nothing structural moves.
