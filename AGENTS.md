# Working on ckmux

ckmux is a terminal multiplexer with a visible interface: tmux's persistence
model plus a **ckVision** desktop — permanent menu bar, permanent
context-sensitive hotkey footer, terminals in movable windows.

**Read [plans/README.md](plans/README.md) first.** The plans are the
specification; this file is only the working agreement.

**"Work on the next open package" means:** open
[plans/12-work-packages.md](plans/12-work-packages.md), continue the row marked
`In progress` if there is one, otherwise take the first row marked `Ready`.
Never pick work out of milestone prose — [plans/10-roadmap.md](plans/10-roadmap.md)
says what a milestone must satisfy, not what is open.

## The rule that matters most

**Fix ckVision, never work around it.** ckmux is one of the first real
applications of ckVision (`../ckvision`) and deliberately its proving ground.
We develop both repositories. When a capability, widget, or API is missing,
awkward, or slow, the change is made *in ckVision*, under ckVision's own
conventions and verification gates — then consumed here.

Litmus test: *would a second ckVision application plausibly want this?* → it
belongs in ckVision. *Does it need ckmux's server, protocol, or session
semantics?* → it belongs here. When in doubt, ckVision.

A ckmux commit that patches around a library limitation is a design failure,
not a shortcut. If a library change is not done yet, say so out loud in the
roadmap risk table rather than papering over it.

Every such change is recorded in
[plans/07-ckvision-integration.md](plans/07-ckvision-integration.md).

## Layout

```
plans/     the specification (living documents)
src/common protocol, config, logging          (M2+)
src/server sessions, PTYs, emulators          (M2+)
src/client the ckVision application           ← M1 lives here
src/platform sockets, poller, paths           (M2+)
tests/     cktest suites, one CTest entry per file
```

Dependency rule: `common` and `server` may include only ckVision `core` and
`term` headers — never `ui`, `widgets`, or `scene` — so the server builds and
runs with no host terminal. `client` is the only UI consumer.

## Build and verify

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j8
ctest --test-dir build --output-on-failure
```

ckVision is found as an installed package, or built from `../ckvision`
(override with `-DCKMUX_CKVISION_SOURCE_DIR=…`). Sanitizers:

```bash
cmake -S . -B build-asan -DCKMUX_SANITIZE=address,undefined -DCKVISION_SANITIZE=address,undefined
```

## Definition of done (every landing)

Inherited from ckVision, unchanged:

- zero-warning build (`-Wall -Wextra -Wpedantic -Wshadow -Werror`);
- all CTest suites green, and green again under ASan/UBSan;
- a regression test that fails without the change;
- plans updated in the same commit when behavior changed — the plans are the
  spec, so drift is a bug in one of them;
- commit message: one-line summary, what and why, then verification evidence
  naming the tests and sanitizer configurations actually run. "All tests
  pass" is not evidence.

Tests drive the public path: dispatch real events through `Application`,
click real menus. Calling a handler directly proves the handler runs, not
that a reader can reach it.

## Conventions

C++20, no external dependencies beyond `ckvision::cvision`. `snake_case`
functions and variables, trailing `_` members, `PascalCase` types,
`kCamelCase` constants, namespace `ckm::`. Assertions for contract
violations, typed values for environmental failures; exceptions never cross
callback or protocol boundaries. No wall-clock, environment, or filesystem
access outside injected interfaces — determinism is testable or it is not
real. No threads (v1). Trunk-only: commit to `main`, no feature branches.

Every file opens with `Copyright (c) 2026 C. Klukas. All rights reserved.`
No SPDX identifier until the license question resolves.

## Provenance (binding, no exceptions)

Never open, read, quote, or transcribe the source of tmux, GNU screen,
zellij, dtach/abduco, mtm, Twin, Turbo Vision, or any port or derivative —
not for a constant, not for a terminfo string, not for anything. Derive
behavior only from published standards (ECMA-48, xterm ctlseqs, the kitty
protocol specs, Unicode UAX #11/#29, terminfo(5), POSIX), from ckVision's own
documentation, and from documented black-box observation of terminals. Their
manuals are fine; their source is not.
