# ckmux

**ckmux** is a terminal multiplexer — like tmux — with a face. It keeps your
terminal sessions and the programs inside them alive when you disconnect, and
lets you reattach later exactly where you left off. Unlike tmux, its interface
is *visible and discoverable*: a permanent menu bar, a permanent footer of
context-sensitive hotkeys, and each terminal in a movable, resizable window on
a desktop. Everything works by mouse or keyboard; nothing has to be memorized
before it can be found.

Inside each terminal window you can run any terminal program — shells, vim,
htop, mc — with full color, mouse support, and Sixel graphics.

```
 Session  Terminal  Window  View  Help
░░╔═[■]═══════════════════ Terminal 1 ═══════════════════[↑]═╗░░░
░░║ $ vim plans/02-architecture.md                           ║░░░
░░║                          ┌─ ^B … ──────────────────────┐ ║░░░
░░║                          │ c   new term   t   tile     │ ║░░░
░░║                          │ n   next       m   menu     │ ║░░░
░░║                          │ w   windows    d   detach   │ ║░░░
░░║                          └─────────────────────────────┘ ║░░░
░░╚══════════════════════════════════════════════════════════╝░░░
 ^B m menu  ^B c new term  ^B d detach  ^B n next  ^B ? keys
```

It is built on [ckVision](../ckvision), a windowed terminal-UI framework, and
is developed together with it.

## Status: M2 in progress

The core promise works: terminals run in a detached server, survive the
client quitting, and reattach exactly where they left off — a test forks a
real server, kills the client mid-count, and proves the program kept running
unwatched. Sessions are plural, named, and killable; the interface — menu bar
with clock and calendar, footer, floating terminal windows, the prefix and its
which-key popup, scrollback with a frame scrollbar, Sixel graphics, themes in
`Settings ▸ General…` — is real and usable. Not every M2 edge is closed yet:
the open items live in [plans/12-work-packages.md](plans/12-work-packages.md)
and [plans/13-architecture-review.md](plans/13-architecture-review.md). See
[plans/10-roadmap.md](plans/10-roadmap.md) for what each milestone means.

## Build

Needs a C++20 compiler, CMake 3.25+, and a ckVision checkout beside this one
(or an installed ckVision package).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
./build/ckmux
```

Tests:

```bash
ctest --test-dir build --output-on-failure
```

## Keys

One key is taken from the program you are running: the prefix, `Ctrl+B`.
Press it, then one of the keys the popup shows — `c` for a new terminal, `n`
and `p` to move between them, `d` to detach, `?` for the full list. Every
command is also in the menu bar, and every menu entry shows its key. Press
`Ctrl+B` twice to send a literal `Ctrl+B` to the program.

Everything else — function keys, Alt combinations, the mouse — belongs to the
program in the window, exactly as it would without ckmux.

## Documentation

[plans/](plans/README.md) is the specification: what ckmux is
([01](plans/01-vision.md)), how it is built ([02](plans/02-architecture.md)),
the session model ([03](plans/03-session-model.md)), terminal emulation
([04](plans/04-terminal-emulation.md)), the client/server protocol
([05](plans/05-protocol.md)), the interface ([06](plans/06-ui-design.md)),
and the rest.

---

Copyright (c) 2026 C. Klukas. All rights reserved.
