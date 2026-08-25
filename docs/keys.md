---
title: Keys
---

`^B` (Ctrl-b) is the *prefix*: press it, then the command key. The footer
shows the most useful chords for your terminal's width, `^B ?` opens the full
key reference, and every command here is also in the menu bar — a chord is a
shortcut, never the only way.

A reader's `bind` lines in the [configuration](configuration.md) rewrite
these chords; commands without a default chord remain available in the menus.

## Default key appendix

This table is generated from the same registry that drives dispatch, the
menus, footer, which-key popup, and in-application help. `Prefix` means `^B` by
default. A `bind` or `unbind` line can change any row with an action name.

| Default | Command | `bind` action |
|---|---|---|
| `^B c` | New Terminal | `new-terminal` |
| `^B x` | Close Terminal | `close-terminal` |
| `Menu only` | Kill Terminal | `kill-terminal` |
| `^B .` | Move to Session... | `move-terminal` |
| `^B ,` | Rename Terminal... | `rename-terminal` |
| `^B P` | Print Output... | `print-output` |
| `Menu only` | Printer Settings... | `printer-settings` |
| `^B n` | Next Terminal | `next-terminal` |
| `^B p` | Previous Terminal | `previous-terminal` |
| `^B 1-9` | Focus Terminal by Number | — |
| `^B w` | Window List... | `window-list` |
| `^B z` | Zoom | `zoom` |
| `^B _` | Minimize | `minimize` |
| `^B M` | Move / Resize | `move-resize` |
| `^B h` | Tile Horizontally | `tile-horizontally` |
| `^B v` | Tile Vertically | `tile-vertically` |
| `^B g` | Tile Grid | `tile-grid` |
| `^B T` | Cascade | `cascade` |
| `^B m` | Menu Bar | `menu-bar` |
| `^B [` | Copy Mode | `copy-mode` |
| `^B ]` | Paste | `paste` |
| `^B ^B` | Send Prefix to Program | `send-prefix` |
| `^B d` | Detach | `detach` |
| `^B s` | Sessions... | `sessions` |
| `^B S` | New Session... | `new-session` |
| `^B R` | Rename Session... | `rename-session` |
| `^B K` | End Session... | `kill-session` |
| `^B ?` | Keys... | `key-reference` |
| `^B q` | Quit ckmux | `quit` |
| `Menu only` | Settings... | `settings` |
| `^B b` | Status Bar | `status-bar` |
| `Menu only` | Fit Desktop to This Screen | `fit-desktop` |
| `Menu only` | Show CPU Usage | `show-cpu` |
| `Menu only` | Show Memory Usage (RSS) | `show-memory-rss` |
| `Menu only` | Show Memory Usage (Real) | `show-memory-real` |
| `Menu only` | About ckmux... | `about` |

## Copy mode and paste

`^B [` enters copy mode on the focused terminal (its scrollback included);
`^B ]` pastes what was copied.

Inside copy mode:

| Keys | Movement / action |
|---|---|
| Arrows, `h j k l` | Move the cursor |
| `0` / `$` | Start / end of line |
| `g` / `G` | Top / bottom of history |
| PageUp / PageDown, `^B` / `^F` | Page up / down |
| `v` | Select by character |
| `V` | Select by line |
| `^V` | Select a rectangle |
| `/` and `?` | Search forward / backward |
| Enter | Copy the selection and leave |
| Esc | Leave without copying |

Where a copy goes — the outer terminal's clipboard, `pbcopy`, or a command of
your own — is the `[terminal] clipboard` setting in the
[configuration](configuration.md).
