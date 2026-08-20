#!/usr/bin/env python3
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
"""A function ckmux defines and never calls is a feature nobody can reach.

The sibling of tools/check_seams.py, aimed one category over.  That checker
walks `std::function` members of the option structs; this one walks free
functions at namespace scope.  Both exist because the same defect keeps
landing in different disguises:

  * Terminal > Window List referenced a command whose handler nobody
    installed -- it drew normally, accepted the click, and did nothing.
  * `Attach.share` was declared, encoded, decoded and handled, and produced
    by nothing.
  * PRINT-1..6 shipped 504 lines of dialog with nine `ClientOptions` seams
    assigned nowhere.  `check_seams.py` was written after that one.
  * `expand_user_path` was landed with its tests while its single call site
    was held back, and sat defined and uncalled in HEAD for an hour.  Nothing
    complained: a free function is not a seam, so `check_seams.py` cannot see
    it.  That is the gap this file closes.

WHY THE LINK MAP AND NOT A SOURCE SCAN.  Two cheaper instruments were built
and measured first, and both were unusable:

  * Grepping for `name(` across src/ reported `start_server` and `apply_delta`
    as uncalled.  Both are called.  A regex cannot parse C++, and this is the
    same failure that made a proto-field producer audit report 17 false
    positives against a clean tree.
  * Reading undefined symbols out of the object files (`nm -u`) reported 29
    functions dead, of which the first four checked by hand were three false
    positives: `parse_byte_size`, `print_job_as_text` and `selected_text` are
    each called from within their OWN translation unit, which emits no
    undefined symbol at all.

The linker's dead-strip pass answers the question the other two only
approximate: *is this reachable from `main` in the production binary?*  It
sees intra-translation-unit calls, it sees inlining, and it does not guess --
it is the same analysis that decides what ships.  On the five functions that
defeated the cheaper instruments it was right five times out of five.

VALIDATED AGAINST THE INCIDENT, not against an argument.  With the sole call
to `expand_user_path` removed and the archives genuinely rebuilt, this
reports it by name and exits 1; with the call restored it exits 0.  The first
attempt at that validation said "not caught" and was wrong -- it linked a
stale archive that still referenced the function, so the measurement was of
the wrong build rather than of the wrong thing.  Rebuild before believing a
negative result here.

Exits non-zero on any unexplained dead function.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# Dead on purpose, each with the reason.  A bare name is not accepted: an
# allow-list without reasons is where dead code goes to be forgotten, which is
# the failure `check_seams.py` has the same guard against.  Say what it is for
# and what would retire the entry.
DELIBERATELY_UNCALLED = {
    "same_state":
        "test-only invariant helper: 16 callers in tests/, none in src/. "
        "Retire by making the grid-delta suites use it through a public "
        "operation, or by deleting it if the suites stop needing it.",
    "command_for_action":
        "test-only mapping helper: 3 callers in tests/, none in src/. Same "
        "disposition as same_state.",
    "describe":
        "NO callers at all, in src/ or tests/. A protocol-diagnostic helper "
        "that nothing uses. Candidate for deletion -- listed rather than "
        "removed because deleting another package's code is not this "
        "checker's call.",
    "action_names":
        "test-only table accessor: the config parser reaches `kActions` "
        "directly and only the keymap suite goes through this. Retire by "
        "routing the parser through it, or by deleting it.",
    "key_context_names":
        "test-only table accessor, same shape as action_names.",
    "process_stats_supported":
        "test-only capability predicate, defined twice behind platform "
        "guards. Nothing in src/ asks before sampling. Retire when the stats "
        "path consults it -- which is arguably the bug this records.",
    "encode_grid":
        "NO callers at all, in src/ or tests/. One line over "
        "`proto::to_runs`. Same disposition as describe.",
}


def free_function_names(root: Path) -> dict:
    """Free functions declared at namespace scope in ckmux's own headers.

    Class and struct bodies are cut out first, by brace matching, so member
    functions are not counted: an unused inline accessor is dead-stripped as a
    matter of course and is not a defect.
    """
    def strip_class_bodies(text: str) -> str:
        out, i = [], 0
        for match in re.finditer(r"\b(?:struct|class)\s+\w+[^;{]*\{", text):
            if match.start() < i:
                continue
            out.append(text[i:match.start()])
            depth, j = 1, match.end()
            while j < len(text) and depth:
                if text[j] == "{":
                    depth += 1
                elif text[j] == "}":
                    depth -= 1
                j += 1
            i = j
        out.append(text[i:])
        return "".join(out)

    names = {}
    for header in sorted((root / "src").rglob("*.hpp")):
        text = re.sub(r"//[^\n]*", "", header.read_text(encoding="utf-8"))
        for match in re.finditer(
                r"(?:^|\n)\s*(?:inline\s+|constexpr\s+)*"
                r"(?:[A-Za-z_][\w:<>,\s\*&]*?)\s+([a-z_]\w*)\s*\([^;{]*\)\s*"
                r"(?:noexcept\s*)?;", strip_class_bodies(text)):
            names.setdefault(match.group(1), str(header.relative_to(root)))
    return names


def dead_stripped_symbols(root: Path, build: Path) -> list:
    """Relink the production binary with -dead_strip and read the map.

    The relink is done here rather than by adding flags to the build, so that
    an ordinary build is unchanged and does not pay for a 45 MB map file.
    """
    link_txt = build / "CMakeFiles" / "ckmux.dir" / "link.txt"
    if not link_txt.is_file():
        print(f"dead-code-check: no link line at {link_txt} -- build ckmux first",
              file=sys.stderr)
        return None
    command = link_txt.read_text(encoding="utf-8").strip()
    with tempfile.TemporaryDirectory() as tmp:
        binary, mapfile = Path(tmp) / "probe", Path(tmp) / "probe.map"
        command = re.sub(r"-o\s+\S+", f"-o {binary}", command, count=1)
        # Every ckmux archive is force-loaded, and this is the whole reason
        # the checker works. A static archive contributes only the members
        # something references: an object nobody calls is never pulled in, so
        # its symbols never appear in the map AT ALL -- not as live, and not
        # as dead-stripped. Measured: without this, removing the sole call to
        # `expand_user_path` produced a clean run, which is precisely the
        # incident the checker exists for. Force-loading puts every member in
        # the link so that dead-strip has something to remove and to report.
        command = re.sub(r"(?<![,=])\b(\S*libckmux_\w+\.a)\b",
                         r"-Wl,-force_load,\1", command)
        command += f" -Wl,-dead_strip -Wl,-map,{mapfile}"
        done = subprocess.run(command, shell=True, cwd=build,
                              capture_output=True, text=True)
        if done.returncode != 0 or not mapfile.is_file():
            print("dead-code-check: probe link failed:\n" + done.stderr[-800:],
                  file=sys.stderr)
            return None
        section = mapfile.read_text(encoding="utf-8", errors="replace").split(
            "# Dead Stripped Symbols:")
        if len(section) < 2:
            return []
        # Exactly one leading underscore, not all of them: Mach-O prefixes
        # symbols with `_`, so an Itanium `_ZN...` name appears as `__ZN...`
        # and `lstrip("_")` would eat both and leave something no demangler
        # recognises -- a silent empty result that reads as a clean tree.
        mangled = []
        for line in section[1].splitlines():
            if not line.strip():
                continue
            symbol = line.split()[-1]
            mangled.append(symbol[1:] if symbol.startswith("_") else symbol)
    filt = shutil.which("c++filt") or shutil.which("llvm-cxxfilt")
    if filt is None:
        print("dead-code-check: no c++filt available", file=sys.stderr)
        return None
    out = subprocess.run([filt], input="\n".join(mangled),
                         capture_output=True, text=True).stdout
    return [line for line in out.splitlines() if line.startswith("ckm::")]


def check_root(root: Path, build: Path) -> int:
    if sys.platform != "darwin":
        # `-dead_strip` and `-map` are Mach-O. The Linux port (WP-22) wants
        # `--gc-sections` with `--print-gc-sections`; until then this skips
        # rather than failing, because a check that cannot run is not a
        # failure and must not be reported as one.
        print("dead-code-check: skipped (needs a Mach-O linker; see WP-22)")
        return 0

    names = free_function_names(root)
    if not names:
        print("dead-code-check: no free functions found -- has the header "
              "layout changed?", file=sys.stderr)
        return 1
    dead_symbols = dead_stripped_symbols(root, build)
    if dead_symbols is None:
        return 1
    blob = "\n".join(dead_symbols)

    violations, excused = 0, []
    for name, header in sorted(names.items()):
        if not re.search(r"(?:^|::)" + re.escape(name) + r"\(", blob, re.M):
            continue
        reason = DELIBERATELY_UNCALLED.get(name)
        if reason:
            excused.append((name, header, reason))
            continue
        print(f"dead-code-check: {name} ({header}) is defined and never "
              f"reached from main -- the linker removed it from the "
              f"production binary. Call it, delete it, or add it to "
              f"DELIBERATELY_UNCALLED with the reason.", file=sys.stderr)
        violations += 1

    for name, header, reason in excused:
        print(f"dead-code-check: {name} ({header}) deliberately uncalled -- {reason}")
    if violations:
        print(f"dead-code-check: {violations} unexplained dead function(s)",
              file=sys.stderr)
    else:
        print(f"dead-code-check: OK ({len(excused)} deliberately uncalled, "
              f"{len(names)} free functions checked)")
    return violations


def self_test(root: Path, build: Path) -> int:
    """A checker that has quietly stopped checking reads like a clean repo."""
    global DELIBERATELY_UNCALLED
    saved = dict(DELIBERATELY_UNCALLED)
    try:
        if sys.platform != "darwin":
            print("dead-code-check self-test: skipped (not Mach-O)")
            return 0
        # Every known-dead function must be seen when it is NOT excused.
        DELIBERATELY_UNCALLED = {}
        found = check_root(root, build)
        if found < len(saved):
            print(f"dead-code-check self-test: expected at least {len(saved)} "
                  f"findings with the exemptions removed, got {found} -- the "
                  f"checker has stopped seeing what it is for", file=sys.stderr)
            return 1
        # And excusing them all must produce a clean run, so a green result
        # means "reached or excused" rather than "not looked at".
        DELIBERATELY_UNCALLED = saved
        if check_root(root, build) != 0:
            print("dead-code-check self-test: exemptions are not honoured",
                  file=sys.stderr)
            return 1
    finally:
        DELIBERATELY_UNCALLED = saved
    print("dead-code-check self-test: OK")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build",
                        help="build directory holding CMakeFiles/ckmux.dir/link.txt")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    build = Path(args.build)
    if not build.is_absolute():
        build = root / build
    if args.self_test:
        return self_test(root, build)
    return 1 if check_root(root, build) else 0


if __name__ == "__main__":
    sys.exit(main())
