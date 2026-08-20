<!-- Copyright (c) 2026 C. Klukas. All rights reserved. -->

# Status and document map

Development-internal. The public README states what ckmux is and how to build
it; it deliberately says nothing about milestones, work packages, or which
plan owns which gap. That material lives here, so that a status claim can be
kept honest and detailed without it becoming a promise on the front page.

## Current status: M2 in progress

The core promise works: terminals run in a detached server, survive the client
quitting, and reattach exactly where they left off — a test forks a real
server, kills the client mid-count, and proves the program kept running
unwatched. Sessions are plural, named, and killable; the interface — menu bar
with clock and calendar, footer, floating terminal windows, the prefix and its
which-key popup, scrollback with a frame scrollbar, Sixel graphics, themes in
`Settings ▸ General…` — is real and usable.

Not every M2 edge is closed. The open items live in
[12-work-packages.md](12-work-packages.md) and
[13-architecture-review.md](13-architecture-review.md). See
[10-roadmap.md](10-roadmap.md) for what each milestone means, and
[14-m4-acceptance.md](14-m4-acceptance.md) for the acceptance criteria of the
milestone after this one.

A "Landed" row is a claim about the code, not the code. Check the code before
repeating it.

## The specification

[README.md](README.md) in this directory is the index. In reading order:

| Document | Content |
|---|---|
| [01-vision.md](01-vision.md) | What ckmux is, who it is for, and what it refuses to be |
| [02-architecture.md](02-architecture.md) | Layers, the client/server split, portability seams |
| [03-session-model.md](03-session-model.md) | Sessions, terminals, attach/detach, the command table |
| [04-terminal-emulation.md](04-terminal-emulation.md) | The emulator: parsing, the grid, capabilities, graphics |
| [05-protocol.md](05-protocol.md) | The wire protocol between client and server |
| [06-ui-design.md](06-ui-design.md) | The interface: menus, footer, windows, the prefix grammar |
| [07-ckvision-integration.md](07-ckvision-integration.md) | What ckmux uses from ckVision, and the co-development rule |
| [08-configuration.md](08-configuration.md) | The configuration file, its grammar and defaults |
| [09-testing.md](09-testing.md) | The testing culture, suite taxonomy, budgets, fuzzing |
| [10-roadmap.md](10-roadmap.md) | Milestones and what each one means |
| [11-conventions.md](11-conventions.md) | Code conventions, warning policy, commit standards |
| [12-work-packages.md](12-work-packages.md) | The authoritative work queue — pick work from here |
| [13-architecture-review.md](13-architecture-review.md) | Review findings and the repairs that own them |
| [14-m4-acceptance.md](14-m4-acceptance.md) | M4 acceptance criteria |
| [term_capabilities.md](term_capabilities.md) | Observed terminal capability matrix |

[AGENTS.md](../AGENTS.md) at the repository root is the working agreement.

## Publication

None of this directory is published. `tools/publish/make-public.sh` builds the
public snapshot branch from `main` minus everything named in
`tools/publish/publicignore`; `plans/` and `AGENTS.md` are on that list.
