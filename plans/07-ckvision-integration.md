# ckmux — ckVision Integration

Plan 07 · Status: draft · 2026-08-16 · [Index](README.md)

What ckVision (sibling repo, `/Volumes/PRO-BLADE/git/ckvision`, library
target `ckvision::cvision`, C++20, zero mandatory dependencies, POSIX
implemented / Windows deferred) actually provides for ckmux — verified
against the source on 2026-08-16 — plus the integration seams and the
ckVision work ckmux drives.

**The relationship is co-development, not consumption.** We have write
access to both repos; ckmux is one of the first real ckVision applications
and deliberately its proving ground. ckVision's own adopter policy already
says it — *capability gaps are fixed in the library, never worked around in
applications* — and ckmux takes it literally: when something is missing or
awkward, **we implement the change directly in ckVision**, under ckVision's
conventions, decision records, and verification gates, then consume it.
The litmus test for where a change belongs: *would a second ckVision
application plausibly want it?* → ckVision. *Does it require ckmux's
server, protocol, or session semantics?* → ckmux. When in doubt, prefer
ckVision — a generalized widget or API there beats a private one here.

## Verified inventory (the load-bearing pieces)

| Piece | Where | What it gives ckmux |
|---|---|---|
| `ckv::term::TerminalEmulator` | `include/cvision/term/terminal_emulator.hpp` | Child-facing xterm-subset VT emulator. **Headless**: no PTY/process/UI needed (fuzzed + ~37 unit tests standalone). Fed via `feed_output()`, read via `snapshot()`; replies (DA/DSR/mouse reports) come back via `take_pending_input()`. Runs per-terminal in our server. |
| `ckv::term::PosixTerminalSubsession` | `include/cvision/term/posix_terminal_subsession.hpp` | PTY owner: `forkpty`+`execve` (explicit argv/cwd/env, no ambient environment), non-blocking master, `TIOCSWINSZ` incl. pixels, `drain(byte_budget)`, `wait_handles()` → PTY fd for our poll loop, SIGHUP→SIGTERM→SIGKILL process-group close with grace policy. Embeds the emulator. Usable without `Application`. |
| `ckv::core::TerminalSubsession` | `include/cvision/core/terminal_subsession.hpp` | The 7-virtual seam (`snapshot/profile/feed_output/resize/send_input/take_pending_input/state`) that `TerminalView` consumes — small enough for ckmux's client-side mirror to implement. |
| `TerminalSnapshot` | same header | Exactly the state a multiplexer must sync: cells, cursor, title, rasters, scrollback, and the mode flags (`mouse_reporting_enabled`, `mouse_encoding` X10/SGR, `bracketed_paste_enabled`, `application_cursor_keys`, `focus_reporting_enabled`, `alternate_buffer`). Our protocol snapshot mirrors it 1:1 ([05-protocol.md](05-protocol.md)). |
| `ckv::widgets::TerminalView` | `include/cvision/widgets/terminal_view.hpp` | The terminal widget: renders snapshots; encodes keys (xterm modifier params, DECCKM-aware, F-keys); translates mouse per the child's chosen encoding; bracketed paste; focus reports; PgUp/PgDn scrollback; Shift-drag selection → `on_selection_copy`; Sixel via `draw_image` with text fallback; exited/failed status lines; configurable `parent_escape` chord + `on_parent_escape` callback. |
| Windowing | `widgets::Window`, `widgets::Desktop` | Frame chrome, mouse *and* keyboard move/resize modes, zoom, vetoable close, min sizes, per-window backing store (moving = composition only); z-order, activation, cycling, `select_by_number`, `tile()`/`cascade()`, popups, `dock_top`/`dock_bottom` chrome, typed layout `Snapshot`/`restore`. |
| Menus, footer, commands | `widgets::MenuBar`, `widgets::StatusLine`, `ui::CommandRegistry` | Declarative menus with mnemonics + live accelerator hints; footer with priority-pruned, clickable, command-bound items and a focus-derived hint provider; command descriptors with contexts and enablement. Standard command ids (`kQuit`, `kZoom`, `kNextWindow`, `kTile`, `kWindowList`, `kHelp`/F1, …) with Desktop default handlers. |
| Help | help-context keys per `View`, `Application::set_help_provider`, `widgets::HelpViewer`, `MemoryHelpProvider` | F1 context routing exists end-to-end; content format is ours to define. `StatusLine` hints resolve through the *same* context keys — one mechanism for both ([06-ui-design.md](06-ui-design.md)). |
| Event loop | `ui::Application` (D-021): `step(deadline)`, `wake()`, `post(fn)`, `wait_handles()`, `next_timer_deadline_nanos()`, injected `Clock`, timers | The documented external-loop pattern our client uses to multiplex the server socket ([02-architecture.md](02-architecture.md)). |
| Rendering & Sixel out | `scene::Compositor` → `RasterSlice` → `term::Presenter` | Cell diffing, color-depth degradation, synchronized output, occlusion-sliced **Sixel re-emission with encoding cache**, automatic text fallback when the host lacks Sixel. Host capability model (`term::Capabilities` + probes) included. |
| Host terminal layer | `term::PosixTerminal`, `HeadlessTerminal` (+`VirtualDisplay`), `RecordingTerminal`/`ReplayTerminal` | Raw-mode/altscreen entry with async-signal-safe restore ledger, SIGWINCH/SIGTSTP/SIGCONT handling, lossless backpressured writes; headless + record/replay backends are the backbone of our client tests ([09-testing.md](09-testing.md)). |
| Primitives & utilities | `Cell` (grapheme+`Style`), `Color` (truecolor+default sentinel), `Image` (RGBA), `ckv::text` (UAX #29 graphemes, widths, Unicode 15.1 pinned), `golden` (hardened frame text codec), `DiagnosticsSink`, `Clock`/`ManualClock`, `FileSystem`, `HistoryRegistry` | Shared width authority between emulator and renderer (kills a classic multiplexer bug class); golden format + diagnostics + clocks power deterministic tests. |
| Reference app | `examples/terminal/` | **A single-process, non-detachable mini-multiplexer already exists upstream**: `/bin/sh -i` in `TerminalView` windows, File→New Terminal, Window→Next/Tile/Cascade, Shift-drag copy, Sixel demo. ckmux M1 starts from this pattern ([10-roadmap.md](10-roadmap.md)). |

Also available when wanted: `CommandPalette`, `NotificationCenter`,
`ToolBar`, file dialogs, `Table`/`ListView`/`TreeView` (session picker
material), themes Dark/Light/Mono with interned roles,
`ApplicationShell` (one-call app scaffold), OSC 52 host-clipboard export via
`ClipboardWriter` (write-only by design; reads never issued).

**What ckVision deliberately does not do** (its embedded-terminal plan
excludes "a host shell, process manager, remote terminal/network protocol,
terminal multiplexer, or unrestricted pass-through"): sockets/IPC,
daemonization, serialization (beyond the golden frame format), config-file
parsing, CLI parsing, detach/reattach. **That scope line is exactly the
ckmux/ckVision boundary** — everything on the excluded list is ckmux's job,
everything terminal/UI-shaped is ckVision's.

## Integration seams (the four pieces ckmux writes against ckVision)

1. **Server terminal** = `PosixTerminalSubsession` (spawn/drain/close/resize)
   — the server links only ckVision's `core`+`term` layers; no UI, no host
   terminal, respecting both projects' layer rules.
2. **`RemoteTerminalSubsession` (client)** — ckmux class implementing
   `core::TerminalSubsession` over protocol mirror state: `snapshot()`
   returns the rehydrated mirror; `send_input(bytes)` → `Input` message;
   `resize()` is driven by layout messages. Result: **`TerminalView` is
   reused verbatim**, including its input encoding, mouse translation,
   selection, scrollback paging, and Sixel drawing.
3. **The client loop** — our `poll()` over `app.wait_handles()` ∪
   `{socket}`, deadline-capped by `next_timer_deadline_nanos()`, then
   `app.step(now)`; `post()`/`wake()` for cross-source nudges
   ([02-architecture.md](02-architecture.md)).
4. **The prefix** — `TerminalView::parent_escape` set to the ckmux prefix;
   `on_parent_escape` starts the chord state machine / which-key popup
   ([06-ui-design.md](06-ui-design.md)).

## ckVision work ckmux drives (tracked here, implemented in ckVision)

These are ckmux work items that happen to live in the ckVision tree — we
do them ourselves, directly, as part of the ckmux milestone that needs
them; a ckmux milestone is not done while its U-items are open
([10-roadmap.md](10-roadmap.md)). Ordered by ckmux milestone need; the
U-numbers are referenced throughout these plans
([04-terminal-emulation.md](04-terminal-emulation.md) has the behavior
detail for the emulator items). The list will grow as M1/M3 UI work
exercises the widget set — new needs get evaluated for generalization into
ckVision first (the litmus test above), and only then, if truly
ckmux-specific, built here.

### Landed (M1)

Found by building the M1 client; each fixed in ckVision with tests, and the
whole ckVision suite re-run green (133/133).

| # | Item | Why ckmux hit it |
|---|---|---|
| L-1 | `ApplicationShellOptions::always_dock_status_line` — the shell created a `StatusLine` only when `status_items` was non-empty, so an application whose footer is composed from live state could not get one at all. | ckmux's footer is regenerated per context; it has no items to hand over at construction. The workaround (a placeholder item) would have left the shell holding contents ckmux immediately overwrites. |
| L-2 | `CommandPresentation::chord` — a surface could already override a command's *label*, but not the key it advertises; menus and the status line always derived the chord from the registry. | ckmux reaches commands by a two-key prefix sequence, which a `KeyChord` cannot express. The footer literally rendered `F10 ^B m menu`, and menus advertised `F6` — a key the focused child program consumes and ckmux never sees. |
| L-3 | Three stale verification points in ckVision's own suite (two tests' title-bar click coordinates, one byte budget) left behind when the window chrome gained its maximize/restore control. | ckmux needs a green upstream baseline to tell its own regressions from inherited ones; window drag and zoom are M1 features. |
| L-4 | `cktest` moved to `include/cvision/testing/cktest.hpp` and installed with the library (new `testing` layer in the include-direction checker: it may include nothing, and nothing may include it). | ckVision's zero-dependency policy means an adopter following its conventions had no test harness available at all. |
| L-5 | The help viewer stated no size for its prose, so automatic placement sized the window from its links list and button row — every topic, in every application, was clipped to nothing. It now asks for a readable prose column, still clamped to the desktop. | ckmux's F1/`^B ?` help opened a window with no readable help in it. |
| L-6 | The help viewer's cross-link list was unlabelled, fixed at four rows however many links a topic had, and drew a scrollbar it could not use — so it read as a text field that mysteriously changed the page when clicked. It now carries a heading (`StandardStrings::help_links`), is exactly as tall as it has links (bounded, then it scrolls), auto-hides the scrollbar, and disappears with its heading on a topic that links nowhere. | Reported from the running ckmux help window: "a strange black box that changes text and affects other content". |
| L-7 | `Row`/`Column` reserved main-axis space and counted size hints for **hidden** children, so hiding one left a gap the reader could not account for. They now lay out only visible children. | L-6 needs to hide the link list and its heading together on a link-less topic without leaving a hole above the buttons. |
| L-8 | `ListView::set_scrollbar_auto_hide()` — a list had no way to drop a scrollbar that cannot scroll (the `Scrollbar` supported it; nothing exposed it). Default unchanged. | The help viewer's link list is short by nature; a dead control was the only thing implying hidden content. |
| L-9 | Dark and Light drew the **active** window caption on an accent background (`kDarkAccent`, `kLightAccent`) while its frame used a different one, so the title read as a selected item pasted onto the border. Both now take the frame's own background and stand apart by weight; a test pins caption-bg == frame-bg for every hued scheme. Mono and High Contrast are excluded — with no colour to spend, inverting the caption is their only means. | Visible in the running client: a blue block around "Terminal 1" in a grey frame. |
| L-10 | The emulator recognised only **OSC 0** for the title, ignoring **OSC 2** — the sequence shells and editors actually use — so a host's caption froze at whatever it started as. Now both set it, OSC 1 (icon only) deliberately does not, and the stored title is run through `sanitize_display_text` because it is child-supplied text bound for a window frame. | ckmux window captions follow the running program (see [06-ui-design.md](06-ui-design.md)). |
| L-11 | A selected list row was marked by **underline alone**, with the same colours as an unselected one, in every scheme — it read as a text field or a hyperlink rather than a selection. Selection is now a highlight bar (a background across the row) everywhere, and a new `list_selected_inactive` role shows the selection muted while the list does not hold the keyboard, so a reader with two lists on screen can tell which one their arrow keys will move. `ListView` became focus-aware to use it. | Reported from the help window: "the lines get underlined, strangely looking and unclear meaning". Applies to the file dialog and window list too. |
| L-12 | The help viewer was rewritten as a **two-pane browser**: every topic listed permanently on the left with the current one highlighted, its prose on the right, and a search box above the index that filters it (through `HelpProvider::search`, which existed and was never used — as was `index()`). Cross-references are now a "See also" line in the prose. | The old viewer showed only the current topic's outgoing links, so clicking one replaced both the page and the list of ways off it — nothing stayed still long enough to read as navigation. |
| L-13 | `ListView::set_cursor()` — `set_selected()` marks a row chosen but leaves the cursor behind, so a list restored to a known row painted **two** highlighted rows and the next arrow key moved from the wrong place. Nothing public could move the cursor. | Opening help on a topic put the highlight in one place and the cursor in another; the first Down key jumped to the second topic instead of the one after the current. |
| L-14 | **`SearchBox` could not be typed into at all.** It appended text only from `TextEvent`, but terminals report typed characters as `Key::Char` key events — `TextEvent` is for IMEs and paste. `InputLine` already normalised this and documented why; the search box never did. | Wiring the help search: characters reached the widget and vanished. |
| L-15 | **`SearchBox` did not look like a field.** It drew `"Search: <query> [x]"` as one run of label-coloured cells, so it read as a caption; and its `[x]` landed wherever the query happened to end, while only the last three columns answered a click on it. It now draws a prompt plus an input-coloured field, shows a caret when focused, and puts the clear control at the right edge — the one place that answers a click — and only when there is something to clear. | Reported as "the search input (where is it???) … a strange `[x]` which can't be clicked and looks out of place". |
| L-16 | `Window::set_content_margins(left, top, right, bottom)` — padding was symmetric only, so a layout ending in a button row got a blank row under its cast shadow that nothing could remove without also stripping the top margin. | The help window had one row too many below its buttons. |
| L-17 | The help viewer is now **resizable and modeless**. It was a fixed-size modal, but it shows a document beside an index of documents — how much of either to have on screen is the reader's call — and it is a reference consulted *while* doing the thing it explains, so scoping input to it meant dismissing the answer to use it. | Both reported directly. |

### Landed (terminal capabilities)

The six gaps ckVision's own
[`term_capabilities.md`](../../ckvision/plans/term_capabilities.md) listed as
"worth closing next" are closed, in ckVision, with tests and reference
measurement. What ckmux gets from each:

| # | Item | Why ckmux wanted it |
|---|---|---|
| L-18 | **`OSC 4/10/11` colour queries**, answered as `rgb:RRRR/GGGG/BBBB` with the terminator the child asked with, under the existing query policy. Setting them stays refused. | vim and nvim probe `OSC 11` before choosing a colour scheme; silence made them guess light on a dark theme, and every syntax colour came out wrong at once. |
| L-19 | **`CSI ? 1007` alternate scroll**, on by default and child-controllable (`TerminalCapabilityProfile::alternate_scroll`); `TerminalView` turns a wheel notch over the alternate screen into three cursor keys in the child's own dialect, and leaves the primary buffer's wheel alone. | `less` and `man` never ask for mouse reporting, so the wheel did nothing over them — a broken wheel, as far as a reader can tell. |
| L-20 | **`SGR 4:0`–`4:5`, `21`, `58`/`59`** — underline shape and the rule's own colour, carried on `Style` and emitted to the outer terminal only where it declares `underline_styles`. | nvim marks a spelling mistake with a curl and a type error with dots; one rule for both loses the distinction the editor was making. |
| L-21 | **Child `OSC 52`**, opt-in per profile, size-capped, strictly decoded, clipboard-sanitised, surfaced as text plus a request count and forwarded by `TerminalView::on_clipboard_write`. Reads stay refused under every policy. | Copying out of a program running inside ckmux. The policy is ckmux's to set per terminal. |
| L-22 | **Indexed colour preserved end to end** — `Color` gained a palette-index kind, `core/palette.hpp` resolves one only where pixels are needed, and an index reaches the outer terminal as an index. | This is exactly what a per-terminal palette re-theming feature needs, and it means a reader's own terminal theme applies to a child's `SGR 31` under ckmux as it does outside it. |
| L-23 | **Kitty keyboard protocol, child side** — the flag stack, per screen, with a requested flag masked to what the session can really deliver before it takes effect. | Progressive enhancement for full-screen programs run inside ckmux; the legacy encoding stays the default and the fallback. |
| L-24 | **Child graphics, end to end.** DA1 carries parameter 4 when the profile declares Sixel, and **XTSMGRAPHICS** answers with the decoder's 256 colour registers and the window as maximum geometry (it had been read as SU, scrolling the child's screen once per probe). Then the model behind it (ckVision D-048): a picture is decoded **at its own size** rather than into a plane the size of the window — 8 ms became 0.03 ms for a dialog logo, and flat across window sizes — it is **erased by writing over its cells**, pictures **accumulate** rather than replace, and they scroll with their text. On the outbound side a picture already on the host is **not re-encoded and re-sent every frame**, which is what made a terminal with a picture in it slower than one without. | Reported from a running ckmux, in three rounds: `cksetup` said `SIXEL graphics no — DA1 (CSI c), parameter 4` and drew its text fallback; then its logo appeared but took about a second; then closing the dialog left the logo on screen over the text that replaced it. |

