# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
"""Which byte ranges of a source file are prose, and which are code.

A citation of an unpublished document is a comment. A string literal that
happens to contain the same characters is not: `TEXT_NAMES = {"AGENTS.md"}`
names a file the tool opens, and a terminal title asserted as
`"vim: plans/06"` is test data compared byte for byte. Rewriting either would
change behavior rather than a pointer, so the rewriter is only ever handed the
ranges this returns.
"""

from __future__ import annotations

import re

C_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp"}
HASH_SUFFIXES = {".cmake", ".sh", ".yaml", ".yml"}
HASH_NAMES = {"CMakeLists.txt"}


# `'a'`, `'\\n'`, `'\\''` — but not the `'` in `10'000`, which is a digit
# separator. Reading that one as a literal swallows everything to the next
# apostrophe, and every comment inside it stops being prose.
CHAR_LITERAL = re.compile(r"'(?:\\.|[^'\\\n])'")
# R"delim( ... )delim" — the one string that may legally contain a newline.
RAW_STRING = re.compile(r'R"([^()\\ \t\n]{0,16})\(')


def _c_spans(text: str) -> list[tuple[int, int]]:
    """Comment ranges of a C or C++ file, string and char literals excluded."""
    spans: list[tuple[int, int]] = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            start = i
            while i < n and text[i] != "\n":
                i += 1
            spans.append((start, i))
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            start = i
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i = min(i + 2, n)
            spans.append((start, i))
        elif c == "R" and (raw := RAW_STRING.match(text, i)):
            close = text.find(f'){raw.group(1)}"', raw.end())
            i = n if close < 0 else close + len(raw.group(1)) + 2
        elif c == '"':
            # A plain literal cannot hold a raw newline. Stopping at one bounds
            # the damage of any construct this scanner reads wrongly to the
            # line it is on, instead of to the rest of the file.
            j = i + 1
            while j < n and text[j] not in '"\n':
                j += 2 if text[j] == "\\" else 1
            i = j + 1 if j < n and text[j] == '"' else i + 1
        elif c == "'":
            literal = CHAR_LITERAL.match(text, i)
            i = literal.end() if literal else i + 1
        else:
            i += 1
    return spans


def _hash_spans(text: str, triple_quoted: bool = False) -> list[tuple[int, int]]:
    """Comment ranges of a `#`-commented file.

    With `triple_quoted`, Python's triple-quoted strings count as prose too:
    those are the docstrings, which explain a rule the same way a comment does.
    Ordinary quoted strings never do.
    """
    spans: list[tuple[int, int]] = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if triple_quoted and text[i:i + 3] in ('"""', "'''"):
            fence = text[i:i + 3]
            start = i
            i += 3
            while i < n and text[i:i + 3] != fence:
                i += 2 if text[i] == "\\" else 1
            i = min(i + 3, n)
            spans.append((start, i))
        elif c == "#":
            start = i
            while i < n and text[i] != "\n":
                i += 1
            spans.append((start, i))
        elif c in "\"'":
            quote = c
            i += 1
            while i < n and text[i] != quote and text[i] != "\n":
                i += 2 if text[i] == "\\" else 1
            i += 1
        else:
            i += 1
    return spans


def rewritable_spans(path: str, text: str) -> list[tuple[int, int]]:
    """Ranges of `text` that may be rewritten, in order and non-overlapping."""
    name = path.rsplit("/", 1)[-1]
    suffix = name[name.rfind("."):] if "." in name else ""
    if suffix == ".md":
        return [(0, len(text))]
    if suffix in C_SUFFIXES:
        return _c_spans(text)
    if suffix == ".py":
        return _hash_spans(text, triple_quoted=True)
    if suffix in HASH_SUFFIXES or name in HASH_NAMES:
        return _hash_spans(text)
    return []
