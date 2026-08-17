# ckmux — Architecture

Plan 02 · Status: draft · 2026-08-16 · [Index](README.md)

## The one diagram

```
 host terminal (iTerm2, WezTerm, …)
 ┌───────────────────────────────┐
 │  ckmux client (ckVision app)  │   ephemeral: dies freely, holds no state
 │  menu bar · desktop · footer  │
 └──────────────┬────────────────┘
                │ Unix socket, framed messages (plan 05)
 ┌──────────────┴────────────────┐
 │        ckmux server           │   persistent: owns everything
 │  ┌─ Session "build" ────────┐ │
 │  │ Terminal 1: PTY ⇄ VT emu │ │──── fork/exec ──▶ zsh ─▶ vim
 │  │ Terminal 2: PTY ⇄ VT emu │ │──── fork/exec ──▶ zsh ─▶ make
 │  │ layout · z-order · focus │ │
 │  └──────────────────────────┘ │
 │  ┌─ Session "ops" ──────────┐ │
 │  │ Terminal 3: PTY ⇄ VT emu │ │──── fork/exec ──▶ ssh …
 │  └──────────────────────────┘ │
 └───────────────────────────────┘
```

**Core decision: the terminal state lives in the server.** Each terminal's
PTY master *and* its full VT-emulation state (grid, scrollback, modes,
images) are owned by the detached server process. The client is a renderer
and input device. This is the only design that satisfies the requirements
simultaneously:

- child apps survive client exit (PTY master never closes on detach);
- reattach shows *current* screens instantly (no replay heuristics);
- terminals can move between sessions inside one process (pointer move);
- a future second client is a broadcast, not a redesign.

The alternative (client-side emulation + raw-byte replay on attach, as
dtach does) was rejected: replay is unbounded, loses alternate-screen and
mode state, and cannot reconstruct scrollback accumulated while detached.

Where the VT engine itself comes from (ckVision component vs. ckmux code) is
a ckVision-grounding question — see [07-ckvision-integration.md](07-ckvision-integration.md).
The architectural requirement it must meet: **usable headless** (no host
terminal, no UI) inside the server.

## Decision: one server, all sessions

One server per socket path owns *all* sessions (tmux model), rather than one
daemon per session (screen/abduco model). Rationale: `move-terminal` between
sessions becomes an in-process pointer move; one socket to discover; one
process to supervise; the session picker is a single `list` call. Cost: one
bug can take down all sessions — countered by keeping the server small,
UI-free, and heavily tested ([09-testing.md](09-testing.md)).

Parallel universes remain possible: `CKMUX_SOCKET=/path` gives tests and
daring users an isolated server.

## Processes

One binary, three roles (dispatch on argv):

| Invocation | Role |
|---|---|
| `ckmux` (and `attach`, `new`, `ls`, …) | client / CLI |
| `ckmux --server <socket>` (internal) | server; users never type this |
| `ckmux check-config`, `kill-server`, … | CLI utilities over the same socket protocol |

Single binary keeps client/server versions paired (they still handshake —
plan 05 — because old servers meet new binaries after upgrades).

### Server startup (lazy, race-free)

First client: connect → `ECONNREFUSED`/`ENOENT` → fork+exec
`ckmux --server`, which double-forks, `setsid()`, chdir `/`, redirects stdio
to the log, then **binds the socket** under a start lock (below): a concurrent
starter loses the race and simply retries connecting (with backoff,
~2 s cap). Stale socket files (server crashed): connect fails with
`ECONNREFUSED` → unlink, start fresh.

**The bind is not the lock** — that first sentence was wrong, and WP-2 found out
why. A stale socket file has to be unlinked before a bind can succeed, so two
starters that each unlink and bind both succeed, and the one that went first is
left listening on a socket file nobody will ever reach again. What settles the
race is a sibling `default.sock.lock`, `flock`ed exclusively and held for the
server's whole life: a starter takes the lock, and only under it probes for a
live server, unlinks a stale socket and binds. A starter that cannot take the
lock has lost, and goes back to connecting with the same backoff.

