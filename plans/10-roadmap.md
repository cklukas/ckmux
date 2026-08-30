# ckmux — Roadmap, Risks & Decisions

Plan 10 · Status: current · 2026-08-25 · [Index](README.md)

Milestones are acceptance-based (ckVision's discipline): a milestone is done
when its criteria hold with test evidence per [09-testing.md](09-testing.md),
not when its code lands. Order matters; dates don't (iterative solo
development). The ckVision work items (U0/U1/U2, defined in
[07-ckvision-integration.md](07-ckvision-integration.md)) are **part of
these milestones**: we implement them ourselves, directly in the ckVision
tree under its gates, and a ckmux milestone is not done while its U-items
are open. Expect the U-list to grow as ckmux exercises the library — that
is the point of being its proving ground.

**Status: M0–M2 are implemented; M3 and M4 are in their acceptance passes;
Linux, licensing, packaging and documentation have landed for M5.** WP-15
owns M3's final menu-driven walkthrough. WP-21 owns the
three-host/reference-app/vttest matrix and its changing-environment cases.
v0.1.2 is published for macOS arm64 and Linux x86_64. What each milestone
means in practice, and what remains before v1, is recorded at the end of each
section below.
What is *selectable* is never read from here —
[12-work-packages.md](12-work-packages.md) is the queue, and
[13-architecture-review.md](13-architecture-review.md) is the now-complete
remediation record.

**This file is not the queue.** A milestone here is a paragraph of intent and a
set of acceptance criteria — it cannot be selected from, and reading M2–M5 as
"what is open" is what let the entire multiplexer look like one line of prose.
The selectable list of packages, each with a state and a prerequisite, is
[12-work-packages.md](12-work-packages.md). Start there; come here to find out
what a milestone must satisfy before it may be called done.

## M0 — Scaffolding ✅

Repo skeleton per [02-architecture.md](02-architecture.md) layout; CMake
consuming ckVision (`find_package` or pinned release fetch, sibling source for co-development); CI with
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
file; GitHub Actions lanes for macOS, macOS+ASan/UBSan, and Linux. The Linux
lane became blocking with WP-22. The questions formerly left for M0 are now
decisions recorded at the end of this document.

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
server exists. `check-config` landed with WP-11.

**At the M1 gate (historical):** terminals lived in the client process, so
quitting killed them — that is precisely what M2 changed. `Detach` and
`Sessions…` were registered, visible, and *disabled*, so the interface showed
its real shape and said plainly that the server did not exist yet rather than
hiding or pretending. Copy mode, rename, and new-terminal-with-command were
M3/M4 and were absent, not stubbed.

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

**Both gates are green (2026-08-17, WP-7).** The core promise: a real server
process, a real client, a child counting in a real PTY — `count 3` before the
client died, `count 11` when the next one attached 500 ms later, still counting.
The flood gate's reproducible baseline is roughly 550 ms worst / 490 ms mean
in Debug and roughly 22 ms mean optimized while a child runs `yes`; the earlier
32/24 ms record omitted its build type and is not used as a Debug baseline.
`ckmux` itself is now a client of its own server: lazy start, attach, detach,
reattach, and the M1 interface unchanged above the seam. The daily-use half is
operational evidence rather than an automated gate and continues through the
pre-1.0 acceptance passes.

**Four criticals at M2's own seams were found by review on 2026-08-18.** A
default-config attach snapshot can exceed the decoder's 16 MiB cap and leave a
session permanently unattachable; the `needs_snapshot` scan broadcasts to every
client with no session filter and latches; a shrinking terminal silently
diverges the mirror because deltas carry no geometry; and a reattach with held
mirrors opened no windows. They are C1…C4 in
[13-architecture-review.md](13-architecture-review.md). All four and the rest
of R1–R10 are closed; the review therefore no longer reopens M2. The historical
record remains because those failures explain the executable bounds, fan-out
rules, geometry op and reattach reconciliation that now hold the milestone.

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

