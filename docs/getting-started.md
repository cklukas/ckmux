---
title: Getting started
---

# Your first session

[Install ckmux](installation.md), then open your terminal and run:

```sh
ckmux
```

On Windows, use the [installed executable or portable ZIP](installation.md#windows).
Where this guide says `ckmux`, substitute that executable path if its `bin`
directory is not on your PATH.

ckmux starts its server if needed and opens a desktop with a menu bar, a
shortcut footer, and a terminal running your shell. No configuration is needed.

## Find your way around

**Ctrl+B is the prefix.** Press it, release it, then press the command key.
The popup shows your choices. In the guide and the footer, `^B` means Ctrl+B.

| Shortcut | Action |
|---|---|
| `^B c` | Open another terminal |
| `^B n` / `^B p` | Switch between terminals |
| `^B m` | Open the menu bar |
| `^B w` | Open the window list |
| `^B ?` | Show all key bindings |
| `^B ^B` | Send a literal Ctrl+B to the program |

Click a window to focus it, drag its title to move it, or drag an edge to
resize it. The Window menu also offers tiling and cascading. Use the Help
menu for the in-application guide; the [key reference](keys.md) covers copy
mode, selection, and paste.

## Leave work running

Press **`^B d`** to detach. The client exits; the server and programs keep
running. Run `ckmux` again to return to their current state.

Closing the client or losing your terminal connection also leaves the server
running. This is persistence across client connections: sessions do not survive
a machine reboot or an explicit server shutdown.
Desktop logout and SSH cleanup can also end or disconnect sessions. See
[SSH and session lifetime](session-lifetime.md) for remote use and OS limits.

## Keep separate workspaces

A session is a named desktop with its own terminal windows. One server can
hold several sessions:

```sh
ckmux new -s work       # Create a session without attaching
ckmux ls                # List sessions
ckmux attach work       # Attach to that session
```

From the desktop, use **`^B s`** for the session picker, **`^B S`** to create
a session, and **`^B R`** to rename one.

## Join or watch a session

From another terminal under the same OS account:

```sh
ckmux attach --share work   # Join with keyboard and mouse control
ckmux attach --watch work   # Join read-only
```

Plain `ckmux attach work` takes the session over, sending other attached
clients back to their pickers. Sharing is for clients within the same account's
local trust boundary, not a multi-user access-control system.

In the Session menu, **Watch Only** changes your own mode. Controlling readers
can use **Others Read-Only** and **Take Session Over**; a watcher can leave
Watch Only to regain control. There is no privileged session owner.

## Close what you mean to close

| Action | Result |
|---|---|
| Detach (`^B d`) or quit the client (`^B q`) | Leave the server and programs running |
| `ckmux kill-session work` | End that session and its programs |
| `ckmux kill-server` | End all sessions and stop the server |

To change your shell, theme, or prefix, open **Settings ▸ General…** or read
[Configuration](configuration.md). For flags and environment variables, see
the [command-line reference](cli.md).