### Landed (text layout)

Three widgets had grown three different answers to "where does this line
break?", and only one of them wrapped at all. The rule now exists once:

| # | Item | Why ckmux wanted it |
|---|---|---|
| L-24 | **`widgets/text_layout.hpp`** — one wrap rule (`wrap_graphemes`, `wrap_text`) and one scrollbar-geometry resolver (`resolve_scroll_geometry`), shared by `TextView`, `Memo` and `TextEditor`. The resolver settles the two bars against each other by iterating to a fixed point, because each bar's presence changes the viewport the other one measures. | Every panel ckmux shows is one of these three. Three wrap rules meant a help page, a memo and an editor broke the same sentence in three places. |
| L-25 | **`WrapMode {None, Word, Character}`** on all three widgets, replacing `set_word_wrap(bool)` — a boolean could not say *which* of the two wrapping behaviours it meant. Word wrap keeps a word whole and lets an over-long one overflow rather than hiding that a path is wider than the window. | The editor offers all three on a cycle; the help viewer picks Word for prose. ckmux panes get to choose per pane instead of inheriting one hard-coded answer. |
| L-26 | **`ScrollbarPolicy {Auto, Always, Hidden}`** on `Memo` and `TextEditor`, with both axes, half-cell thumbs, and cursor tracking moved into display coordinates so a wrapped row scrolls to the caret's real position. `TextEditor`'s gutter is subtracted from the viewport and never scrolls. | "Auto" means *content does not fit*, not *has focus*. A pane that silently hides content it cannot show is the failure ckmux would inherit everywhere. |

