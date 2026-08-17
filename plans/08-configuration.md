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

```ini
# ~/.config/ckmux/ckmux.conf — everything below shows the default
[general]
prefix          = C-b          # the one stolen key; C-a, F12, … accepted
shell           = $SHELL       # what New Terminal runs
login-shell     = true         # start it as a login shell (Settings ▸ General…)
scrollback      = 10000        # lines per terminal (0 = off)
theme           = dark         # dark | light | mono
on-exit         = hold-on-error# close | hold | hold-on-error
confirm-kill    = true         # confirm dialogs for kill/close of live apps
kill-empty-session = true      # session dies when its last terminal closes
clock           = false        # clock in the footer status area

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

Chord grammar: `C-x` (Ctrl), `S-x` (Shift), `A-x`/`M-x` (Alt/Meta), `F1`–`F12`,
named keys (`Enter`, `Esc`, `Space`, `Up`, …). Chords in `[keys]` are the key
*after* the prefix, except in non-terminal contexts where they bind direct
keys.

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
  `Save as default` persists them. `login-shell` and
  `sixel-max-megapixels` are edited in `Settings ▸ General…`. The write is
  key-targeted — matching `key = value`
  lines are rewritten in place, a missing key joins its section, and a
  missing section is appended — so comments, ordering, inline notes and
  unrelated keys survive. Everything else in this file stays hand-edited in
  v1.

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
`0`), `theme`, `confirm-kill`, `term`, `mouse`, `alternate-scroll`, `sixel`,
`sixel-max-megapixels`, `osc52`, `clipboard`, and the whole of `[keys]`.

Read but not yet honoured: `on-exit` (WP-13), `kill-empty-session` (WP-8),
`clock` (WP-14), all of `[printer]` (U1-c and PRINT-1…6), and
`[render] max-fps` (WP-2). A key leaves that list in the commit that wires it
up — `[terminal] clipboard` left it with WP-17, which is where copying began.

Notes on the parts that are easy to get wrong:

- **Chords** are `C-`/`S-`/`A-`/`M-` prefixes on a character, an `F1`–`F12`,
  or a named key (`Enter`, `Esc`, `Space`, `Up`). `M-` and `A-` are one
  modifier written two ways and are stored as one, so a table cannot hold the
  same key twice. On a printable character the case *is* the shift — `T` is
  Shift+t — so `S-t` is refused rather than accepted as a second spelling of
  a key that already has one.
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
- **The View menu's theme choice is for this session only.** The file is the
  reader's; a passing look at Monochrome should not rewrite it.

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
