<!-- Copyright (c) 2026 C. Klukas. All rights reserved. -->
<!-- SPDX-License-Identifier: MIT -->

# Native Windows compatibility

Status: In progress · 2026-10-03 · owner directed

### Paste-credit native coverage candidate — 2026-10-05

The final coverage audit found that all twelve existing paste-credit cases
were excluded on Windows, including the platform-independent client credit
rules. The candidate removes that whole-suite guard, uses the existing portable
server scratch/stream helpers and a real native idle ConPTY child, and retains
long-paste ordering, independent per-terminal credit, takeover/disconnect
cleanup, unknown-terminal acknowledgement, competing-client serialization
and mid-paste disconnect recovery assertions. Readiness waits observe actual
messages within bounded deadlines rather than counting tight asynchronous
polls. Native inventory becomes54, explicitly required by CI and release.
Strict native execution, a compiled credit-omission negative and final
cross-platform integration remain required; this is not completion evidence.

The corrected twelve-case paste candidate passes strict MSVC ARM64 and
x64-on-ARM64 execution. A compiled production credit-limit omission fails
the intended long-paste assertion; exact restoration/rebuild passes all twelve
again. The first native fixture requested its second connection before the
first was accepted/rearmed; greeting the first then connecting the second
retains two live competing readers without changing production transport.

A separate native reader case fills the actual server-owned child with
sustained output while two real named-pipe peers request paired Pongs. One
decodes actual attached deltas into the production mirror, the other remains
unattached, and the original client must still open and dismiss its window
menu. Native execution and a compiled missing-reply negative remain required.

### Native clipboard and current integration checkpoint — 2026-10-05

Isolated test candidate `caeb3394` drives actual ckmux executable copy-mode
entry, search and yank with a child-generated Unicode line. It reads real
CF_UNICODETEXT, then holds the clipboard open while the actual client tries
another copy: the warning must be visible, prior external content preserved,
internal paste must reach the real child, and a new copy after unlock must
publish the exact Unicode text. No clipboard adapter is injected into ckmux.
Both MSVC ARM64 and x64 strict Release builds pass the context control and
both actual copy cases, as do the matching downloaded hosted `d0c9a920` ZIPs
in `native-copy.ce82cbae`. Two-case times are11.21–11.65s. This native host is
Windows11 ARM64; x64 is emulated, not a native AMD64 desktop walkthrough.

