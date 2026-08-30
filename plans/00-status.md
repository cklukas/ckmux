<!-- Copyright (c) 2026 C. Klukas. All rights reserved. -->

# Status and document map

Development-internal. The public README states what ckmux is, how to build it,
and a concise milestone-level status. Work-package ownership, evidence and the
exact gaps live here so those claims remain auditable without turning the
front page into the queue.

## Current status: M3/M4 acceptance in progress

M0–M2 are implemented and their automated gates are green. The core promise
works: terminals run in a detached server, survive the client
quitting, and reattach exactly where they left off — a test forks a real
server, kills the client mid-count, and proves the program kept running
unwatched. Sessions are plural, named, and killable; the interface — menu bar
with clock and calendar, footer, floating terminal windows, the prefix and its
which-key popup, scrollback with a frame scrollbar, Sixel graphics, themes in
`Settings ▸ General…` — is real and usable. Shared/read-only readers, captured
printing, process statistics, the Linux port, installable packages and the
v0.1.1 release have landed. The user guide, installed man page and generated
key appendix have now landed as well. The packaging path is being expanded for
v0.1.2 with RPM and an install-tested Homebrew formula alongside the existing
DEB and platform archives; its release workflow has a non-publishing manual
mode so that exact path can be gated before the tag is created. That manual
path passed at public snapshot `fdf3c9b` (GitHub run `33341038991`), including
both native 53-suite Release gates, package-content inspection, Homebrew
installation/test, artifact upload, and downloaded checksum verification.

WP-15 is completing M3's menu-driven operation walkthrough. WP-21 is completing
M4's reference-app, vttest and changing-environment matrix. The architecture
review remediation queue is closed. M5 has Linux, licensing and packaging;
WP-24's documentation row is complete. The terminfo row waits on WP-21, and
the final v1 audit then waits on that terminfo row. The authoritative states
live in [12-work-packages.md](12-work-packages.md).
See
[10-roadmap.md](10-roadmap.md) for what each milestone means, and
[14-m4-acceptance.md](14-m4-acceptance.md) for the acceptance criteria of the
current fidelity milestone.

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
