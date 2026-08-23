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
import json
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
    # Category 1 -- reachable from tests/ but not from main. Retire by routing
    # production through them, or by deleting them.
    "same_state":
        "test-only invariant helper: 16 callers in tests/, none in src/.",
    "command_for_action":
        "test-only mapping helper: 3 callers in tests/, none in src/.",
    "action_names":
        "test-only table accessor: the config parser reaches `kActions` "
        "directly and only the keymap suite goes through this.",
    "key_context_names":
        "test-only table accessor, same shape as action_names.",

    # Category 2 -- no callers at all, in src/ OR tests/. Deletion candidates,
    # listed rather than removed because deleting another package's code is
    # not this checker's call.
    "describe":
        "no callers anywhere. A protocol-diagnostic helper nothing uses.",
    "encode_grid":
        "no callers anywhere. One line over `proto::to_runs`.",

    # Category 3, and NOT a weaker version of the others: excused HERE because
    # this platform cannot reach it, and it must be REACHED on the platform
    # that can. `reached_on` records that so the Linux form of this checker
    # (see check_root's skip) can assert the inverse mechanically instead of
    # re-reading prose. An exemption that can never retire on the platform
    # granting it is not an exemption -- it is a blind spot with a caption.
    # Found by ckmux-53; the framing is theirs.
    "parse_proc_stat": {
        "reason": "the parse half of the Linux /proc fill (WP-22): compiled "
                  "on every platform so the suite can feed it fixture text, "
                  "called only from the __linux__ arm of process_stats.cpp "
                  "-- a call a Mach-O link cannot contain.",
        "reached_on": "linux",
    },
    "parse_smaps_rollup_pss": {
        "reason": "same shape and same obligation as parse_proc_stat: "
                  "excused here, owed a call site on Linux.",
        "reached_on": "linux",
    },
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


def optimization_level(build: Path):
    """The `-O` setting this build actually compiles with, and where it was read.

    Read from the build's own compile flags rather than from
    `CMAKE_BUILD_TYPE`, because the two can disagree: a reader may pass `-O2`
    in `CMAKE_CXX_FLAGS` under any build type, and the build type is a label
    while the flag is the fact.

    Answers (flag, source) with flag None when nothing above `-O0` is found.
    """
    wanted = re.compile(r"(?<![\w-])-O(?:[1-3]|s|z|fast|g)\b")

    commands = build / "compile_commands.json"
    if commands.is_file():
        try:
            entries = json.loads(commands.read_text(encoding="utf-8"))
        except ValueError:
            entries = []
        for entry in entries:
            text = entry.get("command") or " ".join(entry.get("arguments", []))
            found = wanted.search(text or "")
            if found:
                return found.group(0), "compile_commands.json"
        if entries:
            return None, "compile_commands.json"

    for flags in sorted(build.glob("CMakeFiles/ckmux*.dir/flags.make")):
        found = wanted.search(flags.read_text(encoding="utf-8"))
        if found:
            return found.group(0), str(flags.relative_to(build))
    if list(build.glob("CMakeFiles/ckmux*.dir/flags.make")):
        return None, "flags.make"

    cache = build / "CMakeCache.txt"
    if cache.is_file():
        found = wanted.search(cache.read_text(encoding="utf-8"))
        if found:
            return found.group(0), "CMakeCache.txt"
    return None, "no flags found"


