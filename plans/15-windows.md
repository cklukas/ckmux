<!-- Copyright (c) 2026 C. Klukas. All rights reserved. -->
<!-- SPDX-License-Identifier: MIT -->

# Native Windows compatibility

Status: In progress · 2026-10-03 · owner directed

Windows leaves the post-v1 parking lot at the owner's request. This port
preserves the complete ckmux client/server model: closing a client leaves
its programs running; reattachment restores current cells, history, images,
and layout. Native PowerShell and cmd sessions are supported. WSL does not
substitute for the native port. macOS and Linux remain required gates.

## Architecture

The protocol, session model, diff engine and ckVision desktop stay shared.
ckVision's latest committed library supplies `WindowsTerminal`,
`WindowsClock`, and `launch_terminal_subsession()` (ConPTY on Windows).
Missing portable child lifecycle APIs belong upstream. Use the common
subsession interface, including selective snapshots and nonblocking
termination/escalation, without casting to platform implementations.

Local IPC uses byte-mode overlapped named pipes with a protected ACL for the
current user and SYSTEM, remote clients refused, and the client's token
checked against the server's user. Names and the startup mutex are scoped
to the user's SID. No public TCP transport is introduced. Discovery,
locking, error classification, bounded queues and backpressure retain the
Unix transport's contracts. Integer file descriptors must not carry Win32
HANDLEs; readiness sources and connection identity are separate values.

Windows waits combine IPC completion events, ConPTY completion/process
events, console input/wake events and deadlines. The loop owns all state;
use overlapped I/O, no persistent transport worker threads. More than 64
native sources must work without invalid waits or unfairness. ckVision's
owned short-lived pseudoconsole close thread remains its responsibility.

Start the server as a detached native process with explicit handle
inheritance and the same executable as the client. Console control events
request orderly server shutdown; client console close must not kill the
server. Child job objects and ConPTY teardown remain ckVision-owned.
Termination is courteous first and escalates at the reader's configured
deadline; force-kill reaches the whole child job, not only its root PID.

Default shell selection uses a valid configured executable, then ComSpec,
then the system cmd.exe. PowerShell command arguments and cmd command
arguments are explicit and distinct; POSIX login argv[0] has no Windows
meaning. Unicode paths/arguments/environment use Windows wide APIs.
Configuration and durable logs live under the user's LocalAppData, with
documented overrides. Native clipboard, file saving and process statistics
must expose environmental failures rather than quietly losing work.

## Work packages and acceptance

| Package | Deliverable and evidence |
|---|---|
| WP-52 | Latest ckVision portable subsession ownership, selective snapshots, nonblocking graceful termination and job escalation; native regression tests prove a stubborn child stays alive during grace, another child still answers, then the requested child exits. |
| WP-53 | Windows build, shell, paths, filesystem and host terminal selection; strict MSVC build, Unicode path/argument cases, default cmd and explicit PowerShell launch, input/resize/exit behavior. |
| WP-54 | Secure named-pipe IPC, startup race, detached process, generalized wait loop and shutdown; real concurrent starters, isolation, malformed input, queue saturation and more than 64 readiness sources. |
| WP-55 | Complete Windows native acceptance: two sessions and clients, shared/watch/takeover, client termination with surviving children, reattach history/current screen/layout, respawn and graceful/forced descendant teardown, native stats, clipboard/copy/save/printing, environment changes during resize/capability negotiation, contained Sixel plus honest no-graphics fallback, live keyboard/menu/mouse/window interaction in Windows Terminal. |
| WP-56 | Blocking Windows x64 and ARM64 CI, supported sanitizers, install-tested ZIP and installer release artifacts, hashes, dependency/runtime policy and user instructions. Use a newly released ckVision pin after its full gates. |
| WP-57 | Final audit against every row above, native named-host walkthrough, isolated commit gates on all three OSes, package installation/launch and published artifact verification. |

Existing Unix-only suites are retained. Port their common assertions or add
equivalent Windows suites with a checked test inventory: an empty guarded
suite is not Windows coverage. Real events and real child processes prove
public routes; fixtures alone do not prove the user's running terminal.

