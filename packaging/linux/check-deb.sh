#!/usr/bin/env bash
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
set -euo pipefail

if [[ $# -lt 4 || $# -gt 5 ]]; then
    echo 'Usage: bash check-deb.sh PACKAGE SCRATCH VERSION ARCHITECTURE [INSTALLED_EXECUTABLE]' >&2
    exit 2
fi
package=$(realpath "$1")
scratch=$(realpath -m "$2")
version=$3
architecture=$4
if ! (cd "$(dirname "$package")" && sha256sum --strict -c "$(basename "$package").sha256"); then
    echo 'DEB checksum verification failed' >&2
    exit 1
fi
if [[ $(dpkg-deb -f "$package" Package) != ckmux || $(dpkg-deb -f "$package" Version) != "$version" ]]; then
    echo 'DEB package name or version mismatch' >&2
    exit 1
fi
if [[ $(dpkg-deb -f "$package" Architecture) != "$architecture" || $(dpkg --print-architecture) != "$architecture" ]]; then
    echo 'DEB architecture mismatch' >&2
    exit 1
fi
mkdir "$scratch"
mkdir "$scratch/tmp"
export TMPDIR="$scratch/tmp"
dpkg-deb -x "$package" "$scratch/root"
executable=$(realpath "${5:-$scratch/root/usr/bin/ckmux}")
if [[ $# -eq 5 ]]; then
    while IFS= read -r -d '' packaged_file; do
        cmp "$packaged_file" "${packaged_file#"$scratch/root"}"
    done < <(find "$scratch/root/usr" -type f -print0)
fi
[[ $("$executable" --version) == "ckmux $version" ]]
export CKMUX_CONFIG="$scratch/config"
export CKMUX_SOCKET="$scratch/sock"
"$executable" check-config
trap '"$executable" kill-server >/dev/null 2>&1 || true' EXIT
"$executable" new -s package-check "printf verified > \"$scratch/child\"; exec sleep 30"
for attempt in {1..50}; do
    [[ -f "$scratch/child" ]] && break
    sleep 0.1
done
[[ $(cat "$scratch/child") == verified ]]
"$executable" ls > "$scratch/sessions.txt"
grep -F 'package-check: 1 terminal' "$scratch/sessions.txt"
server_pid=
for process_executable in /proc/[0-9]*/exe; do
    [[ $(readlink "$process_executable" 2>/dev/null || true) == "$executable" ]] || continue
    candidate_pid=${process_executable#/proc/}
    candidate_pid=${candidate_pid%/exe}
    if tr '\0' '\n' < "/proc/$candidate_pid/cmdline" | grep -Fxq "$CKMUX_SOCKET"; then
        [[ -z "$server_pid" ]]
        server_pid=$candidate_pid
    fi
done
[[ -n "$server_pid" ]]
"$executable" kill-server
for attempt in {1..50}; do
    [[ ! -e /proc/$server_pid/exe ]] && break
    sleep 0.1
done
[[ ! -e /proc/$server_pid/exe ]]
trap - EXIT
{
    printf 'PASS: checksum, version, native architecture, child command, session listing, exact server executable, shutdown\n'
    printf 'Version=%s\nArchitecture=%s\nExecutable=%s\nServerPID=%s\n' "$version" "$architecture" "$executable" "$server_pid"
    sha256sum "$package" "$executable"
} | tee "$scratch/verification.txt"
