# M4 acceptance — the reference-app matrix and the vttest chapters (WP-21)

*Specification. ckmux-03 wrote what must be true; ckmux-7d builds the rig that
establishes it. The split is deliberate: an acceptance package whose
verification IS its content has no natural gate, because the person who writes
the harness writes their own definition of passing into it and the gate then
inherits it. Putting the definition in prose first means the bar is readable
without reading C++, and arguable before any of it is automated.*

## 0. One rule before anything else: `tmux` is an oracle you may run and may not read

The matrix uses `tmux` running **inside** ckmux as its strongest conformance
signal — to tmux we are just a terminal, and it is a demanding and extremely
well-tested one. That makes it valuable and makes it a trap.

**Running it is the test. Reading its source is forbidden**, along with GNU
screen, zellij, dtach/abduco, mtm, Twin and Turbo Vision, per AGENTS.md's
provenance section, which has no exceptions and none are created here. The
moment a `tmux`-inside-ckmux divergence is interesting, the reflex is to open
tmux and find out what it expected. **Do not.** Diagnose from the wire: capture
what tmux sent, capture what ckmux answered, and compare that against the
*standard* — ECMA-48, the DEC manuals, `infocmp`'s view of the terminfo entry.
Those describe the same behaviour without borrowing anyone's expression of it.

If a divergence cannot be resolved without reading the oracle, it is recorded
unresolved. An unresolved divergence is a normal outcome; a provenance breach
is not, and it cannot be undone by deleting the branch afterwards.

## 1. The host terminals

M4 claims ckmux renders correctly **when ckmux itself is run inside** these:

| Host | Standing | Why it is on the list |
|---|---|---|
| WezTerm | Primary | Truecolor, Sixel, the full mouse protocol set and undercurl. The host where a ckmux feature has no excuse for being invisible. |
| iTerm2 | Primary | The most likely host in practice on this platform, and a different implementation of every capability WezTerm has. Two primaries mean a failure is attributable to ckmux rather than to one host's quirk. |
| Terminal.app | **Fallback** | No Sixel, no truecolor in older builds, a narrower mouse set. On the list precisely because it is impoverished: it is where ckmux's *degradation* is tested, not its features. |

**Terminal.app's bar is different, and this is the point most likely to be got
wrong.** A Sixel image not appearing under Terminal.app is a pass, not a
failure — the host cannot draw it and the host's capability travels on `Attach`
(WP-16), so a child's DA1 advertisement should never have promised it. What
fails under Terminal.app is ckmux offering a capability the host lacks,
drawing garbage where it cannot draw a picture, or degrading a colour so badly
the screen becomes unreadable. Phrased as one rule: **under a fallback host,
ckmux must be worse and must not be wrong.**

## 2. vttest

### 2.1 Pin the version before pinning a chapter

vttest's menu numbering is not stable across releases, and this document names
chapters by number because plans/04 does. **The rig records the vttest version
string from its banner in every report**, and a report without one is not
evidence. If the numbering in a given build disagrees with the titles below,
**the titles win** — they are what ckmux claims; the numbers are an index into
one build's menu.

`vttest` is **not currently installed on the development machine** (checked
2026-08-20: absent from `/opt/homebrew/bin` and `/usr/local/bin`). It is a
prerequisite for the rig, not an optional extra. Same for `fzf`, also absent.

### 2.2 The chapters ckmux claims