**Implemented:** multiple sessions and their lifecycle (WP-8); the picker and
takeover flow (WP-9); move-terminal (WP-10); CLI subcommands (WP-11); config v1
(WP-12); exit policy, banners and respawn (WP-13); notifications and complete
help pages (WP-14); server-owned window layout with restoration, tiling,
minimize and the switcher bar (WP-28…WP-35); and opt-in shared/read-only readers
with the on-screen choice (WP-40…WP-50). **Remaining:** WP-15's acceptance
walkthrough — every operation row over the wire and every reader-facing action
reachable from menus alone.

## M4 — Fidelity (any app)

U1 emulator round (OSC 4/10/11 replies, child OSC 52 policy, tab stops,
REP, mode 1007, underline styles); Sixel transport + placement + DA1
advertisement end-to-end; copy mode completed (search, vi keys, rectangular,
clipboard targets); paste credit flow; bell/activity monitors; performance
budget gates green under floods; reference-app matrix
([04-terminal-emulation.md](04-terminal-emulation.md)) passes; vttest
chapters recorded. Plus **WP-PRINT**, below.

**Implemented:** U1-a tab stops; U1-c Media Copy capture behind a policy; U1-d
typed dialog fields; WP-16 Sixel transport and host-dependent advertisement;
WP-17 complete copy mode; WP-18 paste credit flow; WP-19 bell/activity
monitors; WP-20 the flood budgets; and PRINT-1…6, including the repaired client
route from a captured job to preview/save. **Remaining:** WP-21's acceptance
matrix. Its tmux-inside-ckmux, printing and nine-reference-app rigs exist; the
three-host/vttest matrix and the environment-changing cases listed in
[14-m4-acceptance.md](14-m4-acceptance.md) are what keep M4 open.

### WP-PRINT — the virtual printer

Media Copy captured into a bounded spool instead of a device, surfaced on
the window frame, governed by a settings dialog
([04](04-terminal-emulation.md) has the behavior,
[06](06-ui-design.md) the UI, [08](08-configuration.md) the scopes).

| ID | Where | Item | Size |
|---|---|---|---|
| U1-c | ckVision | Emulator MC capture behind a profile policy defaulting to deny ([07](07-ckvision-integration.md)) — **done 2026-08-17**, L-40 | 1–2 d |
| U1-d | ckVision | Typed dialog fields (radio/check/combo/number) — descriptors materialized text inputs only — **done 2026-08-17**, L-41/L-42 | 2–3 d |
| PRINT-1 | server | `PrintSpool` as session state; job boundaries; three-scope policy resolution | 1 d |
| PRINT-2 | protocol | `SetPrinterPolicy`, `PrintState`, `PrintJobAdded/Fetch/Data/Discard`, snapshot fields ([05](05-protocol.md)) | 0.5 d |
| PRINT-3 | client | Frame button: four states, live counter, unit formatter | 1 d |
| PRINT-4 | client | Ask popup and answer scopes | 0.5 d |
| PRINT-5 | client | Printer Settings dialog + key-targeted config write-back | 1 d |
| PRINT-6 | client | Print Output window; `.txt` and `.ansi` writers; save via `FileDialog` | 1 d |

≈ 8–9 days, 3–5 of them upstream. **Complete, 2026-08-20:** the server and
spool landed in ckmux `6c88466`, the missing reader route in `7ef0a1c`, and
the ckVision seams in `6648d90`; the route repair was gated 38/38 and verified
with a live child producing a byte-exact saved file. PDF export (PRINT-7) is
parked below.

**Done when**: the matrix table in the release notes shows pass for every
reference app on WezTerm + iTerm2 + Terminal.app (fallback), `tmux`
runs correctly *inside* ckmux, and a program printing in a ckmux terminal
produces a saved file without ckmux ever opening a device or running a
command.

## M5 — Portability & release (v1)

