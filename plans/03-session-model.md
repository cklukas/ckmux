# ckmux — Session Model

Plan 03 · Status: draft · 2026-08-16 · [Index](README.md)

Defines the server-side object model: what exists, who owns what, how things
are named, and the exact semantics of every lifecycle operation. The wire
messages that carry these operations live in [05-protocol.md](05-protocol.md);
the UI that triggers them lives in [06-ui-design.md](06-ui-design.md).

## Entity tree

```
Server (one per user, per socket path)
├── Session "build"        (id 1)
│   ├── Terminal 1  vim    (PTY, emulator, scrollback, window geometry)
│   ├── Terminal 2  make   (…)
│   └── SessionState: desktop size, z-order, focused terminal, next-window slot
├── Session "ops"          (id 2)
│   └── Terminal 3  ssh …
└── Clients (attached UIs)
    └── Client A ── attached to "build" (tty /dev/ttys004, size 190×52)
```

### Server

- Exactly one server per socket path; default path is per-user
  (see [02-architecture.md](02-architecture.md) for socket location and
  daemonization).
- Owns all sessions and all PTY master fds. Runs the VT emulators. Holds all
  state that must survive client exits.
- Starts lazily on first client use; exits when the last session is killed
  (and no client is connected). `ckmux kill-server` forces it down.

### Session

| Field | Notes |
|---|---|
| `id` | u64, monotonically assigned by the server, never reused. Wire-level identity. |
| `name` | UTF-8, unique among live sessions. Auto-named `1`, `2`, … on creation; renameable any time (`Session → Rename…`). |
| `created_at`, `last_attached_at` | For the session picker. |
| `terminals` | Ordered list (creation order = default cycling order). |
| `desktop_size` | Cols×rows of the session's virtual desktop (see multi-client rules below). **Not yet real: `Server` keeps a desktop per CONNECTION (`Client::desktop`) and `Session` has no size at all.** Indistinguishable while one client is attached, and the whole of the problem the moment two are — WP-40. |
| `layout` | Per-terminal window geometry + z-order + focused terminal + maximized flag. |
| `attached_clients` | 0..n; v1 enforces 0..1 (takeover). Opt-in `0..n` is WP-44. |

**Decision: window layout is session state, owned by the server** — not
client-local. Rationale: the user expects window arrangement to survive
detach/reattach exactly like screen contents; and two clients viewing one
session must agree on what they see. Consequence: move/resize/z-order/focus
gestures in the client are *requests* sent to the server, which applies them
and broadcasts the new layout (see [05-protocol.md](05-protocol.md)).

### Terminal

| Field | Notes |
|---|---|
| `id` | u64, server-unique across all sessions (ids never move even when the terminal changes session). |
| `index` | Small per-session ordinal shown in the UI/window list (`1:`, `2:`, …), recomputed on move/close. |
| `name` | Optional user override; otherwise display title = index + inner-app title (OSC 0/2). |
| `pty` | Master fd; child pid; child's argv for diagnostics. |
| `emulator` | Full VT state: grid, cursor, modes (alternate screen, mouse modes, bracketed paste, …), tabs, charsets, colors, pending Sixel images. See [04-terminal-emulation.md](04-terminal-emulation.md). |
| `scrollback` | Ring buffer of lines scrolled off the primary screen; capacity from config (default 10 000 lines). |
| `geometry` | Window rect on the session desktop + z-position + min size. Content area of the window = PTY size. |
| `flags` | Bell pending, activity-while-unfocused, exited (+ exit status). |

A terminal's PTY size is always the *content* size of its window (frame
minus chrome). Window resize ⇒ `TIOCSWINSZ` + `SIGWINCH` to the child,
throttled while dragging (apply at ~10 Hz and once on release) to avoid
SIGWINCH storms.

### Client

Clients are ephemeral: they hold *no* authoritative state. A client knows the
session snapshot it received on attach plus deltas since. Menu state, open
dialogs, the session picker, copy-mode UI — all client-local, never synced.

## Operations and their exact semantics

