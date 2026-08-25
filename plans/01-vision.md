# ckmux — Vision & Goals

Plan 01 · Status: current · 2026-08-25 · [Index](README.md)

## Elevator pitch

**ckmux** is a terminal multiplexer — like tmux — with a face. It keeps your
terminal sessions and the programs inside them alive when you disconnect, and
lets you reattach later exactly where you left off. Unlike tmux, its interface
is *visible and discoverable*: a permanent menu bar at the top, a permanent
footer with context-sensitive hotkey hints at the bottom, and each terminal
living in a movable, resizable **ckVision** window on a desktop. Everything can
be driven by mouse or keyboard; nothing requires memorizing an invisible
command grammar first.

Inside each terminal window you can run *any* terminal program — shells, vim,
htop, mc, IDEs — with full color, mouse support, and Sixel graphics.

## Why

tmux is powerful but famously opaque: an invisible prefix-key grammar, a
minimal status line, and configuration required before it feels humane. GNU
screen is the same, older. zellij proved that a *visible* hint bar makes a
multiplexer dramatically more approachable, but it stays a tiling-pane UI
without menus or a windowed desktop.

ckVision provides exactly the missing half: a Turbo-Vision-style desktop with
overlapping windows, drop-down menus, dialogs, themes, mouse and Sixel support.
ckmux marries the two ideas:

> **tmux's persistence model + a ckVision desktop UI.**

## Product principles

1. **Discoverable first.** Every action is reachable through the menu bar and
   listed in context help. Hotkeys are accelerators, not the only path. The
   footer always shows the most relevant keys for the current context.
2. **Transparent to inner apps.** Programs running inside a terminal window
   must behave as if they ran in a plain terminal: keys, modifiers, mouse,
   colors, alternate screen, paste, Sixel. ckmux steals as few keys as
   possible (a single prefix key plus mouse; see [06-ui-design.md](06-ui-design.md)).
3. **Persistent by default.** Closing or losing the ckmux client never kills
   the programs inside. Detach is the normal way to leave; killing things is
   always explicit and confirmed.
4. **Honest state.** After reattach, the screen shows the *current* state of
   every program (not a replay), including scrollback accumulated while
   detached.
5. **Mouse is first-class.** Focus, move, resize, menus, selection, scrolling —
   all work by mouse. Keyboard-only operation is equally complete.
6. **Deterministic and testable.** Same inputs → same screens. The emulator,
   the protocol, and the UI are all testable headlessly (following ckVision's
   own determinism discipline).
7. **macOS and Linux, by named gates.** Development happens on macOS and the
   Linux port is now shipped and blocking in CI. Platform-specific code stays
   behind small seams, and a clean-build claim names its compiler and host.
   (Windows/ConPTY is a possible later follow-on since ckVision targets it,
   but it is out of scope for now.)
8. **Fix ckVision, never work around it.** ckmux is one of the first real
   applications of ckVision and deliberately serves as its proving ground.
   We own and co-develop both repos: when a capability, widget, or API is
   missing, awkward, or slow, the change is made **at the right level —
   directly in ckVision** — following ckVision's own discipline, and ckmux
   consumes the improved library. A workaround in ckmux for a ckVision
   limitation is a design failure by definition
   ([07-ckvision-integration.md](07-ckvision-integration.md) keeps the
   worklist; [11-conventions.md](11-conventions.md) states the rule).

## What ckmux is not (non-goals for v1)

- **Not a terminal emulator for your OS.** ckmux runs *inside* your favorite
  terminal (Terminal.app, iTerm2, WezTerm, xterm, …); it does not replace it.
- **Not a tiling window manager.** Floating ckVision windows (with Tile /
  Cascade commands) come first; tmux-style split panes may come later.
- **No remote/network transparency.** Client and server talk over a local
  Unix socket only. Remote use is `ssh host ckmux`, like tmux.
- **No session survival across reboots or server crashes.** Like tmux: if the
  server dies, the PTYs close and children receive SIGHUP. A
  "resurrect"-style relaunch feature may come later.