### Landed (measurement)

| # | Item | Why ckmux wanted it |
|---|---|---|
| L-27 | **`tools/vtconform/`**, a differential conformance harness that plays a script into ckVision's emulator and into a reference terminal and compares the two cell by cell. 49 cases; deviations must be named and justified in the script or the gate fails. It found the IRM, DECAWM and wide-character bugs rather than being told about them. | Fidelity is settled by measurement, not by argument. When ckmux and a real terminal disagree about a program's screen, this says which one is wrong. |

### Landed (login shells and the Settings dialog)

| # | Item | Why ckmux hit it |
|---|---|---|
| L-28 | **`TerminalLaunchSpec::argv0`** — the spec fused "what to run" with "what to call it", always passing the executable path as `argv[0]`. A shell is told it is a LOGIN shell by a leading `-` on its own `argv[0]` and by nothing else, so no ckVision host could open one. | ckmux starts login shells, as tmux does. Without this the reader's `~/.zprofile` never runs and their `PATH` is whatever ckmux happened to inherit. |
| L-29 | **`FieldKind::Check`** on `FieldDescriptor`, with `MaterializedDialog::checks` and `DialogResult::checked` parallel to the fields — the Check slice of U1-d. New members go on the END of the aggregate, because 29 call sites brace-initialize it positionally. | `Settings ▸ General…` is a checkbox and a Save button. Hand-building it would have been the "every settings form is hand-built" complaint U1-d is about. |
| L-30 | **`FieldKind::Note`** — static text a form *says* rather than asks, and consecutive notes materialize as one un-spaced paragraph instead of inheriting the form's blank row between answerable fields. | A checkbox can be labelled truthfully and still leave its reader guessing what ticking it costs them. The Settings dialog needs four lines of "here is what this actually does". |
| L-31 | **`CheckGroup` and `RadioGroup` no longer consume `Enter`.** They toggled on it and reported it handled, so a dialog's default button was unreachable from the keyboard while any box had focus — which, in a settings dialog, is from the moment it opens. `Space` ticks a box, as it always did; `Enter` now reaches the form. | Found by driving the Settings dialog: tick the box, press Enter to save, and the box unticked itself instead. |

