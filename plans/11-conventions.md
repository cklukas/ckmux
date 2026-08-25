# ckmux — Engineering Conventions

Plan 11 · Status: current · 2026-08-25 · [Index](README.md)

ckmux inherits ckVision's engineering culture deliberately — two repos, one
discipline. This doc records the rules a contributor (human or AI agent)
must follow; the repo's `AGENTS.md` (written at M0) will point here.

## Language & style

- **C++20**, CMake ≥ 3.28, zero external dependencies beyond
  `ckvision::cvision` and platform libs. Adding any dependency — including
  vendored or test-only code — is a decision-record matter first.
- Naming, ckVision-style: `snake_case` functions/variables, trailing `_`
  members, `PascalCase` types, `kCamelCase` constants; namespace `ckm::`
  with sub-namespaces per layer (`ckm::proto`, `ckm::server`, `ckm::client`,
  `ckm::platform`).
- Layering: `common` → (`server` | `client`) → nothing circular; `common`
  and `server` may include ckVision `core`/`term` only; `client` is the
  sole `ui`/`widgets`/`scene` consumer. Enforced by an include-lint in CI
  (ckVision's layer-checker pattern).
- Errors: assertions (always-on) for contract violations; typed error
  values for environmental failures; **exceptions never cross callback or
  protocol boundaries**. A failed spawn or connect is a *state*, not a
  throw.
- Determinism: no wall-clock, locale, environment, or filesystem access
  outside injected interfaces (`ckv::Clock`, `ckv::FileSystem`, explicit
  env structs). Identical inputs ⇒ identical frames and identical wire
  bytes, on every platform.
- No global mutable state; no threads (v1 — revisiting either is a
  decision record).

## Provenance (binding, no exceptions)

Adopted verbatim from ckVision and **extended to multiplexers**: never
open, read, quote, or transcribe the source code of tmux, GNU screen,
zellij, dtach/abduco, mtm, Twin, Turbo Vision, or any port/derivative — not
for a constant, not for a terminfo string, not for anything. Derive
behavior only from published standards and documentation (ECMA-48, xterm
ctlseqs, the kitty protocol specs, Unicode UAX #11/#29, terminfo(5),
POSIX), from ckVision's own public docs, and from *documented black-box
observation* of terminal behavior (record what you probed and saw).
Man pages and user manuals of those tools are fine; their source is not.

## Workflow

- Trunk-only: commit to `main`, linear history, no feature branches, no
  merge commits. Small, complete landings.
- Commit message: one-line summary; then what and why; then verification
  evidence (test names/counts by category, sanitizer configs, platforms
  run). Never commit with failing gates.
- Pre-1.0: no backward compatibility inside ckmux — fix the design, update
  every caller and test in the same change, remove what is superseded.
  (The *protocol* version handshake exists precisely so we can do this.)
- ckVision is pinned by release version for adopter and CI builds; the sibling
  source outranks that pin for co-development. Version bumps run the full
  suite and update conformance pins when upstream behavior changed
  intentionally.
- **Right level, hands-on (the co-development rule)**: ckmux is a proving
  ground for ckVision, and we develop both repos. A ckVision capability
  gap — missing widget, API, performance problem, awkward seam — is
  **implemented by us directly in ckVision**, under ckVision's conventions,
  decision records, and verification gates, then consumed; it is *never*
  worked around in ckmux. Litmus test: would a second ckVision app
  plausibly want it? → ckVision. Needs ckmux's server/protocol/session
  semantics? → ckmux. When in doubt, ckVision. The worklist lives in
  [07-ckvision-integration.md](07-ckvision-integration.md); paired commits
  reference each other; "not done yet" goes in the roadmap risk table, out
  loud.

## Documentation & headers

- These plans are living documents: behavior changes edit the plan in the
  same commit ("the plans are the spec" — [09-testing.md](09-testing.md)).
  Contradictions between code and plans are bugs in one of them.
- Every source/build/tooling file starts with the copyright line
  (`Copyright (c) 2026 C. Klukas. All rights reserved.` in that file's
  comment syntax). **No SPDX identifier** until the license decision
  ([10-roadmap.md](10-roadmap.md) Q7) resolves.
- User-visible behavior (a command, a key, a config option, a message) is
  not done without: its plan-doc row, its help-registry entry (which
  generates the docs appendix), and its test evidence. ckVision's bar —
  "a feature without golden coverage and reference documentation is not
  done" — applies unchanged.

## Definition of done (every landing)

Zero-warning build (`-Wall -Wextra -Wpedantic -Wshadow -Werror`); all CTest
labels green locally; ASan/UBSan clean; fuzz corpora run where the change
touches a decoder; budget gates green where it touches hot paths; a
regression test that fails without the change; plans/docs updated. "All
tests pass", alone, is not evidence — name what was added.