Linux port (poller/paths/creds seams; container CI lane becomes blocking);
`ckmux-256color` terminfo shipped and default flipped; man page + user
guide (generated key appendix); packaging (`make install`, TGZ, DEB, RPM, and
a release-pinned Homebrew formula); license resolved jointly with ckVision's
D-OPEN-1; version 1.0 acceptance audit against
[01-vision.md](01-vision.md) success criteria.

**Implemented ahead of v1:** WP-22 makes Linux a blocking platform, with local
GCC 13/14 and Clang coverage and public 51/51 Linux lanes; WP-26 resolved both
projects to MIT; WP-25 provides `make install`, CPack TGZ/DEB/RPM, a generated
and install-tested Homebrew formula, and tag-triggered GitHub releases; WP-24
provides the browser guide, installed `ckmux(1)`, and a key appendix generated
from the live registry. v0.1.0, v0.1.1 and v0.1.2 are published; v0.1.2 adds
RPM and the install-tested Homebrew formula. **Remaining:** WP-23
`ckmux-256color` waits for WP-21; WP-27 is the final v1 audit after WP-23.

## Parking lot (post-v1, explicitly not now)

~~Simultaneous multi-client attach~~ — **left the parking lot 2026-08-20**,
reader-directed: designed in [03](03-session-model.md) "Two clients at once"
and scheduled as WP-40…WP-44 plus U7-a, extended the same day with WP-48…WP-50
— the fan-out WP-44 declared but did not finish, a read-only reader mode, and
the picker that lets a reader choose between joining and taking over. Takeover stays the specified default;
the flag sits beside it. Three of those packages are latent defects in the
single-client design rather than new work — the session does not own its
desktop, four pieces of a reader's own state live on the session, and two
readers pasting into one terminal would interleave — so they are worth doing
whether or not the flag is ever built. tmux-style tiled pane mode inside a
window; control mode /
scripting API; resurrect (relaunch
layout+commands after server death); Windows/ConPTY (after upstream WP-50);
kitty keyboard passthrough and kitty graphics; hyperlink activation;
scrollback reflow; per-terminal palette re-theming; session groups;
**PRINT-7** — PDF export of captured print
output via a dependency-free base-14 `Courier` writer (~200 lines, no font
embedding). Deliberately never an external converter: spawning a command
over a child-controlled stream is the exact hazard WP-PRINT removes.

Two of those no longer wait on anything upstream, which is worth saying so
nobody re-schedules them behind a library item that is finished: indexed colour
survives end to end (L-22) and [05](05-protocol.md)'s cell carries all three
kinds, so **per-terminal re-theming** is ckmux UI work somebody has to want;
and the kitty keyboard **child** side has landed, so what is parked there is
the passthrough, not the protocol.

## Risks

`R#` here is a risk in this table. The `R#` in
[13-architecture-review.md](13-architecture-review.md) is a remediation topic
and means something else; the two never appear in the same table, the same way
`WP-n` is disambiguated in [12](12-work-packages.md).

