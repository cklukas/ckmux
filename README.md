# ckmux

**Persistent terminals. A desktop you can see.**

Keep shells, editors, and long-running commands alive after you disconnect.
Come back to a desktop of movable terminal windows, with menus you can browse
and a footer that shows the keys available right now.

[Get started](docs/getting-started.md) · [Install](docs/installation.md) ·
[User guide](https://cklukas.github.io/ckmux/) ·
[Downloads](https://github.com/cklukas/ckmux/releases/latest) ·
[Changes](CHANGES.md)

```text
 Session  Terminal  Window  View  Settings  Help
 ┌─ Terminal 1 ────────────────────────┐
 │ $ make                            │
 │ Building…       ┌─ Terminal 2 ─────────────────────┐
 │                 │ $ vim notes.md                  │
 └─────────────────│                                 │
                   │                                 │
                   └─────────────────────────────────┘
 ^B m menu   ^B c new terminal   ^B d detach   ^B ? keys
```

*An illustration of the desktop layout.* ckmux runs inside your terminal
emulator, on macOS, Linux, and native Windows.

## Why ckmux?

- **Leave work running.** Detach or close the client; the server keeps your
  sessions and programs alive. Reattach to their current state.
- **Find commands as you go.** Menus, mouse interaction, and a prefix-key
  popup make the interface usable before you learn the shortcuts.
- **Arrange your workspace.** Move, resize, tile, or cascade terminal windows;
  keep separate named sessions for different projects.
- **Share a view.** Join a session from another client, or watch it read-only.
- **Keep terminal features.** Color, mouse input, scrollback, copy mode, and
  Sixel graphics on capable hosts, plus captured print output and process stats.

## Install

| Platform | Installation |
|---|---|
| macOS | `brew install cklukas/ckmux/ckmux`; Apple Silicon archive for macOS 26+ also available |
| Linux x86_64 — Debian / Ubuntu | Download the DEB, then `sudo apt install ./ckmux_*_amd64.deb` |
| Linux x86_64 — RPM distributions | Download the RPM, then `sudo dnf install ./ckmux-*.x86_64.rpm` |
| Windows x64 / ARM64 | Download the matching per-user MSI or portable ZIP; run in Windows Terminal |

Use the [latest release](https://github.com/cklukas/ckmux/releases/latest).
The [installation guide](docs/installation.md) covers platform requirements,
checksums, archive installation, upgrades, and removal. For other architectures
or development, see [building from source](docs/building.md).

## Your first minute

Start ckmux from a terminal:

```sh
ckmux
```

On Windows, use the installed executable or `bin\ckmux.exe` from the extracted
ZIP; see the [Windows launch instructions](docs/installation.md#windows).

Press **Ctrl+B**, release it, then press the command key:

| Key after Ctrl+B | Action |
|---|---|
| `c` | Open a terminal window |
| `n` / `p` | Switch to the next / previous terminal |
| `m` | Open the menu bar |
| `d` | Detach and leave programs running |
| `?` | Show the key reference |

Run `ckmux` again to return. Press Ctrl+B twice to send a literal Ctrl+B to
the program. No configuration file is required.

## Learn more

| I want to… | Read |
|---|---|
| Create sessions, detach, reattach, or share | [Getting started](docs/getting-started.md) |
| Use the command line | [CLI reference](docs/cli.md) |
| Find a shortcut or copy text | [Keys and copy mode](docs/keys.md) |
| Choose a shell, theme, or key binding | [Configuration](docs/configuration.md) |
| Understand Windows runtime and testing requirements | [Native Windows](docs/windows.md) |
| Build, test, or contribute | [Build guide](docs/building.md) · [Contribution terms](CONTRIBUTING.md) |

ckmux is pre-1.0: expect interface and protocol changes. Native Windows packages
are published and automated installation/lifecycle checks pass; the manual
Windows Terminal and installer walkthrough remains pending. Sixel requires a
capable outer terminal. Sessions survive client disconnection, not a reboot or
server shutdown.

## Under the hood

A detached server owns the shells, terminal state, scrollback, and layout.
Clients connect locally over a Unix socket or Windows named pipe, receive the
current state, and render subsequent updates. The interface and terminal stack
use [ckVision](https://github.com/cklukas/ckVision), a C++20 terminal UI library.

ckmux is independently implemented from published standards and documented
terminal behavior; it shares no code with other terminal multiplexers.
Contributions follow the [provenance rules](CONTRIBUTING.md#provenance-binding--read-before-writing-a-line).

[MIT License](LICENSE) · Copyright (c) 2026 Dr. Christian Klukas.
