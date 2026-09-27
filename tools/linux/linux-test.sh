#!/bin/sh
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
#
# Build and test ckVision and ckmux on Linux, from the Mac, in the ckdev VM
# (tools/linux/ckdev.yaml). Configure, build, ctest, both repositories, in
# that order, with the result of each step printed as it lands — the point is
# to see what a Linux compiler says about the same bytes a Mac build passed.
#
#   tools/linux/linux-test.sh                   # HEAD of both repos, Release
#   tools/linux/linux-test.sh --debug           # Debug (GCC's -Wmaybe-uninitialized
#                                               #   and the ASan/UBSan lanes live here)
#   tools/linux/linux-test.sh --clang           # Clang instead of GCC
#   tools/linux/linux-test.sh --worktree        # the Mac working trees as they are,
#                                               #   uncommitted changes included
#   tools/linux/linux-test.sh --sanitize        # Debug + ASan/UBSan, like CI's lanes
#   tools/linux/linux-test.sh --only ckvision   # one repository
#
# What it builds by default is HEAD — a fresh clone of each repository's
# committed state, the same thing CI and a stranger's `git clone` would get.
# The working tree is what --worktree asks for; it is on a virtiofs mount, so
# the build reads it in place and writes to ~/build on the VM's own disk,
# which is where every build directory goes regardless. The two never share
# one: HEAD builds in ~/build/<Type>-<cc>-head, --worktree in ...-wt.
#
# Exit status is the number of failing steps, so `&&` chains read naturally.

set -u

vm=ckdev
git_root=/Volumes/PRO-BLADE/git
build_type=Release
compiler=gnu13
source=head
sanitize=
only=

while [ $# -gt 0 ]; do
    case $1 in
        --debug) build_type=Debug; shift ;;
        --release) build_type=Release; shift ;;
        --clang) compiler=clang; shift ;;
        --gcc|--gnu|--gcc13) compiler=gnu13; shift ;;
        --gcc14) compiler=gnu14; shift ;;
        --hard) compiler=hard; shift ;;
        --worktree) source=worktree; shift ;;
        --head) source=head; shift ;;
        --sanitize) sanitize=address,undefined; build_type=Debug; shift ;;
        --only) only=$2; shift 2 ;;
        --vm) vm=$2; shift 2 ;;
        -h|--help) sed -n '5,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "linux-test: unknown argument: $1" >&2; exit 2 ;;
    esac
done

if ! limactl list --format '{{.Name}} {{.Status}}' 2>/dev/null | grep -q "^$vm Running"; then
    echo "linux-test: starting VM $vm"
    limactl start --tty=false "$vm" >/dev/null 2>&1 || {
        echo "linux-test: VM $vm is not available. Create it with:" >&2
        echo "    limactl create --name $vm tools/linux/ckdev.yaml" >&2
        exit 1
    }
fi

# GCC 13 is the default, not GCC 14: GitHub's ubuntu-latest runs 13.3, and a
# gate that builds on a newer compiler than CI is a gate that reports green on
# code CI will reject (a -Wdangling-reference false positive GCC 14 dropped hit
# exactly this way). --hard sweeps every compiler CI can use, so the union of
# their diagnostics is seen here before a push, never first on a runner.
case $compiler in
    gnu13) cxx=g++-13;  cc=gcc-13 ;;
    gnu14) cxx=g++-14;  cc=gcc-14 ;;
    clang) cxx=clang++; cc=clang ;;
    hard)  cxx=g++-13;  cc=gcc-13 ;;   # first pass; the sweep runs the rest
esac

# --hard: the union of every compiler CI can reach, so nothing waits for a
# runner to be found. Re-invokes this script once per compiler in the chosen
# build type; exit status is the number of configurations that failed.
if [ "$compiler" = hard ]; then
    bt_flag=--release; [ "$build_type" = Debug ] && bt_flag=--debug
    extra=""; [ -n "$only" ] && extra="--only $only"
    failed=0
    for c in --gcc13 --gcc14 --clang; do
        echo "================ hard sweep: $c $bt_flag ================"
        sh "$0" $c "$bt_flag" $extra || failed=$((failed + 1))
    done
    echo "linux-test --hard: $failed of 3 compilers reported failures"
    exit $failed
