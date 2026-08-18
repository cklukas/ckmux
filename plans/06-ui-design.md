# ckmux — UI & Interaction Design

Plan 06 · Status: draft · 2026-08-16 · [Index](README.md)

The client is a full-screen ckVision application. This doc defines the screen
layout, the menu tree, the hotkey philosophy and default keymap, the
context-sensitive help system, and the mouse rules. Widget-level grounding in
ckVision (what exists vs. what we add) is in
[07-ckvision-integration.md](07-ckvision-integration.md).

## Screen layout

```
 Session  Terminal  Window  Settings  Help                       09:41:07
─────────────────────────────────────────────────────────────────────────
   ┌ 1: vim plans/02-architecture.md ─────────[▫][x]┐
   │ # ckmux — Architecture                         │
   │ …                                              │ ┌ 2: make ──[▫][x]┐
   │                                                │ │ cc -O2 -c pty.c │
   └────────────────────────────────────────────────┘ │ link ckmux      │
                                                      └─────────────────┘
─────────────────────────────────────────────────────────────────────────
 ^B menu │ ^B c new term │ ^B d detach │ ^B ? keys      build · 2 terms
```

- **Row 1 — menu bar** (permanent): ckVision MenuBar with drop-down menus.
  Right edge carries the clock — `HH:MM:SS`, or `HH:MM`, or nothing, per
  `[general] clock` ([08-configuration.md](08-configuration.md)) and
  `Settings ▸ General…` — and clicking it drops a
  calendar out of it, opened on today and marking it. The clock is a title on
  the bar rather than decoration beside it, so the keyboard reaches it too:
  open the bar (`^B` then the menu key), walk with `←`/`→` — one step left of
  `Session` is the clock — and `Enter` opens the calendar. `Esc`, or a click
  anywhere else, closes it again.
  The app's own name and version are in `Help ▸ About`, which is where a reader
  looks for them, rather than spending the one permanent row that has room to
  spare on text that never changes.
- **Middle — desktop**: ckVision Desktop holding one floating, movable,
  resizable window per terminal in the attached session. Chrome: close `[x]`,
  zoom `[▫]`, drag-to-move on the title bar, drag-to-resize on the corner.
- **Window captions follow the program.** A terminal window is named
  `Terminal N` until its child claims a title with OSC 0 or OSC 2 — which
  shells, vim, htop and ssh all do — and then shows that instead. When the
  child hands the caption back (an empty title, typically on exit), the
  window returns to its ckmux name rather than keeping stale text. The title
  is sanitized and length-bounded by the emulator before it reaches the
  frame, so a hostile child cannot draw into the chrome
  ([02-architecture.md](02-architecture.md) security posture). A user rename
  (M3) will pin the caption and stop it following the child.
- **Last row — footer** (permanent): left side = context-sensitive hotkey
  hints; right side = status (session name · terminal count, prefix-pending
  indicator, bell/activity flags).

Modal surfaces (menus, dialogs, the session picker, message boxes) float
above the desktop. The footer switches to hints for the active modal surface.

## Hotkey philosophy

**Rule 1 — steal almost nothing.** Programs inside terminals must receive
every key they would receive in a bare terminal. While a terminal window is
focused, ckmux intercepts by default *only*:

| Stolen | Purpose |
|---|---|
| `Ctrl+B` (**the prefix**, configurable) | Entry point for every chord |
| `Shift` + mouse gestures | ckmux-local selection & context menu override |

Everything else — F-keys, Alt/Esc sequences, Ctrl keys, plain mouse when the
app requested mouse reporting — flows through untouched. (To send a literal
`Ctrl+B` to the app: press it twice, tmux-style.)

**Rule 2 — everything is in the menu.** Every command is reachable via menu
bar with the mouse; chords are accelerators shown next to each menu item
(e.g. `Detach   ^B d`). The menu bar is mouse-reachable at all times, so no
key needs to be stolen to guarantee reachability.

**Rule 3 — the UI teaches the keys.** Three mechanisms, all generated from
one keymap registry (below): footer hints, menu accelerator labels, and the
prefix "which-key" popup.

