---
title: Installation
---

# Install ckmux

Download packages from the [latest release](https://github.com/cklukas/ckmux/releases/latest).
Choose the operating system and architecture of the machine that will run ckmux.
In filenames below, `VERSION` means the release number shown on that page.

| System | Package | Notes |
|---|---|---|
| macOS Apple Silicon | Homebrew or `Darwin-arm64.tar.gz` | macOS 26+ with the standard Apple toolchain; Homebrew builds from source |
| Debian / Ubuntu x86_64 | `_amd64.deb` | Packages built on Ubuntu 24.04; CI also checks Ubuntu 26.04 |
| RPM-based Linux x86_64 | `.x86_64.rpm` | Install with DNF; runtime dependencies must be available |
| Other Linux x86_64 | `Linux-x86_64.tar.gz` | Same runtime baseline as the Linux packages |
| Windows x64 | `Windows-x64.msi` or `.zip` | Native Intel/AMD build |
| Windows ARM64 | `Windows-arm64.msi` or `.zip` | Use on Windows on ARM |

Linux packages require a compatible modern system runtime; they are not
universal static binaries. Ubuntu 20.04 is unsupported. For Linux ARM64,
Intel Macs, or another system without a matching binary, see
[Building from source](building.md); no matching prebuilt archive is published.
Windows packages are checked on Windows 11 ARM64 and Windows Server x64;
manual Windows Terminal and installer acceptance remains pending.

## macOS

With [Homebrew](https://brew.sh/):

```sh
brew install cklukas/ckmux/ckmux
ckmux --version
ckmux
```

The prebuilt Apple Silicon archive and source builds using Apple's system
C++ library require **macOS 26 or newer**. For installation without Homebrew,
use the [archive instructions](#macos-and-linux-archives). Older macOS versions
would need a separately verified alternative standard library; see the
[build requirements](building.md#requirements).

## Linux

Download only the package you intend to install into a fresh directory, then
run the appropriate command there.

### Debian and Ubuntu

```sh
sudo apt install ./ckmux_*_amd64.deb
ckmux --version
ckmux
```

APT resolves the package's runtime dependencies. If the distribution cannot
satisfy them, use a supported newer system or build from source.

### Fedora and other RPM distributions

```sh
sudo dnf install ./ckmux-*.x86_64.rpm
ckmux --version
ckmux
```

The RPM is built on the same Ubuntu baseline as the other Linux binaries.
Its format alone does not establish compatibility with every Fedora or RHEL
release; DNF must be able to resolve its runtime dependencies.

## Windows

Use **Windows Terminal**. Choose `arm64` for Windows on ARM, or `x64` for
Intel/AMD Windows. WSL is separate: install the Linux package inside WSL if
that is where you want your sessions to run.

### Per-user MSI

1. Download and open the matching `.msi` from the release page.
2. Install for your current user. Administrator access is not required.
3. Open PowerShell in Windows Terminal and launch the installed executable.

The installer places ckmux under your LocalAppData folder unless you select
another destination. Note the installation folder shown in the installer.
In PowerShell, change to that folder (the one containing `bin`) and run:

```powershell
.\bin\ckmux.exe --version
.\bin\ckmux.exe
```

The MSI does not add ckmux to the global PATH. Optionally add that installation's
`bin` folder to your **user** PATH and open a new terminal to use `ckmux` by name.

### Portable ZIP

Extract the matching ZIP to a folder you control. Open PowerShell in the
extracted package directory that contains `bin`, then run:

```powershell
.\bin\ckmux.exe --version
.\bin\ckmux.exe
```

Keep the entire package together, including the ConPTY DLL, architecture
subdirectories, runtime identity record, and license notice. Copying only
`ckmux.exe` does not preserve the packaged runtime.

The default shell is `cmd.exe`. To choose PowerShell, set an explicit shell
path in [configuration](configuration.md#choose-a-shell). See
[Native Windows](windows.md) for runtime details and build/test instructions.

## macOS and Linux archives

Download the archive for your platform and its `.sha256` sidecar. Use a
user-owned installation prefix so no administrator access is needed. The
example below uses Linux; on macOS choose `Darwin-arm64` instead:

```sh
archive=ckmux-VERSION-Linux-x86_64.tar.gz
mkdir -p "$HOME/.local/opt"
tar -xzf "$archive" -C "$HOME/.local/opt"
```

The archive contains a top-level directory matching its filename without
`.tar.gz`. Keep its `bin/` and `share/` directories together, then add that
installation's `bin` directory to PATH:

```sh
export PATH="$HOME/.local/opt/ckmux-VERSION-Linux-x86_64/bin:$PATH"
ckmux --version
ckmux
```

To keep this setting, add the `export` line to your shell's startup file with
the actual version/platform. The manual is in `share/man/man1/ckmux.1` and the
Markdown guide in `share/doc/ckmux/` under that same installation directory.

## Verify a download

Binary packages and archives have SHA-256 sidecars. Run these commands in the
directory holding the downloaded file and its matching `.sha256` file,
substituting the exact filename:

```sh
# Linux
sha256sum -c ckmux-VERSION-Linux-x86_64.tar.gz.sha256
# macOS
shasum -a 256 -c ckmux-VERSION-Darwin-arm64.tar.gz.sha256
```

In PowerShell:

```powershell
$file = 'ckmux-VERSION-Windows-arm64.zip'
$expected = ((Get-Content "$file.sha256" -Raw).Trim() -split '\s+')[0]
if ((Get-FileHash $file -Algorithm SHA256).Hash -ine $expected) {
    throw 'Checksum mismatch'
}
```

## Upgrade or remove

Before upgrading, finish or save work in your running sessions. Stop the old
server with `ckmux kill-server` only when you are ready to end **all** its
programs. Client and server build identities must match; replacing the client
does not upgrade an already-running server.

| Installation | Upgrade | Remove |
|---|---|---|
| Homebrew | `brew update` then `brew upgrade ckmux` | `brew uninstall ckmux` |
| DEB / RPM | Download the new package and repeat the install command | `sudo apt remove ckmux` / `sudo dnf remove ckmux` |
| Windows MSI | Run the new MSI for the same architecture | Windows Settings → Apps → Installed apps |
| Archive / ZIP | Extract to a new folder and update your launch path | Delete the extracted installation folder after stopping its processes |

Windows uninstall preserves configuration and durable logs. Configuration
locations on every platform are listed in [Configuration](configuration.md).

## If something does not work

- **Command not found:** check PATH, or run the executable by its full path.
- **Client/server version mismatch:** use the matching client to save work and
  stop the old server, then start the newly installed version.
- **No Sixel images:** the outer terminal must support Sixel; text remains
  usable without it. On Windows, keep all packaged runtime files together.
- **Windows cannot start a detached server:** some parent process jobs forbid
  independent child processes. Start from a normal Windows Terminal session;
  see [Native Windows](windows.md).

Continue with [your first session](getting-started.md).