| # | Risk | Mitigation |
|---|---|---|
| ~~R1~~ | ~~**Snapshot deep-copy cost** (U0-b) makes tick-rate diffing expensive~~ — **closed 2026-08-17**: U0-b landed in ckVision (`c4f96d2`) before any ckmux hot path existed to work around it. Measured on the pinned machine: 574 µs/op for a full snapshot, 126 µs declining the history, 57 µs for damage plus borrowed cells. The last two do not grow with the history, which was the risk. | The mitigation was carried out as written — the fix went in the library first, and `scene_budget_gate` now pins the steady-state tick. |
| R2 | **ckVision is pre-1.0**; behavior may shift under us, and our U-items add churn of our own | Pin adopter/CI builds to a release version; let the co-development sibling source outrank it locally; conformance-pin goldens ([09](09-testing.md) §2) catch drift at bump time. v0.1.1 is the current paired release. |
| R2a | **Upstream work named out loud rather than patched around.** Open/deferred in [07](07-ckvision-integration.md): U4-d/e/f/h are non-blocking API cleanup; U3 is a deferred presenter scroll optimisation; U1-b has no known concrete gap; U2 is app-driven XTGETTCAP/DECRQSS/rectangles, inner OSC 8 and SGR 53. | WP-20's budgets are green without U3, so it is an optimisation rather than a hidden prerequisite. U1-b/U2 wait for a concrete app or WP-21 finding. None is worked around silently in ckmux; a blocking discovery gets a queue row. |
| R3 | Emulator fidelity long tail (some app misbehaves) | Per-terminal diagnostics surfaced in UI (`Terminal → Info…` shows unknown-sequence log); reference matrix per milestone; upstream fix loop per adopter policy |
| R4 | Protocol/perf under floods or huge scrollbacks — **and it materialised once**: a 10 000-line scrollback encoded a 16.29 MiB attach snapshot, past the decoder's own cap, and made that session permanently unattachable (C1) | Closed as a correctness risk by remediation R1/R2: encode enforces caps, snapshots are budget-bounded at 4 MiB, history is fair-shared, and the queue has 256 KiB screen suppression, 4 MiB high-water and a 32 MiB drop-the-laggard ceiling. WP-20 keeps performance as a measured gate rather than a correctness hope. |
| R5 | UX scope creep (menus and options multiply) | v1 non-goals ([01](01-vision.md)) + config minimalism ([08](08-configuration.md)) are binding; additions need a plan edit first |
| R6 | Single server = single blast radius | Server stays small, UI-free, thread-free, sanitizer-clean, crash-only; the honest-failure table in [03](03-session-model.md) is user-visible truth |
| ~~R7~~ | ~~No license upstream yet (D-OPEN-1)~~ — **closed 2026-08-20**: both repositories are MIT, with contribution terms in `CONTRIBUTING.md`. | v0.1.0, v0.1.1 and v0.1.2 have been published under that decision. |
| R8 | **Invariants asserted in a comment or a plan and enforced by nothing.** The 2026-08-18 review's central verdict: the design-level engineering holds, and the weaknesses cluster in this one pattern — a size cap that only the decoder applies, a high-water mark that stops nothing, a layer rule a comment claimed the build enforced, a queue whose `Blocked` rows had no unmet prerequisite | Remediation tracked topic by topic in [13-architecture-review.md](13-architecture-review.md), each with a state and a commit. The durable half is that an invariant now has to be *executable*: the layer rule became `tools/check_layers.py` and two CTest cases, and R1 turns the snapshot bound into code on the sending side. A plan row is a claim; the code is the evidence, and re-derivation is somebody's job on every landing |

## Decisions formerly open at M0

1. **Prefix default**: **resolved** — `C-b`, shipped as the default in
   `Settings::prefix` and rebindable through `[keys]`
   ([08](08-configuration.md)).
2. **F10 direct menu** while a terminal is focused: **resolved off**. A focused
   terminal receives F10 (notably mc's quit key); `^B m` and Alt+mnemonic open
   ckmux's menu.
3. **Startup**: **resolved** by WP-9 — auto-attach when there is exactly one
   session and nobody is watching it, because that is the reattach the whole
   project is for and it needs no permission; the picker appears for every
   other case, including a session already in use.
4. **Session auto-names**: **resolved** — `session-N`, N one past the largest
   occupied number, so renaming "session-2" to "build" does not produce a
   second "session-2" tomorrow.
5. **Terminal-window scrollbars**: **resolved** — on, frame-mounted on the
   window's right border (ckVision D-051, [06](06-ui-design.md) open
   question 3), so they cost neither a column nor a child reflow.
6. **Naming**: **resolved** — binary `ckmux`, C++ namespace `ckm::`.
7. **License**: **resolved** — MIT for ckmux and ckVision, with DCO, patent and
   relicense grants in `CONTRIBUTING.md`.
8. v1 **packaging**: **resolved for the current release line** — `make install`,
   TGZ on both platforms, DEB and RPM on Linux, and a release-pinned Homebrew
   formula installed and tested on macOS before the release workflow publishes
   any of them. Manual dispatch exercises the same package jobs without making
   a release.