**Rule 4 — ckVision-native contexts keep ckVision keys.** When focus is on a
ckmux surface (session picker, dialogs, menus, copy mode, empty desktop),
classic keys work directly: `F1` help, `F10`/`Alt+mnemonic` menus, `Esc`,
`Tab`. They are only unavailable *inside* a focused terminal, where they
belong to the inner app.

### The prefix & which-key popup

Mechanically, the prefix rides on a hook ckVision's terminal widget already
provides: a configurable `parent_escape` chord whose callback returns
control to the surrounding application. ckmux sets it to the prefix and runs
the chord state machine from there (details in
[07-ckvision-integration.md](07-ckvision-integration.md)).

- Pressing the prefix arms "prefix-pending" (footer shows `^B…`).
- The next key selects a chord; `Esc` cancels; prefix again sends a literal
  prefix byte to the app.
- If no key follows within ~500 ms, a small centered popup lists the
  available continuations (borrowed from editor "which-key" UIs — this makes
  the chord table discoverable without documentation):

```
┌ ^B … ────────────────────────────────┐
│ c  new terminal     d  detach        │
│ n  next window      p  previous      │
│ w  window list      s  sessions      │
│ [  copy mode        ]  paste         │
│ M  move/resize      z  zoom          │
│ x  close terminal   m  menu          │
│ 1-9 focus window    ?  all keys      │
└──────────────────────────────────────┘
```

### Default keymap (context: terminal focused)

| Chord | Action |
|---|---|
| `^B c` | New terminal (default shell) |
| `^B C` | New terminal with command… (dialog) |
| `^B d` | Detach |
| `^B n` / `^B p` | Focus next / previous terminal window |
| `^B 1`…`^B 9` | Focus terminal by index |
| `^B w` | Window list dialog |
| `^B s` | Session picker (switch / manage sessions) |
| `^B ,` | Rename terminal · `^B $` rename session (tmux muscle memory) |
| `^B z` | Zoom (maximize/restore) focused window |
| `^B M` | Enter move/resize mode (arrows move, `Shift`+arrows resize, `Enter`/`Esc` done) |
| `^B t` | Tile windows · `^B T` cascade |
| `^B [` | Copy mode (scrollback) |
| `^B ]` | Paste internal clipboard |
| `^B x` | Close terminal (dialog while the child is alive: quit + optional kill, or move) |
| `^B .` | Move terminal to session… (picker) |
| `^B P` | Print output… (captured printer output for this terminal) |
| `^B m` | Open menu bar |
| `^B ?` | Context help window (all current bindings) |
| `^B q` | Quit ckmux client (= detach; never kills anything) |
| `^B ^B` | Send literal prefix to the app |

All rebindable via config ([08-configuration.md](08-configuration.md)) —
implemented, WP-12 — with two exceptions that are exceptions for a reason.
`^B 1`…`^B 9` carry a value rather than choosing a handler, so there is
nothing for a binding to point at; and a row of this table that ckmux has not
built yet has no action name, because a name that parses and then does
nothing is the defect the reachability rule exists for. The names appear as
their packages do.

### Closing a terminal (the dialog tells the whole truth)

`^B x`, the window's close box, and Terminal ▸ Close Terminal all ask the
same question when the child is still alive — and only then; a dead child's
window just closes, and `confirm-kill = false` skips the asking entirely.
The question is about the PROGRAM, so yes/no was the wrong shape; the dialog
names each outcome:

```
┌ Close terminal ─────────────────────────────────────┐
│ "htop" is still running a program. Closing asks it  │
│ to quit.                                            │
│ [x] Kill it if it has not quit after 5 seconds      │
│   Unticked, a program that declines keeps running — │
│   window and all.                                   │
│   The wait is Settings ▸ "Give programs this long   │
│   to quit".                                         │
│   This is the last terminal: closing it ends        │
│   session "work".          ← only when it is true   │
│                                                     │
│  [ Close terminal ]  [ Move instead… ]  [ Cancel ]  │
└─────────────────────────────────────────────────────┘
```