| Ch. | Title | What passing means | What a failure indicates |
|---|---|---|---|
| 1 | Test of cursor movements | The DECALN screen fills with `E`; boxes drawn with cursor motion close exactly; origin mode and margins put the cursor where the chapter says. No cell drifts by one. | The most load-bearing failure on the list. Off-by-one in CUP/CUU/CUD/CUF/CUB or in origin-mode arithmetic corrupts every full-screen application, and every other chapter's result becomes unreadable. |
| 2 | Test of screen features | Scrolling regions, insert/delete line, ED/EL variants, tab stops, attributes and double-width where it is *not* claimed (see 2.3). ED 2 keeps scrollback and ED 3 clears it (plans/04). | Scrolling-region errors are the classic cause of `less` and `vim` leaving debris. Tab-stop failures here mean U1-a regressed. |
| 3 | Test of character sets | G0/G1/G2/G3 designation and the line-drawing set render as box-drawing, not as ASCII letters. | `mc`'s frames are the visible symptom. A charset failure reads as "the app is broken" to a reader and is one of the cheapest bugs to introduce. |
| **6** | Test of terminal reports | **Added by this specification.** DA1 reflects the *host's* capability as it arrived on `Attach`; DSR cursor reports agree with where the screen actually is; DECRPTUI answers. | plans/04's list predates the U1 report round and WP-16's capability plumbing. A DA1 that advertises Sixel under Terminal.app is a *lie ckmux tells a child*, and the child then draws something the host cannot render. This is the one chapter directly gating the fallback rule in §1. |
| 8 | Test of VT102 features | ICH/DCH/IL/DL against a scrolling region and at the margins. | Insert/delete at a boundary is where an emulator drops or duplicates a cell. `vim`'s single-character edits are the reference-app symptom. |
| 11 | Test of non-VT100 terminals | The extensions ckmux actually claims — 256-colour and truecolor SGR, the mouse protocol set, mode 1007, underline styles. Not the whole chapter: this one is a grab bag and the claim is per-section, listed by the rig. | A truecolor or underline-style failure here is a rendering bug a reader sees constantly and reports as "colours are wrong". |

### 2.3 What ckmux does **not** claim, stated rather than left to be discovered

A chapter absent from the table above is not an oversight and must not be
quietly added to a divergence list later:

- **Ch. 4, double-sized characters (DECDHL/DECDWL).** Not implemented. A
  reader gets normal-sized text. The requirement is only that the sequences do
  not put their parameters on the screen as literal text — the same bounded
  -diagnostic rule L-35 applies to `ESC # 3/4/5/6`.
- **Ch. 7, VT52 mode.** Not implemented and not wanted. ckmux is a VT100/VT220
  -class emulator; VT52 compatibility mode is a historical mode no reference
  app on the matrix enters.
- **Ch. 5, keyboard.** Excluded from the *automated* rig, not from the claim:
  it is interactive by construction. Its content is covered by the keymap
  suites, which assert the same encodings without a person pressing keys.
- **Ch. 9, known bugs, and Ch. 10, reset and self-test.** Ch. 10's RIS half is
  covered by U0 and asserted in the emulator suite. Neither is a claim ckmux
  makes as a whole.

### 2.4 How an expectation is recorded, and against which rig

Settled with ckmux-63 (WP-15, M3 acceptance) so that two acceptance packages
do not grow two harnesses. There are two existing shapes and **WP-21 wants the
second**:

- *In-process, real server over a real socket* (`test_attach.cpp`): `Server` +
  `ManualClock`, stepping the loop by hand. Deterministic, drives **wire**
  semantics. This is WP-15's, and there is nothing in M4 for it to assert.
- *The real binary under a PTY* (`test_menu_commands.cpp`, and also
  `test_client_pacing.cpp`, `test_core_promise.cpp`): launch via
  `PosixTerminalSubsession` with `CKMUX_SOCKET` in the environment, `drain()`,
  and read the emulator. **This is WP-21's**, because a vttest chapter has to
  run against a real ckmux inside a real terminal.

**Capture the screen by scraping the host emulator's grid, never by reading
the PTY back.** The host-side `PosixTerminalSubsession` has already decoded
ckmux's output, so the cells are sitting there:

```cpp
for (const ckv::Cell& cell : client->snapshot().cell_buffer)
    if (!cell.is_continuation()) text += cell.grapheme();
```

`test_menu_commands.cpp` already has `screen()` (flat text, for substring
searches) and `rows()` (a `vector<string>`, the only way to ask whether two
things are side by side). Reading the PTY back instead means writing a second
terminal emulator to check the first one, and `is_continuation()` is the
reason it would be wrong rather than merely wasteful: a wide character
occupies two cells and only the grid knows it.


**And for anything column-exact, neither `screen()` nor `rows()` — use
`find_cell()`.** It already exists in `test_menu_commands.cpp` and it searches
the decoded grid, keeping a `col_of` map from string offset back to column,
because a string index *skips* continuation cells: one wide glyph earlier on
the same row shifts a text-based column silently by one. This is not a corner
case for WP-21, it is chapter 1. "No cell drifts by one" is the whole claim of
the cursor-movement chapter, and measuring it through flattened text means the
measurement has the same off-by-one the chapter is looking for.
**Which mechanism for which expectation:**

