# ckmux — ckVision Integration

Plan 07 · Status: current · 2026-08-25 · [Index](README.md)

Released cross-architecture executable inspection (2026-10-05): x64 Windows
GetBinaryTypeW rejects the actual ARM64 system cmd.exe with193, while launching
that identical absolute image succeeds and exits37. The native ARM64 caller
accepts it. Generic read-only executable-header/host-architecture inspection
belongs in ckVision, not an ARM shell exception here. Released ckVision0.1.18
adds inspect_windows_process_image with malformed-file, Unicode-path,
architecture and actual system-image execution tests. ckmux consumes it only
for native image preflight; shell preferences and recursive ckmux launch refusal
remain application policy. Upstream private42e20367/publicfbc83742 passes
Mac/GCC Release230/230, Mac ASanUBSan+fuzz240/240, TSan229/229,
Windows ARM64 and x64-on-ARM64224/224, benchmarks and filtered-public228/228
with strict docs. Exact hosted37320737354 passes all seven jobs; v0.1.18
is published at2026-10-05T14:20:15Z. This application candidate pins18;
its final clean adoption gates remain required, and Windows completion is not
claimed. Historical checkpoints below describe their own earlier snapshots.

Windows integration checkpoint (2026-10-05): application privatef45c5a2e /
public9f821819 passes exact eight-job CI37262790342. Generic optional pinned
ConPTY deployment belongs upstream: ckVision private40e522c5/publicc89d0d7b
passes clean Mac/GCC/native/sanitizer and filtered-public gates, with all seven
hosted CI37265637549 jobs green and released17 at2026-10-05T05:16:31Z.
Application preparation pins17 and calls
only `ckvision_deploy_conpty`; ckmux selects the approved archive pin and owns
ZIP/MSI distribution, not a library loader/launcher workaround. Application
adoption uses17. The SDK's installed-consumer static-CRT fixture correction is
landed at private4975c258/public21f2f685; all seven hosted CI37277400770 jobs
pass. It changes test configuration, not runtime code or APIs, so release17
remains the current library release. The application packaging implementation
passes clean Mac Release/ASan65/65, GCC13.3 Release65/65 and native ARM64 MSVC
Release52/52. ARM64 prototypes pass a real non-admin per-user installer,
registration, byte-layout, repair and uninstall test. That installer-only host
does not establish independent installed-server persistence; x64 packages,
complete native equivalence, live acceptance and release publication remain open.

What ckVision (sibling repo, `/Volumes/PRO-BLADE/git/ckvision`, library
target `ckvision::cvision`, C++20, zero mandatory dependencies, POSIX
implemented / Windows adapters available, ckmux port in progress) actually provides for ckmux — initially
verified on 2026-08-16 and maintained through 2026-08-25 — plus the
integration seams and the ckVision work ckmux drives.

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
| `ckv::core::TerminalSubsession` | `include/cvision/core/terminal_subsession.hpp` | The seam `TerminalView` consumes and ckmux's client-side mirror implements: `snapshot/profile/feed_output/resize/send_input/take_pending_input/state`, plus the U0-b reads a host needs at tick rate — `status()` for the scalars (L-51) and `damage()`/`clear_damage()`/`cells()`/`scrollback()` for what changed and where to read it (L-52). Twelve virtuals, and every one of them is something a mirror has an answer for. |
| `TerminalSnapshot` | same header | Exactly the state a multiplexer must sync: cells, cursor, title, rasters, scrollback, and the mode flags (`mouse_reporting_enabled`, `mouse_encoding` X10/SGR, `bracketed_paste_enabled`, `application_cursor_keys`, `focus_reporting_enabled`, `alternate_buffer`). Our protocol snapshot mirrors it 1:1 ([05-protocol.md](05-protocol.md)). |
| `ckv::widgets::TerminalView` | `include/cvision/widgets/terminal_view.hpp` | The terminal widget: renders snapshots; encodes keys (xterm modifier params, DECCKM-aware, F-keys); translates mouse per the child's chosen encoding; bracketed paste; focus reports; PgUp/PgDn scrollback; Shift-drag selection → `on_selection_copy`; Sixel via `draw_image` with text fallback; exited/failed status lines; configurable `parent_escape` chord + `on_parent_escape` callback. |
| Windowing | `widgets::Window`, `widgets::Desktop` | Frame chrome, mouse *and* keyboard move/resize modes, zoom, vetoable close, min sizes, per-window backing store (moving = composition only); z-order, activation, cycling, `select_by_number`, `tile()`/`cascade()`, popups, `dock_top`/`dock_bottom` chrome, typed layout `Snapshot`/`restore`. |
| Menus, footer, commands | `widgets::MenuBar`, `widgets::StatusLine`, `ui::CommandRegistry` | Declarative menus with mnemonics + live accelerator hints; footer with priority-pruned, clickable, command-bound items and a focus-derived hint provider; command descriptors with contexts and enablement. **A command is identified by a namespaced string key it declares itself under, and the registry assigns its runtime `CommandId`** (D-013) — the standard set is `registry.standard().quit` / `std_command_keys::kQuit` and friends, with Desktop default handlers. ckmux declares under `ckmux.*` and references the library's commands by their own keys. `MenuBar::set_trailing_view()` pins a view to the bar's right end and re-places it on every resize; a trailing view implementing `MenuBarAccessory` (which `widgets::ClockView` does) is walked onto and activated by the keyboard like any menu title. ckmux's clock is that view, and `show_calendar_dropdown()` hangs a `CalendarDropdown` under it — the same wiring as `examples/terminal/`, with the wall-clock reading injected into the client rather than taken inside it. |
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

WP-53 configuration saving uses the injected `FileSystem` read/revision and
conditional atomic-write contract, with native `PosixFileSystem` or upstream
`WindowsFileSystem` selected at the host boundary. The previous application
`getpid`/scratch/`fsync`/rename implementation is removed, not ported. Settings
parsing and line preservation remain application semantics. Deterministic
injected tests cover edits and creations arriving between read and publication,
unreadability, directory failure and preservation of unrelated configuration.
The new Windows adapter is upstream commit `d58d78bf`, not yet a released library;
the public release pin must change only after its gates and release.

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

U1-d is closed: `Radio`, `Combo` and `Number` landed on 2026-08-17 beside the
`Check` and `Note` above (L-41…L-42), so Printer Settings
([06](06-ui-design.md)) can be a descriptor rather than a hand-built form.

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

### Landed (U1-c — the printer that opens no device)

| # | Item | Why ckmux hit it |
|---|---|---|
| L-40 | **Media Copy, behind `TerminalPrinterPolicy{Deny, Capture}`, defaulting to `Deny`.** The printer controller (`CSI 5 i` … `CSI 4 i`) with a terminator matched across `feed_output` boundaries, print screen, print line, autoprint that coalesces its lines into one job, DECPFF and DECPEX, honest `CSI ? 15 n` replies (ready / not-ready while sinking / no printer), `max_printer_spool_bytes`, and `take_printer_jobs()` — with exactly three scalars added to `TerminalSnapshot` (controller active, bytes pending, jobs ready). Overflow frees the buffer and sinks the rest rather than truncating, and never resumes the screen mid-document. | WP-PRINT ([04](04-terminal-emulation.md)). `xterm-256color` advertises `mc0`/`mc4`/`mc5`, so a program that prints inside ckmux today gets silence and its user gets no clue why. This is the half of that plan that had to happen upstream; the dialogs, the frame button and the save path are PRINT-1…6 here. |