Enter is the fast path a tmux hand expects — asked, then killed after the
grace — and every claim is backed: the checkbox is the End session dialog's,
sharing `kill-grace-seconds`; the last-terminal note appears exactly when
`kill-empty-session` makes it true; and the window falls when the server says
the terminal ended (`TermClosed`), never before — which is also what lets an
unticked decline keep the window honestly alive. A serverless (M1) client
shows the same dialog without the checkbox or the Move button: its terminal
cannot outlive the window, and the note says what closing does instead.

"Move instead…" dismisses the close question and opens the move picker in
its place — one question on screen at a time. The same picker is `^B .`:

```
┌ Move terminal ──────────────────────────────┐
│ Where "htop" goes on running.               │
│ Its window closes here; the program never   │
│ notices the move.                           │
│ To (•) build — 3 terminals                  │
│    ( ) a new session                        │
│                                             │
│           [ Move ]  [ Cancel ]              │
└─────────────────────────────────────────────┘
```

The rows are every session that is not this one, then "a new session" — a
row that is always true, which is what keeps the Move button from ever being
the greyed kind of lie. Moving the last terminal away ends the emptied
session (`kill-empty-session`); the reader lands on the picker, where the
destination is one Enter away.

### Keymap registry (single source of truth)

This is not a ckmux invention — it is ckVision's `CommandRegistry` used with
discipline: every ckmux action is one `CommandDescriptor` (key, title,
category, chord, handler, enabled-predicate), declared once. Menus bind
`CommandPresentation{id}` items (surface-specific labels, shared handler),
the `StatusLine` footer renders command items live as `"{chord} {title}"`
with per-item priority for narrow widths, `chord_for_command()` prints
accelerators in menus, and contexts map to registry
`push_context()`/`pop_context()`. On top of that shared truth ckmux adds:

1. the which-key popup (renders the chord table for the pending prefix),
2. the `^B ?` / `F1` help pages (a `HelpProvider` that lists the registry,
   grouped by context, user rebinds applied and marked),
3. the docs keybindings appendix (generated by a small tool from the same
   descriptors).

**A command is named by a string; its number is nobody's to choose.**
ckVision's D-013 model (adopted 2026-08-17): a command declares itself under
a namespaced key — ckmux's own are `ckmux.*`, the library's are `ckv.*` — and
the registry assigns the `CommandId` that stands for that key at runtime. The
`base + index` scheme this replaced is not merely discouraged, it no longer
compiles, which ends the class of bug where two parties pick the same number.

Three consequences ckmux lives with. The keymap table stores keys and learns
its ids once at startup (`Keymap::resolve`), because an id means nothing
before a registry has assigned one. Everything durable — a `bind` line in a
config file, and at M3 a CLI naming a command — keys on the string, since an
id is meaningless outside the process that assigned it. And ckmux references
the library's commands by *their* keys rather than declaring synonyms: Next
Terminal IS `ckv.window.next` wearing a ckmux label, because a terminal
window is a window.

That model brings its own way to ship something dead: a table row naming a
key nothing declared resolves to no id, which is both unhandled and
unavailable — indistinguishable, to the dead-entry check, from a command
greyed on purpose. `tests/test_command_reachability.cpp` therefore checks
resolution first and names the offending key.

Help can therefore never drift out of sync with actual bindings, and user
rebinds ([08-configuration.md](08-configuration.md)) show up everywhere
automatically.

## Contexts & the footer

The footer's left side always reflects the **active context**:

| Context | Example footer hints |
|---|---|
| Terminal focused | `^B menu │ ^B c new term │ ^B d detach │ ^B ? keys` |
| Desktop empty (no terminals) | `^B c new terminal │ ^B s sessions │ F10 menu │ F1 help` |
| Prefix pending | `c new │ d detach │ n next │ … │ Esc cancel` (+ popup) |
| Menu open | `←→ menus │ ↑↓ items │ Enter select │ Esc close │ F1 help` |
| Session picker | `Enter attach │ n new │ r rename │ k kill │ Esc back` |
| Copy mode | `↑↓ scroll │ v select │ y copy │ / search │ q exit` |
| Move/resize mode | `arrows move │ shift+arrows resize │ Enter done │ Esc cancel` |
| Dialog | `Tab next field │ Enter ok │ Esc cancel │ F1 help` |