U1-d is not closed: `Radio`, `Combo` and `Number` remain, and Printer
Settings ([06](06-ui-design.md)) still wants them at M4. What landed is the
part `Settings ▸ General…` needed, built so the rest slots in beside it.

### Landed (U0-c — what a terminal does when it is not drawing)

| # | Item | Why ckmux hit it |
|---|---|---|
| L-33 | **BEL is counted in the snapshot** (`TerminalSnapshot::bell_serial`), not latched. A flag that reading clears makes the first reader the only one to see the bell — with a session picker, a footer badge and a title marker all reading the same snapshot, that is three consumers and one winner; a flag that reading does not clear cannot say a second bell arrived. A count is read as often as anyone likes and answers both. The bell deliberately does not flush the printable run: it writes nothing, and flushing there splits a UTF-8 character that has not finished arriving. | WP-19's bell and activity monitors, and the footer/title badges in [06](06-ui-design.md). |
| L-34 | **RIS (`ESC c`)** — everything a child can reach goes back to the profile's declaration: both screens, the history, the cursor, the scroll region, the charsets, autowrap, insert mode, the mouse/paste/cursor-key modes, alternate scroll, the kitty keyboard stacks and the Sixel colour registers. The window caption and the session's own state are deliberately kept: a program clearing its screen has not renamed the window, and a reset cannot bring a dead child back to life. | `reset(1)` inside a ckmux terminal must actually reset it. Before this, the one thing a reader reaches for when a `cat` of a binary wedges their screen did nothing at all. |
| L-35 | **DECALN (`ESC # 8`)** and a `Hash` parse state — the screen fills with E, the cursor comes home and the margins go back to the whole screen. `ESC # 3/4/5/6` stay unimplemented, but as a bounded diagnostic: what must not happen is the digit landing on the screen as text. | vttest opens most of its chapters with DECALN, so an emulator that ignores it grades a blank screen — and [09-testing.md](09-testing.md) settles fidelity by measurement. |
| L-36 | **Scrollback capacity honoured at both ends.** `max_scrollback_lines = 0` now means the line is never copied anywhere, rather than copied into the history and dropped again on the next statement. And a width change re-lays the stored lines to the new width instead of leaving them read at the old one — the history is flat rows of the grid's width, so without it the whole scrollback sheared one column further out per line. That is re-laying, not reflow: a line that wrapped at 80 stays two lines at 100, because nothing records which breaks the child chose. | `scrollback` in [08-configuration.md](08-configuration.md), including `0`. The shear was found while testing the option and belongs to the same package. |

