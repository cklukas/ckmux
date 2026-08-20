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
- **The tester commits work it has approved (owner, 2026-08-20).** A worker
  writes the code and its tests and hands over files and intent; the tester
  gates them, and when they pass, *the tester makes the commit*. Authorship
  stays with the worker and the message says whose work it is. A worker does
  not land its own package while a tester is active.

  The reason is not trust, it is arithmetic. On the day this was written, six
  separate incidents happened at the moment of landing and none happened in
  the code: a shared working tree behind HEAD, a shared index carrying another
  session's staging, a prepared tree built against a ref read minutes earlier,
  a commit that swept a peer's staged reversion into itself, a commit whose
  tree equalled its parent's, and a package landed by whole-file add that took
  ~550 foreign lines with it. Every countermeasure — explicit pathspec, the
  cached audit in the same chain, the ref read once, the replay afterwards — is
  a discipline that five sessions must each apply perfectly every time.
  **One writer to the index removes the class instead of mitigating it five
  times over.**
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

## Saying what you checked (2026-08-20)

Sessions cannot see each other's work. What one session says about the tree is
what every other one acts on, so a claim is an artefact with the same standing
as code — and the day this file's shared-tree section was written, the most
common defect in the whole fleet was not a bug. It was **a claim whose scope
exceeded what was checked.**

Every instance had the same shape: one path verified, a property of the system
asserted. "ckmux is not exposed to that default" — the *server* was checked,
while the client's local terminal ran a reader's own login shell under it.
"The marks mean since you last heard" — the field was read, its lifetime was
not, and it never cleared. A green test run — from a piped tail whose exit
status belonged to `tail`. A "pristine" build — pristine on one axis. An audit
that protected a staging step, run minutes before the staging it was meant to
protect.

None of these were reasoning errors and none were caught by care. They were
caught by somebody checking the second path.

The tester's own, recorded because a gate's claims travel furthest and its
instruments are the ones nobody else audits:

- **"Claim audit clean", reported on every gate for a day, while the check
  matched nothing.** It grepped `In progress (` unbolded and anywhere, so it
  hit the legend's cautionary `(ckmux-a5)` example and missed every real row,
  which reads `**In progress** (…)`. Its verdicts were right by luck. Found
  only because the false positive was surprising enough to look at.
- **"Three compile errors at HEAD" when one was real.** The gate's own
  `git checkout` had printed `Aborting` and left the old pin; two of the three
  errors were fiction. A false red from the gate is worse than any session's
  false green, because it is the report everyone treats as authoritative.
  Countermeasure: assert the pin after checkout — the SHA you got, not the one
  you typed.
- **A fleet-wide hunt for "whoever is holding the stale `plans/12`".** Nobody
  was: plumbing commits never write the working tree, so the shared copy drifts
  behind HEAD by the sum of everyone's. The evidence was already in hand — the
  copy held three sessions' superseded states at once, which no single edit
  produces — and was read as a culprit rather than as a mechanism. Four
  sessions checked buffers that do not exist.

- **Say what you checked, not what you concluded.** "`terminals.cpp:98` names
  the policy" is worth more to a peer than "ckmux is fine", because they can
  see its edges. A conclusion travels further than its evidence and arrives
  without it.
- **Before generalising from one site, ask what the second site is.** Server
  and client. Header and source. The wire and the field. If there is a second
  and you did not look, the claim is about the first one only — say so.
- **A claim about somebody else's work needs its own check.** Authorship
  inferred from a commit subject, and a live claim read as abandoned because a
  name did not resolve, were both wrong on the same afternoon and in opposite
  directions. Asking costs a message; being wrong costs their day.
The budget-gate session's own, recorded because three of the four are claims
about *other sessions' work*, which is the kind this file says needs its own
check:

- **Attributed a stale working copy's diff to a named session.** 267 lines
  appeared to leave `test_session_desktop.cpp` beside an untracked WP-43 file,
  so the `FitDesktop` removal was reported as ckmux-53's in-flight work and
  they were asked to check their own tree. Nothing had been removed by anyone.
  A copy behind HEAD renders arrivals it never received as departures, so
  non-delivery reads as deletion — the diff was checked and what the diff was
  *of* was not.
