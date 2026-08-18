# ckmux — Configuration

Plan 08 · Status: draft · 2026-08-16 · [Index](README.md)

Deliberately small in v1. ckmux must be excellent with an *empty* config —
configuration exists to adjust, never to make it usable (contrast: stock
tmux). Zero new dependencies: the config format is a tiny self-parsed
INI-style file.

## Locations & precedence

1. CLI flags (highest)
2. `$CKMUX_CONFIG` if set
3. `$XDG_CONFIG_HOME/ckmux/ckmux.conf`, default `~/.config/ckmux/ckmux.conf`
4. Built-in defaults (lowest)

Environment: `CKMUX_SOCKET` overrides the server socket path (parallel dev
servers, tests). `CKMUX=<socket>,<session-id>` is exported *into* terminals
(like tmux's `$TMUX`) so nested ckmux can warn, and scripts can detect us.

## Format

UTF-8, `#` comments, `[section]` headers, `key = value`, plus repeatable
`bind`/`unbind` directives. Unknown keys warn (with the line number), never
abort. Values: string, int, bool (`true/false`), enum.

**One escape, and only one.** `\#` is a literal `#` rather than the start of a
comment, so that a value which needs one — `exec:my-copy --tag=\#1`, or `#`
bound as a chord — is not silently cut in half. Every other backslash stands
for itself, *including one before another backslash*: the values that need the
escape are shell commands, and a format that rewrote `\\` or `\n` would break
more commands than the escape saves. The comma in `[terminal] clipboard` is
that list's separator and has no escape; a copy command that needs a comma
belongs in a script.

**An unknown `[section]` warns once, at its header, and nothing under it is
read** — one mistake by the reader deserves one sentence back, not a warning
at every well-spelled key underneath a misspelled header. **A key its section
holds twice** is legal and the last line wins; the second one says so, because
"I changed it and nothing happened" is otherwise unanswerable, and because a
dialog's write-back has to land on a file that means one thing.

**A key written under the wrong header is told which header it belongs to**
(`kill-grace-seconds belongs in [general], not [terminal]`) rather than called
unknown: it is a key this plan documents, and a reader who has clearly read
about it is helped by being told where it lives.

```ini
# ~/.config/ckmux/ckmux.conf — everything below shows the default
[general]
prefix          = C-b          # the one stolen key; C-a, F12, … accepted
shell           = $SHELL       # what New Terminal runs
login-shell     = true         # start it as a login shell (Settings ▸ General…)
scrollback      = 10000        # lines per terminal (0 = off)
theme           = dark         # dark | light | mono (Settings ▸ General…)
on-exit         = hold-on-error# close | hold | hold-on-error
confirm-kill    = true         # confirm dialogs for kill/close of live apps
kill-empty-session = true      # session dies when its last terminal closes
kill-grace-seconds = 5         # how long End Session and Close Terminal give
                               # programs to quit before the kill — IF the
                               # reader ticked the dialog's kill box; the
                               # asking (SIGHUP+SIGTERM) happens regardless
                               # (Settings ▸ Settings…, 0–600)
clock           = seconds      # seconds | minutes | off — the clock at the
                               # right end of the menu bar, with a calendar
                               # that drops out of it when clicked
                               # (Settings ▸ General…)

[terminal]
term            = auto         # value of $TERM inside terminals (see plan 04)
mouse           = true         # forward mouse to inner apps that ask
alternate-scroll= true         # wheel→arrows on alternate screen (mode 1007)
sixel           = auto         # auto | off   (auto = probe host support)
sixel-max-megapixels = 64      # largest picture a program may draw, in
                               # megapixels. One too large for the window is
                               # cut off at its edge, so this only stops an
                               # absurd one (1–4096; Settings ▸ General…)
osc52           = true         # inner apps may set the clipboard (64 KiB cap)
clipboard       = osc52,pbcopy # copy targets, in order; also: pbcopy-style
                               # helper cmd via  exec:<command>

[printer]                      # WP-PRINT — edited in Terminal ▸ Printer Settings…
mode            = ask          # ask | capture | off
ask-cache       = 256K         # held per terminal until the user answers;
                               # discarded whole if exceeded (plan 04)
spool-limit     = 1M           # per terminal once capturing was accepted
save-format     = txt          # txt | ansi
save-folder     = ~/Documents  # where the save dialog starts
save-ask-name   = true         # false = auto-name the file

[render]
max-fps         = 30           # output coalescing tick (server→client flush)

[keys]
# bind <context> <chord> <action>   — actions are the registry names (plan 06)
# unbind <context> <chord>
# Contexts: terminal, desktop, copy-mode, picker, move-resize
bind terminal d detach
bind terminal c new-terminal
# example user tweaks:
# bind terminal C new-terminal-command
# unbind terminal q
```

Chord grammar: `^X` or `C-x` (Ctrl), `S-x` (Shift), `A-x`/`M-x` (Alt/Meta),
`F1`–`F12`, named keys (`Enter`, `Esc`, `Space`, `Up`, …). Chords in `[keys]`
are the key *after* the prefix, except in non-terminal contexts where they
bind direct keys.

A Ctrl chord may be written either way and is one chord; it is *shown* as
`^B`, which is what [06](06-ui-design.md) writes in the footer, the menu
accelerators, the which-key popup and the help pages. One spelling out of two
in, because a chord dispatches under exactly the string it is displayed as —
when the prefix was shown as `^B` and spelled `C-b`, the Send Prefix menu item
advertised `^B C-b` for what is one key pressed twice.

Both of the upstream dependencies this table used to carry have landed:
`scrollback` reaches ckVision's `max_scrollback_lines`, `0` included (U0-c),
and `osc52` reaches its clipboard policy (L-21). What remains unhonoured is
listed under "What exists today" below, key by key, with the package each one
waits for.

## Semantics & lifecycle

- **Client-side options** (theme, keys, clock, clipboard) are read by each
  client at startup — re-attach to apply changes.
- **Server-side options** (scrollback, shell, on-exit, kill-empty-session,
  max-fps) are read by the server at *server* start and sent per-terminal at
  creation; changing them affects new terminals, not existing ones (honest
  and simple; a `reload` command is a later feature).
- **`[printer]` is scoped and live** (WP-PRINT), the one exception to both
  rules above: it is server-side (the emulator lives there), it resolves
  **terminal → session → global → built-in**, and it takes effect
  immediately, because the setting is the answer to a question a running
  program just asked. The file holds the global scope only; terminal and
  session scopes are server session state and are never written.
- **A dialog may write this file back.** Printer settings are edited in
  `Terminal ▸ Printer Settings…` ([06](06-ui-design.md)); `Global` /
  `Save as default` persists them. `login-shell`, `sixel-max-megapixels`,
  `clock`, `kill-grace-seconds` and `theme` are edited in
  `Settings ▸ General…`. The write is key-targeted — matching `key = value`
  lines are rewritten in place (*every* matching line, since the file is read
  back last-line-wins and a Save that lands on the losing line is a Save the
  reader cannot see), a missing key joins its section, and a missing section
  is appended — so comments, ordering, inline notes and unrelated keys
  survive. The write is also atomic and durable: a per-process scratch file,
  flushed, renamed over the target, and the directory entry flushed after it,
  because a settings dialog that reports success and loses the setting is the
  one failure it must not have. Everything else in this file stays hand-edited
  in v1.
- **A file that cannot be read is not a file that is not there.** No file is
  the ordinary case and says nothing; a file that exists and cannot be opened
  warns, naming the path and the reason, because that is the reader's own
  configuration being ignored wholesale. A write-back into such a file is
  refused rather than replacing it with the single key being saved.

## What exists today (WP-12, 2026-08-17)

**Every key above is read, validated and defaulted** — the table is the
specification of `ckm::Settings`, key for key, and a test walks the whole of
it. `bind` and `unbind` work, and a rebinding reaches dispatch, the footer,
the which-key popup and the help pages at once, because all four read one
table (`client/commands.hpp`'s `Keymap`).

Three states, and the difference between them is the point:

| | Meaning |
|---|---|
| **Honoured** | The value reaches something today. |
| **Read, not yet honoured** | The value is parsed and validated; the package that acts on it has not landed. `ckm::keys_not_honoured_yet()` names each one with its package, and `check-config` prints that list — so "I set this, why did nothing happen?" has an answer that is neither a lie nor a warning about a documented key. |
| **Unknown** | No such key. Warns with its line, and the rest of the file is still read. |

Honoured today: `prefix`, `shell`, `login-shell`, `scrollback` (including
`0`), `theme`, `clock`, `confirm-kill`, `kill-grace-seconds`,
`kill-empty-session`, `term`, `mouse`, `alternate-scroll`, `sixel`,
`sixel-max-megapixels`, `osc52`, `clipboard`, `[render] max-fps`,
`[printer] mode` and `[printer] spool-limit`, and the whole of `[keys]`.

Read but not yet honoured: `on-exit` (WP-13), and `[printer]`'s `ask-cache`,
`save-format`, `save-folder` and `save-ask-name` (PRINT-1…6). A key leaves
that list in the commit that wires it up — `[terminal] clipboard` left it with
WP-17, which is where copying began, and `clock` left it with the menu-bar
clock.

Four keys stayed on that list *after* they were wired up, which is the same
defect facing the other way: `check-config` told a reader that setting them
would do nothing while they were quietly doing something. `kill-empty-session`
reaches the server's session bookkeeping, `[render] max-fps` its output
coalescing tick, and `[printer] mode` and `spool-limit` the terminal launch
profile. Corrected 2026-08-18 (plans/13, m-conf), with a test that fails if
any of the four returns to the list.

The one remaining printer gap is about a *value*, not a key: `mode = off`
genuinely denies capture, but until PRINT-1…6 lands there is nobody to ask, so
`mode = ask` captures rather than asking. `keys_not_honoured_yet()` is a list
of keys nothing reads, so it does not carry that — this paragraph does.

Notes on the parts that are easy to get wrong:

- **Chords** are `^`/`C-`/`S-`/`A-`/`M-` prefixes on a character, an
  `F1`–`F12`, or a named key (`Enter`, `Esc`, `Space`, `Up`). `M-` and `A-`
  are one modifier written two ways, and `^X` and `C-x` are one modifier
  written two ways, and each pair is stored as one, so a table cannot hold the
  same key twice. On a printable character the case *is* the shift — `T` is
  Shift+t — so `S-t` is refused rather than accepted as a second spelling of
  a key that already has one.
- **With Ctrl, the case is not the shift.** A terminal sends one byte for
  Ctrl+b and Ctrl+Shift+b alike, so `C-B` is folded to `C-b` and shown as
  `^B`, rather than parsing into a second binding no reader could ever press.
  `C-Space` keeps its long spelling: Space is a named key here, and `^ ` would
  be an invisible one. A lone `^` is still the character `^`.
- **Only `terminal` bindings are honoured today.** `desktop`, `copy-mode`,
  `picker` and `move-resize` parse as real context names and are then refused
  *out loud*, because a binding that looks live in a reader's file and does
  nothing in the program is exactly the defect
  [12-work-packages.md](12-work-packages.md)'s reachability rule exists for.
- **The digits `1`–`9` cannot be rebound**: they carry a value (focus the
  n-th terminal) rather than selecting a handler, so there is nothing for a
  binding to point at. Asking is refused with that reason.
- **Action names are exactly the actions that exist.** The list grows with
  the packages that add them — `copy-mode` and `paste` arrived with WP-17 —
  so every name a file can contain is a name that does something. Copy mode's
  own keys are not among them: they are modal motions rather than commands,
  and are fixed in v1 ([06-ui-design.md](06-ui-design.md)).
- **The theme is chosen in `Settings ▸ General…` and stored.** It used to be
  three items in a View menu that changed the theme for the session only, on
  the reasoning that a passing look at Monochrome should not rewrite the file —
  but a reader who chose Light then had to choose it again every morning, and
  the setting they had actually made was the one the file kept saying. A
  dialog with a Save button is a decision; a menu item is a look.

Two keys landed before the rest, because both are edited in a dialog and a
settings dialog that forgets what it was told is not a setting:

- The file format above — sections, `key = value`, `#` comments — and the
  location precedence, read at client start.
- `sixel-max-megapixels` takes a whole number between 1 and 4096 and nothing
  else — no unit suffix, since `64M` in a file full of byte sizes would read
  as megabytes. Out of range or unreadable, it warns with its line and keeps
  the default rather than clamping to a number the reader never chose. The
  same `parse_sixel_megapixels()` backs the dialog's field, so the file and
  the dialog cannot come to disagree about what a valid value is.
- Warnings are shown once, in a dialog naming the offending lines, rather
  than printed where the alternate screen would swallow them. A refused
  binding joins that same list, so one dialog answers "what in my file did
  not work".
- The key-targeted write-back, exercised by `Settings ▸ General…`.

Every rejection says four things: what the key wanted, what the file wrote,
that the line was not obeyed, and what is in force instead. A message missing
the last one leaves a reader who wrote `theme = drak` unsure whether they now
have light.

`src/common/config.*` parses and writes, `src/common/keymap.*` owns the
vocabulary a binding is written in — action names, context names, the chord
grammar — and `src/platform/paths.*` decides where the file lives. The keymap
vocabulary sits in `common` rather than beside the client's table because
`check-config` has to validate a file without opening a window.

Still open: `ckmux check-config`, which parses and reports without starting
anything. It is CLI plumbing (WP-11) on top of what exists — `load_settings`
already returns every warning it would print, and `keys_not_honoured_yet()`
already returns the rest of its output.

Every default above is also the answer to "what does an empty config do".

## Explicitly not configurable in v1

Status-bar contents/format strings, hooks/scripts on events, theme *editing*
(pick one of three), general option tables per terminal. Each of these is a
magnet for complexity; revisit only with concrete need
([10-roadmap.md](10-roadmap.md)).

The one deliberate exception is **printer policy**, which is per session and
per terminal by nature: a program printing in *this* window is a question
about *this* window, and answering it globally the first time it happens
would be the wrong default. It is settable nowhere else — no env var, no
hidden key — only in its dialog and the `[printer]` section it stores.
