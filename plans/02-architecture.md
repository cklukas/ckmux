# ckmux — Architecture

Plan 02 · Status: draft · 2026-08-18 · [Index](README.md)

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
| `ckmux --server <socket> [--foreground]` (internal) | server; users never type this. `--foreground` is for tests and for debugging: a process that double-forks cannot be waited for |
| `ckmux check-config`, `kill-server`, … | CLI utilities over the same socket protocol |
| `ckmux --help` / `--version` (also `-h`, `help`, `-V`) | answered in `main()` alone; no socket, no server |

**Of that table, `ckmux`, `--server`, `kill-server`, `--help` and `--version`
dispatch today**; every other subcommand meets an explicit refusal naming
`--help` rather than silently starting a client that ignores what was typed.
The rest is WP-11 ([12-work-packages.md](12-work-packages.md)).

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
  – tick due (max-fps) → observe exits, then for each attached session:
    diff/damage per terminal → encode deltas → send (respecting per-client
    backpressure, plan 05)
```

**Exits are observed on the tick, not caught as a signal.** The server installs
no `SIGCHLD` handler: ckVision's subsession reaps its own child and reports
`Exited` with a status, and the loop reads that where it reads everything else.
That is deliberate — a handler cannot safely touch the state a reap has to
update — but two halves of it are still open, and it is worth saying which:
the server currently observes the exit and sends no `TermClosed` for it
(m-exit-msg, remediation topic R2 in
[13-architecture-review.md](13-architecture-review.md)), and there is no
on-exit policy to apply yet (WP-13).

poll() suffices for the target scale (tens of fds); the fd-wait sits behind
a small seam (`platform/poller.{hpp,cpp}`) so Linux epoll / kqueue can slot
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
| Server gone — crashed, or ended with its last session | Children get SIGHUP (PTYs close). The client does **not** exit: it empties its desktop, greys every session-needing command, and the next request that needs a server starts one, throttled to a second so a machine where none can start is not asked twenty times a second (WP-8). Ending the last session and starting another is therefore one uninterrupted thing. Same blast radius as tmux; mitigated by server minimalism + tests + ASan/UBSan CI ([09](09-testing.md)). |
| Mirror desync (a sequence gap, ops that do not fit) | Not fatal: the mirror applies nothing and sets `needs_snapshot()`, the client re-sends `Attach`, and the server answers with every terminal whole. One message, one code path (plan 05). |
| Decode / framing error | Fatal for that connection on both sides: there is no way to resynchronise a stream whose framing is wrong. The server drops that client and serves the others; the client prints one line and exits — deliberately *after* ckVision has given the terminal back, because a line printed mid-frame lands on top of a window border. |
| Slow client (buffer full) | Server never blocks: per-client send queue with high-water mark → stop queueing deltas and collapse into "snapshot on drain" (plan 05). A wedged client cannot stall PTY reading. The mark is advisory today — a frame past it is still queued rather than dropped, so memory is not yet bounded, and the heal gate can re-snapshot repeatedly (M-S1/M-S5 → R2 in [13](13-architecture-review.md)). |
| Descriptor exhaustion at `accept()` (EMFILE) | `Socket::accept_one` returns an `AcceptResult` whose status separates `Idle` (nothing pending — the ordinary answer several times a second) from `Exhausted` and `Failed`, precisely so a caller does not read every `-1` as "try again" and spin on a listener that stays readable. The seam landed with topic R4 of [13](13-architecture-review.md); the server loop's adoption of it is R2's there. |
| `poll()` itself fails | `Poller::outcome()` distinguishes `Ready`/`TimedOut`/`Interrupted`/`Failed`, because three of those return an empty set and a caller that cannot tell a closed-but-watched descriptor from a timeout burns a core for the life of the process. Landed with R4; adopted by the loop in R2. |
| A spawned child inheriting ckmux's descriptors | Every descriptor ckmux opens is close-on-exec (`platform/descriptors.hpp`), so a shell in a terminal window cannot hold the control socket, the listener or the start lock. Fork sites also reset every signal disposition and unblock the mask, so a child does not inherit the server's ignored SIGPIPE. |
| PTY flood (`yes`, `cat /dev/urandom`) | Read caps per *pass* (shared, not per terminal) + emulator batch parsing + fps-coalesced output keep the server responsive. Bench target in [09](09-testing.md); measured `Ping` in 32 ms worst under `yes`. The child→PTY input queue is not yet byte-bounded. |
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
  config off-switch ([08](08-configuration.md)); reads are never allowed —
  refused upstream under every policy, which is the half that matters for this
  posture. The permissive half is **enabled and then dropped**: the profile the
  server gives a child accepts the write, and nothing carries it onward —
  `ClipboardSet` has no producer and no handler ([05](05-protocol.md)), so a
  copy out of a program inside ckmux reaches no host clipboard. M-R1 → R8 in
  [13-architecture-review.md](13-architecture-review.md).
- Resource caps per terminal: scrollback lines (`[general] scrollback`) and
  decoded-image memory (`[terminal] sixel-max-megapixels`, forwarded to the
  emulator's own pixel budget) are real and configurable. The input-queue byte
  cap and the per-server session/terminal limits are **designed and not built**:
  [08](08-configuration.md)'s key table has no key for either,
  [03](03-session-model.md) states a default of 64 terminals per session, and
  the limit is carried in [12](12-work-packages.md) under WP-13 — where the
  policy that decides what an over-limit request is *told* also lives. Saying
  so here rather than describing the intent in the present tense: this section
  is a posture, and a posture nothing enforces is exactly the standing risk
  [10-roadmap.md](10-roadmap.md)'s table now names — an invariant asserted in a
  document and checked by nothing.

## Repository layout

```
ckmux/
├── plans/                  # these documents
├── src/
│   ├── common/             # protocol encode/decode, ids, config, keymap, shell
│   ├── server/             # main loop, sessions, terminals (ckVision
│   │                       #   PosixTerminalSubsession + TerminalEmulator),
│   │                       #   snapshot diffing, tick/flush
│   ├── client/             # ckVision app: shell, views, mirrors
│   │                       #   (RemoteTerminalSubsession), commands
│   ├── platform/           # poller, socket/peercred, daemonize, paths,
│   │                       #   descriptor flags (CLOEXEC/NONBLOCK), clipboard
│   └── main.cpp            # the composition root: which role this argv is
├── tests/                  # unit + golden + integration (plan 09)
├── fuzz/                   # decoder fuzz targets + corpora (plan 09)
├── tools/                  # check_layers.py — the module and layer lint
├── .github/workflows/      # macOS, macOS+ASan/UBSan, Linux (advisory to M5)
└── CMakeLists.txt
```

**Four modules, four libraries.** Each of the directories above is its own
CMake target — `ckmux_platform`, `ckmux_common`, `ckmux_client`,
`ckmux_server` — rather than four folders compiled into two. `platform`
became a library of its own because listing it as a module while compiling its
sources into `ckmux_common` made "module" a claim about a folder: as a target
it is something a Linux port can swap or select as a unit, and its dependency
direction becomes checkable. The split costs nothing downstream — the include
path is unchanged and `ckmux_common` carries it `PUBLIC`.

Dependency rule: `common`, `platform` and `server` include only ckVision
`core`/`term` headers (emulator, PTY adapter, Unicode, diagnostics) — never
`ui`/`widgets`/`scene` — so the server builds and runs with no host terminal.
`client` is the only UI consumer, and `platform` depends on no other ckmux
module, which is what keeps the OS seam a seam instead of a place where common
code accumulates.

**What enforces it**: `tools/check_layers.py`, ported from ckVision's own layer
checker and registered as the CTest cases `layer_check` and
`layer_checker_self_test` (labels `script;architecture`). It reads every
`.hpp`/`.cpp` under `src/`, and fails on a forbidden ckVision layer, a
forbidden ckmux-module edge, a quoted include that is not in full
`"<module>/…"` form, an include target that does not exist, or a cycle in the
internal include graph. The self-test builds a tree containing every forbidden
edge and asserts each one is caught, because a checker that has quietly stopped
checking reads exactly like a clean repository.

The link line cannot say any of this, and a comment here used to claim it
could: `ckvision::cvision` is a single archive containing every layer, so
linking it proves nothing about which headers a translation unit used. The
rule is what lets the server run with no host terminal at all — the whole point
of the split — so it is enforced by something that runs rather than by
something a reader remembers (R9 in
[13-architecture-review.md](13-architecture-review.md)).

Language/build: **C++20 + CMake**, mirroring ckVision's standards and
conventions (zero mandatory external dependencies; ckVision consumed as a
sibling checkout / package per [07](07-ckvision-integration.md)).

## Portability seams (macOS now, Linux next)

| Concern | macOS | Linux | Seam |
|---|---|---|---|
| PTY open/spawn | ckVision `PosixTerminalSubsession` (`forkpty`, group-safe close) | same | upstream `term` layer |
| fd multiplexing | `poll` (later `kqueue`) | `poll` (later `epoll`) | `platform/poller` — `watching_` is the only member a kqueue/epoll build replaces; no caller can see it |
| Peer credentials | `LOCAL_PEERCRED` | `SO_PEERCRED` | `platform/socket` — both branches are written and `#if`'d; there is no separate `creds` file |
| Runtime dir | `$XDG_RUNTIME_DIR`, else `$TMPDIR`, else `/tmp` | same order | `platform/socket`'s `socket_path()` — one ordered chain on both platforms, not a per-platform choice, and each variable is ignored unless it holds an absolute path (XDG requires that, and honouring a relative one is a server nobody can find twice) |
| Config file | `$CKMUX_CONFIG`, else `$XDG_CONFIG_HOME`, else `$HOME/.config` | same | `platform/paths` |
| Close-on-exec / non-blocking | two-call `fcntl` (macOS has no `SOCK_CLOEXEC`/`accept4`) | `pipe2`/`accept4` where available | `platform/descriptors` |
| Clipboard helper | `pbcopy` (optional) | `wl-copy`/`xclip` (optional) | config `[terminal] clipboard` |
| Signals/termios | shared POSIX code | shared | `platform/process` — one `reset_signals_for_child()` every fork site uses, because a rule copied to each site is a rule that will one day differ between them |

Windows/ConPTY: out of scope, and now said in the build rather than implied —
`CMakeLists.txt` fails outright on a non-POSIX host instead of guarding only
the executable with `if(UNIX)` while the libraries that cannot compile
elsewhere built unconditionally. The seams above are the eventual door.
