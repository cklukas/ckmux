# ckmux

**ckmux** is a terminal multiplexer — like tmux — with a face. It keeps your
terminal sessions, and the programs running inside them, alive when you
disconnect, and lets you reattach later exactly where you left off.

**Documentation:** [cklukas.github.io/ckmux](https://cklukas.github.io/ckmux/) —
getting started, the command line, keys, and configuration.

Unlike tmux, its interface is visible. There is a permanent menu bar, a
permanent footer showing the keys that work right now, and every terminal
lives in a movable, resizable window on a desktop. Everything works by mouse
or by keyboard, and nothing has to be memorized before it can be found.

```
 Session  Terminal  Window  View  Help
░░╔═[■]═══════════════════ Terminal 1 ═══════════════════[↑]═╗░░░
░░║ $ vim notes.md                                           ║░░░
░░║                          ┌─ ^B … ──────────────────────┐ ║░░░
░░║                          │ c   new term   t   tile     │ ║░░░
░░║                          │ n   next       m   menu     │ ║░░░
░░║                          │ w   windows    d   detach   │ ║░░░
░░║                          └─────────────────────────────┘ ║░░░
░░╚══════════════════════════════════════════════════════════╝░░░
 ^B m menu  ^B c new term  ^B d detach  ^B n next  ^B ? keys
```

Inside each window you can run any terminal program — a shell, vim, htop, mc —
with full color, mouse support, and Sixel graphics.

## How it works

A detached server owns the terminals and their PTYs; it keeps running with no
client attached. A client connects over a local socket, receives a snapshot,
and then receives only what changed. Closing the client — or losing the
connection — does not touch the programs. Reattaching replays the current
state, so you come back to what is there now, not to what you left.

The interface is built on [ckVision](https://github.com/cklukas/ckVision), a
windowed terminal-UI framework, which the two projects develop together.

## Keys

One key is taken from the program you are running: the prefix, `Ctrl+B`. Press
it and a popup shows what the next key does — `c` for a new terminal, `n` and
`p` to move between them, `d` to detach, `?` for the full list. Every command
is also in the menu bar, and every menu entry shows its key, so the popup is a
shortcut rather than the only way in. Press `Ctrl+B` twice to send a literal
`Ctrl+B` through to the program.

Everything else — function keys, Alt combinations, the mouse — belongs to the
program in the window, exactly as it would without ckmux.

## Install

Release packages are available from the
[latest GitHub release](https://github.com/cklukas/ckmux/releases/latest).
The DEB, RPM, and macOS packages currently target Linux x86_64 and macOS
arm64. Starting with v0.1.2, each release also carries a Homebrew formula that
builds ckmux from that exact release source.

On macOS with [Homebrew](https://brew.sh/):

```bash
curl -LO https://github.com/cklukas/ckmux/releases/latest/download/ckmux.rb
brew install --formula ./ckmux.rb
```

On Debian or Ubuntu, download the `.deb` from the latest release and install
it with APT so system dependencies are resolved:

```bash
sudo apt install ./ckmux_*_amd64.deb
```

On Fedora, RHEL, or another RPM-based distribution, download the `.rpm` and
install it with the distribution package manager:

```bash
sudo dnf install ./ckmux-*.x86_64.rpm
```

Every release also includes `.tar.gz` archives for installation without a
package manager. Extract one and copy its `bin`, `share/man`, and
`share/doc/ckmux` contents under the same prefix. Confirm any installation
with `ckmux --version`.

## Build

Needs a C++20 compiler, CMake 3.28+, and a ckVision checkout beside this one
(or an installed ckVision package).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
./build/ckmux
```

`cmake --install build` installs the executable, `ckmux(1)`, and the browser
guide. The key appendix in both installed formats is generated from the same
registry that drives dispatch, menus, the footer, and in-application help.

Run the suite:

```bash
ctest --test-dir build --output-on-failure
```

## Status

Pre-1.0, with **v0.1.1** published for macOS arm64 and Linux x86_64. The core
promise works and is proven by a test that forks a real
server, kills the client mid-run, and shows the program kept going unwatched.
Sessions are plural, named and killable. The interface is real and usable:
menu bar with clock and calendar, footer, floating terminal windows, the
prefix and its popup, scrollback with a frame scrollbar, Sixel graphics, and
themes under `Settings ▸ General…`. Shared and read-only attaches, copy mode,
captured print output, and per-terminal process statistics are built as well.

M3 and M4 are in their acceptance passes. The user guide, installed man page,
generated key appendix, Linux port, licensing, and packaging are complete;
the v0.1.2 packaging update adds RPM and Homebrew delivery to the existing DEB
and platform archives.
The remaining v1 work is M4's three-host/vttest acceptance, the
`ckmux-256color` terminfo it gates, and the final acceptance audit. Expect
rough edges, and expect the interface to move.

## Layout

| Path | Contents |
|---|---|
| `src/common/` | Wire protocol, configuration, keymap, grid deltas — shared by both ends |
| `src/server/` | The detached server: terminals, PTYs, the diff engine |
| `src/client/` | The attaching client: the ckVision interface, commands, copy mode |
| `src/platform/` | The OS seam — sockets, processes, polling, clipboard, paths |
| `doc/` | Templates for the installed man page and generated key appendix |
| `docs/` | The browser guide; its key page is checked against the compiled registry |
| `tests/` | The suite; behavior here lands with a test that fails without it |
| `fuzz/` | libFuzzer targets and corpora for the protocol and configuration decoders |

macOS and Linux are both gating platforms. The v0.1.1 snapshot passed the full
suite on Linux with the pinned ckVision release and with ckVision HEAD, and the
release workflow built, tested and packaged the Linux binary. The local Debian
gate additionally sweeps GCC 13, GCC 14 and Clang. Windows is a design target —
`src/platform` is written against a seam that ConPTY can fill — and no more.

## Provenance

ckmux shares no code with tmux, GNU screen, zellij, dtach/abduco, mtm, Twin,
Turbo Vision, or any port or derivative of them. Its behavior is derived from
published standards — ECMA-48, xterm ctlseqs, the kitty protocol specs,
Unicode UAX #11/#29, terminfo(5), POSIX — from ckVision's own documentation,
and from documented black-box observation of terminals. Contributions are held
to the same rule; see [CONTRIBUTING.md](CONTRIBUTING.md).

## License

[MIT](LICENSE). Copyright (c) 2026 Dr. Christian Klukas.