The upstream never-list said a child must not be able to write to a printer,
and it was right about the danger it named — but that danger is the **sink**,
not the sequence, which is the argument [04](04-terminal-emulation.md) makes
and the library now implements. Nothing opens a device, runs a command, or
writes a file.

### Landed (U1-d — the rest of the typed fields)

| # | Item | Why ckmux hit it |
|---|---|---|
| L-41 | **`FieldKind::Radio`, `Combo` and `Number`**, with `MaterializedDialog::radios`/`combos` and `DialogResult::selected`/`numbers` parallel to the fields. Radio is captioned by the field's own label rather than labelled beside it — a set of alternatives is one question, and a left-column label would put it level with the first answer. Number stays an InputLine and is checked at accept time, like every other validator, then hands the caller a parsed value rather than text to parse again. Every new member carries an explicit initializer and goes on the end of the aggregate, because most dialogs in the tree brace-initialize the first three positionally. | Printer Settings ([06](06-ui-design.md)) is a radio group, a combo, two numbers and a checkbox. Hand-building it is exactly the "every settings form is hand-built" complaint U1-d exists about — and PRINT-1…6 now has no widget work left in front of it. |
| L-42 | **A closed `ComboBox` no longer swallows `Enter`.** It committed to history and reported the key handled, so a dialog's default button was unreachable from the keyboard for as long as any combo had focus — in a settings dialog that opens on one, from the moment it appears. It now records the value and passes the key on; an open dropdown still takes Enter, because the choice is what Enter means there. Found by building the first descriptor form that contained a combo. | The same defect as L-31, in the third of the three option widgets. ckmux drives its dialogs from the keyboard by design ([06](06-ui-design.md)), so a control that eats the accept key is a dialog nobody can finish. |

### Landed (U0-a — hosting a session the application did not launch)

| # | Item | Why ckmux hit it |
|---|---|---|
| L-43 | **`Application::adopt_terminal_subsession()`** — a caller's own `TerminalSubsession` joins the same vector `step()`'s three loops walk, so it is drained under the same byte budget, folded into the same combined wait, and notified to every view exactly as a launched one is. `launch_terminal_subsession` is now a thin wrapper over it, and the raster identity comes from the same counter — a session left at the default has its pictures silently dropped by the view that would have drawn them. | The whole of `RemoteTerminalSubsession` (WP-5). A mirror is a subsession ckmux builds from protocol state, and without this it could not be hosted at all: ckmux would have had to reproduce `step()`'s child handling, including the second drain after the poll, which is invisible until it is missing. |
| L-44 | **`Application::release_terminal_subsession()`** — the way back out, returning the session to the caller. Adoption without it is a one-way door, and the sessions worth adopting are the ones that come and go. Safe from inside a change notification, which is where a host learns a session ended: the slot empties and the next `step()` compacts, and the drain loop indexes rather than iterates because a notification may also adopt. Verified by perturbation — an immediate erase fails the test under ASan with a container-overflow. | A mirror whose far end closed is not a session any more; without release it would be drained every step and polled every wait for the life of the client. The same was already true of ckmux's *launched* terminals: every closed window left its subsession in `Application` forever. |

The report had called this "a ~5-line addition", and adoption alone nearly is.
Release is the rest, and it is not optional for the use case that motivated
the item. `release` takes the `core::TerminalSubsession` seam because that is
what `on_terminal_subsession_changed` hands over — writing the test is what
showed that the first signature made every caller downcast.

### Landed (U0-b — reading a terminal without copying it)

| # | Item | Why ckmux hit it |
|---|---|---|
| L-45 | **`TerminalDamage`** — a column span per row, plus flags for cursor, modes, title and rasters, plus a count of lines that entered the history. Read with `damage()`, cleared by the host with `clear_damage()` when it has sent everything; reading never clears, because three consumers read one terminal (a diff engine, a title poll, a bell badge) and a read that cleared would give the news to whichever looked first. Damage starts **full** on a fresh terminal, or the first frame after an attach would send nothing. | WP-4b's whole job. Without it the only way to know what a child touched is to keep the previous screen and walk both — a full copy per terminal per frame before any comparison begins. |
| L-46 | **`snapshot(TerminalSnapshotOptions)`** — a caller may decline the scrollback and the rasters, and the declined members are built empty rather than copied and cleared. The no-argument overload is unchanged, so every existing caller gets what it got. | The attach snapshot wants the history; the flush tick does not. Paying for it 30 times a second per terminal is R1. |
| L-47 | **`cells()` and `scrollback()` borrow** instead of copying, so a host reads the damaged rows out of the emulator's own storage and copies only the cells it is about to send. Forwarded through `PosixTerminalSubsession`, which is the seam the server actually holds. | The server links only `core`+`term`; it should not have to reach through to an embedded emulator to avoid a copy. |

