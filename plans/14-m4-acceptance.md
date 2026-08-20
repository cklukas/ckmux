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

## 7. What is not in this, and is not hiding

- **`ckmux-256color` (WP-23).** The matrix runs against the current default
  terminfo. Flipping the default is its own row and it is blocked on this one.
- **Wall-clock performance.** WP-20 gates budgets deterministically by counts;
  the flood row here confirms at the application level, and deliberately does
  not introduce a second, timing-based bar.
- **The Linux hosts (WP-22).** M4's matrix is this platform. The rig should
  not hard-code paths that make the Linux lane a rewrite, but nothing here is
  claimed for Linux.