## Current evidence and remaining work

Initial audit at ckmux `e1bc8ae` and ckVision `6e568df3`: ckmux has no
Windows backend and rejects non-UNIX CMake configurations. ckVision has
working Windows outer-terminal and child adapters, but request_termination
and request_kill are POSIX-only, and selective snapshots are not exposed
through the portable interface. The Windows 11 ARM64 VM is reachable.
WP-52 implementation is verified against ckVision `84c96695` (0.1.9):
ckmux uses portable ownership/factory/lifecycle and its mirror implements
selected snapshots, including images in full snapshots. The library's
compiled no-op escalation mutation fails; restored native tests pass.
Windows ckmux build, IPC, interactive acceptance and packages remain open.

ckVision v0.1.9 is published at public `aea4754a`; GitHub CI `37126856749`
passed all seven jobs (Windows/macOS/Linux Release, ASan, UBSan, TSan and
fuzz). ckmux's exact release pin is now 0.1.9. Its existing v0.1.7 binary
packages still use 0.1.8; changing main is not relabeling those packages.

The latest library's real filesystem adapter is also POSIX-only
(`term::PosixFileSystem`). WP-53 must add a native Windows implementation of
the existing injected `FileSystem` contract upstream, including Unicode
paths, revision-consistent reads and conditional atomic publication. Do not
implement those reusable operations in ckmux to compensate for the gap.

The former Windows clipboard backend used `OpenClipboard(nullptr)` then
`EmptyClipboard()`, contrary to Microsoft's published
[ownership contract](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-openclipboard).
Upstream `987e0b68` adds `WindowsClipboardWriter`, used by `WindowsTerminal`:
each write owns an invisible publication window, verifies ownership, publishes
immediate Unicode data and destroys the window before returning. This leaves
no lingering unpumped clipboard owner in a terminal application's native wait.
Windows 11 25H2 ARM64 nevertheless accepts the old null-owner sequence; data
loss was **not** reproduced on that host. A compiled null-owner mutation with
the new ownership check retained fails three native round-trip assertions.
Both portable APIs still return void: typed export-failure reporting remains
open, alongside the filesystem and scalable-wait gaps. No ckmux clipboard
workaround was added, and WP-53 is not complete.

Clipboard candidate gates (ckmux-windows Codex, isolated ckVision `84c96695`
plus only the subsequently landed owned delta): macOS Apple Clang Release
219/219, Debian 13 ARM64 GCC 13.3 Release/FORTIFY 219/219, Linux Clang 19 Debug
ASan/UBSan plus fuzz 229/229, and native Windows ARM64 MSVC Release `/W4 /WX`
206/206; all CTest exits 0. Modern ConPTY additionally passed 24/24; benchmark
smoke passed on all three hosts. The four native clipboard cases use their own
fresh window station, never the user's desktop clipboard. These are upstream
library gates, not a Windows ckmux executable or user-facing acceptance gate.
The new upstream commit is unreleased; the ckmux GitHub release pin remains
0.1.9 until the next fully gated library release.

The filtered upstream GitHub snapshot is now `b3b304b3` (private source
`987e0b68`), verified before push with 213 non-PTY and four PTY macOS Release
tests plus a strict documentation-site build. GitHub CI `37136544479` was
running at this checkpoint; it is not an attributed hosted-green claim.
Publication preparation/verification ran 16:18:44–16:21:21 UTC, 157 s. Its
temporary verification tree was removed by the publication tool. The wrapper
record retained the task-owned work root but not the tool-generated exact leaf
path; that freshness-record limitation is not retroactively filled in. The
retained native/Mac/Linux clipboard gates above have exact recorded outputs.
The old task-owned `mac-ckvision-commit` build and clean `ckvision-commit`
checkout were removed after validation; current `mac-ckvision-wp53`, native
Release and Linux configurations are retained for WP-53. No peer workspace was
removed. Future ckmux Windows code consumes the current upstream source during
co-development, then the newly released tag; this checkpoint does not move the
existing v0.1.9 tag or relabel ckmux v0.1.7 packages.

