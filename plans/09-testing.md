# ckmux — Testing & Verification

Plan 09 · Status: current · 2026-08-31 · [Index](README.md)

ckmux adopts ckVision's verification culture wholesale (categories, golden
discipline, sanitizers, deterministic benchmarks, "all tests pass" is not
evidence) and reuses its test infrastructure: the dependency-free
`cktest`-style harness, `HeadlessTerminal` + `VirtualDisplay`,
`RecordingTerminal`/`ReplayTerminal`, `ManualClock`, `MemoryFileSystem`, and
the hardened `ckv::golden` frame codec. CTest labels mirror upstream:
`unit`, `golden`, `script`, `pty`, `e2e`, `benchmark`, `fuzz-corpus`,
`portable`.

**Current Windows-foundation integration:** ckmux main consumes ckVision
v0.1.9. The portable ownership and dependency-pin gates, configurations,
inventories and remaining native work are recorded in [15](15-windows.md).
This is not a new native Windows package or a replacement for M4 acceptance.

**Historical v0.1.4 integration baseline:** ckmux v0.1.4 consumes ckVision v0.1.5
(public `0e67f04`). Before the version-only release bumps, macOS Debug and
ASan+UBSan passed 53/53 ckmux suites and 172/172 ckVision suites; Debian 13
GCC 13.3 Release with `_FORTIFY_SOURCE=2` passed the same 53/53 and 172/172.
The matched Release live `htop` audit reduced twenty rapid move frames from
182,028 host bytes to 25,581, with zero unchanged-cell writes and the same
final position. The public v0.1.4 tag at `f0ea0f6` then passed every native,
Ubuntu 20.04, Homebrew, package-inspection and publication job in GitHub run
`33446774634`; all eleven assets were downloaded and every package/archive
checksum verified. WP-15 and WP-21 still own the remaining milestone
acceptance coverage; a green 53-suite integration run is not a substitute for
those matrices.

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
  their shape checked so the numbers stay meaningful on a busier machine. The
  shape is taken from samples interleaved in time and judged by their minimum
  (one terminal against another) or median (a terminal inside the fleet),
  never from two means taken one after the other: a scheduler only adds time,
  and back-to-back means hand whatever the machine did in the second half to
  the second subject. A history walked on every tick is in every batch,
  cheapest included, so the minimum still sees it (mutation-checked: a copy of
  the history per tick, and a read of every history cell, both fail).

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

WP-7 landed the milestone gates as `tests/test_core_promise.cpp`, and they are
the only tests in the suite that run the real `ckmux` binary as a server with a
real child in a real PTY: the core promise (spawn a counter, attach, kill the
client, wait, reattach — `count 3` before, `count 11` after 500 ms unwatched),
the rehydrated frame compared cell for cell and style for style against the same
terminal a moment later, and the flood gate (`Ping` round trip while a child runs
`yes`: **~22 ms mean optimised, ~490 ms mean Debug** — see the note below). **The flood-gate figure, corrected 2026-08-20, and why it mattered.** This
paragraph originally recorded *32 ms worst, 24 ms mean* and **named no build
type**. It is an optimised-build number. In the configuration this project
documents and every gate actually runs — `-DCMAKE_BUILD_TYPE=Debug` — the same
test measures **~550 ms worst, ~490 ms mean**, and it did so at `046d738` too:
the commit that recorded the figure measures the same as HEAD when both are
built Debug. **There is no regression.** Five sessions spent the better part of
two days on one, because an unlabelled number was compared against Debug runs.

Two things follow that the number itself hides. The Debug margin against the
assertion (`worst_ms < 1000.0`) is **1.28x, not the 30x a reader infers from
32** — that is the real headroom, and one regression of ordinary size would
exhaust it. And the cost is located: in Debug, `TerminalEmulator::scroll_up_region`
move-assigns every `ckv::Cell` of every scrolled row, each carrying a
`std::string grapheme_`, so a row of `yes` output is ~80 un-inlined libc++
string moves. ckVision's L-48 fixed this shape for the emulator's history and
`2d3de20` fixed it for the mirror's; the visible region's scroll never got the
same treatment.

**And the original figure is not reconstructible.** `046d738` does not build
against any ckVision commit that existed when it was written — it uses
`ckv::widgets::ButtonRole`, which landed in ckVision at `211a123`, three and a
half hours later. It was measured against an uncommitted working tree. The
number above was taken against `211a123`, the earliest ckVision that compiles
with it, and is a reconstruction rather than the original.

Every defect WP-7 found was found by running it
rather than by reading it — including the one that mattered most, that with no
configuration file at all *no terminal could ever start*, because every other
test had named a shell explicitly.

