// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// The two flags every descriptor ckmux opens has to carry. One place, because
// every part of the platform layer opens descriptors and neither flag is what
// the system gives by default.
//
// FD_CLOEXEC: a descriptor without it is inherited by every program ckmux
// starts. A shell running in a terminal window would hold the control socket
// the whole session is driven through, the server's listening socket and the
// start lock that decides which server wins a race — all of them usable, and
// none of them reachable by anything that could refuse them. Where the system
// can create a descriptor close-on-exec in the same call (O_CLOEXEC,
// SOCK_CLOEXEC, accept4, pipe2) that form is used instead, because a fork
// between the create and the fcntl inherits exactly what this prevents; macOS
// has none of those forms for sockets, so the two-call form here is the path
// the development machine takes, not a fallback nobody exercises.
//
// O_NONBLOCK: ckmux is one thread around one loop, so a read or a write that
// waits is a screen that has stopped being drawn.
#pragma once

#include <fcntl.h>
#include <unistd.h>

namespace ckm::platform {

inline bool set_close_on_exec(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFD, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

inline bool set_non_blocking(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// A pipe whose ends are close-on-exec from the moment they exist. The ends a
// child is meant to have are handed to it by `dup2`, which clears the flag on
// the copy — so this costs the child nothing and keeps every other descriptor
// out of its hands. Leaves both ends at -1 when it fails, so a caller cannot
// close a number it never got.
inline bool make_pipe(int ends[2]) noexcept {
    ends[0] = -1;
    ends[1] = -1;
#if defined(__linux__)
    return ::pipe2(ends, O_CLOEXEC) == 0;
#else
    if (::pipe(ends) != 0) {
        ends[0] = -1;
        ends[1] = -1;
        return false;
    }
    if (set_close_on_exec(ends[0]) && set_close_on_exec(ends[1])) return true;
    (void)::close(ends[0]);
    (void)::close(ends[1]);
    ends[0] = -1;
    ends[1] = -1;
    return false;
#endif
}

}  // namespace ckm::platform
