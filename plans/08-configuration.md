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

Two defaults above depend on ckmux-driven upstream work in ckVision
([04-terminal-emulation.md](04-terminal-emulation.md) gap list): `scrollback`
(the emulator's capacity is currently fixed at 2 000 lines — U0) and `osc52`
(child clipboard writes are currently denied with no allow path — U1). Until
those land, the effective behavior is the current upstream behavior and
`check-config` says so.

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

## What exists today (M1)

Config v1 as a whole is M3 ([10-roadmap.md](10-roadmap.md)). Two keys landed
early, because both are edited in a dialog and a settings dialog that forgets
what it was told is not a setting:

- The file format above — sections, `key = value`, `#` comments — and the
  location precedence, read at client start.
- `[general] login-shell` and `[terminal] sixel-max-megapixels`, and nothing
  else. Every other key in the table above warns as unknown and is otherwise
  ignored; a reader who has written the whole file ahead of time still gets
  the parts that work.
- `sixel-max-megapixels` takes a whole number between 1 and 4096 and nothing
  else — no unit suffix, since `64M` in a file full of byte sizes would read
  as megabytes. Out of range or unreadable, it warns with its line and keeps
  the default rather than clamping to a number the reader never chose. The
  same `parse_sixel_megapixels()` backs the dialog's field, so the file and
  the dialog cannot come to disagree about what a valid value is.
- Warnings are shown once, in a dialog naming the offending lines, rather
  than printed where the alternate screen would swallow them. `check-config`
  arrives with the rest of config v1.
- The key-targeted write-back, exercised by `Settings ▸ General…`.

`src/common/config.*` parses and writes; `src/platform/paths.*` decides
where. Adding the remaining keys is a matter of extending `Settings` and its
one switch — the file handling is done.
- Validation: `ckmux check-config` parses and reports problems without
  starting anything. Bad prefix / unknown action names list valid options.
- Every default above is also the answer to "what does an empty config do".

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
