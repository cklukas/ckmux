---
title: Command line
---

`ckmux --help` prints this same reference in the terminal.

## Commands

| Command | What it does |
|---|---|
| `ckmux` | Attach: your terminal windows, starting the server if none is listening. Quitting the client leaves the server and every program in it running; run `ckmux` again to return to them. |
| `ckmux ls` | The sessions a server holds, one per line. Says so rather than starting one when none is running. |
| `ckmux new [-s name] [command]` | A session, made without attaching to it. Prints its name; the server names it when you do not. A command runs in its first terminal, through your shell. |
| `ckmux attach [--share] [--adopt-size] <name\|id>` | Straight to one session, past the picker. Takes the name `ckmux ls` prints, or an id. |
| `ckmux kill-session <name\|id>` | Ask every program in one session to end, then drop it. |
| `ckmux check-config` | Parse the configuration and report, starting nothing. Exits non-zero when the file has problems. |
| `ckmux kill-server` | End the server and every terminal in it. |
| `ckmux --help` | The help page (also `-h`, `help`). |
| `ckmux --version` | The build identity, which client and server compare when they meet (also `-V`). |

### `attach` flags

- `--share` — join a session someone may already be watching, rather than
  taking it over.
- `--adopt-size` — make the session's desktop this screen, once, on arrival.
  Reflows every window and resizes every child, for everyone watching.

### Internal

`ckmux --server <socket> [--foreground]` is the detached server. A client
starts one by itself when none answers, so the only reason to type it is
debugging: `--foreground` keeps it in front of you with its diagnostics on
stderr; a detached server writes them to a log beside the socket.

## Environment

| Variable | Meaning |
|---|---|
| `CKMUX_SOCKET` | The server socket, used exactly as given. Default: `$XDG_RUNTIME_DIR/ckmux-<uid>/default.sock`, with `$TMPDIR` and then `/tmp` standing in when there is no runtime directory. |
| `CKMUX_CONFIG` | The configuration file, used exactly as given. Default: `$XDG_CONFIG_HOME/ckmux/ckmux.conf`, ordinarily `~/.config/ckmux/ckmux.conf`. A missing file is the ordinary case: every setting has a default. |
