---
title: ckmux
---

**ckmux** is a terminal multiplexer — like tmux — with a face. It keeps your
terminal sessions, and the programs running inside them, alive when you
disconnect, and lets you reattach later exactly where you left off.

Unlike tmux, its interface is visible: a permanent menu bar, a footer showing
the keys that work right now, and every terminal in a movable, resizable
window on a desktop. Everything works by mouse or by keyboard, and nothing
has to be memorized before it can be found.

- Repository: [github.com/cklukas/ckmux](https://github.com/cklukas/ckmux)
- Install: [v0.1.1 and other releases](https://github.com/cklukas/ckmux/releases)
  — Debian package and tarballs for Linux (x86_64) and macOS (arm64)
- Built on [ckVision](https://cklukas.github.io/ckVision/), a C++ library for
  full terminal user interfaces

## Documentation

- [Getting started](getting-started.md) — install, first session, detach and
  reattach
- [Command line](cli.md) — every `ckmux` subcommand and environment variable
- [Keys](keys.md) — a default-key appendix generated from the live registry,
  plus copy mode
- [Configuration](configuration.md) — `ckmux.conf`, every key and its default

Inside ckmux itself, **F1** opens the help pages and **`^B ?`** opens the key
reference — the documentation below is the same story for a browser. An
installed build also provides the command-line reference as `man ckmux`.

## How it works

A detached server owns the terminals and their PTYs; it keeps running with no
client attached. A client connects over a local socket, receives a snapshot,
and then receives only what changed. Closing the client — or losing the
connection — does not touch the programs. Reattaching replays the current
state, so you come back to what is there now, not to what you left.
