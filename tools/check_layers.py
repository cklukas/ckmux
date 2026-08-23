#!/usr/bin/env python3
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
"""Enforce ckmux module discipline (the engineering standard "Layout", the architecture spec).

Ported from ckVision's tools/check_layers.py and adapted to ckmux's four
modules.  Three checks over src/:

1. Direction.  A quoted '"<module>/..."' include must obey the allowed edges
   between ckmux's modules, and a '"cvision/<layer>/..."' include must obey
   the layer rule: `common`, `platform` and `server` may reach ckVision's
   `core` and `term` only — never `ui`, `widgets` or `scene` — so the server
   builds and runs with no host terminal.
2. Include form.  Every quoted include must use the full '"<module>/..."' or
   '"cvision/<layer>/..."' spelling: no relative includes, no '..'.
3. Cycles.  The ckmux-internal include graph must be acyclic, including
   within one module.  (Includes of ckVision headers are checked for
   direction only; that tree has its own checker.)

The linker cannot state any of this for us: ckvision::cvision is a single
archive holding every layer, so linking it says nothing about which headers
a translation unit used.  This script is what turns the rule into a fact.

Exits non-zero on any violation.
"""

import argparse
import re
import sys
import tempfile
from pathlib import Path

# ckmux's own modules and the edges allowed between them.  The direction
# mirrors the library graph in CMakeLists.txt: `platform` is the OS seam and
# depends on nothing else here (that is what makes it swappable for a Linux
# port), `common` is built on it, the two ends are built on `common`, and
# src/main.cpp is the composition root that may see all of it — which is what
# a composition root is for.
MODULE_DIRS = ("client", "common", "platform", "server")
ALLOWED_MODULES = {
    "platform": {"platform"},
    "common": {"common", "platform"},
    "server": {"server", "common", "platform"},
    "client": {"client", "common", "platform"},
    "main": {"main", "client", "server", "common", "platform"},
}

# The rule the split exists for.  `ui`, `widgets` and `scene` paint on a host
# terminal the server does not have; `testing` is the test harness and has no
# business in shipping code at all.
CVISION_LAYERS = ("core", "scene", "term", "testing", "ui", "widgets")
ALLOWED_CVISION = {
    "platform": {"core", "term"},
    "common": {"core", "term"},
    "server": {"core", "term"},
    "client": {"core", "scene", "term", "ui", "widgets"},
    "main": {"core", "scene", "term", "ui", "widgets"},
}

# Anchored at the start of the line: an include shown inside a comment or a
# doc string is prose, not a dependency.
CVISION_RE = re.compile(r'^\s*#\s*include\s+[<"]cvision/([a-z]+)/[^">]+[">]')
QUOTED_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"')


def module_of(relative: Path):
    """Return the ckmux module a src/ path belongs to, or None."""
    parts = relative.parts
    if not parts or parts[0] != "src":
        return None
    if len(parts) == 2:
        return "main"  # src/main.cpp: the composition root
    if parts[1] in MODULE_DIRS:
        return parts[1]
    return None


def check_root(root: Path) -> int:
    source_root = root / "src"
    violations = 0
    graph = {}  # node id -> list of (lineno, target node id)

    files = []
    if source_root.is_dir():
        files = [p for p in sorted(source_root.rglob("*"))
                 if p.suffix in {".hpp", ".h", ".cpp"}]
    if not files:
        print(f"layer-check: no sources found under {source_root}", file=sys.stderr)
        return 1

    for path in files:
        relative = path.relative_to(root)
        module = module_of(relative)
        if module is None:
            print(f"layer-check: {relative} belongs to no ckmux module "
                  f"(expected src/<{'|'.join(MODULE_DIRS)}>/... or src/main.cpp)",
                  file=sys.stderr)
            violations += 1
            continue
        node = str(relative)
        graph.setdefault(node, [])
        for lineno, line in enumerate(
                path.read_text(encoding="utf-8").splitlines(), start=1):
            cvision = CVISION_RE.match(line)
            if cvision:
                layer = cvision.group(1)
                if layer not in CVISION_LAYERS:
                    print(f"{relative}:{lineno}: unknown ckVision layer "
                          f"'cvision/{layer}/'", file=sys.stderr)
                    violations += 1
                elif layer not in ALLOWED_CVISION[module]:
                    print(f"{relative}:{lineno}: module '{module}' may not include "
                          f"cvision/{layer}/", file=sys.stderr)
                    violations += 1
                continue
            quoted = QUOTED_RE.match(line)
            if not quoted:
                continue
            # Anything quoted that was not a well-formed ckVision include is
            # either one of ckmux's own headers or a spelling this tree does
            # not allow — including a malformed "cvision/..." that the check
            # above could not classify.
            target = quoted.group(1)
            head = target.split("/")[0]
            if head not in MODULE_DIRS or ".." in Path(target).parts:
                print(f'{relative}:{lineno}: quoted includes must use the full '
                      f'"<module>/..." or "cvision/<layer>/..." form '
                      f'(got "{target}")', file=sys.stderr)
                violations += 1
                continue
            if head not in ALLOWED_MODULES[module]:
                print(f"{relative}:{lineno}: module '{module}' may not include "
                      f"{head}/", file=sys.stderr)
                violations += 1
                continue
            # A header CMake generates into the build tree (configure_file)
            # is invisible to a static source scan, but its template is not:
            # `src/<target>.in` is the source tree's witness that the header
            # exists and of which module it belongs to — the template lives in
            # the module directory, so the direction rules above have already
            # applied to it. A target with neither file nor template is still
            # a violation, and the self-test holds both directions.
            target_file = source_root / target
            if not target_file.is_file():
                template = source_root / (target + ".in")
                if template.is_file():
                    continue
                print(f"{relative}:{lineno}: include target does not exist: "
                      f"{target}", file=sys.stderr)
                violations += 1
                continue
            graph[node].append((lineno, str(target_file.relative_to(root))))

    # Cycle detection: iterative DFS with white/gray/black coloring.
    WHITE, GRAY, BLACK = 0, 1, 2
    color = {node: WHITE for node in graph}
    for start in sorted(graph):
        if color[start] != WHITE:
            continue
        stack = [(start, iter(graph[start]))]
        path_stack = [start]
        color[start] = GRAY
        while stack:
            node, edges = stack[-1]
            advanced = False
            for _lineno, target in edges:
                if target not in graph:
                    continue
                if color[target] == GRAY:
                    cycle = path_stack[path_stack.index(target):] + [target]
                    print("layer-check: include cycle: " + " -> ".join(cycle),
                          file=sys.stderr)
                    violations += 1
                elif color[target] == WHITE:
                    color[target] = GRAY
                    stack.append((target, iter(graph[target])))
                    path_stack.append(target)
                    advanced = True
                    break
            if not advanced:
                color[node] = BLACK
                stack.pop()
                path_stack.pop()

    if violations:
        return violations
    print(f"layer-check: OK ({len(graph)} files)")
    return 0


