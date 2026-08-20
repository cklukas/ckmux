#!/usr/bin/env python3
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
"""Every seam ckmux declares must be wired, or say plainly that it is not.

The sibling rule to tools/check_layers.py, and the one the project has now
learned three times.  tests/test_command_reachability.cpp opens with it:

    Every key and every menu entry ckmux offers must do one of two things:
    work, or say plainly that it does not.  There is no third state, and
    this suite exists because there was one.

That suite points at *controls*.  This one points at *seams*, which is one
level deeper and is where the same defect kept landing:

  * Terminal > Window List shipped through all of M1 referencing a command
    whose handler nobody installed.  It drew in the ordinary colour, accepted
    the click, and did nothing.
  * `Attach.share` was declared, encoded, decoded and handled -- and produced
    by nothing.
  * PRINT-1..6 shipped 504 lines of dialog and 112 of header with all nine of
    its `ClientOptions` seams assigned nowhere in src/ at all.  A reader saw
    `[ PRINT . 2 . 0 B ]` on the frame button and "Nothing has been captured
    from this terminal." in the window, on one screen, at one moment.

In each case every test passed, because every test exercised the half that
existed.  A `std::function` member that nobody assigns is not a compile error,
is not a link error, and is not visible to a suite written from the same
mental model as the code -- it is an empty `std::function` and a dialog that
politely reports nothing.  This script is what turns "wired" into a fact.

Checked over the seam-bearing structs listed in SEAM_STRUCTS: every
`std::function` member is assigned in one of WIRING_FILES, or appears in
DELIBERATELY_UNWIRED **with a reason**.  A bare allow-list is where unwired
seams go to be forgotten, so a name alone is not accepted; and the reason is
printed on every run, because a silent exemption and a real wire look
identical in green output.

Exits non-zero on any violation.
"""

import argparse
import re
import sys
import tempfile
from pathlib import Path

# The structs whose `std::function` members are seams the production build is
# expected to fill.  Listed rather than discovered: a struct that grows seams
# should have to be added here deliberately, and enumerating them is also what
# stops this checker from being scoped to the one place a defect was found --
# which would be the same class of error, committed inside its own fix.
SEAM_STRUCTS = (
    ("src/client/client_app.hpp", "ClientOptions"),
    ("src/client/server_session.hpp", "ServerSession"),
)

# Where the production build assembles its seams.  BOTH files, deliberately:
# scanning only run_client.cpp reports four perfectly well wired seams
# (clipboard_writer, clipboard_problem, local_now, mouse_reports_probe) as
# missing, because main.cpp is a second wiring site.  An audit scoped to one
# of them answers a narrower question than the one it appears to answer.
WIRING_FILES = (
    "src/main.cpp",
    "src/client/run_client.cpp",
)

# Seams the production build deliberately leaves empty, each with the reason
# it is empty.  The reason is the point: the day it stops being true, somebody
# reads it.  An entry whose reason is "not implemented yet" belongs to a work
# package -- name it, so the entry expires with the package rather than
# outliving it.
DELIBERATELY_UNWIRED = {
    # `printer_effective` asks the SERVER for the resolved printer policy and
    # where each answer came from (terminal / session / global / built-in).
    # There is no server source for it: nothing on the wire carries that
    # string.  Filling it client-side would mean resolving the precedence a
    # second time, which the seam's own comment forbids -- a second
    # implementation of a precedence rule disagrees with the first eventually,
    # and the disagreement shows up as a reader being told the wrong reason.
    # This is a PROTOCOL gap and wants its own package, not a lambda.
    "printer_effective":
        "no server source: the resolved-policy string is not on the wire. "
        "Wiring it client-side would duplicate the precedence rule. Needs a "
        "protocol package, not a lambda.",

    # Left unset by the print-route repair as out of scope; it was unset
    # before that work, so nothing regressed.  Still a real gap: a reader
    # cannot change a terminal's printer policy from the client.
    "set_printer_policy":
        "out of scope for the print-route repair and unset before it. A real "
        "gap: no client route to change a terminal's printer policy.",
}


def struct_body(text: str, name: str) -> str:
    """The braced body of `struct name { ... }`, by brace matching.

    Regex cannot do this: seam declarations span lines, contain '>' inside
    template arguments and '{}' inside default member initialisers.
    """
    match = re.search(r"\b(?:struct|class)\s+" + re.escape(name) + r"\b[^;{]*\{", text)
    if match is None:
        return ""
    depth, i = 1, match.end()
    while i < len(text) and depth:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
        i += 1
    return text[match.end():i]


def seam_names(body: str) -> list:
    return sorted(set(re.findall(r"std::function\s*<.*?>\s*([A-Za-z_]\w*)\s*(?:=|;)",
                                 body, re.S)))


def is_wired(name: str, wiring: str) -> bool:
    # `.name =` covers `options.client.name = ...` and `session.name = ...`;
    # `set_name(` covers the setter form.  Both are assignments of the seam,
    # which is the question -- not whether the name is mentioned anywhere,
    # which a comment would satisfy.
    return bool(re.search(r"\.\s*" + re.escape(name) + r"\s*=", wiring) or
                re.search(r"\bset_" + re.escape(name) + r"\s*\(", wiring))


