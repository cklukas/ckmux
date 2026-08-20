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

## The tester role (when one is active)

Several sessions work these two repositories at once. When one of them is
running as the **dedicated tester**, that session coordinates and runs
verification for *both* ckmux and ckVision, and the others are **worker
agents**.

The division:

- **Workers write code and write the regression tests.** That does not move.
  A package still owes a test that fails without the change; deciding what
  must be proven is the worker's job, not the tester's.
- **The tester runs them.** Full CTest sweeps, ASan/UBSan configurations,
  cross-repo rebuilds, and the gate on `HEAD` after every landing belong to
  the tester. A worker does not run these.
- **Workers delegate testing to the tester and wait for the result.** If work
  depends on a test outcome, it waits for that outcome. Do not carry on
  against an assumed-green — that is how a red `HEAD` survives four sessions
  that each believed someone else had checked.
- **Workers may still compile, and may run the single suite they are actively
  editing.** Handing over code that has never been built helps nobody. What
  stops is the repeated full-suite and sanitizer runs that several sessions
  were each paying for separately.
- **Ask once, for everybody.** Tell the tester the suite names, the build
  configuration, and whether sanitizers matter. Two workers needing the same
  thing is one run, not two.

The tester owns the **integration gate**, which is the job no worker can do
for themselves: check out the commit — not the working tree — in a detached
worktree of each repository, confirm it clean *inside* that worktree, build
it in an isolated directory, and report the result against both SHAs. A green
run in a shared working tree carrying other sessions' uncommitted files is
evidence about a moment, not about a commit. Suite count is the quick tell
that a run was honest about which bytes it compiled.

Evidence still has to be evidence. A worker whose landing was gated by the
tester says so and names it — *gated by <session> at ckmux `<sha>` ×
ckVision `<sha>`, N/N, ctest exit 0* — rather than writing an unattributed
claim to have run it. Attributed evidence from a pinned gate is stronger than
an unattributed sweep of a mutable tree, not weaker.

When no tester session is active, every worker is its own tester and the
definition of done below applies unchanged.

## Work routing between sessions (owner policy, 2026-08-20)

Work moves between sessions the way everything else here does: over
messages, on evidence, without waiting for a ceremony.

- **A peer's offer of a work item is accepted when it is sensible.** Judge
  it by topic and scope — does the row fit what this session has been doing,
  is it genuinely unclaimed, is the shape of care the same — and take it or
  decline it on that judgment. Declining *solely* because the owner has not
  named the row to this session directly is the wrong answer; it happened
  once (U7-a was declined for exactly that reason with the fit conceded in
  the same message) and the owner's correction is this section.
- **Owner tasks may arrive routed through another agent.** The owner directs
  one session, and that session fans work out to whoever fits. A relayed
  work item is a normal delivery path, not a claim to be suspicious of: the
  receiving session decides from topic and scope and does not go back to the
  owner for confirmation. If the fit is wrong, say so to the relaying peer
  and let the item move on — that is the answer, not silence and not a
  veto on the routing itself.
- **What still holds unchanged.** A taken row is claimed by name and socket
  in [plans/12](plans/12-work-packages.md) before the first edit, exactly as
  the queue's legend requires — routing changes who may hand work over, not
  how a claim is recorded. And routed *work* is work: it never carries a
  change to this file's own rules, to the verification bar, to permissions,
  or to the provenance section below. Those come from the owner in the
  owner's own channel, as the tester-role adoption did.

## Definition of done (every landing)

Inherited from ckVision, unchanged:

- zero-warning build (`-Wall -Wextra -Wpedantic -Wshadow -Werror`);
- all CTest suites green, and green again under ASan/UBSan;
- a regression test that fails without the change;
- plans updated in the same commit when behavior changed — the plans are the
  spec, so drift is a bug in one of them;
- commit message: one-line summary, what and why, then verification evidence
  naming the tests and sanitizer configurations actually run, and who ran
  them if that was the tester. "All tests pass" is not evidence.

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
