---
title: Getting started
---

## Install

From the [releases page](https://github.com/cklukas/ckmux/releases):

**Debian / Ubuntu (x86_64):**

```sh
sudo apt install ./ckmux_0.1.0_amd64.deb
```

**Linux tarball (x86_64) or macOS (Apple Silicon):**

```sh
tar xzf ckmux-0.1.0-*.tar.gz
sudo cp ckmux-0.1.0-*/bin/ckmux /usr/local/bin/
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
renames one, `^B K` ends one. Two people (or two terminals) can watch the
same session with `ckmux attach --share`.

Quitting entirely: `^B q` quits the client; `ckmux kill-server` ends the
server and every terminal in it.
