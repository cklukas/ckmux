# ckmux — Planning Documents

Status: living specification · 2026-09-01 · M3/M4 acceptance, M5 release work

**ckmux** is a terminal multiplexer with a face: tmux's persistence model
(sessions and the programs inside them survive disconnection; reattach
anytime) combined with a **ckVision** desktop UI — permanent menu bar,
permanent context-sensitive hotkey footer, and each terminal in a floating,
movable window with full mouse and Sixel support.

These plans began from a verified ckVision inventory on 2026-08-16 and have
been updated through the v0.1.4 release state on 2026-09-01. They are
**living documents**: behavior changes edit the corresponding plan in the
same commit ([11-conventions.md](11-conventions.md)).

**Co-development principle (read this first):** ckmux is one of the first
real applications of ckVision, and deliberately its proving ground. We have
write access to both repos and develop them together. Whenever a change is
best made in the library — a missing widget, an API gap, a performance
problem, an awkward seam — it is made **directly in ckVision**, at the
right level, under ckVision's own conventions and gates. ckmux never works
around a ckVision issue; it fixes ckVision and moves on. The concrete
worklist lives in [07-ckvision-integration.md](07-ckvision-integration.md)
and is scheduled inside the ckmux milestones in [10-roadmap.md](10-roadmap.md).

## The documents

| # | Document | One line |
|---|---|---|
| 01 | [Vision & Goals](01-vision.md) | What ckmux is, why, principles, non-goals, glossary, v1 success criteria |
| 02 | [Architecture](02-architecture.md) | Client/server split, event loops, sockets, failure matrix, security, portability seams, repo layout |
| 03 | [Session Model](03-session-model.md) | Server/Session/Terminal/Client entities, every operation's exact semantics, multi-client policy, honest failure list |
| 04 | [Terminal Emulation](04-terminal-emulation.md) | Build on ckVision's emulator; verified coverage, prioritized gap plan (U0/U1/U2), Sixel pipeline, input path, `$TERM` |
| 05 | [Protocol](05-protocol.md) | Framing, handshake/versioning, message catalog, grid deltas, snapshots, flow control & self-healing |
| 06 | [UI & Interaction Design](06-ui-design.md) | Screen layout, menu tree, prefix + which-key, default keymap, contexts & footer, mouse rules, copy mode, mockups |
| 07 | [ckVision Integration](07-ckvision-integration.md) | Verified capability inventory, the four integration seams, the upstream work list, consumption & pinning |
| 08 | [Configuration](08-configuration.md) | Tiny INI config: locations, defaults, keybinding syntax, what is deliberately not configurable |
| 09 | [Testing & Verification](09-testing.md) | Test pyramid on ckVision's harnesses, conformance pins, e2e core-promise test, fuzzing, budget gates |
| 10 | [Roadmap, Risks & Decisions](10-roadmap.md) | Milestones M0–M5 with acceptance criteria, parking lot, risk table, and recorded decisions |
| 11 | [Engineering Conventions](11-conventions.md) | Inherited ckVision discipline: style, determinism, provenance rule, workflow, definition of done |
| 12 | [Work Packages](12-work-packages.md) | **The queue.** Numbered packages, one state each, and the rule for picking the next one |
| 13 | [Architecture Review Findings](13-architecture-review.md) | The 2026-08-18 review: verified findings and their remediation state |
| 14 | [M4 Acceptance](14-m4-acceptance.md) | Reference apps, vttest, tmux-inside-ckmux, printing, host matrix, and environment-change cases |
| 15 | [Native Windows Compatibility](15-windows.md) | Active Windows port: platform contracts, work packages, native acceptance and release gates |

Suggested reading order: 01 → 02 → 03 → 07 → 04 → 05 → 06 → 08 → 09 → 10 → 11.

**To start work**, go to [12-work-packages.md](12-work-packages.md) and take
the first `Ready` row. 10 says what a milestone means; 12 says what is open.

## Key decisions (summary — details and rationale live in the named docs)

