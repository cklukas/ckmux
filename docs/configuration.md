---
title: Configuration
---

ckmux reads one file: `~/.config/ckmux/ckmux.conf` (precisely:
`$XDG_CONFIG_HOME/ckmux/ckmux.conf`, or the file `CKMUX_CONFIG` names). A
missing file is the ordinary case — every setting has a default, and this
page states each one.

`ckmux check-config` parses the file and reports every problem with its file
and line, starting nothing; it exits non-zero when the file has problems. A
running ckmux shows the same warnings at startup rather than guessing. The
Settings dialog (`Settings ▸ General…`) writes the same keys back to the same
file.

Sizes take an optional upper-case suffix: `K`, `M`, or `G` (powers of 1024).

## [general]

| Key | Default | Meaning |
|---|---|---|
| `prefix` | `C-b` | The one key ckmux takes from the programs inside it. |
| `shell` | `$SHELL` | What New Terminal runs; empty means your shell, resolved at launch. |
| `login-shell` | `true` | Start it as a login shell, like every other terminal window on the machine. |
| `scrollback` | `10000` | Lines of history per terminal; `0` means remember nothing. |
| `max-terminals` | `64` | The most terminals one session may hold. |
| `theme` | `dark` | `dark`, `light`, or `mono`. |
| `on-exit` | `hold-on-error` | A window whose program exited: `close`, `hold`, or `hold-on-error` (keep it only when the exit status was non-zero — when the text on screen is the evidence). |
| `audible-bell` | `false` | Whether a child's bell reaches your terminal as a sound. The visual half — border flash and footer flag — is always on. |
| `confirm-kill` | `true` | Ask before closing or killing something with a live program in it. |
| `kill-grace-seconds` | `5` | How long programs get to end on their own when a session is killed; `0` means don't wait. |
| `kill-empty-session` | `true` | A session whose last terminal closed goes away. |
| `clock` | `seconds` | The menu-bar clock: `seconds`, `minutes`, or `off`. |
| `desktop-size` | `fixed` | Whose screen sizes the shared desktop: `fixed`, `fit-smallest`, or `fit-latest`. A client's own screen never silently reflows a session — resizing SIGWINCHes every child, for every reader watching. |
| `resize-windows-to-fit` | `false` | After a reattach on a smaller screen has moved an oversized window up and left, whether a second, resizing step shrinks it to fit. |
| `show-cpu` | `false` | CPU readout on every terminal window's footer (View menu). |
| `show-memory-rss` | `false` | Memory (RSS) readout. |
| `show-memory-real` | `false` | Memory readout in the platform's own "what does it actually cost" metric. |

## [terminal]

| Key | Default | Meaning |
|---|---|---|
| `term` | `auto` | `$TERM` inside terminals: `auto` lets ckmux claim what it can honestly support; anything else is used verbatim. |
| `mouse` | `true` | Forward mouse events to programs that ask for them. |
| `alternate-scroll` | `true` | Wheel scrolling in full-screen programs becomes arrow keys. |
| `sixel` | `auto` | Sixel graphics: `auto` or `off`. |
| `sixel-max-megapixels` | `64` | The largest picture a program may draw — an allocator guard, far above a full 4K screen. |
| `osc52` | `true` | Whether a program may put text on the clipboard with OSC 52 (capped at 64 KiB). |
| `clipboard` | `osc52, pbcopy` | Where a copy goes, in order: `osc52` (the outer terminal), `pbcopy` (a local helper), or `exec:<command>` (text on stdin). |

## [printer]

Programs can print through the terminal (the ANSI printer controls); ckmux
captures that output per window.

| Key | Default | Meaning |
|---|---|---|
| `mode` | `ask` | `ask` (a popup offers the capture), `capture` (always keep it), or `off`. |
| `ask-cache` | `256K` | How much print output the Ask popup holds. |
| `spool-limit` | `1M` | The most one print job may spool. |
| `save-format` | `txt` | Saved jobs: `txt` (plain text) or `ansi` (with colors and attributes). |
| `save-folder` | `~/Documents` | Where saved jobs go. |
| `save-ask-name` | `true` | Ask for a file name on save. |

## [render]

| Key | Default | Meaning |
|---|---|---|
| `max-fps` | `30` | The most frames per second the client draws. |

## [keys]

`bind` and `unbind` lines rebind the chords; they never add or remove
commands — a command with no chord is still in the menus.

```
[keys]
bind terminal C new-terminal
unbind terminal x
```

(`#` is not a comment character inside a bind line — it is a chord someone
may want to bind.)

The form is `bind <context> <chord> <action>` and
`unbind <context> <chord>`. Order matters: two binds of one chord mean the
later one; an unbind after a bind means neither.

Contexts: `terminal` (the key after the prefix), `desktop`, `copy-mode`,
`picker`, `move-resize`.

Actions: `new-terminal`, `close-terminal`, `kill-terminal`, `move-terminal`,
`rename-terminal`, `print-output`, `printer-settings`, `next-terminal`,
`previous-terminal`, `window-list`, `zoom`, `minimize`, `move-resize`,
`tile-horizontally`, `tile-vertically`, `tile-grid`, `cascade`, `menu-bar`,
`copy-mode`, `paste`, `detach`, `sessions`, `new-session`, `rename-session`,
`kill-session`, `key-reference`, `settings`, `status-bar`, `fit-desktop`,
`show-cpu`, `show-memory-rss`, `show-memory-real`, `about`, `send-prefix`,
`quit`.

`ckmux check-config` also names any key that is parsed and validated but not
yet acted on by this version, with the work it waits for — so "I set this,
why did nothing happen?" always has an answer.

Known v0.1.1 diagnostic defect: `check-config` still lists `ask-cache`,
`save-format`, `save-folder`, and `save-ask-name` as waiting for printer work.
The runtime does honor all four; only that report is stale.
