<!-- Copyright (c) 2026 C. Klukas. All rights reserved. -->
<!-- SPDX-License-Identifier: MIT -->

# Native Windows compatibility

Status: In progress · 2026-10-03 · owner directed

### Latest checkpoint — 2026-10-04

ckVision v0.1.12 is released at 02:25:13Z on 2026-10-04, tag peeling to
public `cfe9fdfd`. GitHub CI `37169919485` completed successfully in all seven
jobs. The ckmux adopter pins 0.1.12. Its actual cold GitHub fetch asserts the
clean tag commit, then passes macOS Apple Clang strict Release 59/59
(236.09 s). Debian 13 ARM64 GCC 13.3 Release passes 59/59 (101.62 s),
Clang 19 Debug ASan/UBSan passes 59/59 (137.85 s), and the native Windows
ARM64 MSVC strict Release shell-only harness passes two CTest entries/eleven
cases (2.40 s). All exits are 0. Frozen app manifest:
`548ee56e20f7223bc8ec4080500b9a481c1c86a779e85b4f9450df3bcd724950`.
These gates precede this record update; a clean committed integration gate
is required before landing. Uncommitted subsequent Windows diagnostic APIs
are not included in the release or these adoption gates.

Mac configure/build ran 02:31:30Z–02:32:09Z, 39 s including five seconds
configuration; the comparable 0.1.11 cold gate used six seconds configuration
and 42 seconds build. Complete fresh compile actions and clean release
source/asset fingerprints are checked, rather than elapsed time being proof.
Output: `/Volumes/PRO-BLADE/tmp/ckmux-windows-20261003-01/mac-ckmux-adopt-012.foVjxB/ckmux`,
SHA256 `1b27df4d5d566987863fc58b05388694bedd0521476755f841a927964165b41c`.
No user-facing application was launched or visually certified.

Readiness/input checkpoint: ckVision `81198549` is landed; filtered public
`cfe9fdfd` is pushed and GitHub CI `37169919485` passed. Native Windows
ARM64 MSVC strict Release passes 211/211 (50.33 s), including ten readiness
cases. Compiled no-rearm and >64-source-drop mutations fail intended assertions;
restoration and full source hashes are checked. Mac clean-commit 222/222
(37.69 s), Linux GCC 13.3 Release 222/222 (23.03 s), Clang 19 Debug
ASan/UBSan plus fuzz 232/232 (126.91 s), GCC 13.3 Debug TSan 221/221
(280.29 s) and all three platform benchmark gates pass, exits 0. Independent
filtered publication passes 216 non-PTY plus four PTY tests and strict docs.
Release 0.1.12 is published; adopter landing awaits the clean commit gate.

Host-selection audit finds another upstream gap: main currently relies on
PosixTerminal-only output-capture, graphics-trace and mouse-report APIs;
WindowsTerminal lacks them and Terminal has no shared diagnostics contract.
U-WIN-HOST-DIAGNOSTICS in [07](07-ckvision-integration.md) must be implemented
upstream, not bypassed here. Complete native app/IPC/acceptance/packages remain
unfinished. Concrete-backend diagnostics parity is being implemented upstream
under D-077/D-127: injected capture and trace, decoder-owned report counts,
normal restoration and record/replay tests. Its first native focused gate
passed capture/count/replay cases but a legacy console viewport resize fixture
failed to observe a resize; a real ConPTY resize fixture now replaces that
assumption and is not yet verified. The records below retain earlier
checkpoints, not current refs.

Adoption gate update (01:26 UTC): the isolated ckmux 0.1.11 pin cold-fetches
the actual GitHub tag at `77356c9d` and builds with zero warnings on Mac.
Linux GCC 13.3 Release passes 59/59 (101.64 s), Clang 19 Debug ASan/UBSan
passes 59/59 (146.64 s), and the native MSVC ARM64 shell-only harness passes
its two CTest entries/eleven cases against a fresh clean `0eb28039` library
build (2.12 s). CTest exits are 0. The released-library Mac sweep is 58/59:
the existing flood prefix-dismissal case failed again at its unchanged
eight-second assertion. This failed gate is not replaced by the earlier
passing candidate record, and adoption is not landed yet.