These tests never use the interactive clipboard. Before any clipboard write,
the caller must have a non-visible system-created service station whose name
matches its token's AuthenticationId. Both actual client/server tokens must
match that noninteractive logon identity. The non-inheriting ckVision launch
sets no explicit desktop, so Windows selects that logon's default station.
After real copy, native handle inventories independently identify the actual
client's matching service station. Failure of any guard fails the case;
there is no interactive fallback or skip. CI and release explicitly select
the existing same-user limited noninteractive scheduled test host.
See [Microsoft's process connection rules](https://learn.microsoft.com/en-us/windows/win32/winstation/process-connection-to-a-window-station)
and [window-station clipboard ownership](https://learn.microsoft.com/en-us/windows/win32/winstation/window-stations).

Initial candidate `7f26345` assumed the complete caption including its dash
was visible and failed before yank. The corrected `0cad90d` uses the actual
COPY-plus-space status, not the child's NATIVE-COPY text. It passes the
actual Unicode/refusal/internal-paste/recovery assertions; its remaining
station-name check exposed PSS including a trailing NUL in named user-object
lengths. The final test instrumentation strips that one terminator before
exact comparison. These were fixture defects; no application/library behavior
changed. Compiled copy-omission negatives `native-copy-negative.81b12779`
pass: external publication omission fails the exact Unicode assertion; both
internal storage paths omitted fail the real child paste witness. Each actual
production mutation compiles, changes the application hash, restores exact
source bytes, rebuilds and passes both cases again. The first internal mutation
removed only one store and correctly still passed via the exporter callback;
that redundant path is not a test failure or application defect.
Final-source gates remain required at this checkpoint. Candidate `b28cd333`
fills more than
one screen with computed cmd output, then searches the oldest retained answer
through actual copy mode after detach/reattach. A missing-query partner must
show not-found; the found answer must appear separately from its search prompt.
It exits with q, without touching any clipboard. Actual built and hosted ZIP
runs pass this history case on both architectures in `native-copy.c1d7418e`
(5.54–5.75s each); the context and both real copy cases also pass again.
The final Mac sanitizer gate follows the serial order used by GitHub CI and
the documented CTest command, retaining the unchanged latency assertion.
The earlier parallel-two failure remains recorded, not called fixed.

Earlier exact print-observer candidate `0aa464cd` passes65/65 GCC13.3 Release
(148.01s),65/65 Apple Clang Release (193.18s), and53/53 each Windows target.
Its filtered public `5f3d6d88` passes65/65 and strict docs (190.90s). Native
print-save/observation and compiled detector controls also pass against both
built and actual hosted ZIP executables. The macOS LLVM23 Debug ASan/UBSan
full gate is64/65: flood worst latency6766.3ms exceeds its unchanged5000ms
budget. Three sequential unchanged core-promise suite repeats pass, with
worst attached latency1727.0–1759.1ms; this does not explain or cancel the
failed full gate. The earlier Mac close-title/flood-footer failures likewise
remain retained. No sanitizer diagnostic occurred in this latency failure.

Hosted `d0c9a920` CI37356424942 passes all eight jobs. Release37357093153
passes both Windows package/install jobs, Linux and Homebrew, but Mac fails
the close-dialog title assertion and publication is skipped. Latest published
ckmux is still0.1.7 without Windows assets; latest released library is0.1.18.
Native clipboard and print tests add no new ckVision gap or app workaround.
Remaining: final source integration, full native behavior audit, the Windows
Terminal GUI walkthrough, release publication and downloaded released assets.

### Native print/save and packaged launch acceptance candidate — 2026-10-05

Two native reader cases drive an actual child through the client/server print
wire route, frame button, capture-policy dialog, job list and Save button.
They require exactly one saved document in the configured Unicode/spaced
folder, plain-text escape removal and exact ANSI bytes, with no file before
the explicit save. These cases do not infer absence of native printer/spooler
activity; that needs its own observation. The Windows reader harness accepts
an explicit absolute CKMUX_TEST_BINARY for actual packaged-executable checks
and refuses an invalid selection instead of silently testing the build copy.
The candidate requires native execution and final integration before landing.

The first strict native compile rejects a deprecated C++20 path helper;
the candidate uses the standard UTF-8 constructor instead. The next native
run reaches print capture and policy acceptance, but the job-list click finds
PRINT in the child's readiness text rather than its frame button. The fixture
now requires the bracketed frame label; no application/library change follows
from that misdirected click. The saved-file assertions still require execution.

The corrected native click reaches Save: text passes and ANSI preserves23
bytes ending CR CR LF. The generator wrote an explicit CR LF through the CRT's
default text mode, which inserts another CR. It now selects binary stdout
before generating the promised wire bytes; the22-byte ANSI expectation is
unchanged. This is a fixture correction, not a capture/library workaround.

The binary-stdout candidate passes both actual ARM64 print/save cases with
13-byte text and22-byte ANSI documents. The fixture also queries the actual
server/client process image paths, so packaged acceptance cannot silently
launch the build copy. x64 and actual packaged repetitions remain required.

### Actual client graphics candidate — 2026-10-05

Two additional native reader cases launch a real child through the actual
ckmux client/server and system cmd. One requires decoded red pixels in the
client's emitted host frame, containment outside menu/footer and usable menus
and shell after a running host resize. Its text-only partner first displays
the literal raster payload as ordinary text, then has the child clear it and
draw unconditionally: the completion marker must arrive without images or
payload garbage, and a computed shell result must still work. These candidate
cases are not yet execution evidence, packaged-binary proof or a live Windows
Terminal visual walkthrough. Native suite inventory remains53.

The first actual ARM64 execution fails only the positive graphics case.
The outer host trace reports Sixel=yes but no emitted pictures; the same
child directly through Terminals decodes red pixels, and a newly opened second
terminal renders them. Both UI NewSession routes used spawn_first=1, causing
the server to create the first child before Attach conveyed host capabilities
and bypassing the existing client-side capability grace. The correction sends
spawn_first=0 from those UI routes and lets the existing capability-aware
NewTerminal path create the child. CLI headless session creation is unchanged.
This is application protocol/session behavior, not a ckVision limitation.
Native repeats, compiled negative and full integration remain pending.

### Native terminal acceptance candidate — 2026-10-05

The Unix server-terminal suite is not native Windows coverage. A new native
suite exercises actual ConPTY children through the application's Terminals
collection: non-recycled ids, exit status, silent live state, respawn identity
and current geometry independently queried by the child, live resizing,
actual child print capture/runtime denial, bounded multi-child draining and
whole-tree termination/destruction observed through retained kernel handles.
The helper's uppercase response distinguishes child progress from input echo.
Native inventory becomes53; Windows CI/release require that exact inventory.
These new cases are unverified candidates until native execution, compiled
negative partners and full clean cross-platform gates pass. They do not replace
wire/UI printing, packaged graphics or the live Windows Terminal walkthrough.

The first ARM64 candidate compiles strictly and executes all nine cases.
Actual print capture fails; the test executable had no app-local ConPTY beside
it despite the application using the selected runtime. The test target now uses
the same upstream deployment helper and CI selects that qualified archive.
This does not yet prove that runtime selection fixes the print failure. The
destruction fixture's immediate handle sample also failed; like the retained
Unix counterpart, it now waits within a bounded deadline for asynchronous job
termination. Production close and containment behavior are unchanged.

The qualified1a0d95b candidate passes all nine new cases on ARM64 and
x64-on-ARM64. A compiled respawn-geometry mutation fails the independently
child-observed size assertion; exact source restore and rebuild pass full53/53
on both targets. The earlier malformed-patch attempt stopped before mutation
or compilation and is not credited as a regression verdict. Full final-source
cross-platform gates and live/graphics acceptance remain required.

The first actual release-matrix run37347991790 atpublic9a5a26c3 passes the
ARM64 full installed gate and the x64 native52 gate/packaging, but x64 MSI
installation fails1625. Its retained MSI log reports DisableMsi=1 and rejects
non-assigned applications for standard users on the Windows Server image.
The candidate keeps this policy intact: native x64 build/runtime stays on x64,
while an additional Windows11 ARM64 job consumes the exact x64 packages and
independently compiled inputs for full installation under x64 emulation.
Publication depends on this job too. No native AMD64 desktop installation or
live visual completion is inferred from emulation; the candidate needs actual
hosted execution before its new release verdict can be credited.

### Current adoption and release-matrix preparation — 2026-10-05

App120606b4/publice7914d51 consumes released ckVision0.1.18. Clean committed
gates pass Mac Release65/65 (189.69s), LLVM23 Debug ASanUBSan65/65 (299.08s),
Debian13 ARM64 GCC13.3 strict Release65/65 (135.54s), Windows11 ARM64 MSVC
Release52/52 (85.91s) and x64-on-ARM64 Release52/52 (91.34s). The filtered
public gate passes65/65 (189.77s), strict docs, and all eight hosted37339799318
jobs. Compiled old-pipe, old-CLI-consumer,64-source-cap and queue-limit negatives
retain their earlier attributed scope. The reproduced final-reply defect is
corrected and landed; the original intermittent picker cause is not proved by
that separate reproduction. SDK contains generic process services only.

Both current matching120606b4 x64 and ARM64 ZIP/MSI pairs pass full fresh
standard-user installation: Unicode/spaced extraction, complete input/inventory
hashes, real eight-client startup/one-server/eight-cmd lifecycle, client-exit
persistence, explicit server/descendant shutdown, per-user context2/same SID,
default LocalAppData installation, repair/uninstall and PATH/data preservation.
The new public full-only fixture repeats those checks on both architectures.
An invocation-omission negative fails at the immediate required lifecycle witness
and verifies worker/host/dispatcher failure plus account/profile/workspace/task
cleanup. The earlier omission attempt failed at a repair file-access error, not
the intended assertion; it remains retained and is not credited as that negative.

The release-matrix candidate adds separate VS2026 x64/ARM64 jobs, CMake4.4.4,
WiX5.0.2, qualified runtime/static CRT/released SDK, full native inventory,
matching ZIP/MSI/sidecars and full public installed acceptance. Publication
depends on both Windows architectures; evidence artifacts are not release assets.
Its seven policy cases and YAML/matrix contract checks pass, but final committed
cross-platform/hosted workflow gates are still required. No Windows release,
live desktop or packaged graphics completion is claimed. Historical checkpoints
below retain their original bytes and verdicts, not current pending status.

### Final-reply transport correction candidate — 2026-10-05

A native x64 full adoption run failed the last-session picker case while the
client remained alive. Separately, a deterministic pipe regression establishes
that a late write after peer close discarded all eleven already-arrived bytes
on three runs. Ordinary close without the late write retained them. The defect
is in ckmux's IPC wrapper, not ckVision's process services.

The candidate retires only outbound work after a write error, refuses further
sends without growing queues, and preserves incoming bytes until read EOF or
explicit close. Unix sockets use the same contract, including both move paths.
The UI flush loop and CLI reply waiter consume final replies instead of closing
or aborting at the write error. Explicit close and the outbound hard-limit policy
remain whole-connection operations.

Strict native x64-on-ARM64 and ARM64 regressions fail against the old pipe
implementation (three native and four portable assertions), then pass three
repeats on each target after the transport/UI correction. All eight actual
reader cases pass on both targets. Mac Release's portable regression also
passes. With the CLI fix omitted, the extended regression fails at the actual
reply waiter; restoring it passes on x64. Full final-byte integration gates
are pending; these focused results neither establish the cause of the
original intermittent picker failure nor complete Windows acceptance.

The first full Mac Release sweep is64/65: the sole failure is an older Unix
lifecycle assertion requiring eight unsendable bytes to remain queued after a
permanent write failure. That expectation is updated to require zero queued
bytes, no write interest, repeated write refusal and an independently open
read side; its focused Mac repeat passes. This does not weaken high-water
backpressure coverage or the byte-exact final-reply regressions. Full gates
must include this final test correction as well.

### Released SDK18 adoption and current package candidate

ckVision0.1.18/publicfbc83742 is released at2026-10-05T14:20:15Z after all
seven hosted37320737354 jobs pass. Private42e20367 is the frozen library gate:
Mac/GCC Release230/230, Mac ASanUBSan+fuzz240/240, TSan229/229,
Windows ARM64 and x64-on-ARM64224/224, benchmark gates and actual selected
1.24 runtime34-case qualification on both targets; filtered-public228/228 and
strict docs pass. New image inspection is generic; it knows no application role.
This candidate prepares application version0.1.8 and pins that released SDK18.
Full clean application adoption, current matching x64/ARM64 ZIP/MSI packages,
fresh-profile installed-server lifecycle, live Windows Terminal, release matrix
and published Windows artifacts remain required. Published application0.1.7
still has no Windows assets. Historical checkpoints below retain their own pins.

### Application queue-saturation candidate

A sixth portable wire-security case creates100 empty sessions through the
authenticated protocol, then repeatedly asks for their lists without reading
one client's replies. It observes real production queue growth beyond the
high-water mark, bounds retained bytes, requires only that offender's removal,
and checks a concurrent healthy reader plus a subsequently accepted connection.
No queue limit is lowered and no internal queue state is injected. Frozen
application6a2f2ee passes all six wire cases: Mac Release19.32s, Debian13
ARM64 GCC13.3 Release18.30s and native ARM64 MSVC17 Release58.97s, using
installed17 on Unix and explicit co-development SDK42 on Windows. Mac compiled
hard-limit omission fails the intended queue-bound and offender-removal checks;
exact restoration/rebuild passes19.36s. The first native constant-false mutation
fails compilation under MSVC's warning gate, so is not a regression verdict.
Native server-only guard removal compiles but still passes because the pipe
transport independently protects the same limit. Removing both guards changes
both source files and the test executable, compiles zero-warning15.43s, fails
the intended saturation case/CTest8 in61.12s; exact source restoration and
rebuild14.65s pass all six cases63.61s. These are focused candidate verdicts,
not final released18 application integration. Native CTest inventory stays52.

### Application readiness beyond64 clients — candidate

The existing130-source pipe test calls the library WindowsWaitSet directly;
it does not establish ckmux Server/Poller dispatch. A new portable wire case
creates67 authenticated application clients, sends all requests before stepping
the server, requires the exact session reply for every client, retires the tail
connection and accepts/serves a replacement while66 others remain. It drives
the real Server::step/readiness/transport path on both platforms, not a handler
or library-only wait probe. Five wire-security cases remain one CTest entry,
so native inventory stays52. Frozen751b6611 native ARM64 execution passes all
five cases51.34s; a compiled64-source cap changes source and executable, fails
the intended67-client case/CTest8, then exact restoration/rebuild passes51.92s.
Unix focused execution passes, and the later six-case saturation gate above
retains this readiness case. This is application dispatch proof, not complete
native/live acceptance or an independently installed released18 adopter gate.

### Cross-architecture executable preflight correction candidate — 2026-10-05

The clean x64-on-ARM64 application gate runs49/52: lifecycle lacks actual shell
children, the native shell-host suite times out and actual readers observe
terminal exit rather than a shell prompt. An independent strict native API
diagnostic proves x64 GetBinaryTypeW returns193 for system cmd.exe while
CreateProcessW of the same file succeeds and exits37. Native ARM64 accepts it.
ckVision now prepares generic read-only PE-header/host-architecture inspection;
the application replaces only its executable-validation call. No ARM-specific
shell exception or process-launch implementation enters ckmux. Whether this
fixes every failed scenario remains a native runtime gate, not an inference.
Complete SDK gates/new release, adoption pin and clean application gates are
still required before landing; release17 remains the published dependency.

### Verified integration and packaging preparation — 2026-10-05

The fragmented-dialog and child-retirement fixture corrections are landed at
private f45c5a2e, public9f821819. Exact hosted CI37262790342 succeeds in all
eight jobs: Windows x64/ARM64/x64ASan, both Mac lanes and all three Linux lanes.
Clean committed gates are MacRelease64/64, MacASanUBSan64/64, GCC13.3
Release64/64 and ARM64 MSVC51/51. Older red-run checkpoints below are historical.

ckVision's generic optional deployment helper is landed at private40e522c5,
publicc89d0d7b (version17). Its clean-commit gates pass MacRelease230/230,
ASanUBSan+fuzz240/240, TSan229/229, GCC13.3 Release230/230 and native MSVC
ARM64 Release223/223 plus actual October1.24 runtime34/34. Filtered public
passes228/228 and strict docs. Exact hosted CI37265637549 succeeds in all seven
jobs, and v0.1.17 is released at2026-10-05T05:16:31Z, publicc89d0d7b.

Application packaging implementation pins released17 and adds an explicit archive call into that helper,
the selected1.24.261001001 pin, static MSVC CRT selection, architecture-labelled
ZIP and per-user MSI configuration, and installed/user documentation. A seven-case
policy regression passes; omitting the actual library call fails its intended
assertion, then restored code passes. The clean implementation a030c7c8,
treee9959ef, passes Mac Release65/65 (187.25s), LLVM23 Debug ASan/UBSan65/65
(300.23s), Debian13 ARM64 GCC13.3 strict Release/FORTIFY65/65 (143.49s), and
Windows11 ARM64 MSVC17 strict Release staticCRT52/52 (79.28s). Full source and
binary identities were checked before/after. These measurements precede this
final planning-note delta; landing gates include the final documentation bytes.

Actual ARM64 ZIP and per-user MSI prototypes were created with portable
CMake4.4.4/WiX5.0.2, using the selected Microsoft runtime through the installed
ckVision17 helper. ZIP SHA2562F0388A3AFD5F5A1B8DDC3445D2A40AFF7F9D9DA8452D8813BD29383BB5EFAAB;
MSI SHA25685F58CCB91A27678CAE4CDA1EA25B81796BE08159F48E001D8537A7DCA4D4F58.
Their executable94643C0B matches the verified static-CRT build. The published
0.1.7 release is unchanged: these prototypes are not release assets.

A real temporary standard-user profile passes installer-only CTest1/1 exit0,
11.74s: non-admin/interactively logged-on token and denied protected-machine
write witness; Unicode/spaced ZIP extraction, runtime/layout hashes, ZIP and
installed version, default LocalAppData/ckmux-arm64 install, current-user
unmanaged registration context2/same SID, byte-identical ZIP/MSI inventory,
actual missing-executable repair, uninstall and persistent PATH/user-data
preservation. The owned account/profile were removed and compact evidence
retained. The separate reduced-same-user ZIP fixture passes eight concurrent
starters, one real server/children, persistence and explicit shutdown. The
standard-profile logon host retains a restrictive job, so its installer test
explicitly does not exercise detached lifecycle. No production containment
guard or library policy was weakened to make these test hosts pass.

The upstream static-CRT installed-consumer fixture is fixed at private4975c258,
public21f2f685, with all seven hosted CI37277400770 jobs passing. No runtime/API
change or new SDK release is needed; it remains released17. ckmux does not
carry a consumer-fixture or process-launch workaround.

Remaining native equivalence, independent installed-server lifecycle, x64
ZIP/MSI installation, live Windows Terminal, packaged graphics, release-matrix
wiring, final source/package checksums and published-artifact verification
remain open. WP53–57 are not done.

### Exact hosted result and fragmented-dialog regression — 2026-10-05

Public d477a856 CI37257763884 has finished: native x64/ARM64/x64ASan,
macOS Release and all three Linux lanes pass. macOS ASan/UBSan runs63/64:
the close-dialog case observes its title, then immediately samples four body
strings without waiting for their delivery. The overall hosted run is red.
An isolated fixture reproduces exactly those four failures by draining only
128 bytes after initial readiness and sending the close key without a fixed
settle delay. Waiting for each required body string instead passes three
focused repeats and full local AppleClang21 Debug ASan/UBSan64/64, CTest0,
254.49s. Production behavior and deadlines are unchanged. This test-only
correction still requires all-platform integration, landing and hosted proof.

The same two-file candidate also passes Mac Release64/64 (187.32s) and
Debian13 ARM64 GCC13.3 Release/FORTIFY64/64 (132.89s). Native ARM64 MSVC
rebuild is zero-warning6.82s; full native runs50/51 (80.36s), with all eight
actual-client cases passing52.70s. The red entry is lifecycle's immediate
child-PID lookup after server exit. An additional fixture correction retains
actual child kernel handles, records identities, and waits for their exit
within the original10s shutdown deadline; persistence past that deadline
still fails. Its native repeat is pending. No production change, fully-green
current candidate, new landing or publication is claimed from these results.

The revised three-test-file frozen candidate's native full repeat now passes
51/51, CTest0,79.37s, with source hashes unchanged afterward. This completes
the native candidate repeat, not the remaining clean-commit/publication gate,
live GUI acceptance, full equivalence or release-package work.

Optional runtime deployment is being implemented generically upstream under
ckVision D-134, not as application launch/installer behavior. Twelve packaging
cases pass on Mac, Debian ARM64 GCC14.2 and Windows ARM64 MSVC17. A real
helper-deployed Microsoft ConPTY1.24.260710001 passes all34 native library
child/outer-host cases. ConPTY1.25.260930003 fails the same pixel-mouse and
graphics/resize cases on two runs. The cause is not established; neither
runtime shipping nor complete compatibility with1.25 is claimed. The library
helper is uncommitted and unreleased; ckmux remains pinned to released16.
Live Windows Terminal acceptance, remaining native equivalence and
install-tested/published ZIP and installer artifacts remain open.

### Actual native client acceptance candidate — 2026-10-05

The reader harness now uses ckVision's portable terminal factory. Windows
fixtures retain owning ConPTY sessions for their foreground servers separately
from each client's process tree, and select the real system cmd.exe through an
explicit private configuration. They do not substitute an echo helper for a
shell, modify production behavior or add native process launch plumbing.
Eight new native cases drive the actual ckmux binary and decoded host screen:
prefix commands/real computed shell results, menus/report, mouse menu opening,
detach/reattach, abrupt client-tree termination/reattach, running host resize,
custom title versus child title, and ending the last session. The proposed
required native inventory is51. Runtime gates are pending at preparation;
registration is not pass evidence. This controlled-ConPTY acceptance does not
prove the user's actual Windows Terminal GUI, detached lazy startup, complete
reference-app/saturation equivalence or install-tested Windows release.

Initial ARM64 MSVC strict Release run executes all eight cases, with six
passing and two fixture failures. A mouse-menu test used only one Escape,
although the shared menu contract deliberately backs out one level per
Escape; it now observes dropdown dismissal then leaves the bar explicitly.
The exit witness compared a process ID after the adapter cleared it on exit;
the registry now retains the immutable launch identity beside the owning
session. Both corrections are test-only. The exact corrected ff4e45bd candidate
passes all eight actual cases on Windows11 ARM64 MSVC strict Release, CTest0,
52.29s after a checked10.25s incremental build against installed ckVision16.
The initial6fbbbc1 common harness also passes the five affected Mac Release
suites in166.06s, checked4s incremental build, installed16. These are focused
candidate verdicts, not full clean-commit integration. Caption witnesses now
also require the title on the real window-frame row beside its close control,
so a child's echoed title command cannot substitute for a renamed frame.
An actual compiled production mutation omits remote input payloads and fails
all eight client startup/computed-shell witnesses, propagating CTest8. Source
hashes are restored and the production executable rebuilt before the positive
repeat; that repeat has three intermittent startup failures. The initial rig
waited for the desktop but not the real shell prompt. It now separately observes
the native shell banner/prompt before input, and emits startup-stage/screen
diagnostics on failure. No production behavior is changed, and stability of
these stronger fixtures is checked at2a278341: three consecutive native
eight-case runs all pass, CTest0, after a checked13.64s incremental build;
the final run takes52.31s. The two resize iterations now
also require different computed answers, preventing old screen content from
standing in for working input after the second environment change.

Exact public555ea330 CI37254249086 now succeeds in all eight jobs, including
Windows x64/ARM64/x64ASan, both macOS lanes and all three Linux lanes, with
the previous50-entry native inventory. This does not prove the new51-entry
candidate or replace its required exact-public hosted gate.

### Native shared-session test inventory — 2026-10-05

The native gate now includes all nine `test_reader_modes` cases and all nineteen
`test_session_desktop` cases, without hiding test bodies behind Windows guards.
They drive the actual server over authenticated named pipes using full-width
owned streams and real ConPTY children. The same cases remain on Unix sockets
and PTYs. Fixtures use unique task-selected scratch namespaces, a real quiet
child for layout operations, and a real native echo child with an observed
startup marker for permission/input cases. Source declarations are an inventory,
not a runtime pass claim; exact clean-commit gates are stated with the landing.
Required native CTest inventory is50, with both new entries explicitly enforced
by blocking CI. These tests do not replace the live Windows Terminal walkthrough,
actual client-console close/reattach or install-tested release gates. WP53–57
remain open.

### Hosted checkout and copy-fixture correction candidate — 2026-10-05

VS2026 configure/build now succeeds in every native CI37250470689 lane at
publice03b919. x64 Release and ARM64 Release run46/47, failing only the exact
checked-in/generated key-reference comparison. x64ASan runs45/47, additionally
failing32 copy-mode assertions. The complete clean0202d7 UI candidate on local
ARM64 similarly runs47/48, failing only that documentation comparison; its
68-case client-smoke entry passes. These are red gates, not native completion.

The x64 hosted input manifest's docs/keys.md SHA256698ba7aecf36df218e30e49de8ccf5d03d585c543d2cd383b24afd6631b14408
exactly matches the committed LF reference converted to CRLF. The committed
LF bytes hash05d372259aa72871d70d3ce5e18b441ec267a5bc4ed15b2e49cbda50010d485f.
An explicit Git attribute keeps this byte-compared artifact LF even with
core.autocrlf=true; the assertion and generated UTF8/LF contract stay intact.

Copy fixtures previously injected selection text before any actual native
child readiness observation. They now require positive live identity and the
existing transformed child response before injecting text, including every
custom helper/refusal fixture. No production output or clipboard behavior is
suppressed or changed. Whether this resolves hosted x64ASan remains to be
verified; passing uninstrumented ARM64 is not x64ASan proof. No library
workaround, Windows release, live acceptance or WP55–57 completion is claimed.

### Client UI native-equivalence candidate — 2026-10-05

The full client-smoke suite now registers on Windows with actual build-owned
ConPTY children, positive identity/live-state checks and response-based title
readiness. Unique configured-root scratch and native environment restoration
replace six fixed POSIX environment fixtures; an actual unsuccessful child
replaces /bin/false. Existing common Application event/render assertions stay.
Unix runs67 cases; Windows substitutes a native-profile Settings policy case
for the POSIX-only login-shell case and adds an existing-false-preference
preservation case, for68. The native CTest inventory is48, explicitly required
by all three blocking CI lanes.

Compiled original production UI runs all67 initial native cases and fails
only four native-policy assertions: it advertises POSIX startup files/login
policy. The application correction removes that Windows-only misleading
control, explains native profiles and leaves an existing true/false POSIX
key alone. It does not change ckVision or shell launch semantics. After the
correction and the extra partner case, native MSVC ARM64 strict Release
focused CTest passes14.81s, exit0; Mac Release runs67 cases, all pass. The
first added false-partner fixture omitted its promised existing file, and
one old modal assertion expected POSIX text; both fixture errors were fixed
without weakening modal/focus/layout or persistence behavior assertions.
An invalid hex literal first stopped native compilation before tests; its
actual corrected rebuild precedes the passing repeat.

These are isolated candidate/headless gates, not a landing, complete native
equivalence, a live WindowsTerminal verdict or a package release. Clean
committed-source Mac/GCC/native/sanitizer gates and hosted CI remain required.

### Hosted generator correction — 2026-10-05

Exact public37a9d57 CI37249181000 stopped all three native lanes at configure:
the actual runner images are windows-2025-vs2026 and windows-11-vs2026-arm64,
but the workflow requested Visual Studio17 2022. No C++ compiled and no native
test ran. The corrected workflow names the explicit VS2026 runner labels and
uses CMake's Visual Studio18 2026 generator; both image inventories include
CMake4.4, which supports this generator. The application's source, dependency
pin, sanitizer selection and all assertions are unchanged. A successful local
VS2022 gate does not establish hosted VS2026 success; the corrected hosted
run remains required before WP56 can be complete.

Runner evidence: CI37249181000 jobs111573097147,111573097151,111573097185.
Generator: https://cmake.org/cmake/help/latest/generator/Visual%20Studio%2018%202026.html .

### Native CI infrastructure candidate — 2026-10-05

Prepared three blocking lanes: Windows2025 x64 strict Release, Windows11
ARM64 strict Release, and Windows2025 x64 MSVCASan RelWithDebInfo. All use
the released pin in CMake, an explicit scratch root, audited47-entry native
inventory and the complete native suite. MSVCASan currently supports x86/x64;
native ARM64 does not claim ASan, UBSan or TSan coverage. Unsupported requests
remain configuration errors rather than silently uninstrumented builds.
Runner labels and sanitizer support checked against GitHub runner reference
and Microsoft's AddressSanitizer documentation on2026-10-05.

The test-only host uses an explicitly named, same-user limited scheduled task
when the runner has uncontrolled job ancestry. The existing positive cases
still verify actual created-child independence; restrictive ancestry is built
and asserted inside the native contract. Host membership alone is not proof
breakaway is impossible. No TaskScheduler path or library workaround enters
production ckmux. Host registration, start, timeout and worker errors remain
red, true CTest exits propagate, and the exact task is removed in finally.

Native Windows11 ARM64 local host verification passes all47 unchanged entries
under the forced scheduled path, CTest0,51.34s at parallel2, using the clean
dc91d744 installed16 executable. An actual generated failing test returns
CTest8, the parent fails and the exact temporary scheduled task is absent
afterward. The initial wrapper stopped before CTest on its overly strict
host-membership condition; that extra assumption was removed, not a child
containment assertion. The final wrapper including task identity, handle-finally
and explicit parent scratch passes47/47 again, CTest0,42.14s j2. All eight
expanded workflow scripts parse on Windows PowerShell5.1 and YAML parses with
all three native jobs non-advisory. Hosted x64/ARM64/ASan results are not yet
available. This is preparation
for WP56, not completion of WP53–57, live acceptance or Windows packaging.

Sources: https://docs.github.com/en/actions/reference/runners/github-hosted-runners
and https://learn.microsoft.com/en-us/cpp/sanitizers/asan?view=msvc-170 .

### Latest checkpoint — 2026-10-04, 23:39 UTC

ckVision v0.1.16 is released at public `a38928cf` (private `b8aca7e5`),
published 23:38:47Z. All seven exact-public CI37243144550 jobs pass. The
source now pins16 and consumes the shared helper and detached-process APIs.
Library generic launch services contain no server/session semantics. A fresh
Mac AppleClang21 strict Release build against installed16, with source
preference and dependency downloads disabled, passes64/64 at parallel2 in
235.17s. All223 source/asset hashes agree before and after the gate. This is
pre-landing evidence; final clean committed-source verdicts are recorded in
the landing's verification section, and native acceptance remains incomplete.

Three more complete suites now run on native Windows: configuration (51
actual cases), statistics-display toggles with real child PID/state guards
(two cases), and session naming over actual named pipes (six cases). The
configuration fixture uses unique configured-root scratch and restores the
Unicode OS environment. Both read-failure assertions execute under a retained
native sharing denial rather than silently relying on POSIX permission bits;
all four POSIX location-policy cases retain their Unix coverage and Windows
has its eight native path-policy cases. A new environment regression proves
Unicode values and prior/absent value restoration.

Mac AppleClang Release and Debug ASan/UBSan, Debian13 ARM64 GCC13.3
Release/FORTIFY and Windows11 ARM64 MSVC Release pass the two new focused
configuration/readout entries. All six naming cases pass ten repeats on
each of those configurations. Full native candidate: 47/47 CTest entries,
exit0 at parallel2, 77.21s; executable SHA256
`792f36c6ec4bf2c958e9e1fd58df67f088844f16f84e2b2e17fcd2cfa230fb3f`.
The first full driver stopped before CTest because PowerShell treated ordinary
server stderr diagnostics as terminating errors; only the driver was corrected
to inspect the native exit status. The successful repeat retains all cases.
This is candidate/headless evidence, not complete native equivalence, live
Windows Terminal acceptance or a Windows release. WP53–57 remain open.

### Earlier checkpoint — 2026-10-04, 22:56 UTC

Persistence defect reproduced with the actual Windows ARM64 executable: the
CLI reports success but a controlled permissive inner/restrictive outer job
retains the server and three terminal-host/shell descendants; closing that
outer job kills all four observed handles. Breakaway alone was insufficient.
The isolated correction delegates generic independent-process launch to
ckVision D-133. The library knows no server, endpoint, session or protocol:
it verifies that a suspended child is outside all jobs before resuming.
ckmux supplies its invocation and retains readiness/IPC semantics. The first
six native library cases, strengthened entry witness and compiled guard-omission
negative pass after restoration. D-133 landed in ckVision `dc3312f3`, filtered
public `786617f8`. Clean committed library gates pass Mac/GCC Release 229/229
and native ARM64 MSVC 222/222. The application candidate passes native 42/42,
including actual CLI launch, authenticated endpoint discovery, positive server
identity, ancestor-job closure, continued use and explicit shutdown. Compiling
the old application startup fails the restrictive-job case; byte-exact restored
source passes 42/42. This is controlled-job persistence, not a live Windows
Terminal close/reattach verdict. WP54/55 remain open.

Four new real-wire cases pass Mac/GCC Release, Mac ASan/UBSan and Windows ARM64
MSVC: oversized length, unknown type and missing required payload drop only
the offender, while an already connected client/session and a subsequent
connection remain usable; partial/coalesced valid frames still work. A compiled
omission of the production server's malformed-frame drop fails exactly three
assertions. Production source is byte-exact restored. This coverage is an
uncommitted candidate, not a completed secure-IPC package.

All 15 original rename UI cases are enabled for native Windows with actual
live-child guards. Repeated native diagnostics showed a late ConPTY startup
title replacing the injected OSC title. The fixture now waits for a real
upper-case child response before caption stimuli, preserving all application
assertions and allowing legitimate child titles. Mac Release repeats pass.
The first Windows readiness build failed at a core/process-interface mismatch
before any tests. The next rig incorrectly sent LF rather than the established
Windows child's CR Enter input and timed out. After correcting only those
fixture errors, all 15 cases pass 10 native Windows repeats in 19.90s, CTest0.
The complete native candidate gate subsequently passes 44/44 in 81.62s,
MSVC strict Release, CTest0. This remains candidate/headless evidence; a
clean committed release adopter and actual Windows Terminal walkthrough are
not established by that run.

Captured helper integration v3 separately passes native Windows ARM64 MSVC
Release41/41, Mac Release63/63, Mac Debug ASan/UBSan63/63 and Debian13 ARM64
GCC13.3 Release/FORTIFY63/63, all CTest exits0. ckVision helper code is landed
privately at b1a4ccd and subsequently dc3312f3/filtered public786617f8, but
is NOT released0.1.16. Hosted CI37239077787 fails Mac and Windows. Mac diagnostics
record first output after 4.275ms followed by more than150ms without progress;
no startup error. The positive external stimulus now continuously produces
output for360ms instead of depending on bounded sleep wake-up; the150ms idle
budget, exact12 retained bytes and success assertions remain, with truncation
required. Windows stopped at controller independence during job-fixture setup.
A test-only same-user scheduled host runs all six unchanged controlled-job
cases without weakening library containment. Both revised native suites pass
10 repeats; the scheduled path also passes explicitly. Full revised Mac/GCC
Release229/229 and LLVM23 ASan/UBSan plus fuzz239/239 pass. Hosted re-verification
remains required. Release pin/adoption, full native
and live acceptance, x64/ARM64 blocking CI, packages and final audit remain.

The following older checkpoints are historical candidate evidence.

Captured helper candidate: ckVision D-132 prepares0.1.16 with shared invocation
values and real native binary-pipe runners, not PTY/ConPTY helpers. Native
ARM64 MSVC220/220, Mac/GCC Release228/228 and Mac ASan/UBSan238/238 pass at
the v6 frozen candidate; four compiled Windows production negatives prove
inheritance, idle deadline, output draining and successful-descendant policy.
Version16 adds explicit timeout-root reaping, POSIX resource failure and
signal wakeup cases. TSan initially timed out the independent instrumented
child's runtime shutdown after all expected bytes; the corrected test keeps
the runner/library instrumented and models an ordinary external executable,
without relaxing deadlines. Final versioned assets/platform gates are pending.
The isolated ckmux bridge now delegates to this library, enables the configured
helper hook on Windows, and tests actual copy-key dispatch plus unknown-exit
and partial-input failures. The released dependency pin remains0.1.15 until
the actual upstream0.1.16 release exists. Nothing is landed/released by this
checkpoint; WP53/54 and full Windows acceptance/packaging remain incomplete.


Reply-delivery correction candidate: Windows11 ARM64 MSVC strict Release
passes40/40 (52.09s), exit0, at manifest
`d31c1ee96fb9386293a7562c16715267581b05826141f807182696ee9c1cf1df`,
against released ckVision0.1.15. The eight actual concurrent application
starters now return their own names. Three deterministic real-IPC cases
preserve UI pushes, named/unnamed CLI replies and kill completion/unsubscribe.
Compiling the original server/CLI with these regressions on Mac Release
fails all three (nine assertions, CTest8); byte-exact restore and rebuild
passes the focused suite. The final native fixture pumps acceptance between
connections because the named-pipe listener rearms in the server loop;
all three clients are greeted before the intentionally ordered requests.
The first fixture also confused a generated name with its wire id; the
actual next_session_name contract selects the next session-N suffix.
Native compiled-negative and clean committed full gates are still required.

Clean-commit gate caught concurrent CLI reply misdelivery: Mac Release61/61
(183.65s) and Debug ASan/UBSan61/61 (261.12s) pass at detached `0ae58237`,
but native ARM64 MSVC is38/39 (48.60s): starter1 prints `win-native-0`
although the server log proves all eight correctly named sessions were made.
The error is an unrelated SessionList broadcast reaching an already-greeted
CLI before its own NewSession reply. This failed gate is not a green landing.
The isolated correction sends creation/rename replies to their requester and
picker updates to UI clients only, retaining a precise pending-kill CLI
subscription until its target disappears. Three deterministic common/native
real-IPC cases cover the forced creation ordering, unnamed creation/current
listing, and kill completion followed by unsubscribe. Inventories become
Unix62 and Windows40. Compiled-negative and full committed gates are still
required; no native completion or live WindowsTerminal acceptance is claimed.

Held-clear/native-equivalence candidate: full frozen v2 manifest
`74165e26a87a604ffaef1c4d95c2f114750b508137d00c6f6c27835627f7afa3`
passes Mac AppleClang Release61/61 (185.24s), Debug ASan/UBSan61/61
(244.51s), Debian13 ARM64 GCC13.3 Release/FORTIFY61/61 (126.29s),
and native Windows11 ARM64 MSVC Release39/39 (48.13s), all exits0 and
zero warnings, against clean released ckVision `ddb73e20`/0.1.15.
The native equivalent uses a real successful zero-exit child and the owned
job's Gone observation before advancing the display clock; the held banner
must not cause another clear. The final fixture additionally asserts positive
initial identity and non-Failed state. These are candidate measurements;
clean committed-source gates still precede landing and filtered publication.
The CI failure mechanism is in ckmux lifecycle bookkeeping, not a ckVision
limitation or an application-side library workaround.
An actually compiled native production mutation also fails exactly the new
held-banner ordering assertion (CTest8). Byte-exact reversal, rebuilding and
the full native39/39 repeat pass (45.99s, exit0); no mutation is left in place.

Post-publication checkpoint: private `3d1a3bc5` is landed clean and filtered
public `70209c31` is pushed to GitHub with the released0.1.15 pin. The same
clean commit passes Mac Release61/61 (182.35s), Mac ASan/UBSan61/61
(251.74s), GCC13.3 ARM64 Release/FORTIFY61/61 (129.59s), and MSVC ARM64
Release35/35 (36.00s), exits0. The cold filtered-public adopter additionally
fetches actual released tag `1dd00956` and passes61/61 (234.96s).
GitHub CI37211950514 passes both Mac jobs but fails both Ubuntu x64 GCC13.3
Debug jobs in the repeated-dead-clear assertions. Local Release evidence
does not establish a passing Debug/public CI gate.

The focused rpi GCC13.3 Debug diagnostic reproduces both failures against
the verified Release0.1.15 SDK. A held TermClosed banner erased its reader's
already-sent Gone record when the statistics pass preceded the display tick.
The isolated correction retires this record only on actual window removal.
A new injected-clock ordering regression fails with compiled original server
code (three assertions across the eight cases, including the new case) and
the corrected server passes all eight. No assertion is disabled or weakened;
temporary packet instrumentation is removed. Native equivalent and full
committed gates of this later correction remain required.

Four existing UI suites are also ported with their assertions intact:
command reachability (8 cases), big clock (9), reader modes (9), and desktop
client panning/rows (9). Each fixture requires positive real child identity
and non-Failed/non-Exited state. Native Windows uses the exact build helper;
Unix retains /bin/cat. Before the later held-clear correction, this candidate
passes MSVC39/39 (50.10s), MacRelease61/61 (183.27s), MacASan/UBSan61/61
(258.88s), and GCCRelease61/61 (126.52s), exits0. These are real ConPTY
children with headless Application event/render assertions, not a live
WindowsTerminal walkthrough. WP53/54 and full native compatibility remain open.

The earlier checkpoints below are historical snapshots, not current release
or completion claims.

Pre-landing checkpoint: the candidate now passes Mac AppleClang Release61/61
(181.84s) and Debug ASan/UBSan61/61 (253.58s), Debian13 ARM64 GCC13.3
Release/FORTIFY61/61 (128.43s), and native Windows11 25H2 ARM64 MSVC
Release35/35 (38.59s), all CTest exits0 and zero-warning builds, against
clean ckVision `ddb73e20` (released0.1.15/public `1dd00956`). Unix runs use
app manifest `f9efe827d1623a3a2357f1386e1d8d110f61f14f3e17012082edd0ba26f84b2d`;
native manifest `54840e4989552ea2cc76f0a28e9c1cab321aa3820487a04f276c992648faf007`
differs only in the native fixture's lifecycle/policy/interval corrections.
The portable resource suite now includes seven cases, including a decoded
wire report surviving the client's local statistics timer.

An actually compiled production root-PID gate mutation fails both new native
server and local footer cases (nine assertions, CTest8); byte-exact restoration
rebuilds and passes the full35/35 again (36.85s, exit0). The root fixture
creates a real detached descendant, touches4MiB, waits for its readiness,
then exits: tests observe rootPID=-1 and one live job member, real resident
memory and subsequent CPU intervals, in both named-pipe fan-out and the local
Application. This is a headless UI fixture, not a live WindowsTerminal
visual acceptance claim.

Fixture failures found: core observation handles cannot close a terminal;
replacing a launch spec erased the mandatory exit policy and made PID=-1
vacuously pass; first CPU samples have no interval. Corrected fixtures use
the Terminal lifecycle wrapper, assert a positive initial identity and actual
Exited state, and require the subsequent measured intervals. The first
mutation restoration also exposed Git's default CRLF conversion; the exact
source hash refused it, bytes were restored and recompiled, then the mutation
was repeated with core.autocrlf=false and both restoration gates passed.

Final candidate docs, corrected labels and retirement of a stale Windows
parking-lot comment are not compiled-behavior changes. A clean-commit gate
still precedes landing. WP53/WP54 remain In progress; helper process service,
console-close/job persistence, full native/live acceptance and installed
Windows packages remain required. No Windows-port completion is claimed.

Release/resource-consumption checkpoint: ckVision0.1.15 is released at
14:00:23Z, tag peeling to public `1dd00956`, with all seven CI37205721888
jobs successful. The isolated app uses its typed resources in both server
and local paths on Windows, querying the owned job even after rootPID=-1.
Protocol4 preserves state, optional-field availability, lifetime scope and
native errors. A shared exact saturating CPU-rate converter handles missing
intervals, rollback and scope changes. Windows memory is labelled Private RSS,
not PSS/footprint/virtual commit; incomplete and failed observations are not
zero measurements or Gone. The per-terminal fan-out cache remains intact.

Mac AppleClang Debug ASan/UBSan passes seven focused suites (6.63s, exit0)
at appv17, including six new pure resource cases, the existing wire/display
cases and protocol codec, against clean ckVision `ddb73e20`. Subsequent
additions cover wire mirrors surviving a local timer, and real native
descendants after root exit in both named-pipe server fan-out and local
Application footer. Those later fixtures are not yet gated. Current expected
inventories are Unix61 and Windows35. Full app gates, actual WindowsTerminal
interaction, helper process services, close/persistence acceptance, packages
and WP57 audit remain unfinished; no full Windows completion is claimed.

Publication/identity checkpoint (13:35 UTC): ckVision `ddb73e20` is on private
main, with the pre-existing 33-path progress-widget payload preserved
(586 insertions/69 deletions) and shared index clean. The identical clean
commit passes Mac Release226/226 (38.31 s), GCC13.3 Linux226/226 (55.79 s),
MSVC Windows ARM64218/218 (40.88 s), LLVM23 ASan/UBSan plus fuzz236/236
(188.93 s), and TSan225/225 (353.50 s); CTest exits0 and normal benchmarks
pass. Public `1dd00956b23d71e23985b08aceb9b203dc613c9c` passes filtered
Release220 non-PTY entries plus four PTY entries, zero warnings and strict
docs, and is pushed to GitHub main. CI37205721888 is running before tagging
0.1.15; the latest actual release remains0.1.14.

The isolated app now preserves the upstream full-width ProcessId through
its Terminal wrapper, both consumers, process-table entries/walk and sampling
parameters. Only actual POSIX native queries narrow after an explicit range
check. A nonempty portable contract pins DWORD maximum and descendant identity
and is registered on Windows as well. Mac AppleClang Debug ASan/UBSan passes
all three targeted identity/POSIX-statistics/wire suites,6.07 s, exit0,
at app manifest `e37c8ab06453e6217452df4c6363211ccb5d0a11ccd12665dc1e5038a8299462`
against clean upstream `ddb73e20`. Native/GCC verification of this later app
delta is still required. The app's dependency is prepared as0.1.15 because
those types are not in0.1.14; installed-release adoption/publication waits
for the actual upstream tag. The current targeted measurement predates these
record/pin edits. Native resource values, visible partial/unavailable reports,
helpers, close persistence, full native/live acceptance and packages remain.

Versioned resource checkpoint (13:10 UTC): ckVision's isolated 0.1.15 candidate
uses one source/asset manifest `5ec3410e75479bcc4e51174353ae1e73a42eb7e2eec2dd10d931432cac629eaf`
for all five lanes: Mac Apple Clang Release 226/226 (42.99 s), Debian 13
ARM64 GCC 13.3 Release/FORTIFY 226/226 (56.96 s), Windows 11 ARM64 MSVC
Release 218/218 (72.18 s), Mac LLVM 23 Debug ASan/UBSan plus fuzz 236/236
(198.17 s), and Debug TSan 225/225 (360.91 s). CTest exits 0, normal benchmark
gates pass, compiler logs have zero warnings. Actual generated version SVG,
sixteen report goldens and literal version assertion are included. The final
validation-document delta is in isolated clean commit
`ddb73e209ae99b09dc326ae4e3a6fa1d3d45d976`, manifest
`26004d7f76bd0ac884b1b36e6257d2ccd53fae2a90377171b658d25cea6acd38`;
committed-source gates are running. Private main and public GitHub have not
advanced, and no 0.1.15 release is claimed.

The app statistics audit found two actual server fan-out defects: later
subscribers derived CPU against the first reader's same-time baseline, and a
late subscriber could miss a held dead terminal's clear notification. The
isolated app now caches one terminal packet per pass and tracks clears per
reader/subscription. Its real-socket seven-case suite passes Apple Clang Debug
ASan/UBSan (5.56 s). Actually recompiling the two original production server
files, with new regressions retained, fails exactly the shared-packet equality
and late-subscriber clear assertions (compile exit 0, CTest exit 8). Byte-exact
restoration, rebuild and targeted repeat pass (5.66 s). This is Mac targeted
candidate evidence, not Linux/native equivalent coverage or an app landing.

Resource consumption in both server and local client remains open, including
full-width identities and visible partial/unavailable wire/frame values.
Non-terminal helper pipes, console-close/job persistence, the complete native
and live behavior matrix, x64/ARM64 blocking CI and installed packages, and
WP-57's literal final audit remain. The latest released library is 0.1.14;
the application's GitHub update is still pending.

Resource-service checkpoint (12:39 UTC): the isolated upstream D-131 candidate
passes MSVC Windows 11 ARM64 strict Release 218/218, exit 0, 61.66 s, plus
the benchmark gate. Its six native process/job cases and the real ConPTY
descendant path cover full-width identity, current resident/private-resident
memory, cumulative job CPU including exited workers, growing membership and
denied memory reads. Both compiled negative regressions fail their intended
assertions (build exit 0, CTest exit 8) and pass after byte-exact restoration;
the final full rebuild/gate uses restored code. The first mutation driver
failed before testing due to argument ordering; its restored source was
rebuilt before any further binary use, and that attempt is not mutation proof.

Earlier frozen candidates pass Mac Release 226/226 (40.55 s) and rpi4 GCC
13.3 Release/FORTIFY 226/226 (59.96 s), plus the Linux benchmark. Those older
manifests do not claim to compile the final Windows-only fixture changes.
Normal compiler builds are zero-warning. The Linux shared-header rebuild is
492 actions/1715 s, not comparable to the prior 166-action/340 s version-only
rebuild. Final sanitizer, committed-source, filtered-publication and release
gates are still required. No process-resource library release is claimed;
the latest published library remains 0.1.14.

Both ckmux statistics consumers and their actual frame/wire acceptance are
still outstanding; the current app reports native sampling unsupported.
Helper processes, console-close/job persistence, complete native/live
acceptance, x64/ARM64 CI/packages and the final audit remain. The primary
ckmux checkout/GitHub publication has not advanced during this checkpoint;
the 0.1.14 pin is in the isolated app candidate, not a published app update.

Release checkpoint (11:43 UTC): all seven ckVision GitHub CI jobs in
`37198293532` pass at public `be5ca6c11bae745f9abab1be1d43f3b5aa0025f7`.
v0.1.14 is published (not draft/prerelease); the annotated tag peels to that
exact public commit. The isolated application's release pin is now 0.1.14.
A clean committed application adopter gate remains required; older v13
source-candidate integration evidence is not evidence for this later pin/doc
delta. Subsequent library process-resource work is isolated and unreleased.
Its first Mac strict clean build passes the two focused core/POSIX entries;
the first full sweep is 224/226, failing two documentation checks because the
new headers are absent from the API index. Native compilation/testing remains
in progress. No native statistics or full Windows port completion is claimed.

The publication and integration checkpoints below record earlier snapshots.

Publication checkpoint (11:18 UTC): ckVision `931cdff8` is landed on private
main with the shared progress-widget changes preserved by a before/after
diff-payload and untracked-inventory audit. The same clean commit passes
Apple Clang Release 225/225 (54.56 s), GCC 13.3 Linux Release/FORTIFY
225/225 (59.03 s), MSVC Windows ARM64 Release 216/216 (36.46 s), LLVM 23
Mac ASan/UBSan plus fuzz 235/235 (211.16 s) and TSan 224/224 (378.04 s),
all CTest exits 0 and normal benchmark gates green. Windows' first full
committed sweep is retained as 213/216: three Python documentation/license
checks could not find Git on PATH. Adding the existing private portable Git
to that invocation's PATH, without changing source or binaries, passes the
complete repeat. It was a gate-environment correction, not an assertion skip.

Filtered public snapshot `be5ca6c11bae745f9abab1be1d43f3b5aa0025f7` passes
219 non-PTY entries, four PTY entries, zero-warning Release and strict docs,
and is pushed to GitHub main. CI `37198293532` is running; no v0.1.14 tag
or release has been created. The temporary snapshot build was removed after
its compact logs and source/asset fingerprints were retained. ckmux still
needs released dependency adoption and a clean committed app integration
gate; the passing source-candidate measurements below are not that claim.

Current integration checkpoint (11:06 UTC, isolated candidates): native
Windows 11 25H2 ARM64 MSVC strict Release passes 32/32 entries, exit 0,
37.47 s. App manifest `22c167929275c92336699804b2301fade49dd2f956e49653cfb01e0d0949bba8`
is paired with corrected upstream pre-version manifest
`ea4dea0000c8d2bfb41e4e482757fbe4bed347cc292ec0a8cf3ae9f39652b962`.
The same app/library pair passes Apple Clang strict Release 59/59
(181.57 s), Apple Clang Debug ASan/UBSan 59/59 (259.86 s), and Debian 13
ARM64 GCC 13.3 Release/FORTIFY 59/59 (125.90 s), all CTest exits 0.
These are source-candidate integration gates, not clean app-commit gates,
published Windows packages or a live Windows Terminal certification.

Build records retain exact outputs, compiled-input logs and hashes. Native
SDK-path replacement rebuilt 59 consumer objects in 97.355 s
(10:32:54.273Z–10:34:31.616Z), zero warnings. Mac Release rebuilt in 61 s
and ASan/UBSan in 34 s; the latter's test binary is newly linked while its
unchanged production executable retains its earlier timestamp. No app was
launched from that timestamp as evidence of a visual change. Linux rebuilt
189 actions in 827 s (10:46:10Z–10:59:57Z), after deliberately invalidating
the imported library's timestamps plus changed application inputs; this is
not comparable to the older 48-action/307 s application-only rebuild.

Native executable remains
`C:/Users/klukas/ckmux-windows-20261003-01/app-native-v1-release/Release/ckmux.exe`,
SHA256 `7109b510190c42a87cb7a7555db7597f818dcec4d4a472eca7f2c121bb0e6031`.
The 22 nonempty common/native suites and two real process/lifecycle scripts
retain their original assertions; two Mach-O-only root checks skip on Windows.
This is still not the complete equivalent native test inventory.

The upstream pre-version candidate now passes Mac Release 225/225,
Linux Release 225/225, Windows Release 216/216, LLVM 23 Mac Debug
ASan/UBSan plus fuzz 235/235 and TSan 224/224, plus the three normal
benchmark gates. Its premature nested-fixture exit is actually corrected,
not attributed to serialization alone: a compiled original fails both
400 ms delayed-reader lifecycle/frame assertions, and the real child now
waits for reader input acknowledgement. The release preparation declares
0.1.14 and updates literal version/report assets. The first versioned
commit's Mac Release gate correctly fails a stale report SVG (224/225);
the reviewed one-glyph regeneration is in the replacement isolated commit
`931cdff8683386b79c5149922d70dee909264619`. It passes Mac Release 225/225
(54.56 s) and LLVM 23 ASan/UBSan plus fuzz 235/235 (211.16 s), CTest
exits 0. Final TSan/native/Linux committed and filtered-publication gates
remain in progress. None of this is a published 0.1.14 dependency pin yet.

Next requirements remain upstream non-terminal helper pipes and native
process statistics, bounded console-close completion and actual client/job
persistence, complete native wire/multi-reader/descendant/environment-change
acceptance, live Windows Terminal input/menu/mouse/window behavior,
published dependency adoption, blocking x64/ARM64 CI, install-tested ZIP and
installer artifacts with runtime policy/checksums, and the final WP-57 audit.
WP-53/WP-54 remain In progress. The checkpoints below are historical.

Native test-inventory checkpoint (10:22 UTC, isolated candidate): Windows 11
25H2 ARM64 MSVC strict Release passes 32/32 CTest entries, exit 0, 45.56 s.
There are 22 nonempty common/native suites, two actual process/lifecycle
scripts and eight build/checker entries. Two existing Mach-O-only checkers
return skipped on Windows; they do not establish native dead-code coverage.
The four newly enabled suites retain their original copy-mode, prefix-key,
notification and minimize/chrome assertions, dispatching real Application
events with an actual ConPTY child from this build. A separate native case
proves that child's process identity and transformed input response, rather
than mistaking console input echo for child output. This is not the complete
Unix-equivalent inventory or the live Windows Terminal walkthrough.

Frozen app manifest: `7fccb2657778b338b32f9b6a9f5eb2388eea6153ee566b2e1a52b1769b5f4f5b`.
Build 10:19:54.617Z–10:20:12.631Z, 18.013 s, 59 compiled objects, zero warnings;
main.cpp, protocol/grid fixtures and the revised native shell fixture compile.
The prior wider fixture rebuild was 70.484 s. Actual executable:
`C:/Users/klukas/ckmux-windows-20261003-01/app-native-v1-release/Release/ckmux.exe`,
SHA256 `fa4687d5370a565a0ead9f76918671e40cc4f0d9ae081b524301c90aec6c4820`.
No user-facing window was launched or visually certified.

The first full native 28-entry sweep passed 27/28: generated documentation
corrupted the UTF-8 em dash and used CRLF instead of the checked-in LF bytes.
GenerateDocs now declares UTF-8 process output and Unix newlines; the exact
byte-equality assertion stays, with explicit LF/non-ASCII partners. The next
32-entry sweep passed every original and newly ported suite except a new
fixture's incorrect requirement that a silent child already be Running.
The shared emulator is Ready until output arrives; the corrected fixture
asserts Ready, a live process id, transformed output and then Running.
Both failed sweeps are retained; neither is reported as a passing gate.

Native integration uses an isolated unreleased ckVision SDK, not an assertion
that these changes are in published 0.1.13. Library source manifest
`6553ac4eaa9a4b36da35230c8bdd163667590d1f4d5d147e6ce93017451868e5`
passes native MSVC ARM64 216/216 (115.39 s) and Linux GCC 13.3
Release/FORTIFY 225/225 (58.78 s), CTest exits 0 and both benchmark gates pass.
The library's CkTest bridge now accepts constant conditions without MSVC
C4127, retaining single evaluation, contextual conversion and real failure
reporting; a strict standalone contract checks positives and expected failure.
No warning is disabled in ckmux. The Mac sanitizer sweeps for these bytes
are not green: concurrent appearance/generated-golden timeouts and nested
child output assertions failed; a serialized ASan repeat still misses the
200 ms nested fixture frame. That fixture is being replaced upstream by
reader acknowledgement, with a delayed-reader regression. New full library
gates, commit gates and a release remain required. Earlier Mac app ASan/UBSan
59/59 (310.54 s) predates the current fixtures and is historical evidence only.

Outstanding work is unchanged in scope: upstream generic child-pipe helpers
and process statistics, bounded console-close/job persistence, complete native
wire/flow-control and child-tree acceptance, live Windows Terminal behavior,
current all-platform/sanitizer commit gates, a newly published dependency pin,
x64/ARM64 blocking CI and installed ZIP/installer verification. WP-53/WP-54
remain In progress. These changes are isolated and not yet landed/published.

Earlier production checkpoint (09:24 UTC):

Actual native application checkpoint (09:24 UTC, isolated candidate): the
production Windows 11 25H2 ARM64 MSVC Release executable builds with `/W4
/WX /permissive- /utf-8`; all 33 production objects are present. Native
host selection, Unicode entry/path conversion, portable connection ownership,
native wait-source routing and detached server startup are integrated. The
Windows guard is removed. The latest incremental process-TU/relink gate is
09:10:43.520Z–09:10:52.380Z, 8.857 s, zero warnings, version/help exits 0.
Frozen application manifest: `7a70ee2442aff4515081775fc42825388a786653dec0b6111dcac5bf3100307c`.
Exact executable: `C:/Users/klukas/ckmux-windows-20261003-01/app-native-v1-release/Release/ckmux.exe`,
SHA256 `0bf00bced297eb1549ce5d441ccb9a49f30348757d03f1d3484ef4bd895405c5`.
This first complete production build has testing OFF: it is not a full native
application test-inventory gate or a user-facing window certification.

A real CLI smoke creates one detached server and one actual cmd child,
proves both survive two CLI exits, then explicitly shuts down both (exit 0).
The stronger application lifecycle script starts eight real ckmux clients
concurrently: one server PID 6128 owns eight cmd children, every named session
survives the starters, diagnostics reach a Unicode configuration-directory
log, and explicit shutdown removes the server and all eight children. Exit 0.
This is native application IPC/startup/persistence evidence, not interactive
multi-reader reattachment, history/layout, or live desktop acceptance.

The first detached smoke failed before listening: separately reopening both
standard writers with secure CRT freopen denied the second writer and closed
stderr. The server then crashed while reporting daemonization failure. Native
daemon setup now opens one shared append log, duplicates its descriptor onto
initialized standard streams, uses NUL input and makes handles non-inheritable.
The production-process regression verifies Unicode names, preservation of
existing bytes across two launches, both binary writer streams, detached
console state, NUL input, no inherited handles and explicit failed-log refusal.
Strict targeted native gate: 1/1, exit 0, 11.51 s. The compiled frozen original
implementation fails that regression (build exit 0, CTest nonzero); restoring
the corrected input passes 1/1 again (5.60 s), source identity checked.
The first concurrency script lost a short-lived starter's ExitCode through
Start-Process's later lookup although all eight output files named their
sessions. It now owns each process handle from creation through exit; no
application behavior, success assertion or timeout was weakened.

Shared-loop regression gates: Apple Clang strict Release 59/59, CTest exit 0,
182.38 s, frozen app manifest `22bc5743669b60e3f94c530031ae31d05227516b4edb93ced30b23859ef83b1b`;
rpi4 Debian 13 ARM64 GCC 13.3 Release/FORTIFY 59/59, exit 0, 125.87 s,
manifest `4b9c266eb9e286ca6ace608675f99da87424ad455906ba2b590054e1953286d6`.
The latter compiles 48 fresh incremental actions in 307 s and explicitly
invalidates changed inputs against the original clean-build manifest: an
earlier archive overlay preserved old mtimes and retained a stale poller object,
causing a link failure. Hashes of source alone did not establish compiled input
freshness; the repaired gate recompiles poller.cpp and checks the full inventory.
These frozen gates precede the latest Windows-only daemon code/tests.

The adopter pin is now released ckVision 0.1.13/public `e001070f`, whose seven
CI jobs passed in `37174128133`. Native compilation additionally consumes the
unreleased isolated D-128/D-129/D-130 candidate: shared CRT argv encoding,
strict UTF-8/UTF-16 services and Unicode trace-file opening. Its frozen v4
manifest `bfb3161744ace523d0ca7f1dea55cb4c994a8d5f8754d531ba4ab515af1b4478`
passes Mac 224/224, GCC 13.3 Linux 224/224 and MSVC ARM64 215/215, all CTest
exits 0 and benchmark gates green. Further LLVM 23 sanitizer/fuzz validation
finds missing explicit cstdlib includes, corrected upstream; its first complete
sweep is 233/234, with all ten corpus cases passing but installed-package
consumer linkage failing after the fixture selected Apple Clang instead of
LLVM 23. The fixture now propagates and verifies its actual compiler; the
replacement gate is pending. No passing full sanitizer claim or new library
release follows from this checkpoint.

Outstanding: upstream generic non-terminal child-pipe service for configured
clipboard helpers (U-WIN-HELPER-PROCESS), native process statistics, console-close
shutdown and client/job persistence policy, complete native suite inventory,
interactive reattachment and the WP-55 live Windows Terminal acceptance matrix,
supported sanitizer/clean-commit gates, a newly published dependency pin, x64
and ARM64 CI and installed ZIP/installer release verification. WP-53 and WP-54
remain In progress; WP-55–WP-57 are not done. The older checkpoints below are
historical snapshots, not current executable or release claims.

Native IPC candidate (07:39 UTC, isolated from the shared checkout):
`src/platform/windows_pipe.*` now supplies SID-scoped canonical pipe/mutex
names, a protected current-user/SYSTEM ACL, first-instance collision refusal,
remote-client refusal, client and server token-SID checks, and non-inheritable
handles. Accept is overlapped and rearms before returning a connection.
`WindowsPipeStream` supplies stable overlapped read/write buffers, bounded
read/flush work, byte-stream framing, disconnect readiness, 4 MiB high water,
and a 32 MiB hard cap. Its 64 KiB chunks also bound queue metadata under
thousands of tiny writes. No integer descriptor carries a native HANDLE.

Windows 11 25H2 ARM64 MSVC strict Release builds the isolated harness with
zero warnings (five compile actions, 8.413 s). Its one CTest entry contains
twelve native cases, all passing, exit 0 (1.15 s). They inspect the actual
protected two-trustee DACL, prove eight real concurrent starters yield one
winner, exercise 130 readiness sources through ckVision's WindowsWaitSet,
transfer binary bytes bidirectionally with small read budgets, grow/drain a
backpressured queue, reject an uncoordinated pipe collision, and repeat fifty
pending accept/read/write teardowns with unchanged process handle counts.
Thread counts are measured before/after the readiness and transfer cases.
Four compiled mutations (ACL protection, first-instance guard, copied tail
byte, chunk coalescing) fail their intended assertions; restored source hashes
match and all twelve cases pass again, CTest exit 0 (1.70 s).

Frozen harness manifest SHA256:
`80711cc276c98a38cd26d5ce3fac7baa5b679e38f675c7c6b46bb4753cff6f8e`.
Restored executable: `C:/Users/klukas/ckmux-windows-20261003-01/ipc-v4-native-build/Release/ckmux_native_ipc.exe`,
SHA256 `1cd10ccde04631ec96a48e2418c54b058bc68311cd949c3cc0b0ec23ccf5fe26`.
The harness uses clean local ckVision `13282588`, library SHA256
`0b850306ebf2e6831e2a640873f39a33c80c1dffa9bc41ad546a7b17ecbcfe04`;
it does not certify a released-library or full-application build. This work
is not landed. Stream/connection identity and waits must still be integrated
into the actual client/server loops, and detached process/shutdown, native
host selection, complete acceptance and packages remain unfinished. The
top-level Windows build guard remains honest; no native app was launched.

Linux fallback checkpoint: frozen ckmux adopter manifest
`c5d4777847fef560141ab82b999c04cd99b6bc87bc8cf3c1cb8995cc1d447a98`
passes strict Debian 13 ARM64 GCC 14.2 Release on rpi4, 59/59 (133.73 s),
CTest exit 0, against the clean actual public v0.1.13 tag `e001070f`.
Fresh 278-action build: 07:02:03Z–07:16:32Z, 869 s; no prior comparable
same-host baseline existed. This proves those frozen bytes, not this later
IPC candidate. Both GCC and Clang sanitizer smoke programs fail before main
on that host; its booted kernel configuration states ARM64_VA_BITS=39. The
ASan project build was interrupted after that startup failure was confirmed,
not reported as a test verdict. Private authenticated Debian GCC 13/Clang 19
packages leave system packages unchanged. Further GCC 13 Release gates are
running separately. The shared ckdev VM's bash still reports I/O error;
it has not been restarted without coordination.

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

The isolated Windows print-acceptance candidate now samples actual client/server
named handles with Process Snapshotting and actual descendant processes with
Toolhelp while the real print/save UI runs. A harmless named pipe and a real
owned child are positive controls. Observation errors fail the assertions;
printing may create neither a new observed descendant nor an observed parallel,
USB-printer or named spooler endpoint. This is sampled coverage, not a continuous
kernel trace or an exhaustive claim about every Windows printer RPC mechanism.
At isolated candidate `1e57043`, strict MSVC ARM64 compilation and the pipe/child
controls pass, as do both real print/save cases (15.98 s). The ANSI case records
145 samples, 290 successful client/server handle inventories, zero new observed
children and zero observed printer endpoints. PSS supplies File types but no
File names on this host; duplicated File handles use the Microsoft diagnostic
name query, with a positively confirmed zero-length pipe filename for nameless
I/O pipes. Observation errors are not silently accepted. The final cleaned-up
instrumentation, x64/package runs and full integration remain pending; no
production application/library behavior changed.

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
