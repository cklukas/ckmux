---
title: Keys
---

`^B` (Ctrl-b) is the *prefix*: press it, then the command key. The footer
shows the most useful chords for your terminal's width, `^B ?` opens the full
key reference, and every command here is also in the menu bar — a chord is a
shortcut, never the only way.

A reader's `bind` lines in the [configuration](configuration.md) rewrite
these chords; the commands themselves are fixed.

## Terminals

| Chord | Command |
|---|---|
| `^B c` | New Terminal |
| `^B x` | Close Terminal |
| `^B ,` | Rename Terminal |
| `^B .` | Move to Session… |
| `^B n` / `^B p` | Next / Previous Terminal |
| `^B 1`–`9` | Focus Terminal by Number |
| `^B w` | Window List… |
| `^B P` | Print Output… |

Kill Terminal deliberately has no default chord — it destroys unsaved work
with no undo, so it lives in the menu; bind `kill-terminal` yourself if you
want it on a key.

## Windows and layout

| Chord | Command |
|---|---|
| `^B z` | Zoom (maximize / restore) |
| `^B _` | Minimize — the window returns from the window bar |
| `^B M` | Move / Resize by keyboard |
| `^B h` / `^B v` / `^B g` | Tile Horizontally / Vertically / Grid |
| `^B T` | Cascade |
| `^B b` | Status Bar on/off |

## Sessions and the client

| Chord | Command |
|---|---|
| `^B d` | Detach — programs keep running |
| `^B s` | Sessions… (the picker) |
| `^B S` / `^B R` / `^B K` | New / Rename / End Session… |
| `^B q` | Quit ckmux |
| `^B m` | Menu Bar |
| `^B ?` | Keys… (the reference) |
| `^B ^B` | Send a literal prefix to the program |

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