- **Relayed a break report without checking it still reproduced.** A ckVision
  `MouseEvent::position` compile error was passed on as current; it had been
  fixed hours earlier in `bad4077`. A second-hand report inherits the
  confidence of the first-hand one and none of its timestamp.
- **Reported two red suites at HEAD when one was a peer's untracked file** that
  CMake compiled off the shared tree. `git status` had been checked minutes
  before the build, and a working-tree measurement was quoted as evidence about
  a commit.
- **Wrote the expiry rule that authorised reading a live claim as abandoned** —
  "a session no longer active reverts to `Ready`" — thinking of a dead session
  blocking the queue, never of a live one whose name had drifted. It cost
  another session an afternoon's wrong turn within hours of landing.

A fifth, not a claim but the same family: the **membership test** this session
proposed — "working-copy lines absent from HEAD" — was retired because it
cannot distinguish unlanded work from a stale copy, and reported phantom work
with full confidence. An instrument that cannot tell two states apart will
report confidently on both.

- **Record your own instances.** Every session in this fleet has entries in
  this list, which is the only reason the list is usable: a ledger with one
  session's failures in it is a reputation, and nobody writes those down. Say
  the mechanism, not the apology — the mechanism is the part that generalises.

### A number without its configuration is not a measurement (2026-08-20)

`plans/09` recorded `32 ms worst, 24 ms mean` for the flood gate and named no
build type. Five sessions spent the better part of two days hunting a 20×
regression against it. There was none: 32 ms was an optimized-build figure,
every session since has run the Debug build this file documents, and the
known-good commit measures the same ~500 ms as HEAD when both are built the
same way.

The figure could not be refuted because it could not be reproduced, and it
could not be reproduced because the one fact needed to reproduce it was never
written down. It then survived every careful attempt to explain it — each of
which produced a true finding about something irrelevant.

**So: a recorded performance figure states its build type, or it is not a
measurement and must not be used as a baseline.** Where both matter, record
both — this project gates in Debug, so the Debug number is the one with
operational meaning and the optimized one is the aspiration.

The same commit turned up the harder half: `046d738` does not build against
any ckVision commit that existed when it was written, so its figure was taken
against an uncommitted working tree and is not reconstructible by anyone,
ever. **A measurement taken against unlanded dependencies is unfalsifiable by
construction.** Land the dependency first, or record the figure as
provisional.

### An instrument pointed at the wrong subject is perfectly self-consistent

Three profiles in a row reported "the server is idle in `poll`", with clean
call graphs and stable sample counts. All three had attached to the wrong
process: `pgrep -f "ckmux --server"` matched a nine-hour-old orphan from
another session's gate tree, and then two short-lived servers belonging to
other tests in the same suite. The truth was the opposite — the server under
test was pegged at 1.01 s of CPU per second of wall clock for ten straight
seconds.

Nothing in the output looked wrong, because nothing WAS wrong except the
subject. This is the same family as a green run from a piped tail: the
instrument answered a question nobody asked.

**Identify the subject by something unique to this run** — the socket the test
actually created, a path inside your own build directory — never by a pattern
that a peer's leftovers can satisfy. A shared machine running eight sessions
has orphans on it; assume yours is not the only process with that name. And
where a cheap total exists, prefer it to a sample: `ps -o time=` settled in
one command what three call graphs had got backwards.

## Working in a shared tree (several sessions at once, 2026-08-20)

Every session edits one checkout. That is workable, and the failures it
produces are mechanical rather than clever — which means they are avoidable
by rule. All of the following cost somebody an afternoon on the day they were
written down.

**Announce before the first edit, with the file and the region.** Not "I am
taking this row" — which files, and roughly where in them. Adjacent hunks in
one function is the case where a hunk-level split is least reliable, and it is
the one worth knowing about in advance rather than at merge time.

**A half-saved file is somebody else's build break.** Between an edit and its
build, the tree is broken for everyone. Keep edits small and land them fast,
and **verify in a worktree pinned to a COMMIT**, not in the shared tree:

```bash
git worktree add --detach <scratch>/wt <sha>     # both repos
git diff HEAD -- <your files> > p.patch && git -C <scratch>/wt apply p.patch
cmake -S <scratch>/wt -B <scratch>/wt/build -DCKMUX_CKVISION_SOURCE_DIR=<scratch>/ckv
```

Pin a commit and apply your own delta. Copying a file *out of* the shared tree
pulls in whoever else is mid-save, and you then verify their unfinished work
and blame the result on your change.

**Flag another session's breakage; do not fix it.** A one-word fix in
somebody's in-flight file is how two sessions end up editing one file, and it
hides the real problem, which is that the tree was left broken. Send the exact
error and the file. **Tell the tester too** — a gate that trips on an in-flight
save reads as a regression in HEAD and costs a run and a false alarm.

**`git add <file>` stages the whole file, including other sessions' hunks.**
Explicit pathspec is not enough when the collision is *inside* one file.
Reconstruct instead: take HEAD's copy, apply only your own edits to it, and
commit with plumbing (`hash-object` / `update-index` / `commit-tree` /
`update-ref`). Then check what you actually committed — `git show HEAD |
grep -c <the other session's symbol>` should be 0 — and that the tree moved:
a commit whose tree equals its parent's is a failed commit whatever its
message says.

**`git add <file>` then `git commit` commits the INDEX, not your file.** The
shared index is shared: another session may have staged something minutes ago,
and `git commit` takes everything staged regardless of what you just added.
This is not the same hazard as `git add` staging a whole file — it is one
level up, and it bites the careful case, because adding exactly one path feels
like committing exactly one path.

```bash
git diff --cached --name-only     # what is ACTUALLY staged, before every commit
git commit -- <paths>             # commits these paths only, ignoring the index
```

`git commit -- <paths>` is the safe form and should be the default here — with
one caveat, because it commits **the working copy** of those paths and on this
tree a working copy may be stale. Two tools for two situations: **pathspec
commit when your copy is known-current** (you just wrote it, or you have
checked it against HEAD), and **build the blob from HEAD and plumb it when it
is not**, so the commit is provably HEAD-plus-your-change rather than whatever
the copy happened to hold. The
audit is the backstop: run it *in the same command chain* as the commit, not
minutes before, or it is a statement about a moment rather than about what you
are about to land.

Written after `3858004` — the commit that added this section — swept a staged
reversion of `plans/12` into itself, reverting another session's claim on row
29 and deleting the eight-line note they had written under it. The author had
staged one file and believed that was what they were committing. The tree at
the time held eight staged files belonging to two other sessions.

**Plumbing does not write the working tree, and this is the expensive one.**
The moment such a commit lands, the shared copy is behind HEAD by exactly
your change, and every further plumbing commit by any session widens the gap.
So replay your own change back into the live tree afterwards:

```bash
git diff <parent> <yours> -- <path> > p.patch && git apply p.patch
```

Plain `git apply`, **not** `--3way`: 3way refuses a dirty file with "does not
match index", while plain apply matches by context. Skipping this step three
times in one day produced a documentation file 68 lines behind HEAD, a
near-revert of a landed commit by its own author, and a source-tree diff that
read as somebody deleting a shipped feature — which sent five sessions hunting
an author who did not exist.

**When you can name the suspect commit, diff against its PARENT.** Ancestry
below tells you a file is stale; this tells you *which change never arrived*,
which is the question an alarm actually raises. If a commit `C` is in the log
and its change is missing from the tree:

```bash
git diff --name-only C^ C                       # the paths C actually changed
git show C^:<path> | cmp -s - <path>            # identical ⇒ C never reached the tree
```

Byte-identity with `C^` on a path `C` touched means non-delivery, not removal.
Anything else is a real edit and belongs to somebody. **Scope it to the paths
`C` changed** — a file `C` never touched is trivially identical to `C^`, and
reading that as non-delivery is the false positive this check otherwise
avoids.

That is what settled the `FitDesktop` alarm as a non-delivery rather than a
deletion: nine of twelve files byte-identical to `3d6cb2d^`, so the commit had
simply never been written to the working tree. Two sessions had already voted
to revert on the reasoning that it *looked* like an unauthored removal, and
both would have been right by accident. Prefer this check to any heuristic
about what a diff resembles.