| Operation | Semantics | Edge cases |
|---|---|---|
| `new-session [name] [cmd]` | Create session; optionally create its first terminal running `cmd` (default: user's shell). Does not implicitly attach. Implemented. | Name collision → reject with `NameTaken` (UI suggests `name-2`). Names compare exactly: `Build` and `build` are two names, because case folding is a locale question this has no business answering. |
| `attach session` | Bind client to session; server sends full snapshot (layout + every terminal's screen + scrollback tail + images). Sets `last_attached_at`. | Already attached elsewhere: **automatic takeover — latest client wins**; the previous client is detached first, informed best-effort, and can never block the transfer (full spec below). |
| `detach` | Unbind client. Session and terminals unaffected. Also triggered by client EOF/crash (socket close = implicit detach). | Detaching the last client from an *empty* session (0 terminals) does not kill it — sessions die only explicitly or when their last terminal closes with `on-exit=close` and config `kill-empty-session=true` (default **true**, matching tmux). |
| `new-terminal [session] [cmd] [geom]` | Allocate PTY, spawn `cmd` (default shell, `TERM` per [04](04-terminal-emulation.md)), place window per placement policy (cascade from last position; honor explicit geom). | Session at terminal limit (config, default 64) → error. |
| `close-terminal id {force, grace}` | Graceful, implemented: the child's process group is ASKED — SIGHUP then SIGTERM, ckVision's `request_termination` — with the PTY still draining, so a goodbye lands on the reader's screen. The terminal is removed and `TermClosed{exited=1, status}` sent when the child exits; the client's window falls only then. | `force=1` (the dialog's ticked-by-default box) closes whatever remains when the grace expires — ckVision's close escalates to SIGKILL. `force=0` and the program declines: terminal and window stay, said in the server log. The grace is config `kill-grace-seconds`, shared with `kill-session`. A repeated close replaces the pending one's terms. A session emptied this way is subject to `kill-empty-session`. |
| Child exits by itself | Behavior per config `on-exit`: `close` (default: window closes), `hold` (window stays showing final screen + exit banner until user closes), `hold-on-error` (hold only when status ≠ 0 or killed by signal). | Exit banner shows status/signal; the window is inert (no PTY) but scrollback/copy still work. |
| `move-terminal id → session` (wire: `MoveTerminal{term, destination, to_new_session}`) | Reparent, implemented: membership moves between the two sessions' terminal lists; PTY, emulator, scrollback, id all unchanged — the child never notices. The source's watcher gets `TermClosed{exited=0}` (the window leaves; nothing exited), the target's watcher is healed by a fresh snapshot the same way a client that fell behind is. `to_new_session=1` creates the destination — an explicit flag, not a magic id, because `Attach` already gave 0 a meaning. | Target = source → no-op. A close pending against the moved terminal is cancelled — a move is the reader deciding to keep it. Moving the *last* terminal out leaves an empty session, subject to `kill-empty-session`: the emptied source ends and its watcher falls to the picker, where the destination is. |
| `rename-session` | Update name; broadcast to every client's picker. Implemented. | Name collision → reject with `NameTaken`, the same rule as `new-session` (owner ruling, 2026-08-20). It has to be the same rule: a name a reader cannot take at creation is one they could otherwise reach in two steps by renaming into it. Renaming a session to the name it ALREADY has is a no-op, not a refusal — confirming a dialog unchanged is not a request. An EMPTY name leaves it alone, because a session with no name is a row in the picker with nothing to point at. |
| `rename-terminal` (wire: `RenameTerminal{id, name}`) | Pin one terminal's caption to a name the reader chose. Implemented, WP-36: kept on the terminal beside its layout, stated on `TermMeta` and in the attach snapshot, so it survives a detach and both clients watching a session agree. | It is an OVERRIDE, not a second writer of the caption: the child goes on claiming titles with OSC 0/2 underneath it and is simply not shown. An **empty name clears it** — the dialog's *Use Default Title* — and the caption returns to what the child is claiming NOW, else `Terminal N`. Clamped to the wire's title bound where it is stored. |
| `kill-terminal id` | SIGKILL the process group, drop the terminal immediately. Confirmed in the UI. Implemented (WP-46). | The confirmation defaults to Cancel, not to Kill: the default button is what a reader presses on a dialog they have not finished reading, and on this one that must be the harmless half. Killing a terminal that is already gone is not an error — what was asked for is that it be dead, and it is. |
| `kill-session id` | Kill all terminals, drop the session. Confirmed in the UI (lists what will die). | Attached clients get `detached(reason=session-killed)` and fall back to the session picker. |
| `kill-server` | Kill all sessions, exit. Double-confirmed in the UI. | — |
| `resize-desktop` | Client (re)declares its size on attach and on host-terminal resize; server sets session `desktop_size`, clamps/repositions windows that would fall fully outside, broadcasts layout. | Windows are clamped, never destroyed; a shrunken desktop may overlap windows. Growing back restores nothing (no memory of pre-clamp positions in v1). |

## Two clients at once: how size and layout work (WP-40…WP-44)

Takeover below stays the DEFAULT and is not a v1 simplification. What this
section specifies is the opt-in that sits beside it, and it is written here
rather than in the parking lot because three of its findings are latent
defects in the single-client design as it stands today.

### The constraint that decides everything

A child has one PTY and one grid. Two readers cannot each give it a size.
tmux answers this at the client's own edge — `window-size` = largest /
smallest / latest — because a tmux pane IS a region of the client's screen, so
a second client of a different size reflows every child in the session.

ckmux has a layer tmux does not: the session owns a **virtual desktop**, a
terminal is a WINDOW on it, and a PTY's size is that window's content rect.
No client's terminal size appears anywhere in that chain. So the answer here
is not to reconcile sizes but to stop propagating them: **a client whose
screen differs from the session desktop has a viewing problem, not a resize
problem, and no child ever learns that a second reader attached.**

That is also what keeps "layout is session state" true rather than trading it
away. Its two purposes — survive reattach, and two clients agree on what they
see — both hold, because every stored rect is in the session desktop's
coordinates and means the same thing to everyone. WP-30's proportional
mapping then runs when the DESKTOP changes, not once per client.

### The three cases

| Client screen vs session desktop | What happens | What it costs |
|---|---|---|
| Larger | The desktop is drawn at its own size; the margin stays blank | Unused screen |
| Smaller | The client pans: a desktop-sized world seen through a smaller hole, offset held per client and never reported back (WP-43) | The reader cannot see all of it at once |
| The reader wants to change it | An explicit act — `Window ▸ Fit desktop to this screen`, or `ckmux attach --adopt-size` | Reflows every window and SIGWINCHes every child, for BOTH readers |

**Explicit is the point.** "Smallest attached wins" is the safe-looking
default that silently reflows a colleague's arrangement the moment somebody
attaches from a laptop, and pays for it with a SIGWINCH storm through every
child in the session. `[general] desktop-size = fixed | fit-smallest |
fit-latest`, default **fixed**: set when the session is created, changed only
when asked. `fit-smallest` is offered for readers arriving from tmux and is
deliberately STICKY — it shrinks to fit a newcomer and does not grow back when
they leave, because a session whose geometry oscillates with people's attach
cycles is worse than one that is merely too small.

### What stops being session state

Under one client these are indistinguishable from session state, which is why
they were built that way and why each is a latent defect rather than a
mistake. All of them are WP-41.

| Today | Under two clients | Why |
|---|---|---|
| `Session::focused`, set on every `Input` | Per client; the session keeps one as the reattach default | Two readers typing steal each other's focus marker |
| `TerminalMarks` (WP-19), set by the server and cleared once it has told **a** client | The FACT stays server-side ("rang at T", "wrote at T"); the MARK becomes each client's own | "A terminal you are not in" is a sentence about a reader. Cleared globally, the second client never sees the mark it was owed |
| Chrome collapse (WP-35) | Per client | Whether a reader hides their own status bar is not a fact about the session |
| Pan offset (WP-43) | Per client | It is where one reader is looking |
| `host_sixel`, folded in per client at attach | Fixed when the TERMINAL is opened | A child's capabilities must not change under it because somebody else attached; the other client renders `TerminalView`'s text fallback |

### Two hazards that are not obvious

**Pastes interleave.** WP-18's credit queue is per connection and the server
writes each chunk as it arrives, so two readers pasting into one terminal
produce a mixture neither of them typed. A terminal needs a write slot held
for the duration of a paste, with the second queued behind it — WP-42, and it
must land WITH multi-attach rather than after it.

**Layout can echo.** A's gesture → server → broadcast → B applies → B's clamp
nudges geometry → B reports → server broadcasts → A applies… The pieces that
prevent it exist: WP-29 reports only on gesture end, WP-30 has
`layout_settled_`, and U4-n validates an arrangement against what the last
mapping wrote so a reader's own drag invalidates rather than snapping back.
The rule to state plainly is **a client never reports a change it did not
originate**: fitting is local, gestures are reported.

## Multi-client policy: latest client wins (specified behavior)

Attaching to a session is **always granted, immediately, to the newest
requester**. This is a product requirement, not a v1 simplification: any
connect to an open session transfers the session to the latest client.

The transfer, step by step:

1. Client B sends `Attach` for a session client A currently holds.
2. The server detaches A *first*: A is sent
   `Detached{reason=takeover, text="taken over by <tty/host>, <time>"}`
   **best-effort** (bounded flush — an unresponsive, wedged, or half-dead A,
   e.g. a dropped SSH connection, can never delay or veto the transfer),
   then A is removed from the session's broadcast list.
3. The server attaches B and sends the full snapshot. From B's point of
   view, attaching an open session is indistinguishable from attaching an
   idle one: **no prompt, no negotiation, no waiting.**
4. A — if still alive — falls back to its session picker with a prominent
   notice (which session, when, taken over by whom). Re-attaching from A
   takes the session back the same way; nothing to unlock, nothing to
   clean up.

Rules that follow:

- "Latest" is well-defined: the single-threaded server serializes attach
  requests; the last arrival holds the session.
- One client is attached to at most one session at a time; switching
  sessions is detach + attach under the hood, seamless in the UI.
- Takeover also settles desktop sizing: the session's desktop is always
  sized by its one current client (a fresh `resize-desktop` accompanies
  every attach).
- **Simultaneous shared attach** (tmux-style mirrored clients) stays in the
  parking lot and would be **opt-in via an explicit flag if ever built** —
  takeover-by-default is the contract. The protocol already broadcasts
  state updates, so that future needs no wire change.

## Identity, ordering, uniqueness — quick rules

- ids (u64) are the only identity on the wire; names are labels.
- Session names unique; terminal names/titles are not.
- Terminal `index` is per-session, 1-based, dense; recomputed on close/move so
  the window list always shows `1..n`.
- Cycling order (`Next/Previous terminal`) = index order; z-order is
  independent (raise-on-focus).

## Lifecycle & failure semantics (the honest list)

| Event | Effect |
|---|---|
| Client exits / crashes / SSH drops | Socket closes → implicit detach. Nothing else happens. **This is the core promise.** |
| Server exits normally (last session killed / kill-server) | All PTY masters close → children get SIGHUP (normal Unix behavior). |
| Server crashes | Same as above: children die with it. Identical to tmux. Mitigations: keep the server minimal and heavily tested (it contains no UI); crash-only design (any client can vanish at any time); optional future "resurrect" (persist layout + argv, offer relaunch). |
| Host machine reboots | Everything gone; future resurrect feature applies. |
| Version mismatch client↔server | Refused at handshake with a clear message ("server is 0.3.1, client is 0.4.0; kill-server or use matching client"). See [05-protocol.md](05-protocol.md). During development this will happen constantly — the error must be excellent. |
| Clock changes | Irrelevant: all timing is monotonic. |

## Explicitly deferred

- Simultaneous multi-client attach (protocol-ready; would be opt-in via an
  explicit flag — automatic takeover remains the default contract).
- Linking one terminal into two sessions (tmux `link-window`): **rejected**
  for now — it complicates layout ownership for marginal value; `move` covers
  the stated need.
- Grouped sessions, session groups following focus (tmux `-t group`): out.
- Panes/splits inside one terminal window: possibly later; a terminal window
  is atomic in v1 (tile the *windows* instead).
- Persist/resurrect across server death: later, separate plan.