The item's fourth bullet, the `ESC ( 0` DEC line-drawing charset, had already
landed; the DA1/XTSMGRAPHICS graphics advertisement half is L-24 above. The
implementation of L-33…L-36 reached ckVision inside `d06bca5`, a batch commit
another session made across the shared tree; its evidence is `08c3dea`.

### Landed (U0-d — the caption a program borrowed)

| # | Item | Why ckmux hit it |
|---|---|---|
| L-37 | **XTWINOPS 22/23, the window-title stack.** A program that renames the window saves the old name first and restores it on the way out; without the pair, a shell keeps the name of the editor that exited an hour ago, because the editor said what it was and nothing ever said otherwise. Both are gated on the same `osc_policy` flag as the title, so a profile that keeps no captions keeps no stack of them either. Ps 0 and 2 act, Ps 1 (icon name) does nothing in both directions, popping an empty stack leaves the caption alone rather than blanking it, the depth is bounded (`max_title_stack_depth`), and RIS empties it. | ckmux window captions follow the running program (L-10, [06](06-ui-design.md)). Following it *down* again is the half that was missing. |
| L-38 | **`max_title_bytes`** — a title is cut to 1 KiB, on a character boundary, with a diagnostic. A caption is one line on a frame, but the reason for the bound is ckmux's: the snapshot carries the title by value, and a multiplexer reads one snapshot per terminal per frame. An unbounded title is an unbounded copy at frame rate for as long as the terminal lives. | The M2 diff engine reads snapshots at tick rate × N terminals ([10-roadmap.md](10-roadmap.md), R1). |