Socket path: `$CKMUX_SOCKET`, else `<runtime>/ckmux-<uid>/default.sock`
where `<runtime>` = `$XDG_RUNTIME_DIR` (Linux) / `$TMPDIR` (macOS) / `/tmp`
fallback. Dir mode `0700`, owner-checked before use; peer UID verified on
every accept (`LOCAL_PEERCRED` / `SO_PEERCRED`) — belt and suspenders.
(macOS `sun_path` limit is 104 bytes; `$TMPDIR` fits comfortably.)

## Event model: single-threaded, poll-based, both sides

**No threads anywhere in v1.** Determinism, no locking, trivial reasoning
about state. Profiling may later justify a worker for Sixel decoding — that
is the only anticipated candidate.

### Server loop

`poll()` over: listener, client sockets, every terminal's PTY master
(exposed by ckVision's `PosixTerminalSubsession::wait_handles()`),
self-pipe for signals, plus a timer for the flush tick. The server links
only ckVision's `core` + `term` layers (emulator, PTY adapter, Unicode,
diagnostics) — no UI, no host terminal, per the layer discipline both
projects enforce.

```
loop:
  poll(fds, next_tick_deadline)
  – accept new clients → handshake
  – client socket readable → decode messages → apply (input, layout ops, …)
  – PTY readable → terminals.drain(64 KiB for the PASS, shared) → emulator
    state advances. Shared, not per terminal: parsing 64 KiB of a flooding
    child is 22 ms of real work in an optimised build, so a budget each means a
    pass that grows with the number of busy children until the loop answers
    nothing else (WP-7 measured exactly that).
  – PTY writable → drain pending input queue (paste backpressure)
  – SIGCHLD → reap, mark terminal exited, apply on-exit policy
  – tick due (max-fps) → for each attached session: diff/damage per
    terminal → encode deltas → send (respecting per-client backpressure,
    plan 05)
```

poll() suffices for the target scale (tens of fds); the fd-wait sits behind
a 30-line seam (`platform/poller.{h,cc}`) so Linux epoll / kqueue can slot
in later without touching logic.

### Client loop

The client owns its loop and drives ckVision as a guest — the pattern
ckVision documents and sanctions (D-021 embeddable loop):

```
loop:
  fds = app.wait_handles() ∪ { server_socket }     // tty fd + wake pipe + …
  deadline = min(app.next_timer_deadline_nanos(), our own deadlines)
  poll(fds, deadline)
  – socket readable → decode → apply GridDelta/Layout/… to mirrors →
    invalidate the affected views
  – app.step(now)   // ckVision dispatches tty input, timers, paints damage
```

Keystrokes for a focused terminal are encoded *inside* `step()` by the
terminal widget and surface as `Input{term, bytes}` messages
([04](04-terminal-emulation.md) / [05](05-protocol.md)).

The client renders from **mirror state**: a per-terminal rehydrated
snapshot (grid, modes, cursor, scrollback, images) plus the session layout,
mutated only by server messages — the server stays authoritative, and a
reconnect simply rebuilds mirrors from a fresh snapshot. Mirrors implement
ckVision's `core::TerminalSubsession` seam so the stock terminal widget
renders them verbatim ([07-ckvision-integration.md](07-ckvision-integration.md)).

## Data-flow walkthroughs (the four that matter)

1. **Keystroke** → host terminal bytes → ckVision input decoder → focused
   ckmux surface. Terminal focused: the terminal widget encodes for the
   inner app ([04](04-terminal-emulation.md)) → `Input{term,bytes}` →
   server → PTY write (queued if full). Menu/dialog focused: handled
   client-side entirely.
2. **Program output** → PTY master readable → `drain()` feeds the emulator
   (grid, modes, scrollback, images advance) → at tick: server diffs →
   `GridDelta` (+ scroll/image ops) → client applies to mirror → ckVision
   repaints those cells.
3. **Attach** → client sends `Attach{session, size}` → server responds with
   a consistent snapshot (layout, every terminal's grid+modes+cursor,
   scrollback tails lazily) → client builds windows → live deltas follow.