Right side (status area): `session-name · N terminals`, prefix indicator,
per-terminal bell `•` / activity `!` flags, `PRN` while the focused terminal
is in printer-controller mode (below). The clock is not here: it is at the
right end of the menu bar, where the calendar can hang from it.

## Context-sensitive help

- `F1` (in ckmux contexts) and `^B ?` (in terminals) open the ckVision Help
  viewer on the page for the active context: what this surface is and its key
  table, generated live from the keymap registry.
- The viewer is a **two-pane browser**: every help topic listed permanently
  on the left with the current one highlighted, its text on the right, and a
  search box above the index that filters it as you type. Choosing a topic
  changes only the text — the list of topics never moves under the reader,
  which is what makes it navigable rather than surprising.
- It is **resizable and modeless**: help is a reference consulted *while*
  doing the thing it explains, so it never locks the reader out of the
  terminal behind it, and how much of a document to have on screen is their
  decision. The index grows vertically, the prose grows both ways, and the
  buttons stay against the bottom edge.
- Menu `Help → All Keybindings…` shows the full registry grouped by context,
  with the user's rebindings applied and marked.
- Menu `Help → Terminal Report` opens ckVision's terminal report dialog
  (`ckv.app.terminal_report`, L-56): the probed capabilities of the OUTER
  host — Sixel included — plus mouse-dispatch diagnostics, with ckmux's
  decoded-SGR-reports probe supplying the byte-stream count. It is the same
  dialog cksetup shows, because evidence about one terminal must not depend
  on which suite application is asked for it.
- First-run experience: on a brand-new server with zero sessions, the picker
  shows a one-paragraph orientation ("ckmux keeps programs running after you
  leave — create a session to start") — help without a manual.

## The session picker

*As built (2026-08-18), which differs from the sketch below in three decided
ways.* First, startup does not always ask: nothing running creates a session
and attaches, and exactly one session with nobody watching it attaches
outright — the picker appears only when there is a genuine choice (the only
session is in use, or there are several). A reader who ran `ckmux` wants their
terminal back, not a dialog about how to get it. Second, the picker's rows are
a radio group and its actions are Attach and a dismiss that is "Cancel" when
attached and "Without a session" when not — the picker CHOOSES among what
exists, and everything that changes what exists lives on the Session menu:
New Session… (`^B S`, straight to the name prompt), Rename Session…, End
Session… (both gated on being attached). The kill confirmation carries the
one decision only a reader can make there: whether whatever ignores the
asking is killed after `kill-grace-seconds`. Third, the Session menu holds
no New Terminal: a terminal is the Terminal menu's to make, and the same
item under two titles is a reader wondering which one they used — the
Session menu is New Session…, Sessions…, Rename Session…, End Session…,
then Detach and Quit. A row somebody else holds says so inline: "(in use —
attaching takes it over)"; the row this client holds says "(this client)".
Created/last-used columns wait for the wall-clock question they raise.

The original sketch, kept for the columns and CLI rows still to come:

Shown at startup, via `^B s`, and via `Session → Sessions…`. A centered
ckVision dialog:

```
┌ ckmux sessions ────────────────────────────────────────────┐
│  Name      Terminals  Attached   Created        Last used  │
│ ▸build     2          —          Aug 16 09:12   2 min ago  │
│  ops       1          ttys004    Aug 15 18:03   attached   │
│  scratch   0          —          Aug 12 11:40   3 d ago    │
│                                                            │
│   [ Attach ]  [ New… ]  [ Rename… ]  [ Kill… ]  [ Quit ]   │
└────────────────────────────────────────────────────────────┘
```

- `Enter` or the `[ Attach ]` button attaches (double-click as a
  convenience). A session attached elsewhere attaches all the same,
  **immediately — latest client wins** ([03-session-model.md](03-session-model.md)):
  the previous client is informed and dropped to its own picker; no prompt,
  no confirmation, no waiting on the new side. The `Attached` column is
  purely informational. Both ends get a toast (`NotificationCenter`):
  *"Took over 'build' from ttys004"* / *"'build' was taken over by
  ttys011"* — and the displaced client's picker shows the notice
  persistently until dismissed.