### WP-52 candidate gates

Run by ckmux-windows Codex, isolated at ckmux `e1bc8ae` plus its five-file
code/test delta, against the owned ckVision lifecycle delta on `6e568df3`:
macOS Apple Clang Release 54/54 (370.64 s), Debian 13 ARM64 GCC 13.3 Release
54/54 (200.83 s), and Linux Clang 19 Debug ASan/UBSan 54/54 (296.53 s), all
CTest exits 0. These are candidate-byte gates, not a claim about a future
published commit. The Mac executable at
`/Volumes/PRO-BLADE/tmp/ckmux-windows-20261003-01/mac-ckmux/ckmux` has SHA256
`953e2e47cadec307a546c3dc59cdea098fa80bfc0e0436e63206544832409e3f`.

Upstream: strict macOS Release 219/219, Linux GCC 13.3 Release 219/219,
Clang 19 Debug ASan/UBSan plus fuzz corpus 229/229, GCC 13.3 Debug TSan
218/218, and Windows 11 ARM64 MSVC Release 205/205 plus 24 modern ConPTY
cases. All exits 0 and benchmark smoke green. Exact native fingerprints,
timings, mutation and inventory scope are recorded in ckVision's portable
lifecycle validation document. A clean detached checkout of the landed
ckVision `84c96695` then passed macOS Release 219/219 (18.74 s, parallel 8);
clean build 102.718 s / 484 Ninja actions, source/asset manifest verified.
Output: `/Volumes/PRO-BLADE/tmp/ckmux-windows-20261003-01/mac-ckvision-commit/tests/cvision_tests`,
SHA256 `47d71c409f297aed8614b05be23a79fa57970fdac53a8c44b7b3470fe6d2b5b3`.

The filtered ckVision public snapshot `aea4754a` independently passed 213
non-PTY and 4 PTY tests in Release, zero-warning build and `ckdocs --strict`.
This smaller inventory omits private planning gates; it is not called 219/219.
ckmux's adopter-mode build (source preference OFF, FetchContent source
override pinned to clean public `aea4754a`, requested version 0.1.9) then
passed 54/54 Release, CTest exit 0, 365.73 s. Clean build: 13:45:01Z to
13:45:42Z, 38.645 s / 186 Ninja actions; no warnings. The exact output at
`/Volumes/PRO-BLADE/tmp/ckmux-windows-20261003-01/mac-ckmux-public/ckmux`
has SHA256 `f8cab40ba95152a182dfb7f30bfc7b2c52bd0cdcb9aabf3d14ccb138e0ad7888`
and reports ckmux 0.1.7. This source override proves the public library's
bytes work; it does not prove that a not-yet-published tag can be fetched.
No native Windows ckmux executable has been built or visually certified.

The clean pinned integration gate at ckmux `7dcaaab` × ckVision `84c96695`
passed macOS Release 54/54, CTest exit 0. Build start/end 14:05:54Z to
14:06:25Z, 29.371 s / 186 Ninja actions; output
`/Volumes/PRO-BLADE/tmp/ckmux-windows-20261003-01/mac-ckmux-commit/ckmux`,
SHA256 `5b90fa8ef273bcefed1396cde22475c4b6c44e9e598a3f2d5c7634f65218a893`.

### Exact installed-release selection

The adopter pin said "exact" but its FetchContent `FIND_PACKAGE_ARGS` omitted
`EXACT`. ckVision's SameMajorVersion package metadata could therefore select
a later installed library. The lookup now requests EXACT. The registered
`ckvision_release_pin` test reads the actual root declaration and executes
its lookup arguments against real CMake package/version fixtures: the exact
release must be selected, both older and later releases must be refused.
Running it against `7dcaaab`'s original declaration fails with exit 1 because
0.1.10 bypasses the 0.1.9 pin; the new declaration passes on macOS, Linux and
native Windows CMake. This is ckmux dependency policy, not a library workaround.