4. **Detach / client death** → socket HUP → server drops the client from
   broadcast lists. Nothing else changes. (This path is exercised by
   `kill -9` of the client in tests — the promise is that it is boring.)

## Failure matrix

| Failure | Handling |
|---|---|
| Client crash / kill -9 / SSH drop | Implicit detach (socket HUP). Server unaffected. |
| Server crash | Children get SIGHUP (PTYs close). Client shows a clear error and exits to the session picker's "no server" state. Same blast radius as tmux; mitigated by server minimalism + tests + ASan/UBSan CI ([09](09-testing.md)). |
| Protocol desync / decode error | Both sides treat it as fatal for that connection: log, close socket. Client auto-reconnects once (fresh snapshot heals all mirror drift). |
| Slow client (buffer full) | Server never blocks: per-client send queue with high-water mark → collapse pending deltas into "snapshot on drain" (plan 05). A wedged client cannot stall PTY reading. |
| PTY flood (`yes`, `cat /dev/urandom`) | Read caps per tick + emulator batch parsing + fps-coalesced output keep the server responsive; input queue bounded with backpressure. Bench target in [09](09-testing.md). |
| Version mismatch | Handshake refuses with actionable message (plan 05). |

## Security posture

- Socket: `0700` dir + `0600` socket + peer-UID check. No TCP, ever (v1
  non-goal; remote = ssh).
- **Sanitization by construction**: child output is *parsed into state*
  (cells, images) and re-rendered by ckVision — raw child bytes are never
  forwarded to the host terminal. Title strings are length-capped and
  control-stripped before display. Sixel is decoded server-side and
  re-encoded client-side. An inner app can therefore never smuggle escape
  sequences at the host terminal (the classic `cat malicious-file` attack).
- OSC 52 clipboard writes from inner apps: allowed by default, 64 KiB cap,
  config off-switch ([08](08-configuration.md)); reads are never allowed.
- Resource caps per terminal: scrollback lines, decoded-image memory (LRU),
  input queue bytes; per server: max sessions/terminals (config).

## Repository layout (proposed)

```
ckmux/
├── plans/                  # these documents
├── src/
│   ├── common/             # protocol encode/decode, ids, config, log, util
│   ├── server/             # main loop, sessions, terminals (ckVision
│   │                       #   PosixTerminalSubsession + TerminalEmulator),
│   │                       #   snapshot diffing, tick/flush
│   ├── client/             # ckVision app: shell, views, mirrors
│   │                       #   (RemoteTerminalSubsession), commands
│   └── platform/           # poller, socket/peercred, daemonize, paths
├── tests/                  # unit + golden + integration (plan 09)
├── tools/                  # dev helpers (protocol dump, session dump, …)
└── CMakeLists.txt
```

Dependency rule: `common` and `server` include only ckVision `core`/`term`
headers (emulator, PTY adapter, Unicode, diagnostics) — never `ui`/
`widgets`/`scene` — so the server builds and runs with no host terminal.
`client` is the only UI consumer. Enforced by an include-lint in CI,
imitating ckVision's own layer checker.

Language/build: **C++20 + CMake**, mirroring ckVision's standards and
conventions (zero mandatory external dependencies; ckVision consumed as a
sibling checkout / package per [07](07-ckvision-integration.md)).

## Portability seams (macOS now, Linux next)

| Concern | macOS | Linux | Seam |
|---|---|---|---|
| PTY open/spawn | ckVision `PosixTerminalSubsession` (`forkpty`, group-safe close) | same | upstream `term` layer |
| fd multiplexing | `poll` (later `kqueue`) | `poll` (later `epoll`) | `platform/poller` |
| Peer credentials | `LOCAL_PEERCRED` | `SO_PEERCRED` | `platform/creds` |
| Runtime dir | `$TMPDIR` | `$XDG_RUNTIME_DIR` | `platform/paths` |
| Clipboard helper | `pbcopy` (optional) | `wl-copy`/`xclip` (optional) | config `clipboard` |
| Signals/termios | shared POSIX code | shared | — |

Windows/ConPTY: out of scope; the seams above are the eventual door.
