# Terminal capabilities, from ckmux's point of view

Plan · Status: current 2026-08-16 · [Index](README.md)

The full inventory — every control sequence, which of the two terminals it
belongs to, and whether it is emulated, host-dependent, or unsupported —
lives in ckVision, because that is where the emulator and the host layer
both are:

**[`../../ckvision/plans/term_capabilities.md`](../../ckvision/plans/term_capabilities.md)**

Duplicating it here would guarantee two versions that disagree. This page
records only what is specific to ckmux.

## The three terminals in a ckmux session

ckVision's document describes two terminals. Under ckmux there are three, and
naming them is usually enough to answer a "why does this look wrong" question:

```
the program (vim, htop)
        ↓ writes VT bytes
  inner terminal   — ckVision's TerminalEmulator, one per ckmux terminal.
                     What we emulate. Ours to fix.
        ↓ parsed into cells, then re-rendered by ckVision
  outer terminal   — the real terminal ckmux is running in.
                     What we can only detect and degrade to.
        ↓
      the reader
```

A child's bytes **never** reach the outer terminal. They are parsed into
cells and images and re-rendered, which is what makes window overlap correct
and makes `cat` of a hostile file harmless.

## What ckmux tells its children

- **`TERM=xterm-256color`.** Our supported set is a subset of it, plus
  `bce` — which `xterm-256color` declares and which curses programs rely on
  to paint full-width coloured bars by erasing rather than by writing spaces.
- `COLORTERM=truecolor`, `TERM_PROGRAM=ckmux`, `TERM_PROGRAM_VERSION`.
- Everything else — `HOME`, `PATH`, `LANG`, shell configuration — is
  inherited from the reader's own environment, and terminals open in `$HOME`.

At M5 a `ckmux-256color` terminfo entry replaces the borrowed one: it
describes exactly what we do, no more, so a program that consults terminfo
is told the truth rather than a close approximation
([04-terminal-emulation.md](04-terminal-emulation.md)).

## What ckmux adds on top of the inner terminal

| Concern | Where it is handled |
|---|---|
| Window titles from `OSC 0`/`OSC 2` | ckmux polls the snapshot title and retitles the window, reverting to its own name when the child stops claiming one ([06-ui-design.md](06-ui-design.md)) |
| Which key never reaches the child | the prefix, and only the prefix ([06-ui-design.md](06-ui-design.md)) |
| Mouse ownership | forwarded when the child asked for it; otherwise ckmux's ([06-ui-design.md](06-ui-design.md)) |
| Sixel display | decoded per terminal by the inner emulator; drawn by ckVision, occlusion-sliced against overlapping windows |

## Which gaps ckmux cared about most, and where they went

ckVision's document ends with an ordered list of gaps. The four that would
have bitten a ckmux reader first are closed — in ckVision, as the rule
requires, and each with a reason a reader would recognise:

1. **`OSC 11` background query.** vim and nvim probe it before picking a
   colour scheme; silence made them guess, and they guess light on a dark
   theme. Answered now, along with `OSC 10` and `OSC 4 ; n ; ?`.
2. **`CSI ? 1007` alternate scroll.** The wheel now scrolls `less` and `man`,
   as three cursor keys in the child's own dialect, on the alternate screen
   only — on the primary buffer the wheel is the terminal's own history.
3. **Child `OSC 52`.** Copying out of a program running inside ckmux, opt-in
   per terminal, size-capped, and with reads refused under every policy.
4. **`SGR 4:3` / `58`.** nvim's undercurl and its colour, drawn where the
   outer terminal can draw them and degraded to a plain rule where it cannot.

Two more from the same list came with them: a child's **indexed colours are
preserved end to end**, so the reader's own terminal theme applies to a
program's `SGR 31` under ckmux exactly as it does outside it — and it is what
a per-terminal palette re-theming feature would be built on — and the **kitty
keyboard protocol's child side**, so a full-screen program run inside ckmux
can tell `Ctrl+I` from `Tab`.

They are recorded as landed items in
[07-ckvision-integration.md](07-ckvision-integration.md). Everything here is
fixed in ckVision, never worked around here; what remains open there is
listed at the end of its own document.

## How a fidelity question gets answered

Not by argument. `ckvision/tools/vtconform/` plays the same byte script
through the inner emulator and through an established terminal and compares
the resulting cells; a difference is either fixed or recorded with the
documentation that justifies it. If something renders differently under
ckmux than in a plain terminal, the first move is to reduce it to a byte
script and add it as a case.
