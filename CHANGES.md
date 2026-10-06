# Changes

User-visible changes, newest first. For current instructions, use the
[README](README.md) and [user guide](https://cklukas.github.io/ckmux/).

Entries through 0.1.8 were reconstructed from tagged Git history and release
records. Dates below are GitHub publication dates in UTC. These summaries do
not imply that every manual acceptance check was complete at release time.

## Unreleased

- Rewrite the README around installation, first use, and the current feature set.
- Separate installation, session basics, references, and source builds in the
  guide; cover macOS, Linux, and native Windows explicitly, including the
  prebuilt macOS archive's minimum OS requirement.
- Correct Windows environment/shell guidance and the description of desktop
  resizing with a single reader. Document existing clock-binding actions.
- Add this changelog and include it and the new guides in installed packages.
- Update the Homebrew tap to the published 0.1.8 formula and its pinned
  ckVision 0.1.18 resource.

## [0.1.8](https://github.com/cklukas/ckmux/releases/tag/v0.1.8) — 2026-10-05

- Add native Windows x64 and ARM64 portable ZIPs and per-user MSI installers,
  with an explicitly selected app-local ConPTY runtime.
- Implement Windows named-pipe IPC, detached server/startup coordination,
  native waits, Unicode paths, and native process-resource observations.
- Use ckVision 0.1.18 for shared terminal lifecycle, shell launch, clipboard,
  conditional file saves, and native process services.
- Report clipboard export failures and preserve failed copies for retry;
  handle final CLI replies correctly when IPC writes fail.
- Negotiate host capabilities before spawning UI session children.
- Gate native Windows builds, installation/lifecycle behavior, and supported
  x64 AddressSanitizer builds. Manual Windows Terminal acceptance remains open.
- Add Ubuntu 26.04 CI coverage while keeping the Ubuntu 24.04 package baseline.
- Keep hosted functional tests serial; make heavy startup/flood stress opt-in.

[Compare with 0.1.7](https://github.com/cklukas/ckmux/compare/v0.1.7...v0.1.8)

## [0.1.7](https://github.com/cklukas/ckmux/releases/tag/v0.1.7) — 2026-09-27

- Use ckVision 0.1.8, correcting mouse coordinates after a drag moves beyond
  the terminal edge on hosts that report cell-based mouse positions.

## [0.1.6](https://github.com/cklukas/ckmux/releases/tag/v0.1.6) — 2026-09-27

- Use ckVision 0.1.7. Help opens alongside the workspace; Move / Resize uses
  one keyboard mode with Enter to keep changes and Esc to restore them.
- Honor `CKVISION_OUTPUT_CAPTURE` and `CKVISION_GRAPHICS_LOG` in ckmux.
- Remove the Ubuntu 20.04 package because the required standard library lacks
  floating-point `std::to_chars` / `std::from_chars` support.

## [0.1.5](https://github.com/cklukas/ckmux/releases/tag/v0.1.5) — 2026-09-23

- Add big clock mode (`Ctrl+B, t`) and date/date-time overlays in the View menu.
- Keep copy mode over its own terminal window.
- Move keyboard focus with the window selected by next/previous-terminal keys.

## [0.1.4](https://github.com/cklukas/ckmux/releases/tag/v0.1.4) — 2026-08-31

- Partition sparse terminal updates by exact encoded wire cost.
- Use ckVision 0.1.5's host-aware large-frame coalescing to reduce redundant
  terminal output while retaining the latest scene.

## [0.1.3](https://github.com/cklukas/ckmux/releases/tag/v0.1.3) — 2026-08-31

- Add a separately built and installation-tested Ubuntu 20.04 DEB.
- Pin ckVision 0.1.4 for its portable numeric conversion and exported thread
  dependency. Ubuntu 20.04 packages were supported through 0.1.5 only.

## [0.1.2](https://github.com/cklukas/ckmux/releases/tag/v0.1.2) — 2026-08-30

- Add RPM packages and a Homebrew tap/formula alongside DEB and platform archives.
- Package ckVision as a pinned Homebrew source resource and test installation.

## [0.1.1](https://github.com/cklukas/ckmux/releases/tag/v0.1.1) — 2026-08-23

- Add shared and read-only session controls, including takeover and reader modes.
- Add explicit shared-desktop sizing behavior and attach options.
- Publish the browser guide and improve session notifications and menu states.
- Use ckVision 0.1.1.

## [0.1.0](https://github.com/cklukas/ckmux/releases/tag/v0.1.0) — 2026-08-23

- Initial public release: detached sessions, reattachment, floating terminal
  windows, menus, keyboard/mouse control, scrollback, copy mode, and Sixel.
- Include configuration, captured printing, process statistics, Unix build/test
  support, and the MIT license.