The rest of the item — the policy flag itself, and OSC 2 alongside OSC 0 —
was already in place (L-10); ckmux's client sets `StoreMetadata` on every
terminal it opens.

### Landed (U1-a — the columns a program laid out)

| # | Item | Why ckmux hit it |
|---|---|---|
| L-39 | **Tab stops, as a table rather than a division.** `\t` went to the next multiple of eight and nothing could change that: HTS set no stop, TBC cleared none, CHT and CBT were unhandled sequences. Now there is a stop table per screen, with HTS (`ESC H`), TBC (`CSI g`), CHT/CBT (`CSI I` / `CSI Z`), CTC (`CSI W`) and DECST8C (`CSI ? 5 W`), reset by RIS and preserved across a resize for the columns that still exist. A tab past the last stop lands on the last column and stays there rather than one past it — a tab does not wrap a line by itself. | [04-terminal-emulation.md](04-terminal-emulation.md)'s long tail and the vttest chapters WP-21 grades. A program that lays out a table sets its stops once and moves between them; without this every field after the first lands in the wrong column, which reads as scrambled output rather than as a missing feature. |

### Landed (a command nothing was listening to)

| # | Item | Why ckmux hit it |
|---|---|---|
| L-32 | **`Desktop` installs the default handler for `kWindowList`**, joining the seven window commands it already covered. The dialog had existed since M6 (`make_window_list_dialog`, over `Desktop::windows()` in the same cycling order `select_by_number` walks); only the wiring was missing, so every application referencing the command got a dead menu item. Presenting is a no-op while a list is open, and the open flag is shared state rather than a member read through `this`, because destroying a Desktop detaches its dialog and completes the presentation. The header where the standard ids live now also says outright that an unhandled command is still *available* — `is_available()` asks the enablement predicate and the focus context, and neither can know whether anyone is listening. | `Terminal ▸ Window List…` drew live, took its click, and did nothing, for all of M1, while [10-roadmap.md](10-roadmap.md) recorded "window list" as landed. Reported from the running client. ckmux now pins the class of defect with `tests/test_command_reachability.cpp`. |