**The layer that was missing, and the report that found it.** A reader ran ckmux
and said: "it seems the app is like a prototype where you say it's done but
actually nothing works or is tested". That was fair, and the reason is visible in
what the tests above ask. Everything underneath was covered — the protocol, the
mirror, the diff engine, the server, the emulator — and the tests that covered
COMMANDS asked whether a handler existed and whether a window appeared. A handler
that opens a dialog nobody can see passes both. So does a menu item wired to a
callback nothing ever set, a dialog with no room for its list, a terminal sized
so its content sits above the window, and a client that opens a stray shell on
every restart. All five shipped.

`tests/test_menu_commands.cpp` is the answer: the real `ckmux` binary, in a real
terminal, against a real server, pressing the keys a reader presses and reading
the screen a reader sees. Anything a reader can invoke belongs here, and a
command is not done until it is.

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
style). A gate that measures how long this machine took measures this machine;
these measure how much the server **says** and how much work it does to say it,
which is the same number on a busy laptop, an idle one, and a sanitizer lane.

Each gate prints its figure, because a budget nobody reads is a budget nobody
notices drifting. Ceilings sit well above what is measured and far below what
the unbounded version produced: what is asserted is a shape, not a machine.

`tests/test_budgets.cpp` (WP-20), measured 2026-08-19 at 80×24:

| Budget | Gate | Measured |
|---|---|---|
| Delta bytes for a scroll — `~O(rows)`, not `O(cells)` | under a quarter of a whole-screen delta, **and** the delta carries a `ScrollOp` | 746 B against 3759 B (19.8%) |
| Cells diffed per frame | one changed cell under 200 B | 58 B |
| Sparse process-monitor tick | 66 changed cells, exactly 66 cell values sent, under 3 KiB | 1 740 B in the deterministic 100×22 `htop`-style fixture |
| Attach snapshot, 10 terminals × 10 k scrollback | inside `kSnapshotPayloadBudget` before history, inside `kMaxSnapshotPayloadBytes` after | 74 694 B mandatory, 1 277 572 B whole |
| Picture payload built for a session nobody watches | exactly zero | 0 B across 59 animation frames |
| An idle terminal | no delta, no picture op, no bytes | 0 across 200 ticks |

The ratio gates carry a positive partner on purpose. A delta can be small
because it says too little, so "cheap" is asserted together with the mechanism
that makes it cheap — the scroll gate fails if the saving arrives without a
`ScrollOp` on the wire.

ckVision separately gates the outer-host half with a real window drag through
`Application`: a dense text window is moved through twenty positions while a
known-answering host withholds frame completion. The retained scene reaches
the twentieth position, the terminal receives only the first and latest large
frames, and their combined output stays below both three times one frame and
96 KiB. Together the two gates cover the independent queues found by the live
audit: sparse child-grid fields on the server wire, and obsolete composed
window positions on the host wire.

The matched live check is Release at 100×30: real `htop`, twenty window moves
10 ms apart, and terminal-status replies delayed by 120 ms to model an SSH
hop. The pre-fix binary emitted 182,028 host bytes in twenty intermediate
frames; the fixed binary emitted 25,581 bytes in three current frames (86%
less) and moved the left edge from column 2 to the requested column 7. Its
five-second steady `htop` phase emitted 8,639 bytes for 1,405 changed cells and
zero unchanged-cell writes.

The picture budgets are the ones WP-16 freed, and they are gated in two places
because they fail in two ways. `test_budgets.cpp` gates the work **built**;
`test_attach.cpp` gates what a client is **owed** when it cannot keep up —
1.8 frames held and 3.6 MiB built across 300 frames, against 354 MiB owed and
356 MiB built before supersession and the differ's readiness question, and
528 MiB owed across 200 window moves (see [05](05-protocol.md), Flow control).
Both were verified 24/24 under ASan+UBSan, and each of the four defects they
guard — debt appended instead of superseded, placements kept instead of
superseded, the payload built anyway, the belief adopted for pixels never sent
— was confirmed to fail exactly one suite when planted deliberately.

**Not gated, stated rather than implied.** *Bytes-parsed-per-flood-tick* has no
counter: the parser budget is ckVision's (`max_parser_work_per_step`) and is
gated upstream, while what ckmux owns — that a flood never stops the server
answering — is `test_core_promise.cpp`'s real `yes` and real `Ping`.
*Allocations per idle minute* is gated by proxy: replacing global `operator new`
to count them collides with the sanitizer lane this suite must also pass under,
so what is asserted is the behaviour that would cause the allocations rather
than the allocations themselves. Both are honest gaps, not silent ones.

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
matching upstream's split). Linux is blocking: the Debian 13 `ckdev` VM runs
GCC 13/14 and Clang locally, while public CI builds against both the pinned
ckVision release and ckVision HEAD. Host-terminal manual matrix per release:
WezTerm + iTerm2 (Sixel), Terminal.app (no Sixel — fallback path), xterm
(`-ti vt340`), plus upstream's `ckvision_terminal_probe` output attached to
release notes for each. The full three-host/vttest pass remains WP-21.