| L-48 | **Dropping the oldest history line is moving a number.** Capacity was kept by erasing the front of the history vector, which moves every cell still in it — O(history) per scrolled line, over a million cell moves per line at ckmux's default 10 000 lines × 120 columns. A start offset replaced it, with the dead prefix reclaimed once it is as large as what is kept (one history copy per history's worth of lines). Two invariants came with it, both pinned by tests: the trim drops whole **lines**, not a count of cells (capacity is counted in lines while storage is rows of the grid's width — the same number only until the width changes, and advancing by the cell excess shears every line one column further out), and a width change re-lays the **live** region only (re-laying the dead prefix resurrects lines the terminal had promised to forget, over capacity by exactly what had been dropped). | Found by WP-3's fairness test: a child running `yes` made its own terminal slower the longer it had been running, and the cost was in the history, not the child. A multiplexer's whole premise is a long-lived session. |
| L-49 | **`PosixTerminalSubsession::exit_code()`** forwarded from the emulator. | An exit banner has to say *how* a program ended; "exited 1" is a different window from "exited 0", and without this the seam the server holds could only say "gone". |
| L-54 | **A list that says nothing about its size gets a dialog with no room in it.** `ListView` had no size hints, so it inherited `View`'s — which report a stored preferred size of zero — and every dialog built around a list was laid out as though the list were empty. Window List opened five rows tall with a Close button and not one entry, however many windows were open. `ListView` now reports one row per item (floored at one, capped at ten) and the width of the widest of the first thirty-two, derived from the model and never from live bounds. The stock dialog also stopped drawing its text against the frame and stopped showing a scrollbar that cannot scroll. | Reported from a running ckmux: "I never saw any entries listed there". The existing upstream test asked whether the dialog was *constructed* — and a dialog with no room in it is constructed perfectly well, which is exactly how this shipped. ckmux now pins what a reader sees: [tests/test_command_reachability.cpp](../tests/test_command_reachability.cpp) decodes the frame and counts the entries inside the dialog's own rectangle. |
| L-53 | **A view reads a terminal at frame rate, so it must not copy one.** `TerminalView` asked for a whole `TerminalSnapshot` eleven times — on every repaint and three or four times per keystroke — and a snapshot carries the grid *and* the history. It now reads `status()` for the scalars, `cells()`/`scrollback()` for painting and selection, and the two borrows this item added to the seam, `rasters()` and `diagnostics()`, for the pictures and the complaint a terminal has to show; `snapshot()` survives in exactly one place, for the clipboard text, after the serial says it changed. A counting session pins it: ten repaints, a text event, two key events and a selection must call `snapshot()` **zero** times. | WP-5. On a local terminal this was slow — tens of megabytes a frame at ten thousand lines of history, growing with the session's age. On a ckmux mirror it is worse: the history is stored as lines and every one of those calls would flatten it again. |
| L-51 | **`TerminalStatus` and `status()`** — the half of a terminal that is not cells: cursor, alternate buffer, title, state, every mode flag, the keyboard flags, the clipboard and bell serials, the printer scalars, and the exit code. A host needs all of it on EVERY tick, because damage flags the cursor, the modes and the title independently of any cell; before this the only way to read them was `snapshot()`, so a server that wanted to know the cursor had moved copied the whole grid to find out — the copy U0-b removed, arriving back through the fields around it. `snapshot()` is now built from `status()`, so the cheap read and the whole-terminal read cannot come to disagree, and a test compares them field by field. Payload-carrying members stay snapshot-only (clipboard text, rasters, diagnostics, the print spool) with a serial or a count in the status saying when to go and look. | WP-4b's flush tick. Measured in one run: **56 µs/op** for the scalars against **127 µs** for the leanest snapshot and **1 040 µs** for one carrying the history (that last differs from the 574 µs recorded above because it is a different run and binary — the three are comparable within a run, not across them). The mutation itself is ~50 µs of each, so the scalars cost almost nothing while a lean snapshot doubles the tick. |
| L-52 | **U0-b's reads promoted to the seam a host holds** — `damage()`, `clear_damage()`, `cells()` and `scrollback()` are now on `core::TerminalSubsession` rather than only on the concrete classes. | Half of U0-b was unreachable through the interface: a host holding a `TerminalSubsession&` could only reach `snapshot()`, so the no-copy path existed for callers that knew which class they were holding and for nobody else. ckmux's diff engine now takes the seam and knows nothing about PTYs — which is also what lets its cost be measured against a bare emulator, with no child's scheduling in the number. |
| L-50 | **The hangup is the signal, not the descriptor.** `close()` closed the PTY master first, then signalled. A child blocked *writing* to its terminal cannot act on any signal until that write completes, and the write cannot complete once nothing will ever empty the PTY again — so the protocol wedged exactly the child it existed to end: on macOS the process enters "exiting" and does not reap even under SIGKILL, and `close()` blocks in `waitpid` with no way out. Now SIGHUP/SIGTERM go first, whatever the child has queued is read and discarded while waiting (the reads are for making room, not for the bytes), the post-SIGKILL wait drains and is bounded the same way, and the master is closed last. | Found by WP-3's fairness test: `yes` in one terminal, something quiet in another, and closing the collection hung the entire test binary — one flooding child taking down the server that owns every other terminal on the machine. The two-terminal shape is not incidental: a terminal drained until the moment it is closed usually happens to have room, which is why a single-terminal suite never saw it. |
| L-55 | **Scrollback a reader can see and steer** (ckVision D-051). `TerminalView` grew the whole reading gesture: the wheel walks the primary screen's history (three rows a notch; under `Shift` even over a mouse-tracking child), a view scrolled back is **pinned** while the child prints on, any key sent to the child returns it to the live edge, the alternate buffer forces the offset to zero, and `scroll_state()`/`on_scroll_state_changed` expose where the reader stands as one observable seam. `Window` frame overlays accept `Edge::Left`/`Edge::Right` — a one-cell slot spanning the border between the corner cells — and `attach_terminal_scrollbar(window, view)` binds a `Scrollbar` there both ways: shown exactly while the primary screen has rows off the top, hidden on the alternate buffer, dragging it scrolls the view and the view's own movement moves it. | plans/06's mouse-rules row promised the primary-screen wheel would scroll ckmux scrollback and it never had; the scrollbar answers 06's open question 3, whose objection — "costs a column" — the frame placement dissolves: a bar inside the view would cover the last column or resize the child, and a PTY resize is a reflow the child pays for. On the frame, its coming and going is free. |

Measured on the pinned machine (120×40, 256 lines of history, one status row
rewritten per tick — the same mutation in all three, so the difference is the
read): **574 µs/op** for a full snapshot, **126 µs/op** declining the history,
**57 µs/op** for damage plus borrowed cells, the last including the mutation.
At 30 fps the first saturates a core at about sixty terminals and the other two
do not grow with the history at all. R1 is therefore mitigated in the library,
which is where [10-roadmap.md](10-roadmap.md) said it had to be.

L-48 is measured the same way, in Release, as one scrolled line on a 120×40
grid with a **full** history — 194 µs/line before and **15.3 µs/line** after at
1 000 lines, 2 067 µs/line before and **14.9 µs/line** after at 10 000. The
benchmark keeps both sizes deliberately: flat between them is the property, and
no single absolute number could show it. A terminal now absorbs roughly 68 000
scrolled lines a second — `find /` no longer outruns the emulator reading it,
and the cost stops depending on how long the session has been alive.

### Landed (chrome a reader chooses: a clock, a theme, and a Cancel that closes)

| # | Item | Why ckmux hit it |
|---|---|---|
| L-48 | **`ButtonRole` on `ButtonDescriptor`** — `Accept`, `Dismiss` or `Neutral`, replacing `is_default` (ckVision D-050). The flag could name only the accept button; every other button was "not the default", and the descriptor path honoured that by running the button's own handler and nothing else. A Cancel with nothing of its own to do was therefore an inert control, and a descriptor is written before its window exists, so no application could wire the close itself either. `Dismiss` now runs its handler and takes the window's cancel path — the same closure `Esc` runs. | Reported from a running ckmux: `Settings ▸ General…` could only be left with `Esc`, because its Cancel button did nothing at all. The upstream Forms example shipped the same dead Cancel, which is what made it a library defect rather than a call-site mistake. |
| L-49 | **A trailing view that changes width is placed again.** `ClockView::refresh()` calls `size_hint_changed()` when the rendered text is a different width, and `MenuBar` overrides `on_child_size_hint_changed` to re-place its trailing view. The bar measured its clock when it was inserted and on resize, and nothing else — so a clock given seconds kept its old width and read `09:41:0`, with the last digit off the end of the row. Also covers a twelve-hour clock passing 9 o'clock, and a meridiem word arriving. | `Settings ▸ General…` switches the clock between seconds and no seconds while it is on screen ([06](06-ui-design.md)), so ckmux is the first application to change a trailing view's width after it was placed. |

### Landed (a command nothing was listening to)

| # | Item | Why ckmux hit it |
|---|---|---|
| L-32 | **`Desktop` installs the default handler for `kWindowList`**, joining the seven window commands it already covered. The dialog had existed since M6 (`make_window_list_dialog`, over `Desktop::windows()` in the same cycling order `select_by_number` walks); only the wiring was missing, so every application referencing the command got a dead menu item. Presenting is a no-op while a list is open, and the open flag is shared state rather than a member read through `this`, because destroying a Desktop detaches its dialog and completes the presentation. The header where the standard ids live now also says outright that an unhandled command is still *available* — `is_available()` asks the enablement predicate and the focus context, and neither can know whether anyone is listening. | `Terminal ▸ Window List…` drew live, took its click, and did nothing, for all of M1, while [10-roadmap.md](10-roadmap.md) recorded "window list" as landed. Reported from the running client. ckmux now pins the class of defect with `tests/test_command_reachability.cpp`. |
| L-56 | **The terminal report is the library's dialog** — cksetup's Terminal report moved into `ckv::widgets` as `make_/present_terminal_report_dialog` (`TerminalReportDialogOptions` carries the optional decoded-SGR-reports probe; the line is omitted probe-less), `Application::terminal_capability_report[_text]()` fronting the evidence so the widgets layer never includes `cvision/term`, and the standard command `ckv.app.terminal_report` with `Desktop` installing its default handler beside `kWindowList`. ckmux claims the handler to add the probe its POSIX host can supply. | Help ▸ Terminal Report has to show what the OUTER host really advertised — Sixel included, the evidence WP-16's advertisement decision reads — and it has to be the same dialog cksetup shows, or two suite applications on one terminal would give two accounts of the same host. |

### Landed (every change the emulator makes is one a mirror can hear about)

ckVision `8ddcd53` and the `request_kill` follow-up, driven by the 2026-08-18
architecture review ([13](13-architecture-review.md) R7/R2c):

| # | Item | Why ckmux hit it |
|---|---|---|
| L-57 | **Five named damage flags** — `clipboard`, `bell`, `printer`, `diagnostics`, `lifecycle` — and the Kitty keyboard flags reporting through `damage_.modes`. Three state changes previously set no damage at all (keyboard flags, `clipboard_serial_`, `mark_exited`/`mark_failed`), the bell rode `modes`, and `finish_printer_job` announced a document with `damage_everything()`. | ckmux's server sends only what damage says changed, so an undamaged change was invisible to every attached client: a child that negotiated the Kitty protocol, wrote the clipboard, or exited on its own did so silently (review M-R2, M-R1, m-exit-msg). |
| L-58 | **Mouse tracking is a level** (`core::TerminalMouseTracking`, D-054), not a bool — 1000/1002/1003 tracked apart, the view dropping motion the level does not cover; a status naming no level keeps the old coarse behaviour, so a mirror that carries one bit stays correct. | The reattach audit found the collapse; the compat rule exists so ckmux's wire can adopt the level on its own schedule (it now carries it in `ModesOp` bits 7–8). |
| L-59 | **`TerminalView::send_key()`** — delivers a chord exactly as a real press does (kitty path, legacy fallback, return-to-live) without the parent-escape interception. | ckmux's Send Prefix must synthesize *any* configured prefix into the child; `encode_key` was private, and hand-rolling Ctrl-only bytes in ckmux was the patch-around AGENTS.md forbids. Any app with macros or key replay wants it. |
| L-60 | **`PosixTerminalSubsession::request_kill()`** — process-group SIGKILL that returns at once, the symmetric half of `request_termination()`; `close()` stays the bounded last resort. | The server's session kill escalated through `close()`, which reaps synchronously — up to ~3 s per stubborn terminal inside the loop that serves every other terminal (review M-S3). |
| L-61 | **The forkpty child resets signal dispositions before exec** (SIG_DFL over all, empty mask), with the POSIX asymmetry stated: exec restores *handled* signals, ignored ones stay ignored. | ckmux's server ignores SIGPIPE process-wide, so every shell in every terminal inherited it ignored and ordinary pipelines misbehaved (review M-F4's other half). |
| L-62 | **The no-graphics sixel refusal actually fires** — the specific diagnostic was dead code behind `profile_.sixel && !graphics_available()`, two reads of the same bit. | Found while verifying the review's diagnostics-transport gap: a ckmux terminal with `sixel = false` told its child only the generic "DCS ignored", never the message written for exactly that case. |