### Open

| # | Item | For |
|---|---|---|
| U0-a | **Attach-an-existing-subsession** entry point on `Application` (today only `launch_terminal_subsession(spec)` exists — no way to hand it a custom/remote subsession). Without it, `TerminalView` change-notification (`notify_terminal_subsession_changed`) and `wait_handles()` aggregation don't reach mirror subsessions. The report calls this "a ~5-line addition"; it is the single cleanest enabler for ckmux. | M2 |
| U0-b | **Damage/no-copy snapshot access**: dirty spans or a borrowed view, plus snapshot-without-scrollback. Today `snapshot()` deep-copies grid+scrollback+rasters per call — unacceptable at tick rate × N terminals, and flagged as a perf risk upstream too. | M2 |
| U1-b | `StatusLine`/`MenuBar` niceties if gaps surface while building the footer (none known today). | M3–M4 |
| U1-c | **Emulator: Media Copy capture** (WP-PRINT, [04](04-terminal-emulation.md)). `TerminalPrinterPolicy{Deny, Capture}` on the capability profile, **defaulting to `Deny`** so no existing ckVision application changes behavior; printer-controller parse state with a terminator match that survives `feed_output()` chunk boundaries; print-screen / print-line / autoprint; DECPFF + DECPEX; honest `CSI ? 15 n` replies (ready / not-ready / no-printer); `max_printer_spool_bytes`; overflow → free the buffer and **sink** (never resume the screen mid-stream); `take_printer_jobs()` drain with only three scalars added to `TerminalSnapshot`. | M4 |
| U1-d | **Typed dialog fields.** `FieldDescriptor` materializes text inputs only, so no descriptor dialog can express a radio group, a checkbox, a combo or a number field — every settings form in every ckVision application has to be hand-built today. Extend the descriptor to a field-kind variant (Text, Check, Radio, Combo, Number) with typed `DialogResult` values. Needed by Printer Settings ([06](06-ui-design.md)); the largest single piece of WP-PRINT, and squarely library-level. | M4 |
| U2 | XTGETTCAP/DECRQSS/rectangles when an app demands them; `OSC 8` hyperlinks, inner side; `SGR 53` overline. *(Kitty keyboard child side and indexed-colour preservation have landed; see below.)* | later |