| ID | Decision | Doc |
|---|---|---|
| D-01 | Terminal state (PTY + VT emulator + scrollback + layout) lives in a detached **server**; the client is a renderer/input device over a Unix socket | 02, 03 |
| D-02 | **One server owns all sessions** (makes move-terminal a pointer move); lazy start, exits when empty | 02 |
| D-03 | UI = ckVision desktop: **floating windows**, menu bar, `StatusLine` footer; window layout is **session state on the server** | 03, 06 |
| D-04 | **Reuse ckVision's terminal stack** (`TerminalEmulator`, `PosixTerminalSubsession`, `TerminalView`) and **co-develop ckVision**: missing capabilities, widgets, and APIs are implemented by us directly in ckVision — never worked around in ckmux | 01, 04, 07 |
| D-05 | **Client-side input encoding** via `TerminalView`; the wire carries opaque encoded bytes (`Input{term, bytes}`) | 04, 05 |
| D-06 | Single-threaded `poll()` loops on both sides; **no threads** in v1 | 02 |
| D-07 | Attach **defaults to latest client wins** (specified requirement): connecting to an open session automatically and immediately transfers it — the old client is informed best-effort, dropped to its picker, and can never block the transfer. Shared attach is **built and opt-in** (WP-44): `Attach.mode` is take over \| join \| watch, the reader chooses in the picker, and **no reader is privileged over another** — there is no owner, because the uid-private socket is already the trust boundary | 03, 05, 06 |
| D-08 | Steal only the **prefix key** (+ Shift-mouse) from inner apps; every action also reachable by mouse/menu; which-key popup teaches chords | 06 |
| D-09 | Hand-rolled framed binary protocol, one version integer pre-1.0, **self-healing by snapshot** on any desync or backpressure overflow | 05 |
| D-10 | `TERM=xterm-256color` first, own `ckmux-256color` terminfo at M5 | 04 |
| D-11 | v1 excludes: panes/tiling-only mode, linking, remote transport, reflow, resurrect, plugins, scripting API (parking lot). **Multi-client left this list 2026-08-20**, reader-directed — see D-07 | 01, 10 |
| D-12 | C++20/CMake, zero new dependencies, ckVision **pinned by release version** for adopter/CI builds (co-development sibling source deliberately outranks it), with conformance-pin goldens | 07, 09, 11 |
| D-13 | **Sanitization by construction**: child bytes are parsed into state and re-rendered — raw child output can never reach the host terminal | 02, 04 |
| D-14 | **Printing is captured, never a printer** (WP-PRINT): Media Copy leaves the never-list because the hazard was the *sink*, not the sequence — output goes to a bounded in-memory spool, surfaced by a frame button, saved only by an explicit user click to a user-chosen path. No device is opened, no command is executed. Default **ask**, which tells the *program* yes and asks the *user* | 04, 06, 08 |

## The one-paragraph architecture

A detached server owns sessions; each terminal is a ckVision
`PosixTerminalSubsession` (PTY) + `TerminalEmulator` (headless VT state).
At a paced tick the server diffs snapshots into grid deltas and broadcasts
them. The ckVision client mirrors that state behind a
`RemoteTerminalSubsession` and renders it with the stock `TerminalView`
inside `Window`s on a `Desktop`, under a `MenuBar` and a `StatusLine`
footer, sending encoded input and layout requests back. Detach is just a
closed socket; reattach is a snapshot. Killing anything is explicit,
confirmed, and menu-visible.

## Current work

The owner activated native Windows compatibility on 2026-10-03. WP-52–WP-57
in [12](12-work-packages.md) implement [15](15-windows.md), starting with the
latest ckVision's shared lifecycle interface. The native implementation and
blocking Windows CI are present; complete acceptance and a Windows release
are still pending.

The current Windows package candidate prepares application0.1.8 and pins
released ckVision0.1.18 (2026-10-05). Application-level67-client readiness and
queue-saturation cases have focused Mac, GCC and native ARM64 evidence, with
compiled negative partners. Clean adoption and actual current ZIP/MSI lifecycle,
the Windows release matrix and the live user-facing walkthrough remain required;
the published0.1.7 release has no Windows assets.

M0–M2 and the architecture-review remediation queue are complete. **WP-15**
(M3's menu-driven acceptance walkthrough) and **WP-21** (M4's reference-app,
vttest and changing-environment matrix) are in progress. **WP-24** is done:
the browser guide, installed man page and generated key appendix are all built
and gated from the command/key registries. The Windows queue now has its own
`Ready` rows. WP-23 (`ckmux-256color`) waits on WP-21, and the final v1 audit waits on
WP-23. Linux, licensing and packaging are done, and v0.1.3 adds a separately
built and install-tested Ubuntu 20.04 DEB to the RPM, Homebrew formula, generic
DEB and platform archives introduced through v0.1.2. v0.1.4 pairs exact
sparse-delta wire budgeting with ckVision v0.1.5's host-aware large-frame
coalescing. The exact public snapshot `f0ea0f6` passed every package and
publication job in run `33446774634`; its eleven assets and five
package/archive checksums were verified after download. Subsequent v0.1.7
uses ckVision v0.1.8. Ubuntu 20.04 support was explicitly withdrawn in
`ee98607`: v0.1.5 is its last supported package; do not infer current support
from the historical v0.1.3/v0.1.4 evidence above.

The former open questions in [10-roadmap.md](10-roadmap.md) are now recorded
as decisions. Work selection still comes only from
[12-work-packages.md](12-work-packages.md).