def check_root(root: Path) -> int:
    wiring = ""
    for rel in WIRING_FILES:
        path = root / rel
        if not path.is_file():
            print(f"seam-check: wiring file missing: {rel}", file=sys.stderr)
            return 1
        wiring += path.read_text(encoding="utf-8")

    violations = 0
    exempted = []
    for rel, struct in SEAM_STRUCTS:
        path = root / rel
        if not path.is_file():
            print(f"seam-check: header missing: {rel}", file=sys.stderr)
            violations += 1
            continue
        body = struct_body(path.read_text(encoding="utf-8"), struct)
        if not body:
            print(f"seam-check: {struct} not found in {rel}", file=sys.stderr)
            violations += 1
            continue
        names = seam_names(body)
        if not names:
            print(f"seam-check: {struct} declares no seams -- is it still the "
                  f"right struct to watch?", file=sys.stderr)
            violations += 1
            continue
        for name in names:
            if is_wired(name, wiring):
                continue
            reason = DELIBERATELY_UNWIRED.get(name)
            if reason:
                exempted.append((struct, name, reason))
                continue
            print(f"seam-check: {struct}::{name} is declared and assigned "
                  f"nowhere in {' or '.join(WIRING_FILES)}. A seam nobody "
                  f"wires is a feature a reader cannot reach: wire it, or add "
                  f"it to DELIBERATELY_UNWIRED with the reason.",
                  file=sys.stderr)
            violations += 1

    # Printed on success too: an exemption that never appears in output is an
    # exemption nobody re-reads.
    for struct, name, reason in exempted:
        print(f"seam-check: {struct}::{name} deliberately unwired -- {reason}")

    if violations:
        print(f"seam-check: {violations} unwired seam(s)", file=sys.stderr)
    else:
        print(f"seam-check: OK ({len(exempted)} deliberately unwired)")
    return violations


def self_test() -> int:
    """A checker that has quietly stopped checking reads like a clean repo."""
    global SEAM_STRUCTS, WIRING_FILES, DELIBERATELY_UNWIRED
    saved = (SEAM_STRUCTS, WIRING_FILES, dict(DELIBERATELY_UNWIRED))
    try:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "src").mkdir()
            (root / "src" / "opts.hpp").write_text(
                "struct Opts {\n"
                "    std::function<void()> wired_one;\n"
                "    std::function<int(std::string, std::vector<int>)> unwired_one;\n"
                "    std::function<void()> exempt_one;\n"
                "    int not_a_seam = 0;\n"
                "};\n", encoding="utf-8")
            (root / "src" / "wire.cpp").write_text(
                "void go() { o.wired_one = []{}; }\n", encoding="utf-8")
            SEAM_STRUCTS = (("src/opts.hpp", "Opts"),)
            WIRING_FILES = ("src/wire.cpp",)

            # One unwired, one exempt-but-unlisted: two violations.
            DELIBERATELY_UNWIRED = {}
            got = check_root(root)
            if got != 2:
                print(f"seam-check self-test: expected 2 violations, got {got}",
                      file=sys.stderr)
                return 1

            # Exempting one leaves exactly one.
            DELIBERATELY_UNWIRED = {"exempt_one": "test-only seam"}
            got = check_root(root)
            if got != 1:
                print(f"seam-check self-test: expected 1 violation, got {got}",
                      file=sys.stderr)
                return 1

            # Exempting both passes -- and proves an exemption is honoured,
            # so a green run below means "wired or excused", not "not looked".
            DELIBERATELY_UNWIRED = {"exempt_one": "test-only seam",
                                    "unwired_one": "also test-only"}
            got = check_root(root)
            if got != 0:
                print(f"seam-check self-test: expected 0 violations, got {got}",
                      file=sys.stderr)
                return 1

            # A multi-line seam declaration must still be seen: this is the
            # form the real ClientOptions uses, and a checker that only sees
            # single-line members would pass a tree full of unwired seams.
            (root / "src" / "opts.hpp").write_text(
                "struct Opts {\n"
                "    std::function<void(const std::string& a,\n"
                "                       std::vector<int> b)>\n"
                "        spanning_seam;\n"
                "};\n", encoding="utf-8")
            DELIBERATELY_UNWIRED = {}
            got = check_root(root)
            if got != 1:
                print("seam-check self-test: a multi-line seam declaration was "
                      f"not seen (expected 1 violation, got {got})",
                      file=sys.stderr)
                return 1
    finally:
        SEAM_STRUCTS, WIRING_FILES, DELIBERATELY_UNWIRED = saved

    print("seam-check self-test: OK")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true",
                        help="verify the checker still catches an unwired seam")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    return 1 if check_root(Path(__file__).resolve().parent.parent) else 0


if __name__ == "__main__":
    sys.exit(main())