Process note: each item goes through ckVision's own decision/verification
discipline (a `DECISIONS.md` record where design changes, regression + unit
+ public-path script + golden + adversarial + portable evidence;
zero-warning, sanitizer-clean) and lands as a normal ckVision commit — the
paired ckmux commit references it. Because we develop both repos,
sequencing is a scheduling question, never a negotiation; the only honest
failure mode is "not done yet", recorded in the
[10-roadmap.md](10-roadmap.md) risk table — never a silent ckmux patch
around the library.

## Consuming ckVision

- **Mechanism**: `find_package(ckvision CONFIG REQUIRED)` +
  `target_link_libraries(ckmux PRIVATE ckvision::cvision)` against an
  installed sibling build; `add_subdirectory` fallback via
  `CKVISION_DIR` for one-tree hacking (ckVision's example/test builds
  auto-quiet as a subproject). Pin by commit in a `ckvision.lock` note until
  ckVision 0.1 tags exist.
- **Version discipline**: ckVision is pre-1.0 with *no* backward-compat
  guarantee (breaking changes land with all callers updated — but only
  callers inside its own tree). ckmux therefore builds against a pinned
  commit and bumps deliberately; every bump runs the full ckmux suite.
  ckVision's current state: substantial implementation through M10 + the
  embedded-terminal stack landed with Mac gates passed; its Mac 0.1 release
  audit is in progress; milestones are acceptance-driven and some remain
  formally open. Practical consequence for ckmux: treat the pinned commit,
  not a version number, as truth.
- **License**: ckVision has no license file yet (open decision, due at its
  0.1; currently "all rights reserved" — same author as ckmux, so this is a
  formality to resolve, tracked in [10-roadmap.md](10-roadmap.md) open
  questions).

## Conventions ckmux inherits

Recorded in [11-conventions.md](11-conventions.md): naming/namespace style,
assert-vs-error discipline, no-exceptions-across-callbacks, determinism
rules (injected clock/fs/env), zero-dependency policy, verification
categories and definition-of-done, trunk-only workflow, copyright headers,
and — adopted verbatim, extended to tmux/screen — the clean-room provenance
rule: never read other multiplexers' or Turbo Vision's source; derive from
public standards (ECMA-48, xterm ctlseqs, kitty spec, Unicode UAX) and
documented observation only.