### Landed (U4-m — a taskbar whose buttons hold still)

| # | Item | Why ckmux hit it |
|---|---|---|
| U4-m | **Damped entry widths on `WindowSwitcherBar`** — `set_width_damping(grow_delay_nanos, shrink_delay_nanos)` puts a floor under how often one entry's box may change width, measured on the injected `Clock` from that entry's own last change in either direction. Off by default, so nothing changes for a consumer that does not ask. What is damped is the LAYOUT width: the complaint is that the button jitters, and a button's length is the width `PagedStrip` lays it out at, so damping a merely drawn width would leave the boxes shuffling underneath at the rate the damping was meant to stop. Page composition therefore depends on time as well as on the labels — as it already did, since a title change re-pages; damping lengthens the interval over which that function is constant rather than making a pure one history-dependent. Three rules complete it: a window the bar has not measured takes its width at once (a new row has nothing to flicker between); the LABEL is never held back, only the box, and the strip's own elision fits the name into it meanwhile; and `settle_width(window)` forgets one window's memory for the change a READER made, since damping absorbs what a program does to a caption and a rename that visibly worked half a minute later reads as a command that did not work. A deferred change is applied by a wake-up the bar arms on the `Application` timer, because nothing else re-reads the widths — a program that renames its window once and then says nothing more would otherwise leave that name elided into its old box for the session. | A terminal caption follows the program in it ([06](06-ui-design.md)), so a shell rewrites it at every prompt and `make` rewrites it per target. Undamped, each rewrite re-sized that button and slid every button after it, several times a second: the row was unreadable long before the titles were, and a reader aiming at a button hit the one that took its place. [12](12-work-packages.md) WP-36. |
| U4-n | **`PagedStrip::press_in_flight()`** (protected) — whether a press is currently held on an item. The strip resolves a click by INDEX: the press remembers one and the release fires for whatever item now sits at it, guarded only by a range check. So a relayout between press and release either moves the item out from under the pointer, and the click does nothing, or shifts the columns and the release names an item the reader never pointed at. Landed in ckVision as `850711f`, driven by this package and implemented by the strip's author. | Damping makes the relayout timer-driven, which widens that race from "needs an external event" to "can happen with the reader's finger down and nothing else going on". U4-m's deferred change now holds still while a press is in flight and re-asks shortly after. The same race exists for an ordinary title change today, with no damping anywhere near it; making the suppression general inside `PagedStrip` is tracked with its author, because a suppressed refresh needs something to re-run it on release and `entries_` holds `Window*` that a suppressed rebuild would leave stale. **This is the third ckmux consumer to want a fact `PagedStrip`/`Window` already knows and keeps private** — see U4-e, where WP-29 infers "the drag ended" from two agreeing samples 150 ms apart because `Window::gesture_active_` is private. Three is the point at which working around it stops being a workaround and becomes a design position nobody chose. |

### Landed (U4-j — a taskbar that says where a window is, and acts on it)

