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
# which is where every build directory goes regardless.
#
# Exit status is the number of failing steps, so `&&` chains read naturally.

set -u

vm=ckdev
git_root=/Volumes/PRO-BLADE/git
build_type=Release
compiler=gnu
source=head
sanitize=
only=

while [ $# -gt 0 ]; do
    case $1 in
        --debug) build_type=Debug; shift ;;
        --release) build_type=Release; shift ;;
        --clang) compiler=clang; shift ;;
        --gcc|--gnu) compiler=gnu; shift ;;
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

case $compiler in
    gnu)   cxx=g++;     cc=gcc ;;
    clang) cxx=clang++; cc=clang ;;
esac

# Everything below runs inside the VM. The script is passed on stdin so that
# quoting stays sane; the variables it needs travel as env arguments, each one
# its own word — an empty value must arrive empty, not as a pair of quotes.
limactl shell "$vm" -- env \
    "BUILD_TYPE=$build_type" "CXX=$cxx" "CC=$cc" "SOURCE=$source" \
    "SANITIZE=$sanitize" "ONLY=$only" "GIT_ROOT=$git_root" \
    bash -s <<'INNER'
set -u
failures=0
tag="$BUILD_TYPE-$CC${SANITIZE:+-san}"
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

step() {   # step <label> <command...>
    label=$1; shift
    if "$@" > "$build_root/$label.log" 2>&1; then
        echo "  ok    $label"
    else
        rc=$?
        failures=$((failures + 1))
        echo "  FAIL  $label  (exit $rc)"
        # The first diagnostics, not the last: the first error is the cause
        # and the rest are usually its consequences.
        grep -nE 'error:|error C|tests failed' "$build_root/$label.log" | grep -v '^[0-9]*:FAILED:' | head -8 | cut -c1-160 | sed 's/^/          /'
        flags=$(grep -oE '\[-W[a-z=-]+\]' "$build_root/$label.log" | sort | uniq -c | sort -rn | awk '{printf "%s x%s  ", $2, $1}')
        [ -n "$flags" ] && echo "          by flag: $flags"
        echo "          full log: $build_root/$label.log"
    fi
}

run_repo() {
    repo=$1; shift
    src=$(source_dir "$repo")
    bld=$build_root/$repo
    echo "== $repo  ($(git -C "$src" rev-parse --short HEAD 2>/dev/null || echo worktree)) =="
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
    step "$repo-configure" cmake -S "$src" -B "$bld" -G Ninja $flags
    # Keep going past the first failure: one run should yield the whole
    # inventory of what this compiler objects to, not the first item of it.
    step "$repo-build"     cmake --build "$bld" -j"$(nproc)" -- -k 0
    step "$repo-test"      ctest --test-dir "$bld" --output-on-failure --timeout 300
    echo
}

# ckVision first: ckmux builds against it as a sibling, and if ckVision does
# not compile there is nothing to say about ckmux yet.
[ -z "$ONLY" ] || [ "$ONLY" = ckvision ] && run_repo ckvision -DCKVISION_BUILD_EXAMPLES=ON
[ -z "$ONLY" ] || [ "$ONLY" = ckmux ]    && run_repo ckmux    -DCKMUX_CKVISION_SOURCE_DIR="$(source_dir ckvision)"

echo "linux-test: $failures failing step(s); build dirs under $build_root"
exit $failures
INNER