- Startup with zero sessions: same dialog, list empty, `[ New… ]` focused,
  orientation paragraph shown.
- `ckmux attach <name>` / `ckmux new` CLI bypasses the picker for scripting.

## Mouse rules

| Gesture (terminal window, content area) | If inner app requested mouse | If not |
|---|---|---|
| Click / drag / release | Forwarded to app (translated coordinates, proper encoding per [04](04-terminal-emulation.md)) | Focus window; click-through suppressed on focus click (config) |
| Wheel | Forwarded as wheel events (`Shift`+wheel stays ckmux's and scrolls scrollback) | Alternate screen: arrow-key fallback (mode 1007 semantics). Primary screen: scroll ckmux scrollback — implemented in `TerminalView` (transient scroll state; any key sent to the child exits to live; the view is pinned while the child prints). A frame-mounted scrollbar on the window's right border shows and steers the same position (ckVision D-051, [07](07-ckvision-integration.md) L-55) |
| `Shift` + click/drag | **Always ckmux**: select text (rectangle with `Alt`) → copy on release to internal clipboard + host clipboard | Same |
| `Shift` + right-click | ckmux context menu (Copy / Paste / window ops) | Right-click: forwarded if app wants it, else context menu |
| Middle-click | Paste internal clipboard (config, default on) | Same |

Window chrome (always ckmux): title-bar drag moves; corner drag resizes;
`[x]` close (confirm if alive); `[▫]` / double-click title zooms; click
raises + focuses. Menu bar and footer: plain click; footer hint segments are
clickable and execute their action.

Pixel-precision: where the host terminal supports SGR-Pixels mouse reporting,
inner apps that request pixel coordinates get correctly translated pixel
positions (offset by the window's content origin in pixels); cell fallback
otherwise.

## Copy mode & clipboard

Implemented, WP-17 (`src/client/copy_mode.*`).

- Enter via `^B [` (also `Terminal ▸ Copy Mode`). `Shift`+drag stays the
  implicit path through `TerminalView`'s own selection. Wheel-up does not
  enter copy mode: it scrolls the view in place ([07](07-ckvision-integration.md)
  L-55) — reading history is lighter than selecting from it, and the
  transient scroll state costs nothing to leave.
- Footer switches to copy-mode hints; the window title bar shows a `COPY`
  badge and the scroll position (`-1024/10000`).
- Keys: arrows/`PgUp`/`PgDn`/`Home`/`End` and `h`/`j`/`k`/`l`, `0`/`$`,
  `g`/`G`, `Ctrl+b`/`Ctrl+f` move; `v` begin selection (`V` line-wise,
  `Ctrl+v` rectangular, the same key again to take it back); `y` / `Enter`
  copy & exit — with nothing marked, `y` takes the cursor's line; `/` and
  `?` search with `n`/`N`; `q`/`Esc` exit. (vi-flavored; a config flag can
  add emacs-style aliases later.)
- **The history is frozen on entry.** A program that keeps printing does not
  move the line a reader is about to select out from under them, and a
  selection made at line 4 000 is still that line when it is yanked.
- Search is **smartcase** — an all-lower-case query matches either case, a
  capital in it means the reader typed one on purpose — and **wraps**, since
  a history is a loop to somebody searching it.
- These keys are **not rebindable in v1**: they are modal motions, not
  commands, so a config binding would need an action name per motion. The
  `copy-mode` context in `[keys]` refuses bindings out loud until that
  changes ([08-configuration.md](08-configuration.md)).
- Copy targets: internal clipboard (for `^B ]` paste into any terminal) and
  the host clipboard via OSC 52 (works over SSH) and/or `pbcopy`-style
  helper — see [08-configuration.md](08-configuration.md).
- Paste (`^B ]`, middle-click, menu): wrapped in bracketed-paste markers iff
  the inner app enabled mode 2004; large pastes are chunked with PTY
  backpressure ([04](04-terminal-emulation.md)).

## Captured print output (WP-PRINT)

When a program prints — `CSI 5 i`, print-screen, or autoprint
([04-terminal-emulation.md](04-terminal-emulation.md)) — ckmux captures the
output instead of writing to a device, and the capture is announced on the
window that produced it, not in a log nobody reads.

**The frame button.** A 1-row `Window::add_frame_overlay` child on the
bottom border, left-aligned — the bottom inset (2 cells each side) clears
the resize grips (2 cells) exactly, and frame overlays cast no shadow. It
appears only when there is something to say:

```
   ┌ 2: report.sh ───────────────────────[▫][x]┐
   │ Generating monthly summary…               │
   │                                           │
   └[ PRINT? · 12.4 KB ]───────────────────────┘
```

| State | Label | Click opens |
|---|---|---|
| `ask`, unanswered | `[ PRINT? · 12.4 KB ]`, counter live | the Ask popup |
| answered / `capture` | `[ PRINT · 3 · 1.2 MB ]` | Print Output |
| ask-cache overflowed | `[ Print settings ]` | Printer Settings |
| `capture` spool full | `[ PRINT · 2 · 1 MB — full ]` | Print Output, warning line |

The live byte counter is not decoration: a program in printer-controller
mode freezes the screen by design, and a user looking at a motionless
window needs to see that something *is* happening. For the same reason the
footer carries a `PRN` flag while controller mode is active, and
`Terminal ▸ Reset` (RIS) stays the escape hatch when a child never sends
`CSI 4 i`.

Clicks reach the button without upstream work: delivery is
`topmost_view_at()`, and an overlay is a `Window` child, so it wins over the
frame's own drag and grip handling.

**Byte counts** use one shared formatter everywhere (button, dialogs, job
list): below 1024 → `512 Bytes`; then `KB`/`MB`/`GB` on a 1024 base, one
decimal below 10 and none above — `1.4 KB`, `12.4 KB`, `1.2 MB`, `340 MB`.

**The Ask popup** — not modal; the program is still running:

```
┌ Print output ───────────────────────────────────┐
│ A program in this terminal is sending output to │
│ a printer. 12.4 KB captured so far.             │
│                                                 │
│  ( ) Keep capturing — this terminal             │
│  (•) Keep capturing — this session              │
│  ( ) Keep capturing — always (save as default)  │
│  ( ) Stop capturing (report "no printer")       │
│                                                 │
│  [ View / Save… ]  [ Discard ]  [ Settings… ]   │
│                    [ OK ]  [ Cancel ]           │
└─────────────────────────────────────────────────┘
```

Leaving it unanswered is a stable state, bounded by the ask cache; what
happens at the limit is specified in [04](04-terminal-emulation.md)
(cache freed, emulator sinks, button becomes `Print settings`, one toast so
nothing disappears silently).

**Printer Settings** (`Terminal ▸ Printer Settings…`, from the Ask popup,
from Print Output). Settings are edited in a dialog, not hidden in a config
file — the file is where the dialog *stores* the global scope
([08-configuration.md](08-configuration.md)):

```
┌ Printer Settings ─────────────────────────────────────────┐
│ Scope:  ( ) This terminal   (•) This session   ( ) Global │
│                                                           │
│ When a program prints:                                    │
│   (•) Ask me (capture, show the button)                   │
│   ( ) Always capture                                      │
│   ( ) Off — report "no printer"                           │
│                                                           │
│ Memory limits                                             │
│   Ask cache      [ 256 KB    ]   discarded if exceeded    │
│   Spool limit    [ 1 MB      ]   per terminal, once on    │
│                                                           │
│ Saving                                                    │
│   Default format [ Plain text (.txt)      ▾]              │
│   Default folder [ ~/Documents            ] [Browse…]     │
│   [ ] Ask for a filename every time (else auto-name)      │
│                                                           │
│ Effective here: Ask · 256 KB · 1 MB   (from: session)     │
│                                                           │
│                  [ Save as default ] [ OK ] [ Cancel ]    │
└───────────────────────────────────────────────────────────┘
```

Precedence terminal → session → global → built-in, and the dialog always
shows the effective values *and where they came from*, so a per-session
override is never invisible. It stays reachable from where the question is
asked, and also joins the `Settings` menu below.

**Settings ▸ General…** (M1) is the top-level settings menu this file
anticipated, arriving before Printer Settings did. It holds what is not
specific to one terminal — today a checkbox, a number, and the two pieces of
chrome a reader chooses for themselves:

```
╔ Settings ══════════════════════════════════════════════════════════╗
║ [X] Start terminals the way your desktop terminal does             ║
║                                                                    ║
║   Runs your login startup files (~/.zprofile, ~/.profile), so a    ║
║   terminal here has the same PATH as one you open from the Dock.   ║
║   Unticked, only ~/.zshrc and its kind run. tmux ticks this.       ║
║                                                                    ║
║ Largest picture a program may draw (megapixels): 64                ║
║                                                 ^^^^^^^^^^^^^^^^^^ ║
║   Pictures larger than the window are cut off at its edge, so this ║
║   only stops an absurd one: 64 is far more than a full screen at   ║
║   4K and 2x scaling. Between 1 and 4096; a larger one is refused,  ║
║   with the reason. Both take effect for terminals opened from now  ║
║   on.                                                              ║
║                                                                    ║
║ Clock in the menu bar: With seconds                             ▾  ║
║                                                                    ║
║ Colour theme: Dark                                              ▾  ║
║                                                                    ║
║                                                 Save      Cancel   ║
╚════════════════════════════════════════════════════════════════════╝
```

(The carets stand for the input field's own colour, which this sketch cannot
show; on screen the number sits in a field, not in the prose.)

The checkbox label is deliberately not "login shell". That phrase names the
mechanism, and a reader who does not already know it cannot tell what ticking
the box costs them; the note under it says which files run and when the
change shows up.

The picture limit is here because it was found the hard way: a program's logo
did not appear inside ckmux, and the only sign of why was a footer diagnostic
naming a limit that lived in a header. A limit a reader can hit is a limit a
reader must be able to see and raise. (It bounds the picture, not the window:
a picture too large for the window is cut off at its edge, so nobody meets
this by having a big screen.) It is validated when Save is pressed —
an unreadable number keeps the dialog up with that field focused rather than
storing something nobody chose ([08](08-configuration.md) owns the accepted
range).

The clock and the theme are here rather than in a menu of their own because
they are settings: a `View` menu once held three themes, changed the theme for
the session and left the file saying something else, so a reader who liked
Light chose it again every morning. Both are combo boxes rather than radio
groups — three visible alternatives are a radio group's job, but two of those
cost eight rows, and this dialog has to keep fitting a 24-row terminal, where
the desktop is 22 rows between the menu bar and the footer. It asks for exactly
those 22, which a test pins: a settings form whose buttons are off the bottom
edge cannot be answered at all.

Saving writes `[general] login-shell`, `[terminal] sixel-max-megapixels`,
`[general] clock` and `[general] theme` ([08](08-configuration.md)). The first
two apply to terminals opened afterwards — existing ones keep what they started
with, which the dialog says rather than leaving to be discovered; the clock and
the theme change as the dialog closes, because they are what the reader was
looking at while they chose. A configuration file that cannot be written is
reported once for the lot, not once per setting.

Cancel closes the dialog and stores nothing, exactly as `Esc` does. That it
does anything at all is a ckVision fix (D-050 there): a dialog button used to
carry only its own handler, so a Cancel with nothing of its own to do was an
inert control — pressing it left the dialog exactly where it was.

**Print Output** (`^B P`, `Terminal ▸ Print output…`): a `Table` of jobs
(kind, time, size) beside a preview, with `Save…` / `Save All…` (ckVision's
`FileDialog` in `Save` mode) / `Discard` / `Settings…`. Formats: `.txt`
(controls stripped except `\n`/`\t`, form feed → page break) and `.ansi`
(verbatim bytes). PDF is deferred to a dependency-free base-14 `Courier`
writer post-v1 ([10-roadmap.md](10-roadmap.md)) — deliberately **not** an
external converter, which would re-import the very hazard this design
removes.

## Window management details

- New windows cascade from the last created position; `Tile` arranges all
  non-minimized windows in a near-square grid; `Cascade` restacks.
- Zoom maximizes to the desktop (layout remembered for restore). While any
  window is zoomed, `^B n/p` cycles zoomed (tmux-like flow).
- Focus policy: click-to-focus + raise; `^B n/p/1-9` keyboard focus.
- Min terminal size 10×3; windows clamp to the desktop on desktop resize.
- Exited terminals (`hold` / `hold-on-error` config): dim border, title badge
  `[exit 1]`, footer hint `Enter restart · x close` (restart = respawn same
  command in same window — handy for build loops).

## Visual design

- Themes: follow ckVision's Dark / Light / Mono schemes; ckmux adds semantic
  roles (footer hint key vs. label, status flags, prefix indicator, exited
  badge) on top of the stock palette. Default: Dark. Chosen in
  `Settings ▸ General…`, which stores it in `[general] theme` — there is no
  View menu, because a theme is a setting rather than a command.
- Terminal content rendering is verbatim (truecolor, attributes, Sixel);
  ckmux chrome never restyles inner-app output.
- Bell: visual flash of the window border + footer flag (audible bell to the
  host optional via config).

## ckVision mapping (what this design costs)

Nearly every surface above is a stock ckVision widget; ckmux mostly writes
*content* and the multiplexer glue:

| ckmux surface | ckVision mechanism | ckmux adds |
|---|---|---|
| Menu bar | `MenuBar` + `CommandPresentation` items, mnemonics, accelerator hints | menu tree content; dynamic "Move to Session ▸" submenu |
| Footer | `StatusLine` docked via `Desktop::dock_bottom` (priorities, dimming, clickable, hint provider) | context hint sets; status segments (session · count · flags) |
| Desktop & windows | `Desktop` (z-order, activate, cycling, `select_by_number` 1-9, tile/cascade) + `Window` (drag move/resize, keyboard move/resize modes, zoom, vetoable close, min sizes, frame overlays) | layout ops routed through the server instead of applied locally |
| Terminal windows | `TerminalView` fed by a ckmux mirror subsession | the mirror; prefix hook; copy-mode extensions (search, vi keys) |
| Session picker | dialog + `Table`/`ListView` (type-ahead, sortable) | the session list model over the protocol |
| Window list | stock window-list dialog | — |
| Which-key popup | `Desktop::add_popup` | the popup view (small) |
| Context help | help-context keys per view + `set_help_provider` + `HelpProvider`/Help viewer; F1 = standard `kHelp` command | help topics content; registry-generated key tables |
| Confirmations | message boxes (standard button sets) | — |
| Exit-banner windows | `Window::add_frame_overlay` border readouts | banner content (`[exit 1]`, restart hint) |
| Print button | `Window::add_frame_overlay` (bottom-left, no shadow, clicks routed by `topmost_view_at`) | the four button states, live byte counter, unit formatter |
| Printer Settings / Ask popup | descriptor dialogs + `wire_dialog_window` | the forms — **needs upstream typed dialog fields** (radio/check/combo/number), today's `FieldDescriptor` materializes text inputs only ([07](07-ckvision-integration.md) U1-d) |
| Print Output | `Table` + `TextView` + `FileDialog` (`Save` mode) | job model over the protocol; `.txt`/`.ansi` writers |
| Themes | Dark/Light/Mono schemes + interned roles | ckmux's semantic roles (hint key, badges, prefix indicator) |
| Toasts ("Detached", "Config reloaded") | `NotificationCenter` | message content |

## Open UX questions (tracked in [10-roadmap.md](10-roadmap.md))

1. Default prefix `Ctrl+B` (tmux muscle memory) vs `Ctrl+A` (screen) — B
   avoids shell `beginning-of-line`; recommend **B**.
2. Should plain `F10` also open the menu even when a terminal is focused
   (breaking mc's F10-quit)? Recommend **no** by default; offer as config.
3. Scrollbars on terminal windows — **resolved** (owner decision, 2026-08-18):
   on, mounted on the window's right frame border (ckVision D-051), which
   dissolves the "costs a column" objection — the border cell was always
   there. Shown exactly while the primary screen has history off the top;
   hidden on the alternate screen.
4. Session picker at startup even when exactly one session exists, or
   auto-attach? Recommend **picker always** (predictability), `ckmux attach`
   for speed.