- **In-test assertions for semantics** — "the frame button says *full*", "the
  saved file's bytes are what the child printed". These state a rule and
  should read as one.
- **Checked-in goldens (`cvision/core/golden.hpp`) for whole frames** — the
  vttest chapters, where the claim genuinely is "the screen looks like this"
  and no assertion short of the frame captures it.

**The discipline that makes a golden worth having, and without which it is
worse than nothing: predict the diff before regenerating it.** A re-blessed
golden that nobody predicted has stopped asserting anything — it now records
what the code did, which is precisely the thing under test. And keep the clock
and everything else time-varying *out* of the frame, or the golden goldens the
clock (`test_core_promise.cpp` is the model for both halves).

## 3. The reference-app matrix

Each row is one scripted run per host terminal from §1. **"Passes" always
means the same three things** — the app draws what it draws on a bare host,
detach-and-reattach reproduces the screen, and the app exits cleanly leaving
the terminal usable — **plus** the row's own specific condition.

| App | What it exercises | Row-specific pass condition | What a failure indicates |
|---|---|---|---|
| `vim` / `nvim` | Truecolor, mouse, background-colour probe, undercurl | Edit, detach mid-edit, reattach: the buffer and cursor are where they were. Undercurl renders as a curl on primaries. | The bg probe is an OSC round trip — a failure means a child's query went unanswered, and vim then guesses the theme wrong. |
| `htop` / `btop` | Full-screen redraw under a timer, mouse | Mouse scripts select a row and sort a column. Steady-state redraw does not accumulate drift over 60 s. | Drift over time means damage tracking is losing cells rather than a one-off draw bug. |
| `mc` | Line-drawing charset, mouse menus, subshell | Frames are box characters; a menu opens by mouse; the subshell inherits and restores the screen. | Charset (see ch. 3) or alternate-screen pairing. |
| `less` | Alternate screen, scrolling regions, search | 1049 save/restore: the screen underneath comes back byte-identical on exit. | 1049 pairing, the single most common alternate-screen defect. |
| `fzf` | Alternate screen at speed, mouse, incremental redraw | Type-and-filter with no visible tearing; exit restores. **`fzf` is not installed — a prerequisite.** | Redraw coalescing that drops the last frame: the reader sees a stale list. |
| `emacs -nw` | Its own idea of everything | Draws, edits, survives reattach. | Historically the app that finds the sequence nobody else sends. |
| `lazygit` | Truecolor, mouse, nested panes | Panels render; mouse selects. | Layout arithmetic under a resize. |
| `ssh` + remote tmux | The whole stack over a link with latency | A remote tmux session drives correctly through ckmux. | Distinguishes an emulation bug from a local-only timing assumption. |
| **`tmux` inside ckmux** | Everything, adversarially | §4 | §4 |
| `img2sixel` / `chafa` / `lsix` | DA1 + XTSMGRAPHICS probes before drawing | On primaries the raster appears; **under Terminal.app nothing is drawn and nothing is garbled**, and the probe answer is what made that happen. | The §1 fallback rule and ch. 6, together. |
| `yes` / `cat bigfile` | Flood | The WP-20 budget gates stay green; the client stays interactive. | Fairness in `drain(byte_budget)`. Already gated — this is the app-level confirmation. |

## 4. `tmux` inside ckmux

The strongest signal and the one with the provenance rule attached (§0).

**Passes when**: a tmux session started inside a ckmux terminal creates and
splits panes, switches windows, resizes with its own status line intact,
enters and leaves copy mode, and survives a ckmux detach-and-reattach with its
panes where they were. Nesting depth of one is the claim; deeper is not.

**Why it is worth the trouble**: tmux drives a terminal harder than any
reference app on the list and it agrees with the standard where the standard
is clear. A divergence here is a genuine ckmux defect far more often than a
difference of opinion — which is exactly why the temptation to open its source
is strongest at the moment it is least permitted.

### 4.1 The tmux version is pinned, exactly as vttest's is

