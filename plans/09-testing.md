# ckmux — Testing & Verification

Plan 09 · Status: draft · 2026-08-16 · [Index](README.md)

ckmux adopts ckVision's verification culture wholesale (categories, golden
discipline, sanitizers, deterministic benchmarks, "all tests pass" is not
evidence) and reuses its test infrastructure: the dependency-free
`cktest`-style harness, `HeadlessTerminal` + `VirtualDisplay`,
`RecordingTerminal`/`ReplayTerminal`, `ManualClock`, `MemoryFileSystem`, and
the hardened `ckv::golden` frame codec. CTest labels mirror upstream:
`unit`, `golden`, `script`, `pty`, `e2e`, `benchmark`, `fuzz-corpus`,
`portable`.

## Layers (bottom-up)

### 1. Unit & property (label `unit`)

- **Protocol codec**: encode/decode round-trip for every message; property
  tests (random valid messages survive; random bytes never crash, only
  error); framing edge cases (split reads, coalesced frames, cap
  violations).
- **Config parser**: every default, every error message, chord grammar.
- **Session model**: pure server logic under `ManualClock` — create/attach/
  detach/move/kill state transitions, id/index/name rules, on-exit
  policies, takeover, empty-session reaping ([03-session-model.md](03-session-model.md)
  is the spec; each table row becomes at least one test).
- **Diff engine**: grid → deltas → apply-to-mirror ⇒ byte-identical grid
  (property-tested with random edit scripts, scroll ops included). Landed with
  WP-4a as `tests/test_grid_delta.cpp`: six hundred rounds of random writes,
  region scrolls both ways, cursor moves, single mode bits, titles and history
  pushes, comparing the mirror against the model after every delta — with the
  script asserting it actually exercised scrolls, since a run whose scroll arm
  never fired would pass while proving nothing. The generator is a fixed-seed
  LCG rather than a clock-seeded engine: a property test that cannot be
  replayed reports "something is wrong somewhere" once and then never again.
  Three deliberate mutations were checked to fail it (a mispredicted shift, a
  repaint pass that stops at the first changed row, an `apply` that skips its
  fit check), because a property test nobody has seen fail is a claim about
  itself.
- **Diff engine on live terminals** (WP-4b, `tests/test_diff_engine.cpp`): real
  PTYs with real children, and a mirror built out of nothing but the deltas the
  server produced — the claim being checked is not "the server thinks it sent
  the right thing" but "a client that saw only these bytes holds what the
  terminal holds", cell for cell. Plus the adversarial sequence test (a silent
  terminal, a chatty one, a flood, one resized under load, one closed mid-run,
  and ticks where nothing happened: per terminal the numbers must still be 1, 2,
  3 with nothing missing and nothing repeated, because a false gap is a reattach
  storm and a missed gap is a screen that quietly stops matching), and the two
  measurements the package is accepted on — printed, not asserted, with only
  their shape checked so the numbers stay meaningful on a busier machine.

### 2. Emulator conformance pins (label `golden`)

The emulator is upstream's, but ckmux depends on exact semantics — so ckmux
keeps its own golden pins: byte scripts (the [04](04-terminal-emulation.md)
gotcha list: pending-wrap, resize, ED 2/3, alt-screen, wide-at-margin,
mid-drag encoding flips, hostile titles, and the WP-PRINT set — `5i…4i`
with the terminator **split across two `feed_output()` calls**, `4 i` with
no `5 i`, nested `5 i`, autoprint across a scroll, cache overflow → sink →
`4 i` → the grid resumes untouched) → `feed_output` → snapshot →
`ckv::golden` serialization. These catch upstream behavior changes at
version-bump time, before users do — the pre-1.0 pin's safety net.

### 3. Server integration (labels `script`, `pty`)

WP-2 landed the lifecycle half of this layer as
`tests/test_server_lifecycle.cpp` (real `ckmux --server` processes on a private
`CKMUX_SOCKET`: lazy start and the detached server outliving its client, an
eight-way start race, stale-socket recovery, version-mismatch refusal with the
remedy in words, a connection that does not say Hello first, `kill-server`,
`Error{NotImplemented}`, and a garbage frame killing one connection and no
other) and `tests/test_server_loop.cpp` (the same server in-process on a
`ManualClock`, for the questions about *when*: a handshake that does not wait
for a tick, a delta that arrives on the tick and not before, a CLI client that
is sent no deltas, a client's death being a detach, and four hundred passes over
a client that never reads). Both were mutation-checked, and the race case was
stressed thirty times after a one-in-twenty flake was found and fixed.

Real server process, scripted test client speaking the real protocol over a
private `CKMUX_SOCKET`:

- lifecycle: lazy start, bind race (two starters), stale socket recovery,
  version-mismatch refusal, kill-server;
- sessions: every op from [03](03-session-model.md) through the wire,
  including move-terminal across sessions with both sessions' update
  streams asserted;
- **the core promise**: spawn `sh` printing a counter → attach → `kill -9`
  the client → wait → reattach → snapshot shows the counter advanced;
  golden-compare the rehydrated frame;
- real PTY children (label `pty`): exit-status reporting, close protocol
  grace path, SIGWINCH on window resize, flood with `yes` (budget gate,
  server stays responsive — asserted via `Ping` round-trip latency during
  the flood);
- backpressure: artificially slow client → high-water → snapshot-on-drain
  self-heal asserted;
- **print jobs** (WP-PRINT): child prints → `PrintJobAdded` metadata only
  (assert the payload never rides the delta stream or the snapshot) →
  `PrintJobFetch` chunked round-trip → save → discard; jobs survive
  detach/reattach; `SetPrinterPolicy` at all three scopes with precedence
  asserted, and `off` making the child's `CSI ? 15 n` answer *no printer*.

