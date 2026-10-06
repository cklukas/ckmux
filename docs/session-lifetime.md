---
title: SSH and session lifetime
---

# Keep work running over SSH

Install and run ckmux **on the machine doing the work**. Your local terminal
displays the remote client through SSH; the remote ckmux server owns the
shells, programs, terminal history, and workspace.

```sh
ssh -t user@host
ckmux new -s work
ckmux attach work
```

Press **Ctrl+B, d** to detach, then exit the remote shell. To return:

```sh
ssh -t user@host
ckmux ls
ckmux attach work
```

Use the same remote account and server endpoint each time. If ckmux is on the
remote PATH, `ssh -t user@host ckmux attach work` combines the two steps.
The `-t` allocates a terminal for the interactive client; `ckmux new` and
`ckmux ls` do not need one. On Windows hosts, use the installed `ckmux.exe`
path when it is not on PATH.

Running ckmux locally and then starting SSH *inside* it preserves the local
SSH process and its screen. It cannot preserve remote programs if that SSH
connection ends. For remote persistence, run ckmux after logging into the
remote host.

## Disconnecting is different from signing out

Closing a client disconnects its display and input. It does not ask ckmux to
stop the server or its programs. An operating system can still terminate the
server when it ends a login session. Detaching is not an exemption from that
policy.

| Event | What to expect |
|---|---|
| Detach or close the ckmux client | Programs remain in the server |
| SSH connection drops | Programs can continue if the host keeps the server and its endpoint available |
| Lock the desktop | The login remains active; sleep can suspend execution |
| Sign out of the desktop | Depends on the OS and the context that started the server; see below |
| Reboot, shut down, or stop the ckmux server | Running sessions end |

## Windows

A server started from Windows Terminal belongs to that desktop login.
**Signing out ends that server and its programs.** ckmux currently does not
install a Windows service to host sessions independently of the desktop.

Windows OpenSSH can start processes in a different context. In the tested
OpenSSH service configuration, the ckmux server ran in Windows session 0,
survived SSH disconnect, and remained reachable on a fresh connection using
the same account and pipe label. Do not infer this behavior for every SSH
server or containment policy.

A launcher whose Windows job forbids independent processes causes ckmux
startup to fail with a diagnostic. A successful `--server --foreground`
process is not a substitute for testing normal automatic server startup.

## Linux

Linux login cleanup is configurable. On systemd hosts, `KillUserProcesses`
can end detached programs when a login session closes. Moving a server into
a systemd user service and enabling user lingering can give it a lifetime
outside individual logins; lingering alone does not move an already-running
server out of its login session. ckmux does not install such a unit.

The tested Debian 13 host kept both the original processes and their default
socket after SSH logout and LightDM/Openbox desktop logout. Its effective
`KillUserProcesses` setting was `no`, with lingering disabled. This result is
specific to that configuration, not a guarantee for every Linux desktop.

There is a second dependency: the socket. The default lives under
`$XDG_RUNTIME_DIR` when it is set. The system can remove that directory at the
last logout, even if a detached process is still alive. A fresh client then
cannot reach the old server. For environments that keep detached processes
but remove the runtime directory, choose a private persistent socket before
starting the server and use it on every connection:

```sh
mkdir -p "$HOME/.local/state/ckmux"
chmod 700 "$HOME/.local/state/ckmux"
export CKMUX_SOCKET="$HOME/.local/state/ckmux/default.sock"
ckmux new -s work
```

Keep the Unix socket path short enough for the OS limit. A persistent socket
path does not prevent an OS policy from terminating the server.

## macOS

ckmux detaches its server from the launching terminal using a double fork
and `setsid()`. That establishes terminal independence; it does not by itself
prove survival through graphical desktop logout. Treat desktop logout
survival as unverified until tested on the macOS version and launch context
you use. A user LaunchAgent should not be confused with a system LaunchDaemon.

## Verified coverage

These checks use ckmux 0.1.8 with ckVision 0.1.18. A worker writes its PID and
an increasing counter once per second. The checks compare process identity,
counter progress, and the ability to reconnect to the original session.

| Host and launch context | Check | Result |
|---|---|---|
| Debian 13.6 ARM64, OpenSSH, disposable user; `KillUserProcesses=no`, `Linger=no` | Last SSH login ends; fresh SSH login and attach; abrupt SSH transport termination while attached | Original server and worker survive; default and explicit socket endpoints remain reachable |
| Same Debian host, LightDM/Openbox X11 desktop using Xvfb | Openbox logout returns LightDM to its greeter; reconnect over SSH | Original server and worker survive; desktop session is closing and the original workspace is reachable |
| Windows 11 Pro ARM64, build 26200, native OpenSSH service | SSH connection ends; abrupt SSH transport termination while attached; fresh SSH attach | Original server and worker survive in Windows session 0 |
| Same Windows VM, Windows Terminal in an interactive desktop login | Sign out that desktop login | Desktop server and worker terminate; the worker counter stops |
| macOS | SSH disconnect and graphical desktop logout | **Not verified**: the available disposable VM requires login access and has an older OS than the release binary supports |

The Linux desktop check uses a real LightDM/PAM login and normal Openbox
session exit with a virtual X display; it is not GNOME or KDE coverage. The
Windows desktop and SSH fixtures use different test accounts; their results
do not establish same-account desktop-to-SSH handover. Native Windows x64
and macOS logout behavior have not been measured by this check.

## Check your host

Use a disposable account or VM for desktop logout tests. Start a named
session with a program that records its PID and increments a counter in a
file. Record the server PID too. Then:

1. Detach, close SSH, and reconnect with the same account and endpoint.
2. Repeat while attached, abruptly closing the SSH connection.
3. Separately, start a session from the graphical desktop and sign out of
   that desktop. Observe from a different account or management connection.
4. Check that the original PIDs still exist, the counter advanced while
   disconnected, and `ckmux attach` reaches the original session.

Wait long enough for delayed login cleanup to run. A new server, a recreated
session, or a surviving PID with an unreachable socket is not a successful
reattachment test. Keep desktop logout, SSH disconnect, and reboot results
separate.

## Platform references

- [Windows logoff process termination](https://learn.microsoft.com/en-us/windows/win32/shutdown/logging-off)
- [systemd login cleanup](https://www.freedesktop.org/software/systemd/man/latest/logind.conf.html)
- [systemd user lingering](https://www.freedesktop.org/software/systemd/man/latest/loginctl.html)
- [Apple login-session lifetimes](https://developer.apple.com/library/archive/documentation/MacOSX/Conceptual/BPMultipleUsers/Concepts/SystemContexts.html)

For endpoint overrides and command syntax, see [Command line](cli.md).