The failure exposed an upstream mechanism: POSIX `poll()` resolved pending
Escape only on a zero return, so a continuously readable extra socket could
starve it. `InputDecoder::next_timeout_nanos()` also omitted lone Escape.
Two deterministic regressions compile and fail on the original code, then
pass with the isolated upstream correction. Broad Mac decoder/POSIX and
documentation/provenance checks pass; three actual ckmux flood runs against
the correction pass without changing the timeout. Further Linux and native
Windows checks plus full library/release gates remain open. No ckmux fix
or workaround is applied for this library defect.

ckVision `462bf4c9` is landed; its initial public CI `37163490098` passed
Linux, Windows, ASan, UBSan, TSan and fuzz but failed the Mac POSIX terminal
suite on SIGALRM (219/220). Upstream `0eb28039` fixes only that fixture's
pending-alarm teardown and adds a deterministic forked regression. A compiled
discard-removal mutation builds successfully and fails, with the child killed
by SIGALRM. No timeout, skipped coverage or application workaround was added.

The correction passes Mac Apple Clang strict Release 222/222 (38.64 s),
Debian 13 ARM64 GCC 13.3 Release 222/222 (24.39 s), Linux Clang 19 Debug
ASan/UBSan plus fuzz 232/232 (136.46 s), GCC 13.3 Debug TSan 221/221
(262.04 s), native Windows ARM64 MSVC strict Release 210/210 (38.88 s);
all CTest exits 0 at parallel 2, benchmark gates pass. The clean detached
commit additionally passes Mac 222/222 (43.46 s). Public `77356c9d` passes
a separate clean Release build, 216 non-PTY plus four PTY tests and strict
documentation. GitHub CI `37165521471` completed successfully in all seven
jobs. [v0.1.11](https://github.com/cklukas/ckVision/releases/tag/v0.1.11)
was published at 00:57:55Z on 2026-10-04, its tag peeling to `77356c9d`.
The isolated ckmux adopter now pins 0.1.11; released-library and clean-commit
gates are required before landing. Peer progress-widget work was preserved.

A standalone native experiment derived from Microsoft's documented
[wait-packet creation](https://learn.microsoft.com/en-us/windows/win32/devnotes/ntcreatewaitcompletionpacket),
[association](https://learn.microsoft.com/en-us/windows/win32/devnotes/ntassociatewaitcompletionpacket)
and [cancellation](https://learn.microsoft.com/en-us/windows/win32/devnotes/ntcancelwaitcompletionpacket)
passes on Windows 11 25H2 ARM64/MSVC strict Release: 130 independent manual
events (half already signaled, half signaled after association) deliver unique
completion keys; process thread counts remain equal. The console extension
uses its own fresh console: an empty input queue remains unsignaled, then a
written key generates one completion and is read back unchanged. Both native
probe exits are 0. This is feasibility evidence, not a shipped API, a fair
wait loop, cancellation/rearming proof, or full native ckmux acceptance.
The reusable implementation belongs upstream. Its new isolated candidate
passes seven native MSVC ARM64 strict Release cases (one CTest entry,
exit 0, 0.81 s total): 130 simultaneous sources, duplicate/empty sources,
rearming and fairness, 200 cancellation races, invalid-handle recovery,
handle cleanup and the public WindowsTerminal input/wake/extra-source path.
The strengthened candidate additionally passes closed-handle rejection,
two-instance isolation and native process-exit readiness (ten native cases).
This implementation is not committed or released; compiled negative tests
and full library gates remain required. WP-53–WP-57 remain open.

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
meaning. `login-shell` is POSIX-only: interactive Windows shells retain their
native profile behavior. PowerShell starts with `-NoLogo`; a one-shot command
uses `-NoLogo -NoProfile -Command`. cmd starts interactively with no synthetic
argv[0], or uses ckVision's explicit command-processor factory for a command.
Other explicitly configured shells must support `-c` for one-shot commands.
Unicode paths/arguments/environment use Windows wide APIs.
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

The isolated shell-host candidate composes launches through the same common
policy for client-local and server-owned terminals, with fresh native host
observations and injected executable checks. It uses ckVision D-125's command
factory rather than quoting cmd text here. Apple Clang strict Release builds
all targets and passes six selected suites (shell policy, existing shell,
server terminals, client smoke and both architecture checks), exit 0, 3.48 s
at parallel 2. The first regex selected only the architecture checks and is
not shell evidence. Native fixtures additionally exercise changing ComSpec,
Unicode shell paths, interactive input/resize/exit and explicit PowerShell.
Their first compile used a concrete-only exit accessor; it is corrected to
the portable status field. The next run exposed malformed UTF-8 conversion
escaping the path boundary; the adapter now uses ckVision's existing validator.
Native output verification is still under investigation. No Windows application
build or completed WP-53 claim follows from these focused candidates.

The subsequently compiled Mac candidate passes 59/59 strict Release suites,
CTest exit 0, 238.10 s at parallel 2. Later edits touch only the native-only
fixture and this evidence note; this count does not claim those later native
bytes were compiled on Mac. Native interactive cmd, explicit PowerShell and
executable validation pass, but the relocated Unicode/ampersand cmd case
still fails its output assertion despite exit 37. Keeping cmd's installed
MUI resources removes one fixture error but does not make that case green.
System cmd succeeds from the same Unicode working directory. A direct
Windows ProcessStartInfo control succeeds in all four combinations of plain/
ampersand Unicode executable paths and system/relocated ComSpec. This points
the next investigation at the ConPTY/native launch context, not a ckmux
quoting workaround. A subsequent separator control isolates the image-token
spelling: eight mixed/forward-slash launches fail output verification while
their native-backslash counterparts pass with the same commands, cwd and
ComSpec. cmd interprets a slash component as a switch. The correction belongs
upstream: D-125 encodes cmd's image token with native separators and explicitly
supports interactive launches as well as one-shot commands. The adopter now
uses that typed factory in both modes, without path normalization or quoting
in production ckmux. New full library and adopter gates are pending; the
earlier counts above do not certify the revised API. Final library
landing/publication and release-pin adoption remain pending.

Final correction checkpoint (2026-10-04 UTC): ckVision `462bf4c9` is landed
locally and its filtered GitHub snapshot `f609cbe7` is pushed. The final source
manifest `080de577b4b3aaa130aca8d445ee2ec0f38b73600f2268f608474f887df56c73`
passes Apple Clang strict Release 222/222 (72.29 s), Debian 13 ARM64 GCC 13.3
Release 222/222 (32.87 s), Clang 19 Debug ASan/UBSan plus fuzz 232/232
(163.71 s), GCC 13.3 Debug TSan 221/221 (275.00 s), and Windows 11 25H2 ARM64
MSVC strict Release 210/210 (92.89 s). CTest exits 0 at parallel 2;
benchmark gates pass, and modern ConPTY passes 32/32. A compiled mutation
removing native image-token separators fails eight assertions; restored native
cases pass 32/32. The final clean committed Mac gate passes 222/222 (43.56 s).
Filtered publication passes 216 non-PTY and four PTY tests plus strict ckdocs.
That initial GitHub CI `37163490098` later failed the Mac SIGALRM fixture;
the correction and successful replacement CI/release are recorded in the
latest checkpoint above. No tag is moved or existing binary relabeled.

The isolated application shell candidate, manifest
`b918461ee586cc3e9da38b7be49b4ab06fc0c2b6bb8674a61a8b82a0577b89a1`, passes
Mac strict Release 59/59 (192.46 s), Linux GCC 13.3 Release 59/59 (99.11 s)
and Linux Clang 19 Debug ASan/UBSan 59/59 (143.44 s), CTest exits 0 at
parallel 2. A repeat Mac sweep fails the flood row's eight-second prefix-footer
dismissal assertion once. The unchanged binaries then pass three isolated
flood reruns and a full 59/59 repeat (183.87 s). That failed sweep is retained,
the test/deadline is unchanged, and its intermittent cause is not established.
These are provisional candidate measurements against the then-unlanded
dependency; the final paired clean-commit gate is still required.

Native MSVC ARM64 shell-only harness: two CTest entries, eleven actual cases,
exit 0, 3.35 s. It covers configured/default shell policy, malformed paths,
changing ComSpec, relocated Unicode/ampersand cmd images with their installed
MUI resources, both image separator spellings, interactive input/resize/exit
and explicit PowerShell. Source manifest
`7fa41cc2db94a8738b8c2c915913dbc084aa15b94cc62501e32405f4cbf421e7`;
library archive `88c17f19cd1eb61c3a787154b7b5ad1cb864f07d373edf2df015b44ea7255c5e`;
runner `1a48aabb3ed8b418a1d43f740065cd366af39271d552eb2e7c728809955ff96c`.
Build 23:34:05.269Z–23:34:21.618Z, 16.361 s; exact output
`C:/Users/klukas/ckmux-windows-20261003-01/ckmux-shell-native-build/Release/ckmux_shell_contract.exe`.
An earlier evidence-lane rename incorrectly changed the stable harness path;
CMake stopped before compiling, and that failed log is retained. The completed
native probe is not a full ckmux executable or live UI acceptance. Later edits
only correct a PowerShell-profile comment and update these records; final
commit gates must include those bytes. WP-53–WP-57 remain open.

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

At the initial host-service audit, the library's real filesystem adapter was POSIX-only
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
At that checkpoint both portable APIs still returned void: typed reporting
was open, alongside the filesystem and scalable-wait gaps. No ckmux clipboard
workaround was added, and WP-53 is not complete.

### Explicit clipboard outcome candidate

ckVision D-124 now provides typed native/memory acceptance, unacknowledged
terminal submission and explicit refusals, including native error numbers and
possible partial external changes. Application retains the internal copy and
offers an injected result handler for standard widget copies. Record/replay
preserves those outcomes and rejects unscripted/mismatched requests. The
isolated ckmux candidate uses that handler, aggregates host/helper failures
into one configured-copy warning and retains text for terminal paste. Missing
helper bridges count as failures. No Windows clipboard API enters ckmux UI.

Initial upstream API candidate on `478c92ee`: Apple Clang strict Release
222/222 (41.59 s), Debian 13 ARM64 GCC 13.3 Release 222/222 (22.44 s),
Clang 19 Debug ASan/UBSan+fuzz 232/232 (198.05 s), GCC 13.3 Debug TSan
221/221 (361.01 s), native Windows ARM64 MSVC strict Release 210/210
(58.24 s). All CTest exits 0, parallel 2; benchmark gates pass on Mac,
Linux Release and Windows. These precede the version bump and final extra
replay/native diagnostic cases; ckVision 0.1.10 is prepared but not published.

ckmux candidate at `317bd221` plus owned code/tests: Mac strict Release
58/58, exit 0, 265.08 s at parallel 2. Added aggregation/missing-helper cases
then pass 2/2 focused suites, exit 0. The focused two-action rebuild's log
creation/last-write interval is 20:30:03–20:30:06 UTC (second-resolution
metadata), not an instrumented stopwatch. No application was launched.
The full sweep's longer duration is explained by fresh alphabetical CTest
scheduling: the 173.65 s reference suite started later. The prior reused
build scheduled its 168.64 s reference suite first and finished in 191.03 s.
This is a scheduling difference, not a claimed performance regression/fix.

Final 0.1.10 candidate gates, run by ckmux-windows Codex, use source/asset
manifest SHA256 `8421c33c1e84558d5f64baffec9dc4757ac8b02e5d0aec360d8bc0dd6d1192fc`:
Apple Clang strict Release 222/222 (54.99 s), Debian 13 ARM64 GCC 13.3
Release 222/222 (26.82 s), Clang 19 Debug ASan/UBSan+fuzz 232/232
(139.63 s), GCC 13.3 Debug TSan 221/221 (276.77 s), and native Windows 11
25H2 ARM64 MSVC strict Release 210/210. All CTest exits 0 at parallel 2;
Mac, Linux Release and Windows benchmark gates pass. This covers the version
bump and final replay/native diagnostic cases, not just the initial API delta.
The committed library `1965f8fd` is clean inside its detached worktree, matches
that manifest, and passes a further Mac Release 222/222, exit 0 (38.59 s).
It is landed locally and published as filtered GitHub snapshot `c12732ce`:
a separate clean Apple Clang Release build passes 216 non-PTY plus four PTY
tests and a strict documentation-site build. Its clean build took 104 s,
comparable to the prior 102.718 s library clean build; no warning was found.
GitHub CI `37155541830` completed successfully in all seven jobs. v0.1.10
is released at that public commit; annotated tag object `0d1b2b99` peels to
`c12732ce5bb42cc2bb079b9d6e331301a46a8e20`. ckmux's exact pin is now 0.1.10.
Existing v0.1.7 app packages are unchanged and still use 0.1.8.

A subsequent black-box native shell probe against those 0.1.10 library bytes
finds both cmd cases fail with exit 1, including a simple echo builtin, while
two PowerShell cases pass. The cmd error includes the requested marker, so
screen substring matching alone would have falsely accepted it; real exit
status rejects it. Upstream D-125's isolated command-processor candidate now
separates command text from argv escaping. Mac focused rejection/ordinary-argv
tests pass 2/2. Native MSVC strict Release passes three CTest entries (five
cmd cases plus PowerShell and ordinary argv), exit 0, 1.63 s. The test's
deprecated C++20 u8path call was corrected, not warning-suppressed; an earlier
harness configuration failed on a backslash path before compilation. Both
failed logs are retained and are not test evidence. The current native
candidate manifest is `81b4267c847051d82bd093ca1e26a2dd1c39321b1f757ba93f7419635cc87d7b`.
The command fix is not yet landed or certified; ckmux must consume the library
contract rather than compose native quoting or temporary script wrappers.

Final adopter candidate gates at `317bd221` plus the owned clipboard delta,
using those library bytes: Apple Clang strict Release 58/58 (183.47 s),
Debian 13 ARM64 GCC 13.3 Release 58/58 (101.92 s), and Linux Clang 19 Debug
ASan/UBSan 58/58 (137.09 s). All CTest exits 0 at parallel 2. A compiled
library mutation that falsely reports success fails the public widget-copy
and replay cases; restoration passes 4/4 focused suites. A compiled ckmux
mutation that suppresses native-error warnings fails both clipboard suites;
restoration passes 2/2, before the final full 58/58 sweep. No mutant is retained.

The 0.1.10 dependency pin follows its actual release and complete gates.
Full Windows application build, shell/paths/host selection, IPC and
native waits, live acceptance and Windows packages remain outstanding.

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
tests plus a strict documentation-site build. GitHub CI `37136544479`
completed with all seven jobs successful (macOS, Linux GCC and Windows
Release, ASan, UBSan, TSan and fuzz).
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

The temporary-fixture audit found hard-coded `/tmp` constructors in ckVision
and HOME/cwd/reference-app paths here that bypassed exported TMPDIR. The
previous functional counts did not establish workspace-policy compliance.
Library fixtures now use an explicitly configured root with unique scoped
children; the reference instrument uses that root for scripts and sockets.
Application fixtures pin isolated homes/cwd and allocate scoped less/vim/fzf/
Sixel/lazygit data, without deleting a fixed shared repository path. New
scratch-helper regressions check allocation, selected-root placement,
prefix rejection, cleanup and quoting. This is test infrastructure, not a
workaround for the still-missing native library filesystem adapter.

ckmux-windows Codex candidate gates (isolated ckmux `65c2325` plus the owned
fixture delta, against isolated ckVision `987e0b68` plus its owned fixture
delta): Apple Clang strict Release 56/56, CTest exit 0, 235.51 s at parallel 2;
Debian 13 ARM64 GCC 13.3 Release 56/56, exit 0, 99.77 s; Linux Clang 19 Debug
ASan/UBSan 56/56, exit 0, 166.37 s. The last two use parallel 2. Mac focused
scratch/core-promise/pacing suites passed 3/3. No app was launched or visually
certified, and optional absent reference apps remain unexercised.

Mac clean default-dependency build: 16:57:46–16:58:46 UTC, 60 s, 187 Ninja
actions, from configure-log creation to final build-log write (second-resolution
record). The earlier comparable 186-action public-adopter clean build took
38.645 s; this run compiled alongside the library's test rebuild. Every
expected action and input was checked; no compiler warnings or cache launcher.
Linux ckmux rebuilds: Release 17:13:14–17:14:00 UTC, 46 s; Debug ASan/UBSan
17:15:40–17:17:02 UTC, 82 s. A changed aggregate-test compile definition
accounts for the translation-unit rebuilds, rather than a supposed no-work gate.
Exact runnable/test outputs are under
`/Volumes/PRO-BLADE/tmp/ckmux-windows-20261003-01/mac-ckmux-wp53`,
`/home/klukas.guest/ckmux-windows-20261003-01/ckmux-release` and sibling
`ckmux-asan`. Test executable SHA256 respectively:
`aaa84826702a6c14c6665a1bda2b758e86aeefe1a6946c6f764a558c8f2eee8e`,
`11c7e1be895626090adb42fdd7087ff255a1a4e79bc3493fb8cc32b590f9a044`,
`3224af32fc7f89b995948208a96f2c09c234692aea7efec2c62ad48c5639612b`.
The first Mac full application sweep was interrupted at 55 completed suites;
it is not the 56/56 gate above. The completed repeat provides that evidence.
Upstream library fixture gates: Mac/Linux Release 221/221, Linux ASan/UBSan
plus fuzz 231/231, native Windows ARM64 MSVC Release 208/208, exits 0 and
benchmark smoke passes. Final documentation is checked before landing; these
candidate counts do not themselves certify a later commit or Windows ckmux.

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

### WP-53 filesystem checkpoint — 2026-10-03

Upstream private commit `d58d78bf` adds `WindowsFileSystem`, with strict UTF-8
drive/UNC paths, long-path support, handle-bound content revisions, preserved
DACLs and cooperating conditional atomic saves. Native MSVC ARM64 Release
passes 209/209 library suites, including 16 native filesystem cases. Three
compiled mutations (conditional guard, DACL and digest) fail their intended
regressions, then restored bytes pass. A real equal-timestamp same-size write
case exposed timestamp-only revision weakness; the upstream implementation now
streams SHA-256 through the Windows system service. No ckmux workaround.

ckmux configuration saving consumes the injected filesystem read/revision and
conditional-write contract instead of application `getpid`/scratch/`fsync`/
rename code. Portable deterministic cases protect edits and creations arriving
after a read, unreadability and directory failure; native Unicode filenames
exercise the production save/load route. The unconditional-write mutation
compiles and fails the two race regressions; restored Mac cases pass.
The persistent library lock is synchronization state, not an obsolete temp.

The library filesystem snapshot is published as `b493c14c` from private
`d58d78bf`; GitHub CI `37145140682` completed all seven jobs successfully.
It is upstream code on main, not a moved 0.1.9 tag. ckmux-windows Codex
configuration/fixture candidate gates on `65c2325` plus its owned delta:
Apple Clang strict Release 57/57 (189.20 s), Debian 13 ARM64 GCC 13.3
Release 57/57 (101.63 s), and Clang 19 Debug ASan/UBSan 57/57 (139.37 s),
parallel 2, CTest exits 0. The library dependency is isolated `d58d78bf`
plus the owned adopter test-header delta, not the shared dirty library tree.
No empty Windows guarded suite is counted as application coverage.

Native Windows 11 ARM64 MSVC configuration-only contract `/W4 /WX` passes
five cases, including Unicode file save/load and UTF-8 warning paths. Build
18:50:11.509Z–18:50:20.561Z, 9.048 s; CTest 1/1, exit 0, 0.61 s total.
Source manifest SHA256
`74ad23b2c9d7341ec0573230246182aa771f1e3124a9931cfc6c71eaa05ae066`;
library archive SHA256
`8ddd6de3c7491452868f45e955d439a3745120e02aad5069fd522d8ceb75cc3f`.
Output: `C:/Users/klukas/ckmux-windows-20261003-01/config-contract-build/Release/config_filesystem_contract.exe`.
This fixture compiles production configuration code, not a Windows ckmux
executable. Its first strict build exposed C4996 in the library's public
test runner; that header is fixed upstream rather than adding an adopter
warning suppression. Full native app/IPC and live interaction remain open.

Mac configuration/fixture incremental build: 18:57:39Z–18:58:03Z, 24 s,
output `/Volumes/PRO-BLADE/tmp/ckmux-windows-20261003-01/mac-ckmux-wp53`;
test executable SHA256
`5c06fa741e69a51bd3af83a10c01629eb4fc9ce1a25885527daf30836f7f9fe3`.
Linux Release rebuild: 18:58:02Z–18:58:44Z, 42 s; ASan/UBSan rebuild:
19:04:30Z–19:05:45Z, 75 s. Exact outputs remain the guest's `ckmux-release`
and `ckmux-asan` paths above. All owned source/asset hashes were checked
against each gate's input manifest before final record-only edits. These are
incremental candidate runs, not clean-commit gates or a visual certification.

The upstream commit is not a new release yet. ckmux's GitHub dependency pin
remains 0.1.9 until a fully gated new library release exists. Windows host
selection, shell defaults, paths, clipboard errors and full application build
remain WP-53 work; native IPC/waits, live acceptance and packages remain open.

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