The isolated pin delta on clean `7dcaaab` × `84c96695` passed macOS Release
55/55 (parallel 2, 183.73 s), exit 0. Linux GCC 13.3 Release passed 55/55
(parallel 2, 101.61 s), and Clang 19 Debug ASan/UBSan passed 55/55
(parallel 2, 181.90 s), all exits 0. The extra test is a script: the Mac
reconfiguration/build reported no compile work, and its production executable
hash above is unchanged. A short Mac build here is expected and proven by
unchanged compiled inputs/output, not interpreted as a new clean build.
Linux reconfiguration rebuilt the 56-action GCC and 112-action Clang ckmux
object graphs (including Clang dependency scans), rather than called no-work.
The root pin also generates `CKMUX_CKVISION_PIN_STRING` in
`generated/common/version.hpp`; changing the guest's former 0.1.8 pin to
0.1.9 invalidated its dependent translation units. The generated header and
actual compile actions were checked. The Mac gate already had that pin.
Release build: 14:17:52Z–14:18:26Z; ASan/UBSan build:
14:20:08Z–14:20:49Z. These incremental times are not clean-build baselines.

### Published source gate

Private `c677676` was filtered to public `db46e1d`. A clean detached
checkout of that public commit configured in adopter mode with source
preference OFF and no FetchContent source override. The actual cold GitHub
fetch of `v0.1.9` resolved to `aea4754a`, asserted clean inside its checkout.
macOS Release passed all 55/55 tests (parallel 2, 253.34 s), CTest exit 0,
zero warnings. Both source checkouts remained clean. The result is published
on ckmux GitHub main; existing release packages are unchanged.

Build record: 14:33:36Z–14:35:44Z, configure/fetch 11.836 s, build 116.033 s,
186 Ninja actions including 176 compiled translation units. This was longer
than the recent 29.371 s private and 38.645 s source-override clean builds.
The outlier was checked: identical Apple Clang executable, Release -O3 and
strict flags, no compiler launcher/cache, complete fresh compile actions,
exact fetched tag identity, clean source/asset trees and successful full
tests. Aggregate compiler user CPU was 248 s over 116 s wall, versus 193 s
over 29 s in the private build; effective parallelism differed. The specific
host-load cause was not measured and is not attributed to another task.
Elapsed time is not the freshness proof.

Verified output:
`/Volumes/PRO-BLADE/tmp/ckmux-windows-20261003-01/mac-ckmux-cold-fetch/ckmux`,
SHA256 `10607a3f79fab0d1172c646495176a97c198ae55cd7cf5bbb689aaa6499d8d5d`,
reports ckmux 0.1.7. This is the current verified build, not a Windows binary.

## Verification instrument repairs during WP-52

The first transferred archive included AppleDouble metadata sidecars. Linux
script scans treated these binary `._*` files as source. The transfer was
corrected to exclude Mac metadata; task-owned sidecars were identified and
removed, then the full gates were repeated. This was a transfer failure, not
a library regression.

A version/golden overlay preserved Mac source mtimes older than Windows'
already compiled version-test object. MSBuild relinked against 0.1.9 but
retained a test expecting 0.1.8. Source hashes alone did not prove compiled
input freshness. The changed test inputs were explicitly made newer, the
compile actions checked, and the native full suite repeated.

Editing a driver script while Bash was blocked inside one of its functions
changed offsets in the unread tail; the driver later attempted a fragment
as a command. Driver files are now frozen per invocation. Actual gate exit
codes are captured separately; a failing wrapper is never reported as a
green aggregate run. Windows PowerShell's Stop policy also treated expected
native stderr as an exception during a negative test; the negative runner
now captures the child's nonzero exit explicitly before restoring/re-gating.

A Lima/SSH invocation inside a stdin-fed Bash driver consumed the driver's
unread tail. The wrapper exited 0 without creating the later Mac gate logs.
Their absence caught the skipped gate; no test verdict was claimed. Gate
drivers are now frozen files, remote commands receive `/dev/null` stdin, and
actual CTest logs/exits are checked. The standalone Windows CMake-policy
driver initially passed native backslashes to CMake 3.31's eval-based version
helper; it now passes canonical CMake paths, as real CTest configuration does.
The unchanged regression then passed; this was a driver-input repair.