| # | Item | Why ckmux hit it |
|---|---|---|
| U4-j | **Status icons and taskbar click semantics on `WindowSwitcherBar`** — `Status` (Active / Visible / Minimized) with one public `status_glyph(Status)` table, a `set_minimized_provider` defaulting to `Window::minimized()`, and the three transitions decided in the widget: a click on the ACTIVE row minimizes, on a VISIBLE row activates and raises, on a MINIMIZED row brings the window back in front. The last two are the same call, because `Desktop::activate` restores a hidden window on its way to the front. What a host supplies is the two VERBS — the existing `set_activate_action` and a new `set_minimize_action` — so an application that installs its own activate action to move the keyboard with the window keeps the semantics without re-deriving which of the three a click was. Marks `■` (U+25A0) for a window on the desktop and `_` (U+005F) for one put away — the frame's own close and minimize controls, so a row and its window wear the same chrome — with active-versus-behind carried in colour rather than in a third shape: the active row draws its mark in `ckv.window.control` while every other row lets it fall back, exactly as `Window::draw` lights the controls of the active window alone (ckVision D-063, revising the `▮`/`▯`/`▄` U+25AE/25AF/2584 this first landed with; U+25A0 is East Asian Ambiguous and resolved to one column by D-019, the same resolution the strip's steering triangles and the frame's close control already rest on). Both are ONE cell, so a window changing state cannot re-flow the row U4-m exists to hold still. The icon is part of the item's text and paid for in `natural_width` — the one place an entry's content is measured — so paging and damping follow it for free, and it sits at the front so the lone-item elision takes the tail of the name rather than the state. ckVision D-059. | ckmux's bar is the only route back to a minimized terminal, so a row that did not say which terminals were away, and did not put one away when the reader clicked the one they were in, would be a taskbar in shape only. [12](12-work-packages.md) WP-34. |
| U4-o | **A standard `ckv.window.minimize` command**, declared last in `CommandRegistry` (so no existing id moves) with a `Desktop::minimize_active_window()` default handler beside `kZoom`'s. It refuses a window whose `minimizable()` is false, because that window draws no `_` control and a command that hid it anyway would be a second route past a gate the window set. | U4-i gave every window a `_` control, which is the one control a reader cannot conveniently aim at for the window they are already working in — the menu item and the key are how they reach it, and ckmux needed both (`Window ▸ Minimize`, `^B _`). Every ckVision application with a Window menu wants the same row, so it is the library's command rather than ckmux's. [12](12-work-packages.md) WP-34. |

### Landed (U4-k — what animation is in this toolkit)

| # | Item | Why ckmux hit it |
|---|---|---|
| U4-k | **`ui::Animation`, and the minimize flight as its first consumer.** ckVision had no animation machinery at all — `Application::start_timer` was the whole toolkit — so the package decided what animation IS here before it drew anything: a duration, a progress callback read from the injected `Clock`, and a terminal callback, with **no ownership of the end state**. The caller applies the end state first and the frames decorate a change that has already happened (D-060). That is the reverse of the usual retained-mode design, and it is what makes the package's four non-negotiable constraints fall out of one decision instead of being defended separately — a host that never gets a timer tick is correct because nothing downstream ever depended on a frame, "interruptible" is only "stop drawing", and a duration of zero is a complete disable answered once in the mechanism rather than by a branch at every call site. Three rules make it safe to build on: progress from the clock and never a step per tick (a slow terminal covers more ground per frame rather than running the effect in slow motion); `on_finished` exactly once for every ending, so an effect has one place to tear its decoration down; and destruction silent, because an owner being destroyed is already tearing down. The flight itself is `Desktop::set_minimize_target_provider` — a provider, because a `Desktop` does not know what lists its windows — plus `set_minimize_animation_duration` and `finish_minimize_animation`. **No provider, no flight**, so no existing ckVision application changes. [12](12-work-packages.md) U4-k. | ckmux is the application with a taskbar, so it is the one where "the window went *somewhere*" is a thing a reader can be shown rather than told. `WindowSwitcherBar` already knew where each window's row was (`drawn_entries()`), and `Window::set_minimized` already applied its whole state before notifying — the two halves the effect needed were both there, and what was missing was the middle. ckmux wires the provider to the bar's entry for the window and gets the flight; a ckVision application with no listing installs nothing and sees no change. |

### Landed (U6-a/U6-b; U6-c remains open)