- **No plugin system in v1.** Get the core right first.
- **No scripting/control-mode API in v1** (tmux's `-C`). The CLI covers
  automation basics; a control protocol can come later since the client/server
  split makes it natural.

## Primary user stories

1. *Start fresh:* I run `ckmux`. I see a session picker listing existing
   sessions (name, terminals, created, last attached). I create a new session
   or attach to one with Enter or a button-click.
2. *Work:* Inside a session I open terminals from the menu (`Terminal → New`)
   or hotkey. Each is a window I can move, resize, maximize, or tile. I run
   vim, htop, a build, a serial console. The footer shows the keys I need.
3. *Leave:* I quit ckmux (or my SSH connection drops, or I close the terminal
   tab). Everything keeps running.
4. *Return:* I run `ckmux` again, attach to the session, and everything is
   there — same windows, same layout, same screens, current output.
5. *Reorganize:* I move a terminal from one session to another
   (`Terminal → Move to Session…`), rename sessions and terminals, kill what
   I no longer need (with confirmation).
6. *Graphics & mouse:* Inside a terminal I run a program that draws Sixel
   images and uses the mouse; both work as in a plain terminal.
7. *Take over, join, or watch:* From my laptop I attach to a session my desk
   machine still has open. The picker shows the reader count and offers all
   three choices. Takeover remains the no-flag/default path and cannot be
   vetoed by an unreachable client; join keeps both readers active; watch is
   enforced read-only by the server.

## Prior art we consciously borrow from

| Project | What we take | What we do differently |
|---|---|---|
| tmux | Client/server persistence model, session/window concepts, prefix key | Visible UI: menus, footer, windows; server-rendered *state*, client-rendered *UI* |
| GNU screen | Detach/attach as the core loop | Everything else |
| zellij | Permanent, context-sensitive key-hint bar | Menu bar + floating windows instead of tiled panes-only |
| Twin (textmode WM) | Desktop-with-windows-in-a-terminal metaphor, detachable display | Modern emulation fidelity (truecolor, Sixel, kitty keys), tmux-grade persistence |
| Turbo Vision / ckVision | Interaction grammar: menu bar, status footer, F1 context help, dialogs | Applied to a multiplexer |
| Norton Commander / mc | Footer as hotkey reference | Context-sensitive, generated from the real keymap |

## Glossary (used consistently across all plan docs)

| Term | Meaning |
|---|---|
| **Server** | The detached background process that owns all sessions, PTYs, and emulator state. Survives client exit. |
| **Client** | The ckVision full-screen application the user sees; attaches to the server over a Unix socket. |
| **Session** | A named workspace owned by the server: a set of terminals plus their window layout. What you attach to and detach from. |
| **Terminal** | One terminal instance inside a session: PTY + VT emulator + scrollback + its window geometry. Shown as one ckVision window. (The thing the user request calls a "terminal session".) |
| **Inner app** | The child process tree running inside a terminal (shell, vim, …). |
| **Host terminal** | The real terminal emulator ckmux's client runs in (iTerm2, WezTerm, …). |
| **Attach / Detach** | Connect a client to a session / disconnect leaving it running. |

## Success criteria for "v1 done"

- Daily-drivable by the author on macOS: survives weeks-long sessions,
  vim/htop/mc/ssh feel native, detach/attach is instant and lossless.
- A first-time user can, without reading docs: create a session, open
  terminals, move windows, detach, reattach — guided only by the menu bar and
  footer.
- Sixel-emitting programs and mouse-hungry programs work inside ckmux under a
  Sixel-capable host terminal (WezTerm, iTerm2, xterm -ti vt340).
- The full test suite (emulator goldens, protocol round-trips, headless
  end-to-end attach/detach) passes on macOS and Linux CI.

## Reading order for these plans

[README.md](README.md) is the index. Architecture ([02](02-architecture.md))
and the session model ([03](03-session-model.md)) define the skeleton;
emulation ([04](04-terminal-emulation.md)) and the protocol
([05](05-protocol.md)) define the hard technical core; UI ([06](06-ui-design.md))
defines what the user sees; ckVision integration ([07](07-ckvision-integration.md))
grounds it all in what the library actually provides today; configuration
([08](08-configuration.md)), testing ([09](09-testing.md)), and the roadmap
([10](10-roadmap.md)) close the loop.
