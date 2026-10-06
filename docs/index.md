---
title: ckmux
---

# A desktop for your terminal sessions

ckmux keeps your shells and programs running when you disconnect. Reattach to
a workspace of movable terminal windows, with a menu bar, mouse support, and
a footer showing the keys you can use right now.

## Start here

1. [Install ckmux](installation.md) on macOS, Linux, or Windows.
2. [Open your first session](getting-started.md), add a terminal, and try
   detaching and reattaching.
3. [Make it yours](configuration.md): choose a shell, theme, and shortcuts.

## Find an answer

| Task | Guide |
|---|---|
| Download, install, upgrade, or remove ckmux | [Installation](installation.md) |
| Manage sessions or join from another client | [Getting started](getting-started.md) |
| Use commands and environment variables | [Command line](cli.md) |
| Navigate, copy, and paste | [Keys](keys.md) |
| Configure the shell, display, clipboard, and printing | [Configuration](configuration.md) |
| Build and test the source | [Building from source](building.md) |
| Understand native Windows runtime and packaging | [Native Windows](windows.md) |

Inside ckmux, **Ctrl+B, ?** opens the key reference. The Help menu opens the
in-application guide. Unix installations also include `man ckmux`.

## Platforms and limits

Packages cover macOS Apple Silicon, Linux x86_64, and native Windows x64/ARM64.
ckmux is pre-1.0; its interface and protocol may change. Windows automated
installation and lifecycle checks pass, while the manual Windows Terminal and
installer walkthrough remains pending. Graphics require a Sixel-capable host.

A detached server owns each session. Closing a client leaves the server and
its programs running; rebooting or stopping the server ends them. Connections
are local, through a Unix socket or Windows named pipe. For remote work, run
ckmux on the remote machine through your usual terminal connection.

[Source code](https://github.com/cklukas/ckmux) ·
[Downloads](https://github.com/cklukas/ckmux/releases/latest) ·
[Release history](https://github.com/cklukas/ckmux/blob/main/CHANGES.md) ·
[ckVision](https://cklukas.github.io/ckVision/)