WP-5 landed the mirror half of the client layer as
`tests/test_remote_terminal.cpp`: a real server-side `DiffEngine` over a real
emulator on one side, a real `TerminalMirror` and `RemoteTerminalSubsession` on
the other, and a real `TerminalView` and `ClientApp` on top — everything except
the socket, which WP-2 already tests. What it pins: the mirror holds the server's
terminal cell for cell; a delta that skips a number is refused rather than
half-applied; the child's dialect survives the wire (the same arrow key encodes
`ESC [ A` or `ESC O A` depending on the mode the server reported); the history
arrives in the snapshot, so a reattached reader can page back with no round trip;
the reader's own `scrollback` bounds what the client keeps; a resize carries the
text area in pixels; and `^B c` opens a second remote terminal without a byte of
that chord reaching the child. Screen assertions decode the client's output
through a `VirtualDisplay` rather than grepping the byte stream — the presenter
writes runs with cursor moves between them, so "is the text in the bytes" is the
wrong question and passed for the wrong reasons until it was asked properly.

WP-6 landed the attach half as `tests/test_attach.cpp`: a real `Server` over a
real Unix socket with the loop stepped on a `ManualClock`, and the real
`ServerSession` on the other end. Attaching hands over every terminal whole; a
second client takes the session and the first is told why; a client that dies
without saying anything is a detach and its terminals do not notice; a mirror
that lost track asks for the terminal whole; and the acceptance case — a client
that stops reading crosses the 4 MiB mark after 258 ticks of full-screen output,
stops being queued for, and is healed by a snapshot when it drains. Three
mutations were checked to fail it (no healing, no takeover, no high-water rule at
all) — and one of those first appeared to pass because the flag it removed is set
in two places, which is worth knowing about the code rather than about the test.

### 4. Client UI (labels `script`, `golden`)

`HeadlessTerminal` hosts the real client against a scripted fake server
(inject protocol messages; capture `Input`/op messages out):

- full-screen goldens: empty desktop, session picker, menus open, which-key
  popup, two overlapping terminal windows with Sixel raster, footer per
  context ([06](06-ui-design.md) contexts each get one);
- interaction scripts through the public event path (ckVision rule: direct
  handler calls are not acceptable): prefix chords, menu mouse trip,
  move/resize mode, copy-mode selection → clipboard message, paste
  chunking;
- mirror correctness: recorded delta streams → `RemoteTerminalSubsession`
  → `TerminalView` render golden.

### 5. End-to-end (label `e2e`)

Client + server + real children, client hosted in a PTY pair and its output
decoded by `VirtualDisplay` (upstream's own e2e pattern): run `vim`, edit,
detach, reattach, assert the decoded screen; run `htop` under mouse scripts;
`img2sixel` a fixture and assert raster presence (and its absence when
`sixel = off`); print from a child, click the frame button in the decoded
screen, answer the Ask popup, save through the file dialog, assert the file
exists with the expected bytes — and separately that overflowing the ask
cache leaves the child's screen output intact once `CSI 4 i` arrives. Reference-app matrix from [04](04-terminal-emulation.md)
runs scripted where feasible, manual checklist per milestone otherwise
(vttest chapters, mc line-drawing, nested tmux).

### 6. Adversarial & fuzz (label `fuzz-corpus`)

libFuzzer targets (LLVM clang lane, as upstream): protocol decoder
server-side, protocol decoder client-side, config parser. Seed corpora
checked in; CI runs bounded corpora under ASan+UBSan. Adversarial unit
cases everywhere state machines live: half-frames, duplicate ids, ops for
dead terminals, snapshot during drain, reentrancy via `post()`. A new parse
state is new attack surface, so the upstream emulator fuzz corpus gains
printer-controller seeds with WP-PRINT (U1-c) — a child that opens `CSI 5 i`
and never closes it must cost exactly the ask cache and not one byte more.

### 7. Benchmarks as gates (label `benchmark`)

Deterministic budget counters, not wall-clock (upstream's `bench_terminal`
style): bytes-parsed-per-flood-tick, cells-diffed-per-frame, delta bytes
for a full-screen `vim` scroll (scroll ops must keep it ~O(rows) not
O(cells)), attach-snapshot size for 10 terminals × 10 k scrollback,
allocations per idle minute (target: zero).

## Invariants the suite enforces globally

1. **Determinism**: `ManualClock` in every test; no sleeps, no wall-clock,
   no test ordering dependence; PTY tests use readiness, not delays.
2. **Sanitizer-clean**: ASan/UBSan across all labels in CI; TSan trivially
   (no threads — the check is that it *stays* trivial).
3. **Zero-warning** builds (`-Wall -Wextra -Wpedantic -Wshadow -Werror`).
4. **Per-landing evidence** (adopted from ckVision): regression test that
   fails pre-fix, category counts reported, sanitizer configs and platforms
   named in the commit message.
5. Every plan-doc behavior table row maps to a named test — the plans are
   the spec; drift is a test failure or a plan edit, never silence.

## Platforms

macOS is the daily gate (Apple clang + a homebrew LLVM lane for fuzzers,
matching upstream's split). Linux container lane joins at the M5 port
milestone and then becomes blocking. Host-terminal manual matrix per
release: WezTerm + iTerm2 (Sixel), Terminal.app (no Sixel — fallback path),
xterm (`-ti vt340`), plus upstream's `ckvision_terminal_probe` output
attached to release notes for each.