def stale_objects(root: Path, build: Path):
    """The objects this link would use, if any predate the sources.

    The one failure mode this checker has that is not a refusal: objects built
    at an older commit still answer, and they answer about a tree that no
    longer exists. A caller added since the last build is not in the object
    file, so the function it calls looks unreached and the checker reports a
    function that is in fact live.

    It is not hypothetical. A peer saw eleven "dead" functions on their
    machine while three independent full builds of the same source reported
    seven, and their eleven were all RECENT arrivals -- exactly the shape an
    object file a few hours old produces.

    A docstring is not the fix. The checker already refuses when an object is
    missing, on the principle that a tool which cannot tell "nothing calls
    this" from "the caller was not compiled" must not answer; an object older
    than its callers is the same question and gets the same refusal. Compared
    by mtime rather than by CMake's dependency graph because the question is
    only "could this object have seen that source", and mtime answers it
    without depending on the generator.
    """
    # The objects the CURRENT link actually uses, taken from the archives
    # rather than by scanning the build directory. A reconfigure leaves
    # ORPHANED objects behind -- `config.cpp.o` sits under `ckmux_client.dir`
    # while belonging only to `ckmux_common.a` today -- and an orphan is
    # arbitrarily old, so scanning the directory refuses on every build.
    # Measured: that false refusal fired on a tree that was correctly built.
    objects = []
    for target in ("ckmux_client", "ckmux_common", "ckmux_platform",
                   "ckmux_server", "ckmux"):
        directory = build / "CMakeFiles" / (target + ".dir")
        if not directory.is_dir():
            continue
        if target == "ckmux":
            objects.extend(directory.rglob("*.o"))  # the executable's own TU
            continue
        archive = build / f"lib{target}.a"
        if not archive.is_file():
            continue
        listed = subprocess.run(["ar", "-t", str(archive)],
                                capture_output=True, text=True)
        if listed.returncode != 0:
            continue
        # Matched per TARGET, not by basename across all archives: the same
        # basename can be a live member of one archive and an orphan under
        # another target's directory, which is exactly how `config.cpp.o`
        # slipped through and refused a correctly built tree.
        names = {line.strip() for line in listed.stdout.splitlines() if line.strip()}
        objects.extend(o for o in directory.rglob("*.o") if o.name in names)
    if not objects:
        return None, None, None
    # Each object against ITS OWN source, not against the newest source in the
    # tree. The strict form refuses after any ordinary incremental build: one
    # file edited and rebuilt makes that source newer than every object the
    # build correctly left alone, and a gate that cries stale on a good build
    # is one people pass `--force` to.
    for obj in sorted(objects):
        parts = obj.relative_to(build / "CMakeFiles").parts[1:]
        source = root.joinpath(*parts)
        if source.name.endswith(".o"):
            source = source.with_suffix("")
        if not source.is_file():
            continue
        if source.stat().st_mtime > obj.stat().st_mtime:
            return source, obj, len(objects)
    return None, None, len(objects)


def generator_of(build: Path) -> str:
    cache = build / "CMakeCache.txt"
    if not cache.is_file():
        return ""
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("CMAKE_GENERATOR:"):
            return line.split("=", 1)[1].strip()
    return ""


def link_command(build: Path):
    """The command that links `ckmux`, however this generator records it.

    Makefiles write `CMakeFiles/ckmux.dir/link.txt`. **Ninja writes no
    link.txt anywhere**, so the checker could not run at all under it, and
    said so with `no link line ... -- build ckmux first` -- advice a reader
    cannot follow, because no amount of rebuilding produces that file under
    Ninja. An error naming a cause that is not the cause is worse than the
    failure it reports.

    Ninja does record the rule, and `ninja -t commands ckmux` prints it, so
    the answer is recovered rather than refused. CMake wraps it as
    `: && <command> && :`, which is stripped here; if that shape ever changes
    the recovery yields nothing and the caller skips with the true reason
    rather than guessing.
    """
    link_txt = build / "CMakeFiles" / "ckmux.dir" / "link.txt"
    if link_txt.is_file():
        return link_txt.read_text(encoding="utf-8").strip()

    if (build / "build.ninja").is_file() and shutil.which("ninja"):
        listed = subprocess.run(["ninja", "-t", "commands", "ckmux"],
                                cwd=build, capture_output=True, text=True)
        if listed.returncode == 0:
            for line in reversed(listed.stdout.splitlines()):
                if " -o ckmux " in line or line.rstrip().endswith(" -o ckmux"):
                    line = line.strip()
                    if line.startswith(": &&"):
                        line = line[4:].strip()
                    if line.endswith("&& :"):
                        line = line[:-4].strip()
                    return line

    generator = generator_of(build) or "unknown"
    print(f"dead-code-check: skipped -- this build directory uses the "
          f"{generator} generator and no link line for `ckmux` could be "
          f"recovered from it (no CMakeFiles/ckmux.dir/link.txt, and "
          f"`ninja -t commands` yielded nothing). The checker needs the "
          f"link command to build its probe. Rebuilding will not change "
          f"this -- use a build directory this checker can read.")
    return "SKIP"