fi

# Everything below runs inside the VM. The script is passed on stdin so that
# quoting stays sane; the variables it needs travel as env arguments, each one
# its own word — an empty value must arrive empty, not as a pair of quotes.
limactl shell "$vm" -- env \
    "BUILD_TYPE=$build_type" "CXX=$cxx" "CC=$cc" "SOURCE=$source" \
    "SANITIZE=$sanitize" "ONLY=$only" "GIT_ROOT=$git_root" \
    bash -s <<'INNER'
set -u
failures=0
skipped=0
# The source mode is part of the key. A CMake cache remembers its source
# directory, so a HEAD run in a --worktree run's directory fails to configure
# -- and on 2026-09-27, with configure's failure not stopping anything, the
# build and ctest that followed ran green against the stale cache's source, the
# mounted working trees, under a header naming the HEAD clones' SHAs.
case $SOURCE in worktree) mode=wt ;; *) mode=head ;; esac
tag="$BUILD_TYPE-$CC${SANITIZE:+-san}-$mode"
build_root=$HOME/build/$tag
mkdir -p "$build_root" "$HOME/src"

echo "linux-test: $(. /etc/os-release; echo "$PRETTY_NAME") / $($CXX --version | head -1)"
echo "linux-test: $BUILD_TYPE, $CXX${SANITIZE:+, sanitize=$SANITIZE}, source=$SOURCE"
echo

# Where each repository's source comes from. HEAD means a fresh clone of the
# committed state into ~/src, refreshed every run; worktree means the mount.
source_dir() {
    repo=$1
    if [ "$SOURCE" = worktree ]; then
        echo "$GIT_ROOT/$repo"
        return
    fi
    dst=$HOME/src/$repo
    if [ -d "$dst/.git" ]; then
        git -C "$dst" fetch -q origin 2>/dev/null
        git -C "$dst" reset -q --hard origin/HEAD 2>/dev/null || git -C "$dst" reset -q --hard FETCH_HEAD
    else
        git clone -q "$GIT_ROOT/$repo" "$dst"
    fi
    echo "$dst"
}

# What a directory's bytes are: the commit, for a HEAD clone; for the mount,
# the commit it sits on plus whatever is uncommitted, which this does not
# enumerate (git status over virtiofs costs ~30 s on ckVision).
describe() {
    sha=$(git -C "$1" rev-parse --short HEAD 2>/dev/null) || { echo "not a git checkout"; return; }
    if [ "$SOURCE" = worktree ]; then echo "working tree at $sha"; else echo "$sha"; fi
}

# The source a build directory's cache actually names -- read back from the
# cache, not repeated from the argument, so the header's claim is checked by
# the one thing the build will really compile from.
cached() {   # cached <build dir> <cache variable>
    sed -n "s/^$2:[A-Z]*=//p" "$1/CMakeCache.txt" 2>/dev/null
}

step() {   # step <label> <command...>; returns the command's status
    label=$1; shift
    if "$@" > "$build_root/$label.log" 2>&1; then
        echo "  ok    $label"
    else
        rc=$?
        failures=$((failures + 1))
        echo "  FAIL  $label  (exit $rc)"
        # The first diagnostics, not the last: the first error is the cause
        # and the rest are usually its consequences.
        grep -nE 'CMake Error|error:|error C|tests failed' "$build_root/$label.log" | grep -v '^[0-9]*:FAILED:' | head -8 | cut -c1-160 | sed 's/^/          /'
        # GCC says [-Werror=shadow]; Clang says [-Werror,-Wshadow]. Count the
        # named warning either way, so the summary reads the same per compiler.
        flags=$(grep -oE '\[-W(error[=,])?-?W?[a-z=-]+\]' "$build_root/$label.log" | sed -E 's/\[-Werror[=,]-?W?/[-W/' | sort | uniq -c | sort -rn | awk '{printf "%s x%s  ", $2, $1}')
        [ -n "$flags" ] && echo "          by flag: $flags"
        echo "          full log: $build_root/$label.log"
        return 1
    fi
}

