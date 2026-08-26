---
title: Getting started
---

## Install

From the [releases page](https://github.com/cklukas/ckmux/releases):

**Debian / Ubuntu (x86_64):**

```sh
sudo apt install ./ckmux_0.1.1_amd64.deb
```

**Linux tarball (x86_64) or macOS (Apple Silicon):**

```sh
tar xzf ckmux-0.1.1-*.tar.gz
sudo cp ckmux-0.1.1-*/bin/ckmux /usr/local/bin/
```

Every archive ships with a `.sha256` checksum file alongside it.

**From source** (CMake ≥ 3.28, a C++20 compiler, POSIX only — macOS or
Linux):

```sh
git clone https://github.com/cklukas/ckmux.git
cmake -S ckmux -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build          # optional, the full suite
sudo cmake --install build
```

The source install includes `ckmux(1)` and the complete browser guide; use
`man ckmux` for the command reference. Its default-key appendix and the
browser [keys page](keys.md) are generated from the compiled command registry,
so they stay aligned with the interface.

The build fetches the matching [ckVision](https://github.com/cklukas/ckVision)
release automatically, pinned to the exact version this ckmux was developed
against; an installed ckVision package of that version is used instead if
present.

## First session

```sh
ckmux
```

That is the whole start: it attaches to your terminal windows, starting the
server first if none is listening. You get a desktop with a menu bar, a
footer, and one terminal window running your shell.

- **`^B c`** opens a new terminal window; **`^B n`** / **`^B p`** cycle
  through them.
- The **mouse works everywhere**: click a window to focus it, drag its title
  to move it, drag its edges to resize, use the menu bar.
- **`^B m`** opens the menu bar by keyboard; **F1** opens help; **`^B ?`**
  shows every key.

`^B` (Ctrl-b) is the *prefix* — the one key ckmux takes for itself. Press it,
then the command key. `^B ^B` sends a literal Ctrl-b to the program in the
window. The prefix is [configurable](configuration.md).

## Detach, reattach, sessions

**`^B d`** detaches: the client exits, the server and every program in it
keep running. Plain `ckmux` later brings you back exactly where you were.

One server can hold several *sessions* — separate desktops, each with its own
terminals:

```sh
ckmux new -s work          # a second session, made without attaching
ckmux ls                   # what the server holds
ckmux attach work          # straight into it
```

`^B s` opens the session picker from inside; `^B S` creates a session, `^B R`
renames one, `^B K` ends one.

Two people — or two of your own terminals — can watch one session at the same
time. `ckmux attach --share` joins a session instead of taking it over, and
`ckmux attach --watch` joins it read-only: you see everything and nothing you
type reaches it. From the picker, a session somebody else is watching offers
the same three answers, and the footer says `2 readers` while you have company.

Once you are in, the Session menu carries the rest: **Watch Only** toggles you
into or out of read-only mode, **Others Read-Only** does it to everybody else,
and **Take Session Over** drops them to their pickers. Any reader can do any of
these to any other — there is no owner, so whatever one of you does, the other
can undo.

Quitting entirely: `^B q` quits the client; `ckmux kill-server` ends the
server and every terminal in it.
