<!-- Copyright (c) 2026 C. Klukas. All rights reserved. -->

# The Linux test bed

A Debian VM on this Mac, for building, testing and running ckmux and ckVision
on Linux. It exists because a Mac build is blind to a whole class of defects:
glibc marks `write()`/`read()` `warn_unused_result` and Apple's headers do
not; GCC raises `-Wshadow`, `-Wreturn-type`, `-Wmaybe-uninitialized` and
`-Wformat-truncation` where Apple Clang stays quiet; MSVC is a third story.
The first public CI run found eleven ignored `write()` results — three losing
terminal output — after weeks of clean builds here.

## What is installed

| | |
|---|---|
| VM | Lima (`brew install lima`), instance **`ckdev`**, Apple Virtualization (`vz`) |
| Guest | Debian 13 "trixie", aarch64, kernel 6.12 cloud image |
| Toolchain | GCC 13.3 + 14.2, Clang 19.1, CMake 3.31, Ninja, libFuzzer, Python 3.13, glibc 2.41 |
| Resources | 4 CPUs, 6 GiB RAM, 40 GiB sparse disk |
| Where it lives | `/Volumes/PRO-BLADE/lima/ckdev` (`~/.lima` is a symlink there; the download cache is `/Volumes/PRO-BLADE/lima-cache`). Nothing on the system disk but the 78 MB `lima` binary. |
| Source | `/Volumes/PRO-BLADE/git` is mounted **at the same path** inside the VM, writable (virtiofs). Edit on the Mac, build on Linux. |
| Builds | `~/build/<Type>-<compiler>/{ckvision,ckmux}` on the VM's own disk — never on the mount, which is slow for a build's many small writes. |

Definition: [ckdev.yaml](ckdev.yaml). Recreate from scratch with
`limactl delete ckdev && limactl create --name ckdev tools/linux/ckdev.yaml && limactl start ckdev`.

## Build and test from the Mac

```bash
tools/linux/linux-test.sh                 # HEAD of both repos, Release, GCC
tools/linux/linux-test.sh --clang         # Clang instead of GCC
tools/linux/linux-test.sh --debug         # Debug (CI's sanitizer lanes live here)
tools/linux/linux-test.sh --sanitize      # Debug + ASan/UBSan, like CI
tools/linux/linux-test.sh --worktree      # the Mac working trees as they are
tools/linux/linux-test.sh --only ckmux    # one repository
```

Each run prints `ok`/`FAIL` per step — configure, build, ctest, ckVision then
ckmux — with the first diagnostics and a by-warning tally under every failure,
and leaves the full logs in the build directory it names. The exit status is
the number of failing steps. The default is **HEAD**, a fresh clone of each
repository's committed state — the same bytes CI and a stranger's `git clone`
would get — because a gate should judge what is landed, not what is on the
desk. `--worktree` is for iterating on a fix before it is committed.

Optimised configurations set `-D_FORTIFY_SOURCE=2`. Ubuntu's GCC defines it by
default and Debian's does not, and glibc's `warn_unused_result` on `write()`
exists only under fortify — without the flag, the VM could not see the first
defect CI ever found. Measured, not assumed: `(void)::write(fd,&c,1)` compiles
clean here without it and is an error with it.

## Run ckmux or a ckVision example interactively on Linux

Open the VM from **your own terminal emulator** — iTerm2, Terminal.app — not
from a VM console window. The terminal doing the drawing is the Mac's, so
Sixel graphics and mouse reporting work exactly as they do locally, and what
you are testing is ckmux's Linux build of the server, the PTYs, the sockets
and the client.

```bash
limactl shell ckdev                       # a Linux shell, in this terminal
```

```bash
~/build/Release-clang/ckmux/ckmux         # after a --clang run
~/build/Release-clang/ckvision/examples/ckvision_gallery
~/build/Release-clang/ckvision/examples/ckvision_terminal
```

`limactl shell ckdev -- <command>` runs one command and returns. The ckmux
server is per-user and lives under `/run/user/<uid>/ckmux-<uid>/`; `ckmux ls`
and `ckmux kill-server` work as on the Mac.

## Stop, start, remove

```bash
limactl stop ckdev        # frees RAM; the disk and builds stay
limactl start ckdev       # ~10 s to a shell
limactl delete ckdev      # everything but the yaml
```

## Known state (2026-08-23, ckmux v0.1.1 × ckVision v0.1.1)

The port is green, not advisory. The local VM builds clean with GCC 13 Release
and Debug, GCC 14 Release, and Clang Release; GCC 13 ran the full ckmux suite
49/49. The published v0.1.1 snapshot then passed both Linux CI lanes 51/51
(pinned ckVision and ckVision HEAD), and the Linux x86_64 release job built,
tested and packaged successfully. The first-run paragraph this replaces is
kept in git history as the inventory that found the port defects; it is no
longer the current operating state.