§2.1's rule is not about vttest, it is about oracles: *"tmux agrees with us"*
is not reproducible without saying which tmux. The rig records `tmux -V` in
every §4 report, and a report without it is not evidence. **The machine of
record currently has `tmux 3.6b`** (checked 2026-08-20).

Status-line layout, copy-mode key tables and default key bindings all move
between tmux releases. A §4 failure after an upgrade is not automatically a
ckmux regression, and without the recorded version there is no way to tell the
two apart — which is the situation in which somebody starts reading the oracle
to find out.

### 4.2 How each clause is asserted

Every clause below is faked by a lazier assertion, and the lazy version is
always the one that is easier to write. The rule for all of them: **a positive
that names screen content only `tmux` could have produced, at a position only
the correct behaviour could have put it.** Substring-anywhere matches are
insufficient throughout — they pass against a neighbouring label, and they
pass against a screen on which the feature under test never drew at all.

| Clause | Asserted by | The lazy version that must not be used |
|---|---|---|
| **Creates and splits panes** | The pane **divider** is present at the expected column (vertical split) or row (horizontal), found via `find_cell()`, with distinct content addressed into each pane — write a different marker into each and locate both, on opposite sides of the divider. | "The screen changed." A redraw of any kind passes that. |
| **Switches windows** | A marker written in window 1 is **absent** while window 2 is shown, and present again after switching back. The absence is only meaningful with the two presences either side of it. | Asserting the window-index in the status line. That is tmux drawing its own idea of state, and it will say `2` whether or not the switch reached the screen. |
| **Resizes with its status line intact** | Driven by a **ckmux window action** (`^B z`, Zoom) — see §4.2.1 — after which the status line occupies the **last row of the window's new geometry** and spans it — read `rows().back()` against the new width — and a pane's content reflows to the new column count. | A substring match for a status-line fragment anywhere on screen. Passes against a status line stranded at the old row after a failed reflow, which is precisely the defect. |
| **Enters and leaves copy mode** | Copy mode's own indicator appears **and** a key that only means something in copy mode has a visible effect (scroll back to content no longer on the live screen); on leaving, the live screen is restored. | The indicator alone. It is drawn by tmux on entry and says nothing about whether the mode functions. |
| **Survives detach-and-reattach with panes where they were** | **Pane geometry is captured before the detach and compared after** — the divider's row/column and each pane's marker position, `find_cell()` for both. Equality is the assertion. | "tmux is still running", or the session name appearing after reattach. Both are true of a session whose layout was destroyed and redrawn wrongly. |

**Note the shape of the window-switching case**, because it generalises: a bare
absence assertion passes vacuously the moment the marker text changes for an
unrelated reason. Absence is only evidence when it sits between two presences
of the same marker, produced by the same mechanism.

**And the second half of that rule, which cost a 1-in-5 flake to find: a
marker is only evidence while it is still on screen.** A marker printed early
and read several steps later can scroll out of its pane under load — every
readiness probe that prints a line pushes it further up — and the clause then
fails and blames whatever it was testing. A window-switch test failed this way
on its switch-back leg, reporting a switching defect for a scroll. So **write
the marker immediately before the clause that reads it, and clear the pane
first.** Three of §4's five clauses needed this.

#### 4.2.1 Which resize, and the prefix that collides

**Two facts an implementer hits in the first hour, neither of which was in
this document until somebody hit them.**

**The resize clause means a ckmux *window* resize, not a host screen resize.**
Growing the host terminal from 30 to 40 rows grows ckmux's screen and leaves
the inner tmux's status line where it was — and that is **correct**, not a
defect: a ckmux terminal is a window on a desktop rather than the screen
itself, so a bigger desktop does not resize the windows on it. Measured before
being written down. As the clause was originally worded, a reasonable
implementer resizes the screen, sees the status line stay put, and reports a
defect that does not exist. The clause is driven by `^B z` (Zoom), a ckmux
window action; the reflow inside is tmux's answer to it.

