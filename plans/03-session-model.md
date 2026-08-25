# ckmux — Session Model

Plan 03 · Status: current · 2026-08-25 · [Index](README.md)

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
| `desktop_size` | Cols×rows of the session's virtual desktop (see multi-client rules below). Server-owned since WP-40; a lone reader may resize it, while an additional reader pans unless an explicit size policy adopts that reader's screen. |
| `layout` | Per-terminal window geometry + z-order + focused terminal + maximized flag. |
| `attached_clients` | 0..n. Takeover is the default; opt-in join and server-enforced read-only watch are live (WP-44, WP-48…50). |

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
| `attach session` | Bind client to session; server sends full snapshot (layout + every terminal's screen + scrollback tail + images). Sets `last_attached_at`. | If readers are already attached, mode **take over** evicts them best-effort, **join** keeps them, and **watch** joins read-only. No flag means take over (full spec below). |
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

Takeover stays the DEFAULT and is not a v1 simplification. The opt-in modes
that sit beside it, plus the reader/session state split they required, landed
through WP-40…50.

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

### What stopped being session state

Under one client these were indistinguishable from session state. WP-41 moved
them to the reader side before simultaneous attach shipped.

| Before WP-41 | Current ownership | Why |
|---|---|---|
| `Session::focused`, set on every `Input` | Per client; the session keeps one as the reattach default | Two readers typing steal each other's focus marker |
| `TerminalMarks` (WP-19), set by the server and cleared once it has told **a** client | The FACT stays server-side ("rang at T", "wrote at T"); the MARK becomes each client's own | "A terminal you are not in" is a sentence about a reader. Cleared globally, the second client never sees the mark it was owed |
| Chrome collapse (WP-35) | Per client | Whether a reader hides their own status bar is not a fact about the session |
| Pan offset (WP-43) | Per client | It is where one reader is looking |
| `host_sixel`, folded in per client at attach | Fixed when the TERMINAL is opened | A child's capabilities must not change under it because somebody else attached; the other client renders `TerminalView`'s text fallback |

### Two hazards that are not obvious

**Pastes would interleave without arbitration.** WP-42 added a terminal-level
write slot for the duration of a paste, with a second paste queued behind it;
it landed before multi-attach.

**Layout can echo.** A's gesture → server → broadcast → B applies → B's clamp
nudges geometry → B reports → server broadcasts → A applies… The pieces that
prevent it exist: WP-29 reports only on gesture end, WP-30 has
`layout_settled_`, and U4-n validates an arrangement against what the last
mapping wrote so a reader's own drag invalidates rather than snapping back.
The rule to state plainly is **a client never reports a change it did not
originate**: fitting is local, gestures are reported.

### Reader modes (WP-49, WP-50)

WP-44 first built the opt-in as a boolean: take the session, or join it.
WP-48…50 widened that byte to `Attach.mode` and added the third reader intent:
**watch without being able to type**.

| Mode | Value | What it does to the readers already there | What this reader may do |
|---|---|---|---|
| Take over | 0 | Every one of them is detached (`Detached{takeover}`), the default path | Everything |
| Join | 1 | Nothing; they stay | Everything |
| Watch | 2 | Nothing; they stay | Read the session, and nothing that changes it |

Values 0 and 1 keep the meaning WP-44 shipped, so the widening is additive on
the wire and no client is misread by an older server. The default with no flag
is still 0, which is D-07 and is not up for revision here.

**Read-only is enforced by the server, not by the client.** A client that
greys its own views has adopted a convention; a reader mode that a stale or
modified client can ignore is not a mode. The client greys as well, and that
is courtesy rather than the mechanism.

What "read-only" means precisely — and it means read-only *to the session*,
not a crippled interface:

| Refused from a watcher | Why |
|---|---|
| `Input`, `PasteChunk` | The point of the mode |
| `NewTerminal`, `CloseTerminal`, `KillTerminal`, `MoveTerminal` | A reader who can end a program is not read-only in any sense worth having |
| `RenameSession`, `KillSession`, `SetDesktopSize` | Change the session for everybody watching it |
| `SetLayout` | Window arrangement is session state; a watcher rearranging it moves the other reader's windows |

| Still theirs | Why |
|---|---|
| Scrollback, copy mode, selection, clipboard | Client-local; no wire message at all |
| Pan offset, chrome collapse, window focus, terminal focus | WP-41 and WP-43 already made these a reader's own |
| `ListSessions`, `WatchStats`, `Attach` (resnapshot), `Detach` | Questions and self-repair, not changes |

A watcher's own drags are not blocked at the client — ckVision windows stay
draggable — so a watcher may arrange their desktop locally, nothing is
reported, and the next `LayoutDelta` from the session overwrites it. That is
the same contract a client that fell behind already has, and it is honest. The
nicer version, where the windows of a watched session are simply not draggable,
is a ckVision capability and belongs in
[07-ckvision-integration.md](07-ckvision-integration.md) as a `U-` item if it
is ever wanted; it is deliberately not a blocker here.

### Changing a mode after the attach, and doing it TO somebody

Re-attaching to change a mode would work — the eviction is keyed off `Attach`
— and it is the wrong shape: it costs a full snapshot to change a fact that
alters nothing on screen. One message carries both directions instead:

`SetReaderMode { scope, mode }`, where `scope` is **me** or **every other
reader of this session**, and `mode` is the same three values.

Four of the six combinations are things a reader actually wants, and
"disconnect the others" falls out of the table rather than needing a verb of
its own:

| scope | mode | What the reader asked for |
|---|---|---|
| others | take over | "I want this session to myself" — evicts, reusing the `Detached{takeover}` path |
| others | watch | "Look, but don't touch" |
| others | join | Hands typing back to the readers put on watch |
| me | watch | "I'm only here to look" — the self-restriction, after the fact |
| me | join | Undoes the above |
| me | take over | Rejected at the wire: attaching is how a session is taken, and a scope of *me* cannot evict me |

A reader whose mode was changed **by somebody else** is told — `ReaderMode` is
pushed to them, and their client says so. A reader who changed their OWN mode
is not told anything, because they just did it.

**Any attached reader may do this to any other. There is no owner.** That is a
decision and not an omission. The socket is already the trust boundary — it is
uid-private, and every reader on it is the same human or somebody that human
invited to their machine — so an owner among readers would be a permission
system with exactly one user in it, and the first thing it would do is
reintroduce the lockout that D-07 exists to forbid. Symmetric also means
recoverable: whatever one reader does, the other can undo.

**No identity, deliberately.** [06-ui-design.md](06-ui-design.md)'s original
picker sketch promised *"Took over 'build' from ttys004"*, and nothing on the
wire carries a client name to fill that in. It stays unfilled: a tty name is
not an identity anybody recognises when both clients are one person's two
windows, and adding a name field is the first half of the owner model just
refused. Messages say "another reader".

**No setting.** A `[general] attach-policy` default was designed and dropped,
on the reader's ruling, and it is recorded here so it is not re-proposed: the
choice is made in the picker where the consequence is visible and the reader
count is on screen, and a stored default would silently change what a bare
`ckmux attach <name>` does to a colleague. The CLI keeps explicit flags —
`--share` (join, shipped with WP-44) and `--watch` (join read-only) — and
takes over when given neither, which needs no flag of its own precisely
because there is no setting for it to disambiguate against.

## Default multi-client policy: latest client wins

A take-over attach is **always granted, immediately, to the newest requester**.
This is the default and a product requirement, not a v1 simplification.
Explicit join and watch attaches preserve existing readers instead.

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
   notice that another reader took over the session. Re-attaching from A
   takes the session back the same way; nothing to unlock, nothing to
   clean up.

Rules that follow:

- "Latest" is well-defined: the single-threaded server serializes attach
  requests; the last arrival holds the session.
- One client is attached to at most one session at a time; switching
  sessions is detach + attach under the hood, seamless in the UI.
- Takeover changes who may read the session, not its geometry. The session's
  desktop-size policy and explicit `--adopt-size` action govern resizing.
- **Simultaneous shared attach** is built and is opt-in (WP-44, landed
  2026-08-20): takeover-by-default remains the contract, and a client that
  asks to join gets to join. What each of the three modes means, and what a
  reader may do to the readers already there, is "Reader modes" above. The
  step-by-step transfer here describes mode **take over** only; under **join**
  and **watch** step 2 does not happen at all.

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

- ~~Simultaneous multi-client attach~~ — **built** (WP-44), opt-in, with
  automatic takeover remaining the default contract. What is still deferred is
  narrower: an *identity* for a reader (a tty or host name shown to the
  others), and a privileged owner among readers. Both are refused rather than
  postponed, for reasons recorded in "Reader modes".
- Linking one terminal into two sessions (tmux `link-window`): **rejected**
  for now — it complicates layout ownership for marginal value; `move` covers
  the stated need.
- Grouped sessions, session groups following focus (tmux `-t group`): out.
- Panes/splits inside one terminal window: possibly later; a terminal window
  is atomic in v1 (tile the *windows* instead).
- Persist/resurrect across server death: later, separate plan.