**A stale copy does not only delete — it also RESURRECTS, and that is the
uncomfortable half.** The same non-delivery that erases another session's work
will, from a copy predating a deletion, put deleted work back. `a633dfc`
restored an eight-line paragraph that `3858004` had removed, silently and by
accident, while committing something unrelated — and `3858004` is an ancestor
of `a633dfc`, so a commit restored content its own history had deleted. Nobody
intended either direction.

The consequence is what matters: **"it is fine now" can be luck rather than
correctness.** A file that looks right after a scare is not evidence that the
scare was handled — it may be evidence that a second stale copy happened to
land on top. Check what a commit was built FROM, not only what it contains.

**Whether anyone is editing a file is answered by ANCESTRY, not by counting
lines.** "Lines the working copy has that HEAD lacks" looks like the test and
gives a false positive on a stale tree: a copy pinned to an older commit
necessarily holds the pre-change version of every line changed since. The
question is whether the working copy equals some ancestor of HEAD:

```bash
git show <ancestor>:<file> | cmp -s - <file>   # identical ⇒ stale, refresh is lossless
```

If it does, nobody is editing it and `git checkout HEAD -- <file>` loses
nothing. If it does not, somebody has unlanded work: find them before staging.

**Verify authorship before attributing a break, and treat a claim as live
until its holder says otherwise.** Both directions of this were got wrong on
one afternoon: a broken commit was attributed by inferring the author from the
commit message (the subject was one session's package, the bad hunk was
another's), and a live claim was read as abandoned because the name in it
resolved to nobody — while its holder was mid-implementation. Absence from a
session listing is not evidence of anything. Ask, and wait.

**Read the row against the code before starting it.** Three rows in one week
overstated what was left — a dependency described as unlanded that had landed,
a defect that could not occur, a blocker already cleared. A row is a claim
about the code, and the code is next to it.

**This file goes stale like any other, and your copy is the oldest one you
have.** It is a shared file: peers land rules into it mid-session at the
owner's direction, and a session that read it at start-up is quoting a version
that no longer exists. The routing section below landed at 08:15 and was not
applied at 09:10 by a session that instead took a peer's offer back to the
owner for confirmation — precisely the move that section names as the wrong
answer. The rule was present, in the owner's own words, and the reader was the
stale part. So re-read it before doing the thing it governs: before accepting
or declining a hand-off, before a landing, before quoting a rule at a peer.
`git log --oneline -5 -- AGENTS.md` is the cheap check; if anything is listed
that you have not read, you do not currently know what this file says.

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
  spec, so drift is a bug in one of them. **This includes the row**: its
  state, and the claim on it, land with the work rather than after it. Twice
  on 2026-08-20 a session finished something correct and left the record
  wrong — the artefact was right and the row was not — and the two failures
  cost another session 397 lines and a third an afternoon of routing. A wrong
  record is not a smaller defect than wrong code; it is the same defect
  arriving through the only channel other sessions can read. The person who
  can prevent it is always the one who landed the change, which is why it is
  the same commit and not a follow-up;
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
followed by `SPDX-License-Identifier: MIT` — WP-26/D-OPEN-1 was resolved by
the owner on 2026-08-20: **MIT** for both repositories (standard MIT, not
MIT-0; `LICENSE` at the root is the authoritative text), with contribution
terms in `CONTRIBUTING.md` (DCO sign-off, explicit patent grant, relicense
grant to the owner). The copyright line stays verbatim; the SPDX line
beneath it is the license statement, swept by `tools/add_spdx.py`.

## Provenance (binding, no exceptions)

Never open, read, quote, or transcribe the source of tmux, GNU screen,
zellij, dtach/abduco, mtm, Twin, Turbo Vision, or any port or derivative —
not for a constant, not for a terminfo string, not for anything. Derive
behavior only from published standards (ECMA-48, xterm ctlseqs, the kitty
protocol specs, Unicode UAX #11/#29, terminfo(5), POSIX), from ckVision's own
documentation, and from documented black-box observation of terminals. Their
manuals are fine; their source is not.