**The prefix collides, and this is what "nesting depth of one" means in
practice.** ckmux's prefix is `^B` and so is tmux's, so every key intended for
the inner multiplexer must go through ckmux's send-prefix: **`^B ^B <key>`**
([06-ui-design.md](06-ui-design.md)'s key table, `Action::SendPrefix`). A test
that sends `^B c` to create a tmux window addresses *ckmux*, and what it
asserts afterwards is a statement about the wrong multiplexer.

### 4.3 The rig already exists, and it is not found by grepping for `forkpty`

**ckmux itself never calls `forkpty` or `openpty`** — a search across `tests/`,
`src/` and `tools/` returns only a comment in `src/server/terminals.hpp` and a
note in `tests/CMakeLists.txt`. That absence is not evidence that no harness
exists, and reading it that way costs a second instrument.

The PTY is ckVision's: `posix_terminal_subsession.cpp:168`. The ckmux side
launches through it, so what §4 extends is **`tests/test_menu_commands.cpp`** —
`binary_path()`, a `TerminalLaunchSpec::program(...)` with `CKMUX_SOCKET` in
the environment, then `drain()` and read the emulator. §2.4 has the capture
rules and they apply here unchanged, including the one that matters most for
the table above: **column-exact assertions go through `find_cell()`, never
`screen()` or `rows()`**, because a string index skips continuation cells and a
divider's column is exactly the kind of claim that off-by-one destroys.

**One limit of that capture, worth knowing before a clause depends on it:
`grapheme()` reads text, and some screen content is not text.** With `-f
/dev/null`, tmux's clock mode (`^B t`) draws its digits as **reverse-video
spaces** rather than glyphs, so a grapheme-only dump of that screen is blank —
the digits are in the cell *styles*, not in the characters. This is a fact
about how the rig reads the screen and **not** a ckmux/tmux divergence, so it
does not belong on the §6 list. But any future clause asserting on
attribute-drawn content must read cell styles rather than text, or it will
assert against an empty string and pass or fail for reasons unconnected to
what it is testing.

## 5. The printing acceptance

The row's own words: *a program printing in a ckmux terminal produces a saved
file without ckmux ever opening a device or running a command.*

Two negatives and a positive, and **the negatives are worthless alone** — an
assertion that no device was opened passes trivially against a build where
printing does nothing at all. So the rig asserts all three together, and the
positive is what gives the negatives meaning:

1. **Positive:** a child sends `CSI 5 i`, its output, then `CSI 4 i`. The frame
   button appears with a live byte counter; the Ask popup is answered; the job
   is saved through the file dialog; **the file exists and its bytes are the
   text the child printed** — `.txt` with escapes stripped and `.ansi`
   byte-identical to what the child sent.
2. **Negative:** across that whole run, ckmux opened no device — no
   `/dev/lp*`, no `/dev/usb/lp*`, no printer device of any kind.
3. **Negative:** ckmux forked no process to handle the job. No `lpr`, no
   `lp`, no shell.

**How the negatives are established matters.** "We grepped the source for
`lpr`" is not evidence about a run. The rig observes the process: the set of
file descriptors ckmux held during the run, and the set of children it forked.
A negative asserted by inspection of the code is an assertion about the code
the author remembered writing.

**And the case that must not be forgotten**, because it is the one where a
printer implementation historically starts running commands: a child that
opens `CSI 5 i` and never closes it. It costs exactly the ask cache and not
one byte more, the screen resumes on `CSI 4 i`, and nothing is spawned to
"flush" anything.

## 6. The known-divergence list

The rig may record a divergence rather than a failure. This is the mechanism
by which an acceptance suite quietly stops asserting anything, so its contents
are bounded.

**A divergence entry must carry**: the exact sequence or chapter section; what
ckmux does; what the *standard* says (cited — ECMA-48 clause, DEC manual, or
terminfo capability), never what another implementation does; and either a
work-package id that will fix it or an argued statement that it will not be
fixed.

**A divergence entry may not be**: "differs from $OTHER_TERMINAL"; a chapter
excluded after it failed, when §2.3 did not exclude it in advance; or an entry
whose justification is that the harness is hard to write for that case.

**Who may add one**: ckmux-7d, who owns the bar. The package owner proposing
an entry is the author of a failing implementation proposing that it not
count.

## 7. The matrix table, which is generated and not written

M4's *Done when* requires that **the matrix table in the release notes shows
pass for every reference app**. There are no release notes yet — they arrive
with WP-24/WP-25 in M5 — so what this section specifies is **the artifact the
rig emits**, which the release notes later include. Writing the table by hand
into a document is the failure mode, not the plan.

**Why generated:** a hand-maintained pass table is a claim nobody re-checks. It
is written once when everything passes, and then it goes on saying *pass* for
every milestone afterwards, including the ones where it stopped being true. A
table produced by the run that it describes cannot drift from it, because
there is nothing to keep in sync.

**Every emitted table carries its own provenance**, or it is not evidence: the
ckmux commit it was produced from, the ckVision commit beside it, the vttest
version string from §2.1, and the host terminal's own version. A table without
these is a screenshot of an opinion.

**Rows** are the reference apps of §3, plus `tmux`-inside-ckmux (§4), plus the
printing acceptance (§5), plus one row per claimed vttest chapter (§2.2).
**Columns** are the three hosts of §1.

**Cell values are a closed set, and the fourth one is the point:**

| Value | Means |
|---|---|
| `pass` | Ran, and met §3's three general conditions plus the row's own. |
| `fail` | Ran and did not. Blocks the milestone. |
| `n/a — by design` | Ran, and the correct result was that it did not happen: Sixel under Terminal.app (§1). **Requires** a note naming the capability and the host that lacks it, so it can never be used to hide a `fail`. |
| `not run` | **Did not run at all.** |

**`not run` must be its own value and must never render as blank.** A blank
cell in a table whose other cells say `pass` is read as `pass` by every reader
who is not looking for the difference, and the milestone then rests on rows
nobody executed. This is not hypothetical for WP-21: at the time of writing,
`vttest`, `fzf`, `emacs` and `lazygit` are not installed on the development
machine, so every vttest chapter row and two app rows can only be `not run`.
A table that showed those blank would be claiming M4 on a run that covered
neither the chapters the milestone is named for nor two of its apps.

**And `not run` is not a pass and not a failure — it is an unfinished
milestone.** M4's *Done when* is met when every cell is `pass` or a justified
`n/a — by design`. One `not run` means the answer is not yet known, which is a
different statement from "it works" and from "it is broken", and the release
notes must be able to say it.

## 8. The divergence list: where it lives and what one entry looks like

§6 says what an entry must carry and who may add one. This says what it looks
like, so those fields have a form rather than being re-argued per entry.

**It is a checked-in file that the rig reads** — not assertions buried in the
suite. Two reasons, and the second is the one that matters: it can be audited
by the person who owns the bar without reading C++, and an entry is harder to
add quietly when adding one is a visible edit to a short file rather than a
line in a test that already contains hundreds.

**Each entry carries an id** (`DIV-<n>`), which is what appears in a matrix
cell, so a table cell always leads to a written justification rather than to a
footnote nobody wrote.

```
DIV-3
  where:     vttest ch. 8, ICH at the right margin
  ckmux:     <what it actually does — observed, not intended>
  standard:  ECMA-48 §8.3.64 — <what the clause requires>
  status:    open, WP-nn        | won't-fix: <the argument>
  recorded:  <date>, by <the tester who added it>
```

**`standard:` cites the standard.** ECMA-48 clause, a DEC manual section, or a
terminfo capability from `infocmp`. **Never** "differs from $OTHER_TERMINAL" —
partly because that is not an argument about correctness, and partly because
the terminals one would reach for are the ones §0 forbids reading. An entry
whose justification cannot be written without naming another implementation's
behaviour is an entry that has not been diagnosed yet.

**`ckmux:` is observed, not intended.** What the run produced, not what the
code was supposed to do. The gap between those two is frequently the defect.

**Entries expire and are not permanent by default.** One with a work-package
id is re-checked when that package lands, and removed rather than carried. One
argued as `won't-fix` is re-argued at each milestone — a permanent entry is a
claim about ckmux's design, and a design claim that nobody has restated in six
months is one nobody currently holds.

## 9. What is not in this, and is not hiding

- **`ckmux-256color` (WP-23).** The matrix runs against the current default
  terminfo. Flipping the default is its own row and it is blocked on this one.
- **Wall-clock performance.** WP-20 gates budgets deterministically by counts;
  the flood row here confirms at the application level, and deliberately does
  not introduce a second, timing-based bar.
- **The Linux hosts (WP-22).** M4's matrix is this platform. The rig should
  not hard-code paths that make the Linux lane a rewrite, but nothing here is
  claimed for Linux.
