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
  (property-tested with random edit scripts, scroll ops included).

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
