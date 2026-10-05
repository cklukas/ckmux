---
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
title: Native Windows
---

# Native Windows builds and packages

The native Windows port is in progress. The published v0.1.7 release does not
contain Windows packages. Passing automated native tests is not certification
of the remaining live Windows Terminal walkthrough or complete installed-app acceptance.

ARM64 prototype packages have passed non-administrator per-user installation,
installed file verification, version reporting, repair and uninstall, including
PATH and user-data preservation. These are not published release packages.
Independent installed-server persistence, x64 package checks and the live
Windows Terminal walkthrough remain required before release.

The intended release formats are a portable ZIP and a per-user MSI for x64 and
ARM64. Select the package matching the operating system; use the ARM64 package
on Windows on ARM. The MSI uses LocalAppData, requires no administrator account,
and does not add a global PATH entry. Configuration and durable logs
are not installed files and must remain unchanged during uninstall.

Run `bin/ckmux.exe` from Windows Terminal. The default native shell is cmd.exe;
PowerShell can be selected explicitly in configuration. Detach with Ctrl+B, D,
or the Session menu. Closing the client must leave its server and programs
alive; use `ckmux.exe kill-server` only for explicit shutdown. An ancestor
Windows job that prohibits independent processes makes startup fail with a
diagnostic instead of promising persistence it cannot provide.

## Runtime policy

The application selects Microsoft's official
`Microsoft.Windows.Console.ConPTY` **1.24.261001001** maintenance runtime,
SHA256 `4d6aaddc1d2385c9f5897df28f33879f699f8f2783315d5204cf3d8c3616ac5f`.
ckVision's optional `ckvision_deploy_conpty` helper validates and deploys the
archive. ckmux contains no substitute loader or process implementation.
The complete distribution includes the DLL, all console hosts needed by the
target architecture, Microsoft's MIT notice, and a runtime identity record.
Keep those files together when moving a ZIP installation.

The selected runtime passes the library's 34 native child/outer-host cases on
the named Windows 11 ARM64 verification host. Newer runtime 1.25.260930003
fails two graphics negotiation/resize cases on that host; its cause is still
under investigation. This pin is a checked distribution policy, not a claim
of compatibility with every Microsoft runtime. ckmux's own packaged graphics
acceptance remains required before a Windows release can be published.

Without an app-local runtime, ckVision uses Windows' inbox ConPTY and reports
its effective child profile without Sixel. An incomplete app-local deployment
is an error, not a silently accepted graphics-capable installation.

## Reproduce packaging

Build with MSVC and CMake 3.28 or later; MSI generation additionally requires
CMake 4.3 or later and WiX .NET tools with a matching UI extension. The build
selects the static MSVC CRT before creating application and library targets;
an installed SDK must have been built with that same CRT selection.

Obtain the exact archive above from Microsoft's official NuGet package, verify
its SHA256, then provide its absolute path explicitly:

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
