---
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
title: Native Windows
---

# Native Windows

For MSI and portable ZIP installation, architecture selection, and launch
commands, start with [Installation → Windows](installation.md#windows).
This page covers runtime choices, packaging, and native test execution.

ckmux uses native Windows named pipes and ConPTY; it does not require WSL.
The default shell is `cmd.exe`; [configuration](configuration.md#choose-a-shell)
can select PowerShell explicitly. Detach with **Ctrl+B, d** or the Session
menu. The server and child programs keep running after the client exits;
`ckmux.exe kill-server` explicitly ends them.

A parent Windows job that prohibits independent processes makes startup fail
with a diagnostic. Start from a normal Windows Terminal session when you need
detached persistence.

## Support status

Windows x64 and ARM64 ZIP/MSI packages are published. Automated checks cover
installation in a fresh standard-user profile, detached-server persistence,
repair, uninstall, clipboard operations, graphics, print saving, and restored
history. The manual Windows Terminal and installer walkthrough remains pending.

Native x64 build/runtime checks run on Windows Server. Full x64 desktop
installer checks run under x64 emulation on Windows 11 ARM64, alongside native
ARM64 checks. This does not establish native x64 desktop visual acceptance.

## Runtime policy

The application selects Microsoft's official
`Microsoft.Windows.Console.ConPTY` **1.24.261001001** maintenance runtime,
SHA256 `4d6aaddc1d2385c9f5897df28f33879f699f8f2783315d5204cf3d8c3616ac5f`.
ckVision's optional `ckvision_deploy_conpty` helper validates and deploys the
archive. ckmux contains no substitute loader or process implementation.
The complete distribution includes the DLL, all console hosts needed by the
target architecture, Microsoft's MIT notice, and a runtime identity record.
Keep those files together when moving a ZIP installation.

Use the qualified runtime when reproducing release packages. A different
ConPTY version needs its own graphics negotiation and resize validation;
compatibility is not implied by a newer version number.

Without an app-local runtime, ckVision uses Windows' inbox ConPTY and reports
its effective child profile without Sixel. An incomplete app-local deployment
is an error, not a silently accepted graphics-capable installation.

## Reproduce packaging

Build with MSVC and CMake 3.28 or later; MSI generation additionally requires
CMake 4.3 or later and WiX .NET tools with a matching UI extension. The build
selects the static MSVC CRT before creating application and library targets;
an installed SDK must have been built with that same CRT selection.

Obtain the exact archive above from Microsoft's
[official NuGet package](https://www.nuget.org/packages/Microsoft.Windows.Console.ConPTY/1.24.261001001),
verify its SHA256, then provide its absolute path explicitly:

```powershell
cmake -S . -B build -A ARM64 -DCKMUX_WINDOWS_ARCHITECTURE=arm64 `
  -DCKMUX_PREFER_CKVISION_SOURCE=OFF -DCKMUX_CONPTY_ARCHIVE=C:/packages/conpty.nupkg
cmake --build build --config Release
cpack --config build/CPackConfig.cmake -C Release -G ZIP
cpack --config build/CPackConfig.cmake -C Release -G WIX
```

For x64 use `-A x64 -DCKMUX_WINDOWS_ARCHITECTURE=x64`. A source-only build may
omit `CKMUX_CONPTY_ARCHIVE` to use the honest no-Sixel inbox fallback. Such a
build is not the intended graphics-enabled Windows release package.

## Run native tests

CI and release builds configure `CKMUX_TEST_STRESS=OFF`; the native host runs
CTest serially. Two simultaneous starters check startup election and detached
survival. Ten-instance and sustained-flood stress checks are not hosted-runner
acceptance gates. Run them on dedicated local hardware. Select that opt-in
configuration with
`cmake -S . -B build -DCKMUX_TEST_STRESS=ON` before rebuilding locally.

Run the full registered inventory through the same-user, limited,
noninteractive test host. Select an existing absolute build directory and a
new explicit temporary root. For example, after the ARM64 build above:

```powershell
$scratch = Join-Path $env:LOCALAPPDATA ('ckmux-native-tests-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch | Out-Null
./tools/windows/test-native.ps1 -BuildDirectory (Resolve-Path build).Path `
  -TemporaryRoot $scratch -Configuration Release -ForceScheduledHost
```

The host captures CTest output, propagates failure and removes its own scheduled
task. Logs remain beneath the selected root. Scheduling uses your account at
limited privilege; it does not elevate the application or weaken containment
checks. A denied scheduling operation is reported as a test-host failure.

Real clipboard tests require a non-visible default logon window station,
verify the actual client/server logon identities before any write and inspect
the actual client's station afterward. Windows stations have separate
clipboards. These tests fail closed in an interactive context: do not run the
complete clipboard suite directly from Windows Terminal or change the guard
to bypass that protection. The context is test infrastructure, not an
application runtime requirement; ordinary ckmux use accesses your native
clipboard through ckVision. See [Microsoft's window-station description](https://learn.microsoft.com/en-us/windows/win32/winstation/window-stations).

## Release verification

The release workflow has separate x64 and ARM64 jobs. Each uses a released
ckVision pin, the qualified runtime, the static CRT and the complete native
Release inventory, then creates matching ZIP/MSI files and SHA256 sidecars.
Publication depends on both architectures as well as Unix/Homebrew packaging.
Manual workflow dispatch exercises those same gates without publishing a release.

`tools/windows/test-package.ps1` runs the full ZIP and MSI installation/lifecycle
checks in a disposable standard-user profile. Its administrative bootstrap is
test infrastructure only: the application is never elevated, the worker proves
it cannot write protected machine settings, and production containment checks
remain enabled. It requires exact absolute package/build/lifecycle inputs and
an explicitly selected temporary root. Logs and records are preserved before
the fixture removes its own account, profile and disposable package copies.
This automated gate does not replace visible interaction in Windows Terminal.

Native x64 build/runtime gates use the Windows Server x64 runner. That runner's
default installer policy rejects unmanaged per-user MSI installation; the
workflow does not weaken it. Full x64 ZIP/MSI installation therefore runs on
Windows 11 ARM64 under supported x64 emulation, consuming those exact x64
packages and separately fingerprinted compiled runtime inputs. Publication
depends on that additional desktop installation job. This is not a claim of
native AMD64 desktop installer or visual walkthrough coverage.
Microsoft documents the non-elevated per-user restriction for
[DisableMSI=1](https://learn.microsoft.com/en-us/windows/win32/msi/disablemsi).
