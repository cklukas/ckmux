#!/usr/bin/env python3
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
"""Adds `SPDX-License-Identifier: MIT` under each file's copyright line.

WP-26 / D-OPEN-1 (resolved 2026-08-20): both repositories are MIT. The
copyright line every file already opens with stays verbatim — the SPDX line
beneath it is the license statement, and this script is the one place the
sweep's rules live so nobody re-derives them per file:

  * Only files whose FIRST line is the project copyright notice are touched;
    anything else is reported and skipped, never guessed at.
  * Idempotent: a file already carrying an SPDX identifier anywhere in its
    first three lines is left alone, so running the sweep twice is safe and
    a partial run can simply be run again.
  * The comment leader is taken from the copyright line itself (`//` or `#`),
    so C++ sources, CMake files, shell and Python tools all come out in
    their own syntax.

Usage: add_spdx.py <repo-root> [--dry-run]
Prints one line per file changed, then a summary; exits non-zero if any
candidate file had to be skipped, so a sweep that missed something says so.
"""

import sys
from pathlib import Path

SPDX = "SPDX-License-Identifier: MIT"
COPYRIGHT_MARK = "Copyright (c) 2026"
SUFFIXES = {".cpp", ".hpp", ".h", ".c", ".py", ".sh", ".cmake"}
NAMES = {"CMakeLists.txt"}
SKIP_DIRS = {".git", "build", "docs"}  # build*: generated; docs: prose


def candidates(root: Path):
    for path in sorted(root.rglob("*")):
        if any(part.startswith("build") or part in SKIP_DIRS for part in path.parts):
            continue
        if not path.is_file():
            continue
        if path.suffix in SUFFIXES or path.name in NAMES:
            yield path


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    root = Path(sys.argv[1]).resolve()
    dry = "--dry-run" in sys.argv[2:]
    changed = skipped = already = 0
    for path in candidates(root):
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError) as error:
            print(f"SKIP (unreadable): {path}: {error}")
            skipped += 1
            continue
        lines = text.splitlines(keepends=True)
        if not lines:
            continue  # an empty file has no header to annotate
        head = "".join(lines[:3])
        if "SPDX-License-Identifier" in head:
            already += 1
            continue
        first = lines[0]
        # A shebang line may precede the copyright line in scripts.
        offset = 1 if first.startswith("#!") else 0
        if offset >= len(lines) or COPYRIGHT_MARK not in lines[offset]:
            print(f"SKIP (no copyright first line): {path}")
            skipped += 1
            continue
        leader = "#" if lines[offset].lstrip().startswith("#") else "//"
        insert = f"{leader} {SPDX}\n"
        lines.insert(offset + 1, insert)
        if not dry:
            path.write_text("".join(lines), encoding="utf-8")
        print(f"{'would add' if dry else 'added'}: {path.relative_to(root)}")
        changed += 1
    print(f"\n{root}: {changed} changed, {already} already tagged, {skipped} skipped")
    return 1 if skipped else 0


if __name__ == "__main__":
    sys.exit(main())