| # | Item | Why ckmux hit it |
|---|---|---|
| U6-a | **Toasts on `NotificationCenter`** — `set_auto_dismiss(nanos)` gives a non-persistent notification a lifetime on the injected Clock, applied by one wake-up armed for the earliest deadline in the stack (the shape `WindowSwitcherBar`'s damping already uses, and for the same reason: an expiry costs a repaint of the whole view, so waking once and sweeping is both cheaper and the only order in which two toasts that came due together leave together). Zero is the default and is no expiry at all, so every consumer written before this is unchanged. `Notification::persistent` now MEANS something — never taken away by time — because the message a reader must not miss is precisely the one they are not at the keyboard for. Plus click-to-dismiss on any line including a persistent one, an `on_changed` callback because expiry happens on a timer the host never sees, and an empty centre that paints nothing rather than filling its rectangle. ckVision `bad4077`. | ckmux needed "Detached", "Config reloaded" and a takeover notice (plans/06). Two of the three are transient and the third must wait for the reader — and the widget could express neither: `persistent` was a field **nothing read**, and nothing expired, so a toast posted was a toast for ever. [12](12-work-packages.md) WP-14. |
| U6-b | **`on_attached()` stops overruling its host** — the focus policy is set in the constructor instead. It was set to `TabStop` on every attach, so a host that had said this centre must NOT be a focus stop had its decision undone when the view was added, and again on any re-attach. Default unchanged. ckVision `6bd14b8`. | ckmux lays toasts over a terminal, where a notification is news rather than something to answer: a focus stop there puts the reader's Tab in a message about something that already happened instead of in the program they are typing into. [12](12-work-packages.md) WP-14. |
| U6-c | **Wanted, not yet built: `DialogDescriptor::help_context`.** A descriptor dialog cannot name a help page, so F1 inside one answers with whatever the surface behind it says. One field on the descriptor, materialized onto the dialog window as `set_help_context_key`. | WP-14 gave every ckmux surface a help page except the ones built from descriptors — the session picker, Settings, the rename and move prompts — because there is nowhere to say which page they mean. [12](12-work-packages.md) WP-14. |

### Landed (a private marker is read before the final byte)

ckVision `45a995e`, 2026-08-23, from a user report against ckmux:

| # | Item | Why ckmux hit it |
|---|---|---|
| L-63 | **`TerminalEmulator::handle_csi` reads ECMA-48's private parameter bytes (`<` `=` `>` `?`) first** and hands a marked sequence to `handle_private_csi`, where every marked form the emulator honours lives and anything else marked is a diagnostic; no public handler is reachable with a marker. XTMODKEYS/XTFMTKEYS (`CSI > Pp ; Pv m` / `f`) are consumed without effect or diagnostic and XTQMODKEYS (`CSI ? Pp m`) is answered truthfully — `0` for modifyOtherKeys, which is not implemented, `2` for the cursor and function keys, which already travel in xterm's level-2 forms (ckVision D-062). Siblings fixed by the same dispatch: XTSAVE/XTRESTORE no longer overwrite the saved cursor or set a scrolling region, XTRMTITLE no longer scrolls, DECXCPR (`CSI ? 6 n`) is answered as `CSI ? r ; c R`. | Exiting `claude` in a ckmux window left "[terminal: malformed child SGR sequence]" at the bottom of the window: Claude Code's teardown writes `ESC[<u ESC[?1049l ESC[>4m`, and the last of those reached the SGR parser. Vim and neovim write the same reset. `tests/test_reference_apps.cpp` plays the exact bytes through a real server, the wire and the client's `TerminalView`, paired with a malformed SGR that still complains. Claude Code also sends `CSI ? 6 n`, whose answer it reads only in the DEC form. The current ckVision v0.1.1 pin contains `45a995e`, so the row is exercised normally and a failed capability probe is a regression; the temporary NOT EXERCISED escape used with the v0.1.0 pin has expired. |

### Landed (an Ubuntu 20.04 consumer was a supported toolchain)

ckVision v0.1.4 (public `5c0e7fb`), 2026-08-31, from the Ubuntu 20.04 package
gate:

| # | Item | Why ckmux hit it |
|---|---|---|
| L-64 | **Floating-point `<charconv>` is feature-detected, with a deterministic classic-locale stream fallback, and `Threads::Threads` is a public exported dependency.** C++20 libraries did not acquire the floating overloads together, so compiler-version inference would be another portability bug; the CMake probe asks for the actual operations. The fallback preserves locale independence and round-trip precision. The installed package config resolves the thread target before importing `ckvision::cvision`. | Ubuntu 20.04's libstdc++ 10 has integer `charconv` but no floating overloads, so ckVision failed to compile before ckmux could package it. Once compiled, the first graphics-log call threw `std::system_error` because focal predates glibc's pthread merge and ckVision's static target had not exported its thread link requirement. Both are general library-consumer requirements, so they were fixed upstream rather than hidden in the ckmux lane. The resulting ckmux Release build passed 53/53 with GCC 10 and its DEB installed in a clean focal container. **Superseded 2026-09-27:** ckVision `90bf31e` withdrew the stream fallback, because its digits differ from `to_chars` and would break byte-identical frames, and ckVision 0.1.7 refuses a library without the floating overloads. ckmux 0.1.6 therefore dropped Ubuntu 20.04 (owner decision): no focal CI lane, no `ubuntu20.04` DEB, and v0.1.5 is the last release for it. |

### Landed (large text frames stop outrunning the host)

ckVision v0.1.5 (public `0e67f04`), 2026-09-01, from the live `htop` host-byte
audit:

| # | Item | Why ckmux hit it |
|---|---|---|
| L-65 | **Large text frames join raster frames behind frame-completion back-pressure.** `Presenter` classifies an emitted text payload of at least 4 KiB as expensive; `Application` retains only the latest dirty scene while a known-answering host owes that frame, and sleeps on the acknowledgement/deadline rather than spinning on retained damage. Small text remains immediate, and a host that never answers is never waited for. | A matched Release 100×30 live audit moved an `htop` window through twenty positions at 10 ms intervals while delaying DSR answers by 120 ms: the pre-fix binary emitted all twenty intermediate frames (182,028 host bytes), while the fixed binary emitted three current frames (25,581 bytes, 86% less), wrote zero unchanged host cells, and reached the same final position. This belongs upstream because every ckVision desktop with large movable text windows needs host-aware coalescing. A deterministic twenty-position drag permits only the first and latest frame when one answer remains outstanding. |

### Landed (a clock a reader can read from across the room)

| # | Item | Why ckmux hit it |
|---|---|---|
| L-66 | **`BigClockView`** (`widgets/big_clock.hpp`): the time, the date, or both (`BigClockContent`) in five-row block glyphs — the seven-segment shapes, drawn as cells of the theme's lit colour — centred in whatever bounds it is given, falling back to the same lines as plain text when the glyphs do not fit, and re-deciding on every draw. It ticks once a second off an injected **`DateTimeValue`** provider (new beside `DateValue`/`TimeValue`: one reading, so a face cannot pair one day's date with the next day's time) and repaints only when what it shows changed. A face stays up when it loses the focus; a key fires `on_dismiss` unless the application binds it to a command (that key is left to the keymap), and so does a completed click that began on an already-focused face — the click that returns to it only focuses it. The face never removes itself. `big_glyph_rows()`/`big_glyph_width()` expose the glyph masks. | The owner asked for tmux's `^B t` (2026-09-23). A big-digit face is a widget any ckVision application with a desktop could want, and ckVision had no block glyphs at all, so it was built there rather than drawn by hand in ckmux. ckmux's `LocalMoment` is now an alias of `DateTimeValue`. |
| L-67 | **`Window::set_content_cover()`**: a view laid over a window's content for a while, kept by the window exactly on `content_rect()` and above the content through every move, resize and margin change, stacked with the window and destroyed with it; `set_content()` re-raises it over new content. | The first big clock was a desktop popup placed at `window->content_rect()` — which is window-local — and drew at the desktop's corner (reported from the running client, 2026-09-23). Copy mode had been placed the same way since WP-17. A popup would also have floated over windows in front of its own and needed a poll to follow a resize; nothing in ckVision let an application put something over one window's content, so it was added there. Both ckmux surfaces now use it, and their tests measure the surface's screen position against the window's interior with the window away from the origin. |

### Landed (a drag past the edge no longer moves every click to the corner)

| # | Item | Why ckmux hit it |
|---|---|---|
| L-68 | **A report beyond the cell grid proves SGR-pixel mode only while the host can be sending pixels** (ckVision D-116, `0991f3c`, released in 0.1.8): `InputDecoder::set_pixel_reports_possible`, which `PosixTerminal` keeps true only while a probe window has mode 1016 enabled, once the mode is proved, and for 250 ms after it resets an unproved mode. | Owner report, 2026-09-27, ckmux 0.1.6 on ckVision 0.1.7 in Terminal.app: after resizing windows, every click landed top-left and opened the menu. Resizing a window by its edge carried the pointer past the host's edge; the host reported that as a cell beyond the grid, ckVision took it as proof of pixel coordinates for good, and divided every later click by the cell size. Reproduced end to end by `test_menu_commands` `a_drag_past_the_hosts_edge_leaves_later_clicks_where_they_are_made`, which fails against ckVision v0.1.7 and passes against 0.1.8. |

### Windows foundation (ckVision 0.1.9 released, 2026-10-03)

| # | Item | Why ckmux hit it |
|---|---|---|
| L-69 | `term::TerminalSubsession` exposes selective snapshots and nonblocking `request_termination()` / `request_kill()`; the Windows adapter sends private ConPTY Control-C and escalates the owned job. POSIX retains process-group behavior. | WP-52 requires portable terminal ownership without concrete downcasts. The old lifecycle calls were POSIX-only. ckmux now owns the shared type and uses the standard launch factory. Native Windows tests exercise a stubborn root and descendant alongside a responsive peer; the compiled no-op escalation mutation fails. Release and sanitizer evidence is recorded with the library release rather than inferred from the shared working tree. |

Scalable combined Windows readiness remains a separate upstream gap: the
outer terminal currently rejects more than `MAXIMUM_WAIT_OBJECTS` sources.
WP-54 must resolve that in ckVision, not reduce ckmux's terminal limit or
poll incomplete sets. This foundation does not claim a runnable Windows
ckmux client/server or close WP-53–WP-57.

L-69 landed in ckVision `84c96695`, published as public `aea4754a` and
[v0.1.9](https://github.com/cklukas/ckVision/releases/tag/v0.1.9). GitHub
CI `37126856749` passed all seven jobs: Windows/macOS/Linux Release, ASan,
UBSan, TSan and fuzz. ckmux pins this release; [15](15-windows.md) records
the candidate and public-library adopter-mode gates and remaining work.

### Upstream status

This table keeps the landed seams beside the genuinely open/deferred ones so
"Open" cannot continue to contain rows whose text itself says they landed.

| # | State | Item | For |
|---|---|---|---|
| U-WIN-PROCESS-IMAGE | Released upstream; current adoption gates pending | Generic read-only PE/COFF and host user-mode architecture inspection is released in ckVision0.1.18/publicfbc83742 after all seven hosted37320737354 jobs pass. ckmux replaces only the image-preflight call; shell preferences, recursive application launch refusal, server arguments/readiness and session policy remain here. No ARM-specific shell exception, loader implementation or server semantics enter the library. | WP-53/WP-56 |
| U1-b | Deferred; no known gap | `StatusLine`/`MenuBar` niceties if a concrete gap surfaces. | M3–M4 |
| U-WIN-CONPTY-DEPLOY | Released upstream as17; consumed by packaging | D-134 provides an opt-in generic CMake helper with an explicit archive/hash/architecture/target, PE checks, complete OS-compatible console hosts, build/install refresh, identity record and Microsoft MIT notice. No automatic network or mandatory runtime dependency. Release17 at publicc89d0d7b passes all seven CI37265637549 jobs and clean Mac/GCC/native/sanitizer gates; subsequent test-only static-CRT correction public21f2f685 passes all seven CI37277400770 jobs. Actual helper-deployed runtime1.24 passes all34 library cases; runtime1.25 still fails pixel-mouse and graphics/resize proof on the named host. ckmux calls only this helper and owns its archive/installer/release policy. Seven distribution-policy cases and their actual-call-omission negative pass; ARM64 prototype installer checks pass, but x64 and full installed/live acceptance remain required. | WP-55/WP-56 |
| U-WIN-DETACHED-PROCESS | Released upstream and consumed here; live acceptance open | D-133 is a generic injected DetachedProcessLauncher with no server/session logic. Windows creates suspended, requests breakaway, verifies absence from ANY job before resume, and retains typed refusal plus separate bounded cleanup. Six native cases and a compiled guard-omission negative prove the boundary. Release v0.1.16 peels to public `a38928cf`, all seven CI37243144550 jobs green. ckmux supplies its executable, --server argument, endpoint and explicit cwd, then handles readiness itself. Controlled native ancestor closure and restrictive refusal pass in the 47/47 application candidate; live Windows Terminal close/reattach remains. Clean committed adopter verdicts are recorded with the landing. | WP-54/WP-55 |
| U-WIN-FS | **Done** (upstream `d58d78bf`) | Native Unicode paths, revision-consistent reads and conditional atomic publication; configuration saves consume the injected contract. Native filesystem and configuration-only gates are recorded in [15](15-windows.md), not a complete Windows app gate. | WP-53 |
| U-WIN-CLIP | **Done** | Scoped native ownership landed upstream in `987e0b68`; D-124 typed export outcomes and Application result observation landed in `1965f8fd`, released as v0.1.10 = public `c12732ce`, CI `37155541830` all seven jobs green. Native 210/210, Mac/Linux Release 222/222 and sanitizer gates pass. ckmux pins 0.1.10 and consumes that API for widget/selection copies and one warning per configured copy, with 58/58 Mac/Linux and Linux ASan/UBSan; no local clipboard workaround. The named Windows 11 host permits old null-owner writes, so original data loss was not reproduced there. | WP-53 |
| U-WIN-WAIT | Released upstream; release pin consumed | ckVision `81198549`, released as v0.1.12/public `cfe9fdfd`, removes the 64-source ceiling through instance-owned native wait packets. Ten native cases cover changed environment, coalescing/rearming, fairness, cancellation, invalid/closed handles, isolation, process exit and public console integration. Compiled rearm and >64-source-drop mutations fail their intended assertions; restoration and full Windows 211/211 pass. Mac clean-commit 222/222, Linux Release 222/222, ASan/UBSan plus fuzz 232/232 and TSan 221/221 pass. GitHub CI `37169919485` passed all seven jobs. ckmux pins 0.1.12 with Mac/Linux Release and Linux ASan/UBSan 59/59; using the native wait in the full application remains WP-54 work. | WP-54 |
| U-INPUT-DEADLINE | Released upstream; release pin consumed | The same v0.1.12 advertises lone Escape deadlines and services quiet decoding after every POSIX wait outcome. Two deterministic regressions compile and fail on original library code, then pass the correction. The application flood assertion is unchanged; the actual cold GitHub v0.1.12 adopter passes 59/59 on Mac and Linux Release and Linux ASan/UBSan. Full library gates above pass; no application workaround. | WP-53/WP-54 |
| U-WIN-HOST-DIAGNOSTICS | Released upstream; integrated in native candidate | D-077/D-127 at ckVision `13282588`, released as 0.1.13/public `e001070f`, supplies concrete WindowsTerminal output capture, graphics trace and decoded mouse-report counts, with native resize, recording/replay and restore regressions. GitHub run `37174128133` passes all seven jobs. The isolated native app selects WindowsTerminal and consumes these library observations without a capture/decoder bypass; its current 32-entry native gate passes, with Mac/Linux Release and Mac ASan/UBSan 59/59 integration. A live user-facing Windows Terminal walkthrough remains required. | WP-53 |
| U-WIN-CMD | Released upstream; factory consumed | D-125 at ckVision `462bf4c9` explicitly launches interactive cmd or one-shot command syntax; its image token uses native separators without rewriting argv or command data. The compiled separator mutation fails eight native assertions, restoration passes 32/32. The first public CI's SIGALRM fixture failure is corrected in `0eb28039`; v0.1.11 and subsequent v0.1.12 have all seven jobs green. ckmux pins 0.1.12 and uses the typed factory in both client-local and server-owned modes. Mac/Linux Release and Linux ASan/UBSan pass 59/59; native shell-only cases pass eleven across two CTest entries. A compiled factory-removal mutation fails both cmd policy regressions. No command-line quoting, path rewrite or script workaround belongs here. | WP-53 |
| U-WIN-ARGV | Released upstream; candidate release pin consumed | D-129 at ckVision `931cdff8` exposes the common CRT argv encoder and correct filename-token grammar, consumed by ConPTY and native server startup. Literal goldens and real wide-argv children cover empty/Unicode/quotes/trailing slashes and invalid image tokens. The isolated commit passes Mac/Linux Release 225/225, native ARM64 MSVC 216/216, LLVM 23 Mac ASan/UBSan plus fuzz 235/235 and TSan 224/224, plus normal benchmark gates. Filtered public `be5ca6c1` passes 219 non-PTY and four PTY entries and strict docs. CI `37198293532` passes all seven jobs; v0.1.14 is published at that public commit and the candidate pins it. A clean committed application adopter gate remains required. | WP-53/WP-54 |
| U-WIN-TEXT | Released upstream; candidate release pin consumed | D-130 in the same 0.1.14 release exposes strict CP_UTF8/native UTF-16 conversion; empty and explicit-NUL text succeed, malformed UTF-8/surrogates refuse. ConPTY/filesystem adapters and native entry-point composition reuse it. Same committed and filtered-public gates as U-WIN-ARGV; all seven GitHub jobs pass. No adopter transcoder is added. | WP-53 |
| U-WIN-HELPER-PROCESS | Released upstream and consumed here; live acceptance open | D-132 supplies shared ProcessLaunchSpec and injected ProcessRunner with private-pipe Windows/POSIX adapters, binary stdin, independent capped/drained diagnostics, mandatory successful-descendant policy and typed failures. Final private `b8aca7e5` gates: Mac/GCC Release229/229, native ARM64 MSVC222/222; frozen compiled inputs LLVM23 Debug ASan/UBSan plus fuzz239/239 and TSan228/228. Eight compiled captured-backend negatives fail as intended and are restored/rebuilt. Released public `a38928cf` passes all seven CI37243144550 jobs. ckmux replaces its handrolled runner with the library bridge, choosing ReleaseOnSuccess for clipboard holders; real copy-key regressions preserve selection and expose unknown exit/partial input. No native pipe, quoting or process-lifetime workaround is added here. Cold installed-release Mac adoption passes64/64; clean committed cross-platform verdicts are recorded with the landing. | WP-53/WP-55 |
| U-WIN-PROCESS-STATS | Released upstream; consumption candidate under verification | D-131 is ckVision 0.1.15, private `ddb73e20`, public `1dd00956`, released at 14:00:23Z with all seven CI37205721888 jobs green. Full-width ProcessId and typed clock-free ProcessResources use the common session seam and reusable native process/job samplers. Lifetime job CPU includes exited children; resident/private-resident memory uses working-set pages, not virtual commit charge. Unreadable members make totals absent with typed/native errors. Clean library gates: Mac/GCC Release226/226, native ARM64 MSVC218/218, LLVM23 ASan/UBSan plus fuzz236/236 and TSan225/225, exits0 and benchmark gates. Both compiled native negatives fail as intended and restored gates pass; filtered public Release220 plus four PTY entries and strict docs pass. The isolated app pins15 and consumes the resource seam in server/local paths without root-PID gating. Protocol4 preserves availability, job scope and errors; a shared pure rate converter and honest footer replace both old rate implementations. Seven focused Mac ASan/UBSan suites pass at pre-native-fixture appv17; later native/root-exit fixtures, GCC and full app gates remain required before landing. | WP-53/WP-55 |
| U5-b | **Done** (`4d0afef`) | `process_id()` on `core::TerminalSubsession`, default −1. | WP-39 |
| U5-a | **Done** (`da11baa`) | `process_id()` on `PosixTerminalSubsession`, observation-only. | WP-37/WP-38 |
| U4-a | **Done** (`fae8f97`) | Window Switcher Bar. | WP-32 |
| U4-b | **Done** (`0c06c3f`) | Horizontal/vertical/grid tiling and filled-layout fractions. | WP-30/WP-31 |
| U4-c | **Done** (`0c06c3f`) | Maximize-follows-on-open. | WP-33 |
| U4-d | Open, non-blocking | A window-order signal on `Desktop`; WP-29 still samples on a settle timer. | follow-up to U4-a |
| U4-g | **Done** (`6d7172b`) | Oversized dialogs scroll with buttons pinned. | M3 |
| U4-i | **Done** (`720e0eb`) | Minimize state and window control. | M3 |
| U4-h | Open, non-blocking | A content-area change subscription on `Desktop`; current consumers chain the bounds observer. | follow-up to U4-g |
| U4-f | Open, non-blocking | `View::set_visible()` should report a size-hint change; ckmux currently triggers relayout explicitly. | follow-up to U4-a |
| U4-e | Open, non-blocking | Public "gesture in progress" state on `Window`; WP-29 still infers the end from stable samples. | follow-up to U4-a |
| U3 | Deferred, demand/performance-driven | A scroll op in `Presenter` for unobstructed aligned bands. WP-20's budgets are green without it. | post-v1 candidate |
| U2 | Deferred, app-driven | XTGETTCAP/DECRQSS/rectangles, inner OSC 8 hyperlinks, SGR 53. | later |

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

- **Mechanism**: the sibling checkout **outranks** an installed package —
  `CKMUX_PREFER_CKVISION_SOURCE` defaults ON when
  `${CKMUX_CKVISION_SOURCE_DIR}/CMakeLists.txt` exists (the definition of a
  co-developer), and the tree is consumed via
  `add_subdirectory(... EXCLUDE_FROM_ALL)` so `cmake --install` ships ckmux
  alone. An adopter with no sibling checkout gets `find_package(ckvision
  CONFIG)` exactly as before. This precedence is deliberate and is the
  opposite of the original plan here: AGENTS.md's "fix ckVision, never work
  around it" requires a library edit to be visible in the very next ckmux
  build, and the old order silently built against yesterday's installed
  library while the developer edited today's source. `CKMUX_SANITIZE`
  forwards to `CKVISION_SANITIZE` so one flag instruments both projects.
- **Version discipline**: adopter and CI builds resolve exactly
  `CKMUX_CKVISION_VERSION` — released v0.1.16 — through `find_package` or a
  FetchContent clone of that release tag. Co-development builds deliberately
  use the sibling source first so an upstream fix is visible immediately.
  The isolated Windows-port candidate requires the shared invocation,
  captured-helper and detached-process APIs in 0.1.16. Its release tag peels
  to public `a38928cf`; all seven GitHub CI37243144550 jobs pass. A
  co-development source gate is not an installed-release adopter gate, and
  the newly published upstream release clears that prerequisite. A fresh
  installed16 Mac Release adopter passes64/64 with dependency downloads
  disabled; the landing records its final clean committed-source gates.
  Every version bump runs the full ckmux suite and updates conformance pins
  when behavior changed intentionally.
- **Following ckVision `main` past v0.1.6** (2026-09-27, at ckVision
  `fbd7897`): standard-dialog calls name their modality (D-087:
  `present_modal_dialog`, `present_modal_message_box`,
  `present_modal_terminal_report_dialog`, and `present_modeless_help_viewer` —
  help no longer scopes input to itself); cell metrics and image extents are
  `ckv::PixelSize`, never a cell `Size`; a help topic's body is a list of
  `HelpSpan` runs (D-101); `Window::enter_move_size_mode()` replaces the move
  and resize modes with one (D-093), in which Esc restores the starting
  bounds, and the "Moving a window" help page now says so;
  `TerminalSubsession::set_graphics_trace()` is pure (D-077), and the client's
  mirror session answers it by saying the server decodes its pictures. The
  library also stopped reading `CKVISION_OUTPUT_CAPTURE` and
  `CKVISION_GRAPHICS_LOG` itself (D-077), so `src/main.cpp` now reads both
  and hands the sinks to `PosixTerminal` and the `Application` — which is
  what keeps `test_client_pacing`'s capture evidence real. The server's own
  sessions are not traced. The pin moved to 0.1.7 the same day, and
  ckVision published `v0.1.7` on GitHub that day (public snapshot
  `ece9142` of private `d8f96be`; the two differ in compiled files only by
  comments).
- **License**: both repositories are MIT. Their `LICENSE` files are
  authoritative; `CONTRIBUTING.md` carries the shared DCO, patent-grant and
  relicense-grant terms (WP-26 / ckVision D-OPEN-1).

## Conventions ckmux inherits

Recorded in [11-conventions.md](11-conventions.md): naming/namespace style,
assert-vs-error discipline, no-exceptions-across-callbacks, determinism
rules (injected clock/fs/env), zero-dependency policy, verification
categories and definition-of-done, trunk-only workflow, copyright headers,
and — adopted verbatim, extended to tmux/screen — the clean-room provenance
rule: never read other multiplexers' or Turbo Vision's source; derive from
public standards (ECMA-48, xterm ctlseqs, kitty spec, Unicode UAX) and
documented observation only.