def dead_stripped_symbols(root: Path, build: Path) -> list:
    """Relink the production binary with -dead_strip and read the map.

    The relink is done here rather than by adding flags to the build, so that
    an ordinary build is unchanged and does not pay for a 45 MB map file.
    """
    command = link_command(build)
    if command is None:
        return None
    if command == "SKIP":
        return "SKIP"
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

    optimized, where = optimization_level(build)
    if optimized:
        # The one thing this checker cannot see through. At `-O1` and above a
        # function called only from within its own translation unit is inlined
        # into its caller, the out-of-line copy is left unreferenced,
        # `-force_load` drags that orphan into the probe link and `-dead_strip`
        # removes it -- and "the linker removed it" then means "inlined away"
        # rather than "never called". Measured: one tree at 08bd856 built two
        # ways reported 7 dead in Debug and 18 in Release, and the extra 11
        # every one had real callers.
        #
        # Note for anyone tempted by a narrower fix: those 11 are NOT `inline`
        # functions in headers. They are ordinary functions declared in a
        # header and defined in a .cpp whose only callers live in that same
        # .cpp. No source-level filter distinguishes them from genuinely dead
        # code, which is why this refuses instead of excluding.
        #
        # Exit 0, not 1: a check that cannot run is not a failure, and
        # README.md documents a Release build as the way to run the suite.
        print(f"dead-code-check: skipped -- this build compiles with "
              f"{optimized} (from {where}), and an inlined function is "
              f"indistinguishable from an uncalled one once the out-of-line "
              f"copy is unreferenced. Run it against a -O0 build.")
        return 0

    names = free_function_names(root)
    if not names:
        print("dead-code-check: no free functions found -- has the header "
              "layout changed?", file=sys.stderr)
        return 1
    newer_source, older_object, object_count = stale_objects(root, build)
    if object_count is None:
        print("dead-code-check: no ckmux objects under "
              f"{build}/CMakeFiles -- build ckmux first", file=sys.stderr)
        return 1
    if newer_source is not None:
        print("dead-code-check: REFUSING to answer -- the build is stale.\n"
              f"  newer source: {newer_source.relative_to(root)}\n"
              f"  older object: {older_object.relative_to(build)}\n"
              "  An object built before its callers cannot show them, so a live "
              "function reads as dead. Rebuild and re-run.", file=sys.stderr)
        return 1

    dead_symbols = dead_stripped_symbols(root, build)
    if dead_symbols == "SKIP":
        return 0
    if dead_symbols is None:
        return 1
    blob = "\n".join(dead_symbols)

    violations, excused = 0, []
    for name, header in sorted(names.items()):
        if not re.search(r"(?:^|::)" + re.escape(name) + r"\(", blob, re.M):
            continue
        entry = DELIBERATELY_UNCALLED.get(name)
        reason = entry["reason"] if isinstance(entry, dict) else entry
        if reason:
            excused.append((name, header, reason))
            continue
        print(f"dead-code-check: {name} ({header}) is defined and never "
              f"reached from main -- the linker removed it from the "
              f"production binary. Call it, delete it, or add it to "
              f"DELIBERATELY_UNCALLED with the reason.", file=sys.stderr)
        violations += 1

    # An exemption for a function that IS reached is how this list rots: the
    # entry outlives its reason, nobody re-reads it, and the next reader
    # believes the code is excused for a cause that stopped being true. It
    # happened here -- `process_stats_supported` was excused as test-only and
    # then wired into production, and nothing said so; the entry was found by
    # hand while chasing an unrelated failure.
    #
    # Reported as a violation rather than a warning, because the fix is to
    # delete one line, and a warning in a green run is a line nobody reads.
    for name in sorted(DELIBERATELY_UNCALLED):
        if name not in names:
            print(f"dead-code-check: {name} is excused but is not a free "
                  f"function declared in any src/ header -- the exemption "
                  f"names something that no longer exists. Remove it.",
                  file=sys.stderr)
            violations += 1
        elif not re.search(r"(?:^|::)" + re.escape(name) + r"\(", blob, re.M):
            print(f"dead-code-check: {name} is excused as uncalled, but it IS "
                  f"reached from main now. The exemption has outlived its "
                  f"reason -- remove it.", file=sys.stderr)
            violations += 1

    for name, header, reason in excused:
        print(f"dead-code-check: {name} ({header}) deliberately uncalled -- {reason}")
    if violations:
        print(f"dead-code-check: {violations} unexplained dead function(s)",
              file=sys.stderr)
    else:
        # The verdict names its own configuration. This checker's answer
        # depends on the build directory in three separate ways -- optimisation
        # level, staleness, and generator -- and all three were found by
        # somebody running it in a configuration nobody else had. A result
        # quoted without its configuration is how the -O3 divergence survived
        # a whole day of confident reports.
        print(f"dead-code-check: OK ({len(excused)} deliberately uncalled, "
              f"{len(names)} free functions checked) "
              f"[build={build}, generator={generator_of(build) or 'unknown'}, "
              f"opt=-O0]")
    return violations


def self_test(root: Path, build: Path) -> int:
    """A checker that has quietly stopped checking reads like a clean repo."""
    global DELIBERATELY_UNCALLED
    saved = dict(DELIBERATELY_UNCALLED)
    try:
        if sys.platform != "darwin":
            print("dead-code-check self-test: skipped (not Mach-O)")
            return 0
        # The self-test asserts that removing the exemptions produces
        # findings, which a SKIPPED run cannot do. It therefore has to skip
        # wherever the check skips, or it fails precisely where the checker is
        # behaving correctly -- which is how a self-test ends up being the
        # thing that reddens the documented Release path.
        optimized, where = optimization_level(build)
        if optimized:
            print(f"dead-code-check self-test: skipped (build compiles with "
                  f"{optimized}, from {where})")
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
