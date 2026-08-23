// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// ckspy — what a process actually opened and actually spawned, recorded from
// inside it, for tests that must assert a NEGATIVE about a run.
//
// WP-21 §5 needs to say "ckmux opened no printer device and ran no command to
// handle the job", and §5 is explicit that such a claim must come from
// watching the run rather than from reading the source: "we grepped for `lpr`"
// is a statement about the code its author remembered writing.
//
// The obvious instrument — sampling `lsof` and `pgrep` in a loop — cannot see
// a device opened and closed between two samples, nor a child that lived and
// died between them. That gap is not theoretical for a printer: opening a
// device, writing a job and closing it is exactly the shape that fits between
// two samples. An interposer has no such gap, because it is called by the
// thing it is measuring.
//
// USAGE
//
//     cc -dynamiclib -o ckspy.dylib tools/ckspy.c
//     CKSPY_LOG=/path/to/log DYLD_INSERT_LIBRARIES=/path/to/ckspy.dylib <program>
//
// Each line is one event: `[OPEN] pid=… path=…`, `[FORK] parent=… child=…`,
// `[SPAWN] parent=… child=…`, `[EXEC] pid=… path=…`. The instrument records;
// the TEST decides what counts as a printer device or a spooler. Keeping the
// judgement out of here is deliberate: a filter compiled into the instrument
// is a filter nobody reviews, and the one question this exists to answer is
// whether something the author did not anticipate happened.
//
// THREE CAVEATS, each of which makes a result wrong if forgotten
//
// 1. `DYLD_INSERT_LIBRARIES` is INHERITED ACROSS EXEC, which is the point: a
//    shell ckmux spawns is measured too, so a spooler run by a child is still
//    caught. It also means the log contains events from every descendant, and
//    a reader who assumes one process will misattribute them. Every line
//    carries its own pid for exactly that reason.
// 2. This cannot see libSystem's INTERNAL calls. A `fork` made inside
//    libSystem does not go through the dynamic symbol this interposes, so a
//    zero here is "nothing went through the public entry points", not "no
//    process was ever created". It is the same limit the earlier `forkspy`
//    had, and the reason `forkspy` was never a superset of `ptyspy`.
// 3. Hardened runtimes and SIP-protected binaries ignore
//    `DYLD_INSERT_LIBRARIES` entirely. A test that does not confirm the
//    instrument FIRED cannot tell that case from a clean run — see below.
//
// AND THE RULE THAT MATTERS MOST: A ZERO FROM AN INSTRUMENT YOU HAVE NOT SEEN
// FIRE IS WORTHLESS. Every one of the three caveats above turns a broken
// measurement into an empty log, and an empty log reads exactly like a clean
// run. So any test asserting "no device was opened" must ALSO assert that the
// log contains events it expected to be there — the process opened SOMETHING,
// the server forked its shell. `ckspy_selftest` beside this file exists to
// make that check cheap.
#include <fcntl.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct interpose_s {
    const void* new_func;
    const void* orig_func;
} interpose_t;

// Written with `write(2)` to an appended fd rather than through stdio, because
// this runs on both sides of a `fork` and stdio buffers are not safe there: a
// buffered line inherited by a child is written twice, once by each process,
// and the log then reports events that never happened.
static void note(const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    const int length = vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (length <= 0) return;
    const size_t bytes = (size_t)length < sizeof line ? (size_t)length : sizeof line - 1;
    const char* const path = getenv("CKSPY_LOG");
    if (path == NULL) {
        (void)write(2, line, bytes);
        return;
    }
    const int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0) return;
    (void)write(fd, line, bytes);
    (void)close(fd);
}

static pid_t spy_fork(void) {
    const pid_t result = fork();
    // The PARENT only. A child logging its own creation would double every
    // event, and doing it between `fork` and `exec` is the least safe moment
    // in a process's life to touch a lock.
    if (result > 0) note("[FORK] parent=%d child=%d\n", (int)getpid(), (int)result);
    return result;
}

static int spy_posix_spawn(pid_t* pid, const char* path, const posix_spawn_file_actions_t* actions,
                           const posix_spawnattr_t* attributes, char* const argv[],
                           char* const environment[]) {
    const int result = posix_spawn(pid, path, actions, attributes, argv, environment);
    if (result == 0 && pid != NULL)
        note("[SPAWN] parent=%d child=%d path=%s\n", (int)getpid(), (int)*pid,
             path != NULL ? path : "?");
    return result;
}

static int spy_execve(const char* path, char* const argv[], char* const environment[]) {
    // Logged BEFORE the call, because a successful `execve` never returns and
    // an event recorded afterwards would only ever record the failures.
    note("[EXEC] pid=%d path=%s\n", (int)getpid(), path != NULL ? path : "?");
    return execve(path, argv, environment);
}

static int spy_open(const char* path, int flags, ...) {
    mode_t mode = 0;
    if ((flags & O_CREAT) != 0) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }
    const int fd = open(path, flags, mode);
    // Failures are recorded too: an ATTEMPT to open a printer device is the
    // thing §5 forbids, and whether the device happened to be present on the
    // machine running the test is not part of the claim.
    note("[OPEN] pid=%d fd=%d path=%s\n", (int)getpid(), fd, path != NULL ? path : "?");
    return fd;
}

__attribute__((used)) static const interpose_t ckspy[] __attribute__((section("__DATA,__interpose"))) = {
    {(const void*)spy_fork, (const void*)fork},
    {(const void*)spy_posix_spawn, (const void*)posix_spawn},
    {(const void*)spy_execve, (const void*)execve},
    {(const void*)spy_open, (const void*)open},
};