def self_test() -> int:
    # Prove every forbidden edge is rejected, rather than relying on the
    # current repository not to contain one: a checker that has quietly
    # stopped checking reads exactly like a clean tree.  Fixtures live outside
    # the worktree so the checker never needs an exemption for its own
    # intentionally-invalid sources.
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        source_root = root / "src"
        expected_violations = 0

        for module in MODULE_DIRS:
            (source_root / module).mkdir(parents=True, exist_ok=True)
            (source_root / module / "target.hpp").write_text(
                "// fixture\n", encoding="utf-8")

        for module in MODULE_DIRS:
            module_dir = source_root / module
            for other in MODULE_DIRS:
                if other not in ALLOWED_MODULES[module]:
                    (module_dir / f"forbidden_{other}.hpp").write_text(
                        f'#include "{other}/target.hpp"\n', encoding="utf-8")
                    expected_violations += 1
            for layer in CVISION_LAYERS:
                if layer not in ALLOWED_CVISION[module]:
                    (module_dir / f"forbidden_cvision_{layer}.hpp").write_text(
                        f'#include "cvision/{layer}/target.hpp"\n', encoding="utf-8")
                    expected_violations += 1

        # The composition root is permissive about ckmux's own modules but is
        # still shipping code: the test harness is out of bounds there too.
        (source_root / "main.cpp").write_text(
            '#include "cvision/testing/target.hpp"\n', encoding="utf-8")
        expected_violations += 1

        # Include form, missing targets, an unknown module, and a cycle.
        (source_root / "common" / "relative.hpp").write_text(
            '#include "../platform/target.hpp"\n', encoding="utf-8")
        expected_violations += 1
        (source_root / "common" / "bare.hpp").write_text(
            '#include "target.hpp"\n', encoding="utf-8")
        expected_violations += 1
        (source_root / "common" / "missing.hpp").write_text(
            '#include "common/absent.hpp"\n', encoding="utf-8")
        expected_violations += 1
        # The positive partner of the missing-target case: a header that
        # exists only as a configure_file template must resolve — this is how
        # the generated common/version.hpp is seen — while `absent.hpp` above,
        # with neither file nor template, keeps failing.
        (source_root / "common" / "generated.hpp.in").write_text(
            "// fixture template\n", encoding="utf-8")
        (source_root / "common" / "uses_generated.hpp").write_text(
            '#include "common/generated.hpp"\n', encoding="utf-8")
        (source_root / "stray").mkdir(parents=True, exist_ok=True)
        (source_root / "stray" / "orphan.hpp").write_text(
            "// fixture\n", encoding="utf-8")
        expected_violations += 1
        (source_root / "common" / "cycle_a.hpp").write_text(
            '#include "common/cycle_b.hpp"\n', encoding="utf-8")
        (source_root / "common" / "cycle_b.hpp").write_text(
            '#include "common/cycle_a.hpp"\n', encoding="utf-8")
        expected_violations += 1

        actual_violations = check_root(root)
        if actual_violations != expected_violations:
            print("layer-check self-test: expected "
                  f"{expected_violations} failures, got {actual_violations}",
                  file=sys.stderr)
            return 1

    print("layer-check self-test: OK")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true",
                        help="verify every forbidden edge is rejected")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    return 1 if check_root(Path(__file__).resolve().parent.parent) else 0


if __name__ == "__main__":
    sys.exit(main())