# A step that cannot mean anything because an earlier one failed. It is not
# run: a build after a failed configure compiles whatever source the previous
# cache named, and a ctest after a failed build answers from stale binaries --
# either reports green about bytes this run never compiled. The log is
# overwritten so that no earlier run's output is mistaken for this one's.
skip() {   # skip <label> <reason>
    echo "skipped: $2" > "$build_root/$1.log"
    skipped=$((skipped + 1))
    echo "  skip  $1  ($2)"
}

run_repo() {
    repo=$1; shift
    src=$(source_dir "$repo")
    bld=$build_root/$repo
    echo "== $repo  ($(describe "$src")) =="
    flags="-DCMAKE_BUILD_TYPE=$BUILD_TYPE -DCMAKE_C_COMPILER=$CC -DCMAKE_CXX_COMPILER=$CXX $*"
    # Ubuntu's GCC defines _FORTIFY_SOURCE=2 by default; Debian's does not.
    # glibc attaches warn_unused_result to write()/read() ONLY under fortify
    # (sys/cdefs.h: `__wur` is `/* Ignore */` otherwise), so without this the
    # VM is blind to the very class CI found first -- eleven ignored writes.
    # Measured, not assumed: `(void)::write(fd,&c,1)` compiles clean here
    # without the define and is an error with it. Fortify needs optimisation
    # to do anything, so it goes with the optimised configurations only.
    [ "$BUILD_TYPE" != Debug ] && flags="$flags -DCMAKE_CXX_FLAGS=-D_FORTIFY_SOURCE=2"
    [ -n "$SANITIZE" ] && flags="$flags -DCKVISION_SANITIZE=$SANITIZE -DCKMUX_SANITIZE=$SANITIZE"
    # shellcheck disable=SC2086
    if ! step "$repo-configure" cmake -S "$src" -B "$bld" -G Ninja $flags; then
        skip "$repo-build" "configure failed"
        skip "$repo-test"  "configure failed"
        echo
        return
    fi
    home=$(cached "$bld" CMAKE_HOME_DIRECTORY)
    echo "        from  $home  ($(describe "$home"))"
    [ "$home" = "$src" ] || echo "        (NOT the source this run asked for: $src)"
    ckv=$(cached "$bld" CKMUX_CKVISION_SOURCE_DIR)
    [ -n "$ckv" ] && echo "        with  ckvision from $ckv  ($(describe "$ckv"))"
    # Keep going past the first failure: one run should yield the whole
    # inventory of what this compiler objects to, not the first item of it.
    # That inventory is the build step's own; the tests do not run on it.
    if step "$repo-build" cmake --build "$bld" -j"$(nproc)" -- -k 0; then
        step "$repo-test" ctest --test-dir "$bld" --output-on-failure --timeout 300
    else
        skip "$repo-test" "build failed"
    fi
    echo
}

# ckVision first: ckmux builds against it as a sibling, and if ckVision does
# not compile there is nothing to say about ckmux yet.
[ -z "$ONLY" ] || [ "$ONLY" = ckvision ] && run_repo ckvision -DCKVISION_BUILD_EXAMPLES=ON
[ -z "$ONLY" ] || [ "$ONLY" = ckmux ]    && run_repo ckmux    -DCKMUX_CKVISION_SOURCE_DIR="$(source_dir ckvision)"

echo "linux-test: $failures failing step(s), $skipped skipped; build dirs under $build_root"
exit $failures
INNER
